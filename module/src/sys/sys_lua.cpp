#include "sys/sys_lua.h"

#include <string>

#include "core/lua_util.h"
#include "sys/sys_memory.h"

using namespace GarrysMod::Lua;

/*
	rcx.sys for Lua: GetMemory takes no argument and returns a fresh table of numbers (megabytes,
	except the counts and rates), or nil and a reason.
*/
namespace rcx::sys
{
	namespace
	{
		constexpr double kMegabyte = 1048576.0;

		void SetNumber(ILuaBase* lua, const char* name, double value)
		{
			lua->PushNumber(value);
			lua->SetField(-2, name);
		}

		void SetMegabytes(ILuaBase* lua, const char* name, uint64_t bytes)
		{
			SetNumber(lua, name, static_cast<double>(bytes) / kMegabyte);
		}

		RCX_FUNCTION(GetMemory)
		{
			MemoryInfo info{};
			std::string reason;

			if (!ReadMemory(info, reason))
			{
				return rcx::lua::PushNilReason(lua, reason);
			}

			lua->CreateTable();
			SetMegabytes(lua, "workingSetMB", info.workingSet);
			SetMegabytes(lua, "peakWorkingSetMB", info.peakWorkingSet);
			SetMegabytes(lua, "privateMB", info.privateBytes);
			SetNumber(lua, "pageFaults", static_cast<double>(info.pageFaults));
			SetNumber(lua, "pageFaultsPerSec", info.pageFaultsPerSecond);
			SetMegabytes(lua, "totalPhysicalMB", info.totalPhysical);
			SetMegabytes(lua, "availablePhysicalMB", info.availablePhysical);
			SetNumber(lua, "memoryLoad", info.memoryLoad);
			SetMegabytes(lua, "commitMB", info.commitCharge);
			SetMegabytes(lua, "commitLimitMB", info.commitLimit);

			lua->PushBool(info.vramValid);
			lua->SetField(-2, "vramValid");

			if (info.vramValid)
			{
				lua->PushString(info.adapterName.c_str());
				lua->SetField(-2, "adapter");
				SetMegabytes(lua, "vramDedicatedMB", info.vramDedicated);
				SetMegabytes(lua, "vramBudgetMB", info.vramBudget);
				SetMegabytes(lua, "vramUsageMB", info.vramUsage);
				SetMegabytes(lua, "sharedBudgetMB", info.sharedBudget);
				SetMegabytes(lua, "sharedUsageMB", info.sharedUsage);
			}
			else
			{
				lua->PushString(info.vramReason.c_str());
				lua->SetField(-2, "vramReason");
			}

			return 1;
		}
	}

	void PushTable(ILuaBase* lua)
	{
		lua->CreateTable();
		lua->PushCFunction(GetMemory);
		lua->SetField(-2, "GetMemory");
	}
}
