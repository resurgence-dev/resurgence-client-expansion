#include "engine/engine_guard.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <set>

#include <windows.h>
#include <bcrypt.h>

#include "core/core.h"

#pragma comment(lib, "bcrypt.lib")

namespace rcx::engine
{
	namespace
	{
		// A game DLL build a feature was tested on. A feature is cleared when every DLL it names matches one of its rows.
		struct Clearance
		{
			const char* feature;
			const char* dll;
			uint32_t timeDateStamp;
			uint32_t sizeOfImage;
			const char* textHash;
		};

		// Garry's Mod x86-64 branch, PatchVersion 2026.04.29 (steam.inf), fingerprinted 2026-11-22.
		constexpr Clearance kClearances[] = {
			{"mesh_combine", "engine.dll", 0x6af5f41c, 0xe53000, "d80caacf0f3c49b56b8cc253b3f08d1145709ed286e2becf58a4b2809bb4a60b"},
			{"mesh_combine", "datacache.dll", 0x6af5f427, 0x8a000, "615794481654d23e658fd245ddf472af5ad5f9937c638203c331ab2b94c12ae4"},
			{"mesh_combine", "studiorender.dll", 0x6af5f3a7, 0x835000, "429b67b9b12482b06d651ba7f81d010675f0ad1a20190913c356800ef2f32cd0"},
		};

		constexpr size_t kMaxDllBytes = 256 * 1024 * 1024;
		constexpr size_t kMaxPolicyBytes = 64 * 1024;

		std::mutex stateMutex;
		std::vector<FeatureBuild> builds;
		std::set<std::string> armed;

		std::string Hex(const unsigned char* bytes, size_t count)
		{
			static const char digits[] = "0123456789abcdef";
			std::string text;
			text.reserve(count * 2);

			for (size_t i = 0; i < count; ++i)
			{
				text.push_back(digits[bytes[i] >> 4]);
				text.push_back(digits[bytes[i] & 15]);
			}

			return text;
		}

		bool Sha256(const unsigned char* data, size_t size, std::string& hash)
		{
			unsigned char digest[32]{};

			if (size > 0xffffffffull)
			{
				return false;
			}

			NTSTATUS status = BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, const_cast<unsigned char*>(data), static_cast<ULONG>(size), digest, sizeof(digest));

			if (!BCRYPT_SUCCESS(status))
			{
				return false;
			}

			hash = Hex(digest, sizeof(digest));
			return true;
		}

		bool ReadFileBytes(const std::wstring& path, size_t limit, std::vector<unsigned char>& bytes)
		{
			std::ifstream file(path, std::ios::binary | std::ios::ate);

			if (!file)
			{
				return false;
			}

			std::streamoff size = file.tellg();

			if (size <= 0 || static_cast<size_t>(size) > limit)
			{
				return false;
			}

			bytes.resize(static_cast<size_t>(size));
			file.seekg(0);
			file.read(reinterpret_cast<char*>(bytes.data()), size);
			return static_cast<bool>(file);
		}

		template <typename T>
		bool ReadAt(const std::vector<unsigned char>& bytes, size_t offset, T& value)
		{
			if (offset > bytes.size() || bytes.size() - offset < sizeof(T))
			{
				return false;
			}

			std::memcpy(&value, bytes.data() + offset, sizeof(T));
			return true;
		}

		// The fingerprint of a DLL already loaded in the game, read from its file: the loaded copy's path, never a path from Lua.
		DllFingerprint Fingerprint(const std::string& name)
		{
			DllFingerprint print{};
			print.name = name;

			std::wstring wide(name.begin(), name.end());
			HMODULE module = GetModuleHandleW(wide.c_str());

			if (module == nullptr)
			{
				print.error = "not loaded";
				return print;
			}

			wchar_t path[MAX_PATH]{};
			DWORD length = GetModuleFileNameW(module, path, MAX_PATH);

			if (length == 0 || length >= MAX_PATH)
			{
				print.error = "path unreadable";
				return print;
			}

			std::vector<unsigned char> bytes;

			if (!ReadFileBytes(std::wstring(path, length), kMaxDllBytes, bytes))
			{
				print.error = "file unreadable";
				return print;
			}

			uint32_t peOffset = 0;
			uint32_t signature = 0;
			uint16_t sectionCount = 0;
			uint16_t optionalSize = 0;

			if (!ReadAt(bytes, 0x3c, peOffset) || !ReadAt(bytes, peOffset, signature) || signature != 0x00004550 ||
				!ReadAt(bytes, peOffset + 6, sectionCount) || !ReadAt(bytes, peOffset + 8, print.timeDateStamp) ||
				!ReadAt(bytes, peOffset + 20, optionalSize) || !ReadAt(bytes, static_cast<size_t>(peOffset) + 24 + 56, print.sizeOfImage))
			{
				print.error = "not a PE file";
				return print;
			}

			size_t sectionTable = static_cast<size_t>(peOffset) + 24 + optionalSize;

			for (uint16_t i = 0; i < sectionCount; ++i)
			{
				size_t section = sectionTable + static_cast<size_t>(i) * 40;
				char sectionName[9]{};
				uint32_t rawSize = 0;
				uint32_t rawPointer = 0;

				if (section + 40 > bytes.size())
				{
					break;
				}

				std::memcpy(sectionName, bytes.data() + section, 8);

				if (std::strcmp(sectionName, ".text") != 0)
				{
					continue;
				}

				if (!ReadAt(bytes, section + 16, rawSize) || !ReadAt(bytes, section + 20, rawPointer) ||
					rawPointer > bytes.size() || bytes.size() - rawPointer < rawSize)
				{
					print.error = ".text out of range";
					return print;
				}

				if (!Sha256(bytes.data() + rawPointer, rawSize, print.textHash))
				{
					print.error = "hash failed";
				}

				return print;
			}

			print.error = "no .text section";
			return print;
		}

		bool Matches(const char* feature, const DllFingerprint& print)
		{
			for (const Clearance& clearance : kClearances)
			{
				if (std::strcmp(clearance.feature, feature) == 0 && print.name == clearance.dll &&
					print.timeDateStamp == clearance.timeDateStamp && print.sizeOfImage == clearance.sizeOfImage &&
					print.textHash == clearance.textHash)
				{
					return true;
				}
			}

			return false;
		}

		std::string BuildKey(const std::vector<DllFingerprint>& prints)
		{
			std::string key;

			for (const DllFingerprint& print : prints)
			{
				char stamp[16]{};
				std::snprintf(stamp, sizeof(stamp), "%08x", print.timeDateStamp);

				if (!key.empty())
				{
					key += ";";
				}

				key += print.name + "@" + stamp + "-" + print.textHash.substr(0, 16);
			}

			return key;
		}

		// The quoted strings of the array named `name` in a small flat JSON file. Strings with escapes are skipped.
		std::vector<std::string> ReadStringArray(const std::string& text, const char* name)
		{
			std::vector<std::string> values;
			std::string quoted = std::string("\"") + name + "\"";
			size_t at = text.find(quoted);

			if (at == std::string::npos)
			{
				return values;
			}

			at = text.find_first_not_of(" \t\r\n", at + quoted.size());

			if (at == std::string::npos || text[at] != ':')
			{
				return values;
			}

			at = text.find_first_not_of(" \t\r\n", at + 1);

			if (at == std::string::npos || text[at] != '[')
			{
				return values;
			}

			size_t end = text.find(']', at);

			if (end == std::string::npos)
			{
				return values;
			}

			size_t cursor = at + 1;

			while (cursor < end && values.size() < 64)
			{
				size_t open = text.find('"', cursor);

				if (open == std::string::npos || open >= end)
				{
					break;
				}

				size_t close = text.find('"', open + 1);

				if (close == std::string::npos || close >= end)
				{
					break;
				}

				std::string value = text.substr(open + 1, close - open - 1);

				if (value.find('\\') == std::string::npos && value.size() <= 256)
				{
					values.push_back(value);
				}

				cursor = close + 1;
			}

			return values;
		}

		std::wstring StateFolder()
		{
			std::wstring folder = rcx::core::GetModuleFolder();
			return folder.empty() ? folder : folder + L"rcx_state\\";
		}

		std::wstring LatchPath(const std::string& feature)
		{
			std::wstring folder = StateFolder();
			return folder.empty() ? folder : folder + L"armed_" + std::wstring(feature.begin(), feature.end());
		}

		void ApplyPolicy(FeatureBuild& build)
		{
			std::wstring folder = rcx::core::GetModuleFolder();

			if (folder.empty())
			{
				return;
			}

			std::vector<unsigned char> bytes;

			if (!ReadFileBytes(folder + L"rcx_policy.json", kMaxPolicyBytes, bytes))
			{
				return;
			}

			std::string text(bytes.begin(), bytes.end());

			for (const std::string& name : ReadStringArray(text, "disabled"))
			{
				if (name == "all" || name == build.feature)
				{
					rcx::core::DisableFeature(build.feature.c_str(), "turned off by the launcher's policy (rcx_policy.json)");
				}
			}

			for (const std::string& key : ReadStringArray(text, "revoked"))
			{
				if (key == build.key)
				{
					rcx::core::DisableFeature(build.feature.c_str(), "this game build's clearance was revoked by the launcher's policy (rcx_policy.json)");
				}
			}
		}

		void CheckLatch(FeatureBuild& build)
		{
			std::wstring path = LatchPath(build.feature);

			if (path.empty())
			{
				return;
			}

			std::vector<unsigned char> bytes;

			if (!ReadFileBytes(path, 4096, bytes))
			{
				return;
			}

			build.latched = true;
			std::string key(bytes.begin(), bytes.end());
			rcx::core::DisableFeature(build.feature.c_str(),
				"the last session ended without a clean close while it was in use (build " + key + "); rcx.engine.ClearLatch turns it back on from the next load");
		}
	}

	void RegisterEngineFeature(const char* feature, const std::vector<std::string>& dlls)
	{
		rcx::core::RegisterFeature(feature);
		rcx::core::ResetFeature(feature);

		FeatureBuild build{};
		build.feature = feature;

		for (const std::string& dll : dlls)
		{
			build.dlls.push_back(Fingerprint(dll));
		}

		build.key = BuildKey(build.dlls);
		build.cleared = !build.dlls.empty();
		std::string failure;

		for (const DllFingerprint& print : build.dlls)
		{
			if (!print.error.empty())
			{
				build.cleared = false;
				failure = print.name + ": " + print.error;
				break;
			}

			if (!Matches(feature, print))
			{
				build.cleared = false;
				failure = print.name + " is a game build this module wasn't cleared on";
				break;
			}
		}

		if (!build.cleared)
		{
			rcx::core::DisableFeature(feature, "unknown game build (" + failure + "; build " + build.key + ")");
		}

		ApplyPolicy(build);
		CheckLatch(build);

		std::lock_guard<std::mutex> lock(stateMutex);

		for (FeatureBuild& existing : builds)
		{
			if (existing.feature == build.feature)
			{
				existing = build;
				return;
			}
		}

		builds.push_back(build);
	}

	std::vector<FeatureBuild> GetBuilds()
	{
		std::lock_guard<std::mutex> lock(stateMutex);
		return builds;
	}

	bool IsEngineFeature(const std::string& feature)
	{
		std::lock_guard<std::mutex> lock(stateMutex);

		for (const FeatureBuild& build : builds)
		{
			if (build.feature == feature)
			{
				return true;
			}
		}

		return false;
	}

	bool Arm(const char* feature)
	{
		std::string key;

		{
			std::lock_guard<std::mutex> lock(stateMutex);

			if (armed.count(feature) != 0)
			{
				return true;
			}

			for (const FeatureBuild& build : builds)
			{
				if (build.feature == feature)
				{
					key = build.key;
				}
			}
		}

		std::wstring folder = StateFolder();
		std::wstring path = LatchPath(feature);

		if (folder.empty() || path.empty())
		{
			return false;
		}

		CreateDirectoryW(folder.c_str(), nullptr);
		std::ofstream file(path, std::ios::binary | std::ios::trunc);

		if (!file)
		{
			return false;
		}

		file << key;
		file.close();

		std::lock_guard<std::mutex> lock(stateMutex);
		armed.insert(feature);
		return true;
	}

	void DisarmAll()
	{
		std::set<std::string> taken;

		{
			std::lock_guard<std::mutex> lock(stateMutex);
			taken.swap(armed);
		}

		for (const std::string& feature : taken)
		{
			std::wstring path = LatchPath(feature);

			if (!path.empty())
			{
				DeleteFileW(path.c_str());
			}
		}
	}

	bool ClearLatch(const std::string& feature, std::string& reason)
	{
		if (!IsEngineFeature(feature))
		{
			reason = "not an engine feature";
			return false;
		}

		{
			std::lock_guard<std::mutex> lock(stateMutex);

			if (armed.count(feature) != 0)
			{
				reason = "the feature is in use this session";
				return false;
			}
		}

		std::wstring path = LatchPath(feature);

		if (path.empty())
		{
			reason = "the module's folder can't be read";
			return false;
		}

		if (!DeleteFileW(path.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND)
		{
			reason = "the latch file can't be removed";
			return false;
		}

		return true;
	}
}
