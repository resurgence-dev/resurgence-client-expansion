#pragma once

#include "GarrysMod/Lua/Interface.h"

namespace rcx::mesh
{
	// Pushes the rcx.mesh table: Build, Poll, Cancel, Exists, Stats.
	void PushTable(GarrysMod::Lua::ILuaBase* lua);

	// Stops the build worker (the module's close; queued jobs are dropped).
	void OnLuaClose();
}
