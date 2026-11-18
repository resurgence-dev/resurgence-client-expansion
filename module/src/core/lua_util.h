#pragma once

#include <cmath>
#include <cstdint>
#include <exception>
#include <string>

#include "GarrysMod/Lua/Interface.h"

/*
	Every Lua-facing function goes through RCX_FUNCTION. It takes ILuaBase from the lua_State and
	calls SetState, prints any queued log lines, and stops a C++ exception at the boundary,
	returning nil and a message. Functions never raise Lua errors on bad input: Lua is hostile
	input (any server can call the module), so they return nil or false and a reason instead.
*/
namespace rcx::lua
{
	void FlushLog(GarrysMod::Lua::ILuaBase* lua);

	inline int PushFailure(GarrysMod::Lua::ILuaBase* lua, const std::string& reason)
	{
		lua->PushBool(false);
		lua->PushString(reason.c_str());
		return 2;
	}

	inline int PushNilReason(GarrysMod::Lua::ILuaBase* lua, const std::string& reason)
	{
		lua->PushNil();
		lua->PushString(reason.c_str());
		return 2;
	}

	// A finite number within [minimum, maximum] at stackPosition, or false.
	inline bool ReadNumber(GarrysMod::Lua::ILuaBase* lua, int stackPosition, double minimum, double maximum, double& value)
	{
		if (!lua->IsType(stackPosition, GarrysMod::Lua::Type::Number))
		{
			return false;
		}

		double read = lua->GetNumber(stackPosition);

		if (!std::isfinite(read) || read < minimum || read > maximum)
		{
			return false;
		}

		value = read;
		return true;
	}

	// A whole number within [minimum, maximum] at stackPosition, or false.
	inline bool ReadInteger(GarrysMod::Lua::ILuaBase* lua, int stackPosition, int64_t minimum, int64_t maximum, int64_t& value)
	{
		double read = 0.0;

		if (!ReadNumber(lua, stackPosition, static_cast<double>(minimum), static_cast<double>(maximum), read))
		{
			return false;
		}

		if (std::floor(read) != read)
		{
			return false;
		}

		value = static_cast<int64_t>(read);
		return true;
	}

	inline Vector MakeVector(double x, double y, double z)
	{
		Vector vector{};
		vector.x = static_cast<float>(x);
		vector.y = static_cast<float>(y);
		vector.z = static_cast<float>(z);
		return vector;
	}
}

#define RCX_FUNCTION(name)                                                      \
	int name##Body(GarrysMod::Lua::ILuaBase* lua);                              \
	int name(lua_State* L)                                                      \
	{                                                                           \
		GarrysMod::Lua::ILuaBase* lua = L->luabase;                             \
		lua->SetState(L);                                                       \
		rcx::lua::FlushLog(lua);                                                \
		std::string failure;                                                    \
		try                                                                     \
		{                                                                       \
			return name##Body(lua);                                             \
		}                                                                       \
		catch (const std::exception& exception)                                 \
		{                                                                       \
			failure = std::string(#name ": ") + exception.what();               \
		}                                                                       \
		lua->PushNil();                                                         \
		lua->PushString(failure.c_str());                                       \
		return 2;                                                               \
	}                                                                           \
	int name##Body(GarrysMod::Lua::ILuaBase* lua)
