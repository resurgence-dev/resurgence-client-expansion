/*
	Reads rcx.sys's memory numbers outside the game and checks they're sane: a working set and private
	bytes above zero, available physical memory below the total, commit charge below its limit, a page
	fault rate after a second read, and the VRAM budget read or a reason why not. Prints them.
*/
#include <cstdio>
#include <string>
#include <vector>

#include "sys/sys_memory.h"

int main()
{
	int failures = 0;
	rcx::sys::MemoryInfo info{};
	std::string reason;

	auto check = [&failures](bool ok, const char* what) {
		std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
		failures += ok ? 0 : 1;
	};

	check(rcx::sys::ReadMemory(info, reason), "the first read works");

	std::vector<char> touched(64 * 1048576, 1);
	check(rcx::sys::ReadMemory(info, reason), "the second read works");

	double megabyte = 1048576.0;
	check(info.workingSet > 0 && info.privateBytes >= touched.size(), "a working set, and private bytes holding the 64 MB just touched");
	check(info.availablePhysical > 0 && info.availablePhysical < info.totalPhysical, "available physical memory below the total");
	check(info.commitCharge > 0 && info.commitCharge <= info.commitLimit, "commit charge within its limit");
	check(info.pageFaultsPerSecond > 0.0, "a page fault rate after touching new pages");
	check(info.vramValid || !info.vramReason.empty(), "the VRAM budget, or a reason it isn't read");

	std::printf("working set %.0f MB, private %.0f MB, page faults %llu (%.0f/s), physical %.0f of %.0f MB free, commit %.0f of %.0f MB\n",
		info.workingSet / megabyte, info.privateBytes / megabyte, static_cast<unsigned long long>(info.pageFaults), info.pageFaultsPerSecond,
		info.availablePhysical / megabyte, info.totalPhysical / megabyte, info.commitCharge / megabyte, info.commitLimit / megabyte);

	if (info.vramValid)
	{
		std::printf("vram on %s: %.0f MB dedicated, budget %.0f MB, used %.0f MB; shared budget %.0f MB, used %.0f MB\n", info.adapterName.c_str(),
			info.vramDedicated / megabyte, info.vramBudget / megabyte, info.vramUsage / megabyte, info.sharedBudget / megabyte, info.sharedUsage / megabyte);
	}
	else
	{
		std::printf("no vram: %s\n", info.vramReason.c_str());
	}

	std::printf("%d failure(s)\n", failures);
	return failures == 0 ? 0 : 1;
}
