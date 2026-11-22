#pragma once

#include "GarrysMod/Lua/Interface.h"

namespace rcx::engine
{
	// Registers the engine features and their build checks with the core. Call at module open.
	void RegisterFeatures();

	// Pushes the rcx.engine table: GetBuilds, Disable (off only), ClearLatch.
	void PushTable(GarrysMod::Lua::ILuaBase* lua);
}
