#pragma once

#include "GarrysMod/Lua/Interface.h"

namespace rcx::ik
{
	void RegisterFeature();

	// Pushes the rcx.ik table.
	void PushTable(GarrysMod::Lua::ILuaBase* lua);
}
