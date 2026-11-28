#include "core/detour.h"

#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include <windows.h>
#include <tlhelp32.h>

namespace rcx::detour
{
	namespace
	{
		constexpr size_t kJumpLength = 5;
		constexpr size_t kSlotSize = 128;
		constexpr size_t kTrampolineOffset = 16;
		constexpr size_t kPageSize = 4096;
		constexpr int64_t kNearRange = 0x60000000;
		constexpr int kWriteAttempts = 50;
		constexpr size_t kMaxThreads = 4096;

		struct Page
		{
			uint8_t* base = nullptr;
			size_t used = 0;
		};

		std::mutex pageMutex;
		std::vector<Page> pages;

		bool Near(const void* a, const void* b)
		{
			int64_t distance = reinterpret_cast<int64_t>(a) - reinterpret_cast<int64_t>(b);
			return distance > -kNearRange && distance < kNearRange;
		}

		uint8_t* TryAllocateAt(uintptr_t address)
		{
			MEMORY_BASIC_INFORMATION info{};

			if (VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info)) == 0 || info.State != MEM_FREE)
			{
				return nullptr;
			}

			return static_cast<uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(address), kPageSize, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
		}

		// A free kSlotSize block of executable memory within kNearRange of target, or nullptr.
		uint8_t* AllocateNear(const void* target)
		{
			std::lock_guard<std::mutex> lock(pageMutex);

			for (Page& page : pages)
			{
				if (page.used + kSlotSize <= kPageSize && Near(page.base, target))
				{
					uint8_t* slot = page.base + page.used;
					page.used += kSlotSize;
					return slot;
				}
			}

			SYSTEM_INFO system{};
			GetSystemInfo(&system);
			uintptr_t granularity = system.dwAllocationGranularity;
			uintptr_t origin = reinterpret_cast<uintptr_t>(target) & ~(granularity - 1);
			uint8_t* base = nullptr;

			for (uintptr_t step = granularity; base == nullptr && step < static_cast<uintptr_t>(kNearRange); step += granularity)
			{
				if (origin > step)
				{
					base = TryAllocateAt(origin - step);
				}

				if (base == nullptr)
				{
					base = TryAllocateAt(origin + step);
				}
			}

			if (base == nullptr || !Near(base, target))
			{
				return nullptr;
			}

			std::memset(base, 0xcc, kPageSize);
			pages.push_back(Page{base, kSlotSize});
			return base;
		}

		void WriteAbsoluteJump(uint8_t* at, const void* destination)
		{
			// jmp qword ptr [rip+0]; dq destination
			at[0] = 0xff;
			at[1] = 0x25;
			std::memset(at + 2, 0, 4);
			uint64_t address = reinterpret_cast<uint64_t>(destination);
			std::memcpy(at + 6, &address, sizeof(address));
		}

		// Jumps through the pointer at `pointer`, read when the jump runs; rax is kept.
		size_t WriteIndirectJump(uint8_t* at, const void* pointer)
		{
			const uint8_t prefix[] = {0x50, 0x48, 0xb8};
			const uint8_t suffix[] = {0x48, 0x8b, 0x00, 0x48, 0x87, 0x04, 0x24, 0xc3};
			uint64_t address = reinterpret_cast<uint64_t>(pointer);
			std::memcpy(at, prefix, sizeof(prefix));
			std::memcpy(at + sizeof(prefix), &address, sizeof(address));
			std::memcpy(at + sizeof(prefix) + sizeof(address), suffix, sizeof(suffix));
			return sizeof(prefix) + sizeof(address) + sizeof(suffix);
		}

		std::string ModuleOf(const void* address)
		{
			HMODULE module = nullptr;
			DWORD flags = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;

			if (!GetModuleHandleExW(flags, reinterpret_cast<LPCWSTR>(address), &module))
			{
				return "no module";
			}

			char path[MAX_PATH]{};
			DWORD length = GetModuleFileNameA(module, path, MAX_PATH);
			std::string text(path, length);
			size_t slash = text.find_last_of("\\/");
			return slash == std::string::npos ? text : text.substr(slash + 1);
		}

		// Writes count bytes at target with every other thread suspended, retrying while one is
		// stopped inside the first guardLength bytes. Nothing between suspend and resume allocates.
		bool WriteSuspended(void* target, const uint8_t* bytes, size_t count, size_t guardLength)
		{
			std::vector<DWORD> threadIds;
			threadIds.reserve(kMaxThreads);
			std::vector<HANDLE> suspended;
			suspended.reserve(kMaxThreads);
			uintptr_t start = reinterpret_cast<uintptr_t>(target);

			for (int attempt = 0; attempt < kWriteAttempts; attempt++)
			{
				threadIds.clear();
				suspended.clear();
				HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);

				if (snapshot == INVALID_HANDLE_VALUE)
				{
					return false;
				}

				THREADENTRY32 entry{};
				entry.dwSize = sizeof(entry);
				DWORD process = GetCurrentProcessId();
				DWORD self = GetCurrentThreadId();

				for (BOOL more = Thread32First(snapshot, &entry); more && threadIds.size() < kMaxThreads; more = Thread32Next(snapshot, &entry))
				{
					if (entry.th32OwnerProcessID == process && entry.th32ThreadID != self)
					{
						threadIds.push_back(entry.th32ThreadID);
					}
				}

				CloseHandle(snapshot);
				bool busy = false;

				for (DWORD id : threadIds)
				{
					HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, id);

					if (thread == nullptr)
					{
						continue;
					}

					if (SuspendThread(thread) == static_cast<DWORD>(-1))
					{
						CloseHandle(thread);
						continue;
					}

					suspended.push_back(thread);
					CONTEXT context{};
					context.ContextFlags = CONTEXT_CONTROL;

					if (GetThreadContext(thread, &context) && context.Rip >= start && context.Rip < start + guardLength)
					{
						busy = true;
					}
				}

				bool written = false;

				if (!busy)
				{
					DWORD oldProtection = 0;

					if (VirtualProtect(target, count, PAGE_EXECUTE_READWRITE, &oldProtection))
					{
						std::memcpy(target, bytes, count);
						DWORD unused = 0;
						VirtualProtect(target, count, oldProtection, &unused);
						FlushInstructionCache(GetCurrentProcess(), target, count);
						written = true;
					}
				}

				for (HANDLE thread : suspended)
				{
					ResumeThread(thread);
					CloseHandle(thread);
				}

				if (!busy)
				{
					return written;
				}

				Sleep(1);
			}

			return false;
		}
	}

	bool Decode(const uint8_t* code, size_t available, Decoded& decoded)
	{
		decoded = Decoded{};
		size_t index = 0;
		bool operand16 = false;
		bool rexW = false;

		while (index < available)
		{
			uint8_t prefix = code[index];
			bool legacy = prefix == 0x66 || prefix == 0x67 || prefix == 0xf2 || prefix == 0xf3 || prefix == 0xf0 || prefix == 0x2e || prefix == 0x3e ||
				prefix == 0x26 || prefix == 0x36 || prefix == 0x64 || prefix == 0x65;

			if (!legacy)
			{
				break;
			}

			operand16 = operand16 || prefix == 0x66;
			index++;
		}

		if (index < available && (code[index] & 0xf0) == 0x40)
		{
			rexW = (code[index] & 0x08) != 0;
			index++;
		}

		if (index >= available)
		{
			return false;
		}

		uint8_t opcode = code[index++];
		bool modrm = false;
		size_t immediate = 0;
		size_t wordImmediate = operand16 ? 2 : 4;
		int groupOpcode = -1;

		if (opcode == 0x0f)
		{
			if (index >= available)
			{
				return false;
			}

			uint8_t second = code[index++];
			bool known = second == 0x1f || second == 0x10 || second == 0x11 || second == 0x28 || second == 0x29 || second == 0x6f || second == 0x7f ||
				second == 0xb6 || second == 0xb7 || second == 0xbe || second == 0xbf || second == 0xaf || second == 0x57 || second == 0x54 ||
				(second >= 0x40 && second <= 0x4f);

			if (!known)
			{
				return false;
			}

			modrm = true;
		}
		else if ((opcode >= 0x50 && opcode <= 0x5f) || opcode == 0x90 || opcode == 0xc3 || opcode == 0xcc || opcode == 0x98 || opcode == 0x99)
		{
		}
		else if (opcode <= 0x3f && (opcode & 0x07) <= 0x03 && opcode != 0x0f)
		{
			// add, or, adc, sbb, and, sub, xor, cmp with a ModRM
			modrm = true;
		}
		else if (opcode <= 0x3f && (opcode & 0x07) == 0x04)
		{
			immediate = 1;
		}
		else if (opcode <= 0x3f && (opcode & 0x07) == 0x05)
		{
			immediate = wordImmediate;
		}
		else if ((opcode >= 0x84 && opcode <= 0x8b) || opcode == 0x8d || opcode == 0x63 || (opcode >= 0xd0 && opcode <= 0xd3) || opcode == 0xfe)
		{
			modrm = true;
		}
		else if (opcode == 0x80 || opcode == 0x83 || opcode == 0xc0 || opcode == 0xc1 || opcode == 0xc6 || opcode == 0x6b)
		{
			modrm = true;
			immediate = 1;
		}
		else if (opcode == 0x81 || opcode == 0xc7 || opcode == 0x69)
		{
			modrm = true;
			immediate = wordImmediate;
		}
		else if (opcode == 0xa8 || opcode == 0x6a || (opcode >= 0xb0 && opcode <= 0xb7))
		{
			immediate = 1;
		}
		else if (opcode == 0xa9 || opcode == 0x68)
		{
			immediate = opcode == 0x68 ? 4 : wordImmediate;
		}
		else if (opcode >= 0xb8 && opcode <= 0xbf)
		{
			immediate = rexW ? 8 : wordImmediate;
		}
		else if (opcode == 0xe9)
		{
			immediate = 4;
			decoded.jump = true;
		}
		else if (opcode == 0xeb)
		{
			immediate = 1;
			decoded.jump = true;
		}
		else if (opcode == 0xf6 || opcode == 0xf7 || opcode == 0xff)
		{
			modrm = true;
			groupOpcode = opcode;
		}
		else
		{
			return false;
		}

		if (modrm)
		{
			if (index >= available)
			{
				return false;
			}

			uint8_t value = code[index++];
			uint8_t mod = value >> 6;
			uint8_t reg = (value >> 3) & 0x07;
			uint8_t rm = value & 0x07;
			size_t displacement = 0;

			if (groupOpcode == 0xf6 && reg <= 1)
			{
				immediate = 1;
			}
			else if (groupOpcode == 0xf7 && reg <= 1)
			{
				immediate = wordImmediate;
			}
			else if (groupOpcode == 0xff)
			{
				// call (2, 3) and far jump (5) aren't moved; jmp (4) is a jump
				if (reg == 2 || reg == 3 || reg == 5 || reg == 7)
				{
					return false;
				}

				decoded.jump = reg == 4;
			}

			if (mod != 3 && rm == 4)
			{
				if (index >= available)
				{
					return false;
				}

				uint8_t sib = code[index++];

				if (mod == 0 && (sib & 0x07) == 5)
				{
					displacement = 4;
				}
			}

			if (mod == 0 && rm == 5)
			{
				decoded.ripDisplacementOffset = index;
				displacement = 4;
			}
			else if (mod == 1)
			{
				displacement = 1;
			}
			else if (mod == 2)
			{
				displacement = 4;
			}

			index += displacement;
		}

		index += immediate;

		if (index > available)
		{
			return false;
		}

		decoded.length = index;
		return true;
	}

	std::string HexBytes(const void* address, size_t count)
	{
		std::string text;
		const uint8_t* bytes = static_cast<const uint8_t*>(address);

		for (size_t index = 0; index < count; index++)
		{
			char part[4]{};
			std::snprintf(part, sizeof(part), "%02x", bytes[index]);
			text += (index == 0 ? "" : " ") + std::string(part);
		}

		return text;
	}

	bool Install(void* target, void* hook, void** trampoline, std::string& how, std::string& reason)
	{
		*trampoline = nullptr;
		const uint8_t* code = static_cast<const uint8_t*>(target);
		std::string bytes = HexBytes(target, 16);
		uint8_t* slot = AllocateNear(target);

		if (slot == nullptr)
		{
			reason = "no executable memory within 2 GB of the function";
			return false;
		}

		uint8_t* moved = slot + kTrampolineOffset;
		size_t movedLength = 0;
		size_t written = 0;
		Decoded first{};

		if (!Decode(code, 32, first))
		{
			reason = "an instruction this decoder doesn't move, at the start: " + bytes;
			return false;
		}

		if (first.jump && code[0] == 0xe9)
		{
			int32_t relative = 0;
			std::memcpy(&relative, code + 1, sizeof(relative));
			const uint8_t* destination = code + kJumpLength + relative;
			WriteAbsoluteJump(moved, destination);
			movedLength = kJumpLength;
			how = "chained onto an existing jump to " + ModuleOf(destination) + " (" + bytes + ")";
		}
		else if (first.jump && code[0] == 0xff && code[1] == 0x25)
		{
			int32_t displacement = 0;
			std::memcpy(&displacement, code + 2, sizeof(displacement));
			const uint8_t* pointer = code + 6 + displacement;
			const void* destination = *reinterpret_cast<void* const*>(pointer);
			WriteIndirectJump(moved, pointer);
			movedLength = kJumpLength;
			how = "chained onto an existing jump to " + ModuleOf(destination) + " (" + bytes + ")";
		}
		else
		{
			bool terminal = false;

			while (movedLength < kJumpLength)
			{
				Decoded instruction{};

				if (!Decode(code + movedLength, 32 - movedLength, instruction))
				{
					reason = "an instruction this decoder doesn't move, at byte " + std::to_string(movedLength) + ": " + bytes;
					return false;
				}

				bool registerJump = instruction.jump && code[movedLength] == 0xff && instruction.ripDisplacementOffset == 0;
				bool rexRegisterJump = instruction.jump && (code[movedLength] & 0xf0) == 0x40 && code[movedLength + 1] == 0xff && instruction.ripDisplacementOffset == 0;
				uint8_t jumpOpcode = code[movedLength];

				// A relative jump ends the moved bytes (a thunk: adjust this, then jump to the body): the
				// trampoline jumps to the same place absolutely.
				if (instruction.jump && (jumpOpcode == 0xe9 || jumpOpcode == 0xeb))
				{
					int32_t offset = 0;

					if (jumpOpcode == 0xe9)
					{
						std::memcpy(&offset, code + movedLength + 1, sizeof(offset));
					}
					else
					{
						offset = static_cast<int8_t>(code[movedLength + 1]);
					}

					const uint8_t* destination = code + movedLength + instruction.length + offset;
					WriteAbsoluteJump(moved + written, destination);
					written += 14;
					movedLength += instruction.length;
					terminal = true;
					break;
				}

				if (instruction.jump && !registerJump && !rexRegisterJump)
				{
					reason = "a relative jump inside the first bytes: " + bytes;
					return false;
				}

				std::memcpy(moved + written, code + movedLength, instruction.length);

				if (instruction.ripDisplacementOffset != 0)
				{
					int32_t displacement = 0;
					std::memcpy(&displacement, code + movedLength + instruction.ripDisplacementOffset, sizeof(displacement));
					const uint8_t* absolute = code + movedLength + instruction.length + displacement;
					int64_t rewritten = absolute - (moved + written + instruction.length);

					if (rewritten < INT32_MIN || rewritten > INT32_MAX)
					{
						reason = "a RIP-relative operand out of reach of the trampoline: " + bytes;
						return false;
					}

					int32_t narrow = static_cast<int32_t>(rewritten);
					std::memcpy(moved + written + instruction.ripDisplacementOffset, &narrow, sizeof(narrow));
				}

				written += instruction.length;
				movedLength += instruction.length;

				if (instruction.jump)
				{
					terminal = true;
					break;
				}
			}

			if (movedLength < kJumpLength)
			{
				reason = "the function ends before 5 bytes: " + bytes;
				return false;
			}

			if (!terminal)
			{
				WriteAbsoluteJump(moved + written, code + movedLength);
			}

			how = "detoured, " + std::to_string(movedLength) + " bytes moved (" + bytes + ")";
		}

		WriteAbsoluteJump(slot, hook);
		FlushInstructionCache(GetCurrentProcess(), slot, kSlotSize);

		int64_t relative = slot - (code + kJumpLength);
		uint8_t patch[kJumpLength] = {0xe9};
		int32_t narrow = static_cast<int32_t>(relative);
		std::memcpy(patch + 1, &narrow, sizeof(narrow));
		*trampoline = moved;

		if (!WriteSuspended(target, patch, kJumpLength, movedLength))
		{
			*trampoline = nullptr;
			reason = "couldn't write the jump (a thread stayed inside the first bytes, or the page couldn't be made writable)";
			return false;
		}

		return true;
	}
}
