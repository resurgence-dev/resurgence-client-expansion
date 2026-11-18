#include <string>

#include "GarrysMod/Lua/Interface.h"

#include "core/core.h"
#include "core/lua_util.h"
#include "ik/ik_lua.h"
#include "vr/vr_lua.h"

using namespace GarrysMod::Lua;

// GMod's lua_State keeps ILuaBase* at 120 bytes on 64-bit. gmod-module-base's master branch only
// knows the 32-bit layout; build against its development branch.
static_assert(sizeof(void*) == 8, "the Client Expansion is a 64-bit module");
static_assert(offsetof(lua_State, luabase) == 120, "gmod-module-base has the wrong lua_State layout: check out its development branch");

namespace rcx::core
{
	// Bump on every build that changes behavior; the text after the number names what changed.
	const char* const kVersion = "rcx 0.57 (vr input: both_ bindings, a chord of one button on both hands in each controller's default bindings; GetHandSkeleton: splay, the four gaps between fingers; rcx 0.56: vr input: the app key claimed for the game's own process id, not 0 (VR-1); rcx 0.55: vr input: press types on a face button (single, double, long, held) in the default bindings with their delays, GetActionOrigins; the system keyboard: ShowKeyboard, HideKeyboard, IsKeyboardOpen, SetKeyboardPose and keyboard_char, keyboard_done and keyboard_closed events with text; vr input: its own app key gmod.resurgence.rcx through a temporary application manifest so SteamVR keeps the bindings apart from other gmod vr addons, action sets main, pointer, pipboy and weapon with priorities, display names for actions and sets, default bindings for windows mixed reality; ik: rcx.ik.SolveArm and SolveArms, a two-bone arm solver; GetDisplay: maxWidth and maxHeight, the largest eye texture the driver takes before downsampling; GetDisplay: tanUp and tanDown the right way round, OpenVR names top and bottom backwards; SetActionSets and RegisterAction read missing arguments right; vr session kept across lua states: a closing state fades to the grid and suspends rendering, the next WaitFrame resumes; vr input: actions with a manifest and default bindings the module writes, haptics, finger curl, play area, fade, events, a SteamVR quit ends the session; one submit per compositor wait; explicit timing: the app does the post-present handoff so WaitGetPoses never touches the vulkan queue; core; vr: openvr session and poses, d3d9 vtable hooks, eye texture capture, submit at present with pose through dxvk vulkan interop or a d3d11 shared texture), built " __DATE__ " " __TIME__;
}

namespace rcx::lua
{
	void FlushLog(ILuaBase* lua)
	{
		std::vector<std::string> lines = rcx::core::TakeLogLines();

		if (lines.empty())
		{
			return;
		}

		lua->PushSpecial(SPECIAL_GLOB);
		lua->GetField(-1, "print");

		if (!lua->IsType(-1, Type::Function))
		{
			lua->Pop(2);
			return;
		}

		for (const std::string& line : lines)
		{
			lua->Push(-1);
			lua->PushString(line.c_str());
			lua->PCall(1, 0, 0);
		}

		lua->Pop(2);
	}
}

namespace
{
	/*
		Each component can be switched off by a client convar named rcx_<feature>, created by
		the Lua that uses the module. A missing convar means on.
	*/
	bool IsConVarOff(ILuaBase* lua, const std::string& feature)
	{
		std::string name = "rcx_" + feature;
		int top = lua->Top();

		lua->PushSpecial(SPECIAL_GLOB);
		lua->GetField(-1, "GetConVar");

		if (!lua->IsType(-1, Type::Function))
		{
			lua->Pop(lua->Top() - top);
			return false;
		}

		lua->PushString(name.c_str());

		if (lua->PCall(1, 1, 0) != 0 || !lua->IsType(-1, Type::ConVar))
		{
			lua->Pop(lua->Top() - top);
			return false;
		}

		lua->GetField(-1, "GetBool");

		if (!lua->IsType(-1, Type::Function))
		{
			lua->Pop(lua->Top() - top);
			return false;
		}

		lua->Push(-2);

		if (lua->PCall(1, 1, 0) != 0)
		{
			lua->Pop(lua->Top() - top);
			return false;
		}

		bool enabled = lua->GetBool(-1);
		lua->Pop(lua->Top() - top);
		return !enabled;
	}

	RCX_FUNCTION(Version)
	{
		lua->PushString(rcx::core::kVersion);
		return 1;
	}

	RCX_FUNCTION(Features)
	{
		std::vector<rcx::core::FeatureState> features = rcx::core::GetFeatures();
		lua->CreateTable();

		for (const rcx::core::FeatureState& feature : features)
		{
			if (!feature.built || !feature.disabledReason.empty())
			{
				continue;
			}

			if (IsConVarOff(lua, feature.name))
			{
				continue;
			}

			lua->PushBool(true);
			lua->SetField(-2, feature.name.c_str());
		}

		return 1;
	}

	RCX_FUNCTION(Disabled)
	{
		std::vector<rcx::core::FeatureState> features = rcx::core::GetFeatures();
		lua->CreateTable();

		for (const rcx::core::FeatureState& feature : features)
		{
			std::string reason = feature.disabledReason;

			if (reason.empty() && IsConVarOff(lua, feature.name))
			{
				reason = "convar rcx_" + feature.name + " is 0";
			}

			if (reason.empty())
			{
				continue;
			}

			lua->PushString(reason.c_str());
			lua->SetField(-2, feature.name.c_str());
		}

		return 1;
	}
}

namespace rcx
{
	bool IsFeatureOff(ILuaBase* lua, const char* feature, std::string& reason)
	{
		if (rcx::core::IsFeatureDisabled(feature, &reason))
		{
			return true;
		}

		if (IsConVarOff(lua, feature))
		{
			reason = std::string("convar rcx_") + feature + " is 0";
			return true;
		}

		return false;
	}
}

GMOD_MODULE_OPEN()
{
	rcx::vr::RegisterFeature();
	rcx::ik::RegisterFeature();

	LUA->PushSpecial(SPECIAL_GLOB);
	LUA->CreateTable();

	LUA->PushCFunction(Version);
	LUA->SetField(-2, "Version");
	LUA->PushCFunction(Features);
	LUA->SetField(-2, "Features");
	LUA->PushCFunction(Disabled);
	LUA->SetField(-2, "Disabled");

	rcx::vr::PushTable(LUA);
	LUA->SetField(-2, "vr");

	rcx::ik::PushTable(LUA);
	LUA->SetField(-2, "ik");

	LUA->SetField(-2, "rcx");
	LUA->Pop();

	rcx::lua::FlushLog(LUA);
	return 0;
}

GMOD_MODULE_CLOSE()
{
	static_cast<void>(LUA);
	rcx::vr::OnLuaClose();
	return 0;
}
