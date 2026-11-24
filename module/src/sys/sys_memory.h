#pragma once

#include <cstdint>
#include <string>

namespace rcx::sys
{
	// One reading of the game process's memory, the machine's and the graphics card's, in bytes.
	// vramValid is false when no DXGI 1.4 adapter could be read (vramReason says why).
	struct MemoryInfo
	{
		uint64_t workingSet = 0;
		uint64_t peakWorkingSet = 0;
		uint64_t privateBytes = 0;
		uint64_t pageFaults = 0;
		double pageFaultsPerSecond = 0.0;
		uint64_t totalPhysical = 0;
		uint64_t availablePhysical = 0;
		uint32_t memoryLoad = 0;
		uint64_t commitCharge = 0;
		uint64_t commitLimit = 0;
		bool vramValid = false;
		std::string vramReason;
		std::string adapterName;
		uint64_t vramDedicated = 0;
		uint64_t vramBudget = 0;
		uint64_t vramUsage = 0;
		uint64_t sharedBudget = 0;
		uint64_t sharedUsage = 0;
	};

	// Reads everything; pageFaultsPerSecond is over the time since the last call (0 on the first).
	// The adapter is the DXGI adapter with the most dedicated video memory, chosen once.
	bool ReadMemory(MemoryInfo& info, std::string& reason);
}
