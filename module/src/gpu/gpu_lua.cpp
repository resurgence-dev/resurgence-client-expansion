#include "gpu/gpu_lua.h"

#include <string>

#include "core/lua_util.h"
#include "gpu/gpu_timing.h"

using namespace GarrysMod::Lua;

namespace rcx
{
	bool IsFeatureOff(ILuaBase* lua, const char* feature, std::string& reason);
}

/*
	rcx.gpu for Lua. No function takes an argument, so there is nothing to check but the feature's
	state; every result is a fresh table of numbers and booleans. Times are milliseconds.
*/
namespace rcx::gpu
{
	namespace
	{
		constexpr const char* kFeature = "gpu";

		void SetNumber(ILuaBase* lua, const char* name, double value)
		{
			lua->PushNumber(value);
			lua->SetField(-2, name);
		}

		void SetBool(ILuaBase* lua, const char* name, bool value)
		{
			lua->PushBool(value);
			lua->SetField(-2, name);
		}

		void SetSummary(ILuaBase* lua, const char* name, const Summary& summary)
		{
			lua->CreateTable();
			SetNumber(lua, "count", summary.count);
			SetNumber(lua, "mean", summary.mean);
			SetNumber(lua, "p50", summary.p50);
			SetNumber(lua, "p95", summary.p95);
			SetNumber(lua, "max", summary.max);
			lua->SetField(-2, name);
		}

		RCX_FUNCTION(StartLua)
		{
			std::string reason;

			if (rcx::IsFeatureOff(lua, kFeature, reason))
			{
				return rcx::lua::PushFailure(lua, reason);
			}

			if (!Start(reason))
			{
				return rcx::lua::PushFailure(lua, reason);
			}

			lua->PushBool(true);
			return 1;
		}

		RCX_FUNCTION(StopLua)
		{
			Stop();
			lua->PushBool(true);
			return 1;
		}

		RCX_FUNCTION(IsRunningLua)
		{
			lua->PushBool(GetStatus().running);
			return 1;
		}

		RCX_FUNCTION(GetStatusLua)
		{
			Status status = GetStatus();
			lua->CreateTable();
			SetBool(lua, "running", status.running);
			SetBool(lua, "deviceFound", status.deviceFound);
			SetBool(lua, "queriesReady", status.queriesReady);
			SetBool(lua, "presentOnMainThread", status.presentOnMainThread);
			SetBool(lua, "busyCalibrated", status.busyCalibrated);
			SetNumber(lua, "frames", static_cast<double>(status.frames));
			SetNumber(lua, "gpuFrames", static_cast<double>(status.gpuFrames));
			SetNumber(lua, "gpuDropped", static_cast<double>(status.gpuDropped));
			SetNumber(lua, "disjoint", static_cast<double>(status.disjoint));
			SetNumber(lua, "resets", static_cast<double>(status.resets));
			SetNumber(lua, "threadChanges", static_cast<double>(status.threadChanges));
			SetNumber(lua, "cyclesPerMs", status.cyclesPerMs);

			if (!status.queryError.empty())
			{
				lua->PushString(status.queryError.c_str());
				lua->SetField(-2, "queryError");
			}

			return 1;
		}

		RCX_FUNCTION(GetLastLua)
		{
			Last last = GetLast();
			lua->CreateTable();
			SetNumber(lua, "frame", static_cast<double>(last.frame));
			SetNumber(lua, "presentInterval", last.presentInterval);
			SetNumber(lua, "presentBlock", last.presentBlock);
			SetNumber(lua, "renderWall", last.renderWall);
			SetNumber(lua, "renderBusy", last.renderBusy);
			SetNumber(lua, "gpuFrame", static_cast<double>(last.gpuFrame));
			SetNumber(lua, "gpuSpan", last.gpuSpan);
			SetNumber(lua, "gpuInterval", last.gpuInterval);
			return 1;
		}

		RCX_FUNCTION(BeginWindowLua)
		{
			BeginWindow();
			lua->PushBool(true);
			return 1;
		}

		RCX_FUNCTION(GetWindowLua)
		{
			Window window = GetWindow();
			lua->CreateTable();
			SetNumber(lua, "seconds", window.seconds);
			SetNumber(lua, "frames", static_cast<double>(window.frames));
			SetNumber(lua, "gpuFrames", static_cast<double>(window.gpuFrames));
			SetNumber(lua, "gpuDropped", static_cast<double>(window.gpuDropped));
			SetNumber(lua, "disjoint", static_cast<double>(window.disjoint));
			SetBool(lua, "truncated", window.truncated);
			SetSummary(lua, "presentInterval", window.presentInterval);
			SetSummary(lua, "presentBlock", window.presentBlock);
			SetSummary(lua, "renderWall", window.renderWall);
			SetSummary(lua, "renderBusy", window.renderBusy);
			SetSummary(lua, "gpuSpan", window.gpuSpan);
			SetSummary(lua, "gpuInterval", window.gpuInterval);
			return 1;
		}

		void SetFunction(ILuaBase* lua, const char* name, CFunc function)
		{
			lua->PushCFunction(function);
			lua->SetField(-2, name);
		}
	}

	void PushTable(ILuaBase* lua)
	{
		lua->CreateTable();
		SetFunction(lua, "Start", StartLua);
		SetFunction(lua, "Stop", StopLua);
		SetFunction(lua, "IsRunning", IsRunningLua);
		SetFunction(lua, "GetStatus", GetStatusLua);
		SetFunction(lua, "GetLast", GetLastLua);
		SetFunction(lua, "BeginWindow", BeginWindowLua);
		SetFunction(lua, "GetWindow", GetWindowLua);
	}
}
