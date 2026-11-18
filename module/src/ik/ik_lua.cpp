#include "ik/ik_lua.h"

#include <string>

#include "core/core.h"
#include "core/lua_util.h"
#include "ik/ik_solver.h"

using namespace GarrysMod::Lua;

/*
	rcx.ik: the body IK solver for Lua. Every argument is a plain number and every result is one,
	returned on the stack rather than in a table, so a call allocates nothing on the Lua side. The
	feature's convar (rcx_ik) is honored through rcx.Features(); the calls themselves don't read it
	again, since a pure solver has nothing to protect and the convar lookup would cost more than the
	solve.
*/
namespace rcx::ik
{
	namespace
	{
		constexpr const char* kFeature = "ik";
		constexpr int kArmArguments = 11;
		constexpr int kArmResults = 7;

		bool ReadPoint(ILuaBase* lua, int stackPosition, Vec3& point)
		{
			return rcx::lua::ReadNumber(lua, stackPosition, -kMaxCoordinate, kMaxCoordinate, point.x)
				&& rcx::lua::ReadNumber(lua, stackPosition + 1, -kMaxCoordinate, kMaxCoordinate, point.y)
				&& rcx::lua::ReadNumber(lua, stackPosition + 2, -kMaxCoordinate, kMaxCoordinate, point.z);
		}

		// One arm's eleven arguments from stackPosition: shoulder, target, pole, upper and lower length.
		bool ReadArm(ILuaBase* lua, int stackPosition, ArmInput& input)
		{
			if (!ReadPoint(lua, stackPosition, input.shoulder) || !ReadPoint(lua, stackPosition + 3, input.target) || !ReadPoint(lua, stackPosition + 6, input.pole))
			{
				return false;
			}

			if (!rcx::lua::ReadNumber(lua, stackPosition + 9, 0.0, kMaxLength, input.upperLength) || !rcx::lua::ReadNumber(lua, stackPosition + 10, 0.0, kMaxLength, input.lowerLength))
			{
				return false;
			}

			return IsValid(input);
		}

		void PushArm(ILuaBase* lua, const ArmResult& result)
		{
			lua->PushNumber(result.elbow.x);
			lua->PushNumber(result.elbow.y);
			lua->PushNumber(result.elbow.z);
			lua->PushNumber(result.hand.x);
			lua->PushNumber(result.hand.y);
			lua->PushNumber(result.hand.z);
			lua->PushBool(result.reached);
		}

		const char* const kArmUsage = "expects shoulder x, y, z, target x, y, z, pole x, y, z, upper length, lower length: finite numbers within 1e6, lengths above 0 and at most 1e4";

		RCX_FUNCTION(SolveArmLua)
		{
			ArmInput input{};
			ArmResult result{};

			if (!ReadArm(lua, 1, input) || !SolveArm(input, result))
			{
				return rcx::lua::PushNilReason(lua, std::string("SolveArm ") + kArmUsage);
			}

			PushArm(lua, result);
			return kArmResults;
		}

		RCX_FUNCTION(SolveArmsLua)
		{
			ArmInput left{};
			ArmInput right{};
			ArmResult leftResult{};
			ArmResult rightResult{};

			if (!ReadArm(lua, 1, left) || !ReadArm(lua, 1 + kArmArguments, right))
			{
				return rcx::lua::PushNilReason(lua, std::string("SolveArms, for each arm in turn, ") + kArmUsage);
			}

			if (!SolveArm(left, leftResult) || !SolveArm(right, rightResult))
			{
				return rcx::lua::PushNilReason(lua, "SolveArms: invalid input");
			}

			PushArm(lua, leftResult);
			PushArm(lua, rightResult);
			return kArmResults * 2;
		}

		void SetFunction(ILuaBase* lua, const char* name, CFunc function)
		{
			lua->PushCFunction(function);
			lua->SetField(-2, name);
		}
	}

	void RegisterFeature()
	{
		rcx::core::RegisterFeature(kFeature);
	}

	void PushTable(ILuaBase* lua)
	{
		lua->CreateTable();
		SetFunction(lua, "SolveArm", SolveArmLua);
		SetFunction(lua, "SolveArms", SolveArmsLua);
	}
}
