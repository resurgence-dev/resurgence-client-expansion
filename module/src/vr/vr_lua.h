#pragma once

#include "GarrysMod/Lua/Interface.h"

namespace rcx::vr
{
	void RegisterFeature();

	// Pushes the rcx.vr table.
	void PushTable(GarrysMod::Lua::ILuaBase* lua);

	// The Lua state that loaded the module is closing.
	void OnLuaClose();
}
