#include "engine/engine_lua.h"

#include <cstdio>
#include <string>

#include "core/core.h"
#include "core/lua_util.h"
#include "engine/engine_guard.h"

using namespace GarrysMod::Lua;

/*
	rcx.engine for Lua: what the build check found, and the two switches Lua may use. Disable is off only and lasts
	the session, so a hostile server can only make the game slower; ClearLatch removes a crash latch, which takes
	effect from the next load.
*/
namespace rcx::engine
{
	namespace
	{
		constexpr size_t kMaxFeatureName = 32;
		constexpr size_t kMaxReason = 120;

		// A feature name from Lua at `position`: a-z, 0-9 and _ only, or false.
		bool ReadFeatureName(ILuaBase* lua, int position, std::string& name)
		{
			if (!lua->IsType(position, Type::String))
			{
				return false;
			}

			unsigned int length = 0;
			const char* text = lua->GetString(position, &length);

			if (text == nullptr || length == 0 || length > kMaxFeatureName)
			{
				return false;
			}

			for (unsigned int i = 0; i < length; ++i)
			{
				char c = text[i];

				if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
				{
					return false;
				}
			}

			name.assign(text, length);
			return true;
		}

		// An optional reason from Lua, cut to kMaxReason and kept to printable ASCII.
		std::string ReadReason(ILuaBase* lua, int position)
		{
			if (!lua->IsType(position, Type::String))
			{
				return "no reason given";
			}

			unsigned int length = 0;
			const char* text = lua->GetString(position, &length);
			std::string reason;

			for (unsigned int i = 0; text != nullptr && i < length && reason.size() < kMaxReason; ++i)
			{
				char c = text[i];
				reason.push_back(c >= 32 && c < 127 ? c : '?');
			}

			return reason.empty() ? "no reason given" : reason;
		}

		void SetString(ILuaBase* lua, const char* name, const std::string& value)
		{
			lua->PushString(value.c_str());
			lua->SetField(-2, name);
		}

		RCX_FUNCTION(GetBuildsLua)
		{
			std::vector<FeatureBuild> builds = GetBuilds();
			lua->CreateTable();

			for (const FeatureBuild& build : builds)
			{
				std::string reason;
				bool off = rcx::core::IsFeatureDisabled(build.feature.c_str(), &reason);

				lua->CreateTable();
				SetString(lua, "key", build.key);
				lua->PushBool(build.cleared);
				lua->SetField(-2, "cleared");
				lua->PushBool(build.latched);
				lua->SetField(-2, "latched");

				if (off)
				{
					SetString(lua, "disabled", reason);
				}

				lua->CreateTable();
				double index = 1;

				for (const DllFingerprint& print : build.dlls)
				{
					char stamp[16]{};
					std::snprintf(stamp, sizeof(stamp), "%08x", print.timeDateStamp);

					lua->PushNumber(index);
					lua->CreateTable();
					SetString(lua, "name", print.name);
					SetString(lua, "timeDateStamp", stamp);
					lua->PushNumber(print.sizeOfImage);
					lua->SetField(-2, "sizeOfImage");
					SetString(lua, "textHash", print.textHash);

					if (!print.error.empty())
					{
						SetString(lua, "error", print.error);
					}

					lua->SetTable(-3);
					index += 1;
				}

				lua->SetField(-2, "dlls");
				lua->SetField(-2, build.feature.c_str());
			}

			return 1;
		}

		RCX_FUNCTION(DisableLua)
		{
			std::string feature;

			if (!ReadFeatureName(lua, 1, feature) || !IsEngineFeature(feature))
			{
				return rcx::lua::PushFailure(lua, "not an engine feature");
			}

			rcx::core::DisableFeature(feature.c_str(), "turned off for this session by Lua: " + ReadReason(lua, 2));
			lua->PushBool(true);
			return 1;
		}

		RCX_FUNCTION(ClearLatchLua)
		{
			std::string feature;

			if (!ReadFeatureName(lua, 1, feature))
			{
				return rcx::lua::PushFailure(lua, "not an engine feature");
			}

			std::string reason;

			if (!ClearLatch(feature, reason))
			{
				return rcx::lua::PushFailure(lua, reason);
			}

			lua->PushBool(true);
			return 1;
		}

		void SetFunction(ILuaBase* lua, const char* name, CFunc function)
		{
			lua->PushCFunction(function);
			lua->SetField(-2, name);
		}
	}

	void RegisterFeatures()
	{
		RegisterEngineFeature("mesh_combine", {"engine.dll", "datacache.dll", "studiorender.dll"});
		RegisterSystemFeature("vr_detours");
		RegisterSystemFeature("vr_device_scan");
	}

	void PushTable(ILuaBase* lua)
	{
		lua->CreateTable();
		SetFunction(lua, "GetBuilds", GetBuildsLua);
		SetFunction(lua, "Disable", DisableLua);
		SetFunction(lua, "ClearLatch", ClearLatchLua);
	}
}
