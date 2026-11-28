#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

/*
	Inline detours for x64 functions: the first instructions of a function are replaced by a
	5-byte jump to a relay near it, which jumps to the hook; the hook calls the original through a
	trampoline holding the moved instructions and a jump back. Conservative by design:

	- Only instructions this decoder knows are moved (prologue moves, pushes, arithmetic with
	  ModRM, lea, immediates, nops). Anything else (a call, a conditional jump, an unknown opcode)
	  refuses the detour with the bytes in the reason, and the caller falls back.
	- RIP-relative operands are rewritten for the trampoline's address; the trampoline and relay
	  are allocated within 2 GB of the function, so every displacement still fits.
	- A function that already starts with a jump (another program's detour: E9 rel32, FF 25
	  [rip], or mov rax, imm64; jmp rax) is chained: the trampoline goes where that jump went, read
	  at call time for FF 25, and only the first 5 bytes are written, so whatever jumps back into
	  the function after that jump still lands on the original bytes.
	- The write happens with every other thread of the process suspended, and is retried while one
	  of them is stopped inside the bytes being replaced.

	Detours stay for the life of the process (the module must be pinned first).
*/
namespace rcx::detour
{
	struct Decoded
	{
		size_t length = 0;
		// Offset of a RIP-relative disp32 inside the instruction (0: none).
		size_t ripDisplacementOffset = 0;
		// An unconditional jump: E9 rel32, EB rel8, FF /4 or FF 25.
		bool jump = false;
	};

	// Decodes one instruction at code (up to `available` bytes). false for one it doesn't know.
	bool Decode(const uint8_t* code, size_t available, Decoded& decoded);

	// The first `count` bytes at address as hex ("48 89 5c 24 08"), for diagnostics.
	std::string HexBytes(const void* address, size_t count);

	// Detours target to hook. On success trampoline calls the original and how says what was done
	// ("detoured, 7 bytes moved" or "chained onto an existing jump to <module>"); on failure reason
	// says why and nothing was written.
	bool Install(void* target, void* hook, void** trampoline, std::string& how, std::string& reason);
}
