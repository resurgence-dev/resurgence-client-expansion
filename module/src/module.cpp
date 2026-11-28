#include <string>

#include "GarrysMod/Lua/Interface.h"

#include "core/core.h"
#include "core/lua_util.h"
#include "engine/engine_guard.h"
#include "engine/engine_lua.h"
#include "mesh/mesh_lua.h"
#include "gpu/gpu_lua.h"
#include "gpu/gpu_timing.h"
#include "sys/sys_lua.h"
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
	const char* const kVersion = "rcx 0.80 (vr: the native Direct3D 9 path does the post-present handoff itself after its submit, in explicit timing mode as on DXVK, and the Direct3D 11 context is multithread protected: WaitGetPoses on the Lua thread used the context while the Present submitted on it, and both hung in the driver; rcx 0.79: vr: the native Direct3D 9 path waits at the Present, up to one headset frame (SetSubmitWait), for the GPU to finish the frame's copy and submits it in the same Present, since Direct3D 11 reads a texture shared with Direct3D 9 without waiting for its writes; a frame a Present late made SteamVR throttle the game; frame pacing in GetStats and the session log: game presents, submits and Lua waits per second, late submits, the headset rate, WaitGetPoses time, SteamVR's frames, reprojected and dropped per second and its throttling; rcx 0.78: vr: a session log, rcx_state\\vr_last.log next to the DLL, overwritten at each VR start: every [rcx] line, the hooks report and the submit path at the start, 5 s in and the stop, stats once a second and why VR stopped, flushed per line, capped at 2 MB; a game device on D3D9On12 submits its Direct3D 12 resources (UnwrapUnderlyingResource against a queue of the module's, TextureType_DirectX12, ReturnUnderlyingResource with a fence), no Direct3D 11 and no shared handle, refused when its GPU is not the headset's; a watchdog stops VR with a reason when the hand-off to SteamVR takes over 100 ms for 10 frames, no frame reaches the headset in 600 waits, or WaitGetPoses takes over 1.5 s three times; rcx 0.77: vr: the crash latch arms vr_device_scan only for the scan and vr_detours only until 600 presents ran through the hooks, so a crash later in a session no longer keeps them off; throwaway devices through D3D9On12 are hooked too, and a game device on D3D9On12 is reported with the GPU its Direct3D 12 device is on, which the same-GPU check then uses; rcx 0.76: vr: the native Direct3D 9 path never waits for the GPU: each frame is copied into one of three textures with its own event query and the newest finished copy is submitted at a later Present, a frame late with its own pose (framesNotReady counts copies overwritten while all were in flight); hooks names the module the game device's functions live in and whether D3D9On12 is loaded; rcx 0.75: vr: the native Direct3D 9 path claims the compositor wait right before Submit, so a slow draw wait no longer causes AlreadySubmitted (108), and frames two waits late are dropped; the Direct3D 11 device must be on the GPU the game draws on (by LUID) or the start is refused with the advice; staleFrames, alreadySubmitted, nestedPresents and compositorAdapter in GetStats; rcx 0.74: vr: the d3d9.dll bodies of Present, PresentEx, the swap chain Present, Reset, ResetEx, SetRenderTarget and CreateTexture are detoured (vr_detours, crash-latched), so a device whose vtable an overlay copied is still caught; vtable slots are patched only where a detour is refused; rcx 0.73: vr: throwaway devices on every Direct3D 9 adapter, the headset's and the game's adapter in GetStats and the failure, with the advice to run the game on the headset's graphics card when they differ; rcx 0.72: vr: the swap chain Present is hooked too, the game device's own vtable is patched once it is found, and throwaway devices are made through every loaded d3d9.dll; rcx 0.71: vr: every distinct Direct3D 9 device vtable is hooked (Ex or plain, software, hardware or pure), the eye texture is also captured when a render target of its size is bound, EndTarget gives a failure kind, and the failure and GetStats().hooks say what the hooks saw; rcx 0.70: vr input: GetActionOrigins keeps no empty read, so an action asked before SteamVR loaded the binding or the controllers is asked again; rcx 0.69: vr input: RegisterAction takes a sixth argument, per controller type that controller's own default in its own input names; the Steam Frame's defaults leave its pad as no face buttons, with one-hand chords of two inputs for the diagonals; rcx 0.68: vr input: GetActionOrigins gives each input of an action after its count: the hand, SteamVR's localized name in full and short, the controller type and the binding's device path, input path, mode, slot and source type, kept per action until SteamVR reloads the bindings or a controller changes (a bindings_changed event); ShowActionOrigins highlights them in the SteamVR overlay; rcx 0.67: sys: rcx.sys.GetMemory, the process working set, private bytes and page faults, physical memory and commit charge, and the graphics card's VRAM budget and usage through DXGI, for the merged-mesh memory gate; rcx 0.66: vr input: default bindings for the steam frame controllers (frame_controller), so SteamVR reports them as themselves instead of emulating oculus_touch; rcx 0.65: vr: a failed eye texture capture says why: the size asked, the last render target made while capturing (its asked and actual size, format and HRESULT) and the device's texture limit; GetDisplay gives maxTextureWidth and maxTextureHeight (D3DCAPS9); rcx 0.64: mesh: a part placed on a bone or attachment is merged in the pose its own first animation draws it in, not its bind (the surplus sack's root turns 90 degrees); rcx 0.63: mesh: rcx.mesh, merged meshes for mesh_combine: Build queues a merge of a character's part models (bytes from Lua) into one model, written by a worker thread into models/rcx_merged/<hash>, one bone per name and bind pose, static skin bones baked into their live ancestors, positioned parts made rigid on their bone; Poll, Cancel, Exists, Stats; the crash latch armed at the first build; rcx 0.62: engine: the safety layer for engine features: a build check that fingerprints engine.dll, datacache.dll and studiorender.dll from their files (PE stamp, image size, sha-256 of .text) against clearances compiled in, the launcher policy file rcx_policy.json (disabled, revoked), the crash latch rcx_state/armed_<feature>, rcx.engine.GetBuilds, Disable (off only) and ClearLatch; mesh_combine registered, off on an unknown build; rcx 0.61: gpu: rcx.gpu, render-thread and gpu timing at the game's present through the d3d9 hooks: present interval, time inside present, the presenting thread's wall and busy time, gpu timestamp span and interval, per frame and per window; rcx 0.60: vr: GetEyeGaze, the eye gaze from an eyetracking action bound to /user/head/eyetracking in default bindings for the steam frame, quest over steam link and other steam link headsets; rcx 0.59: vr: GetTipOffset, SteamVR's pointing pose from the controller render model's tip component, and GetControllerType, the controller type and render model name, both read when a controller changes and cached; rcx 0.58: vr bridge: the backend is chosen again whenever the device resources are released, so a new eye texture after a level change no longer submits through a released dxvk interop device (C1); rcx 0.57: vr input: both_ bindings, a chord of one button on both hands in each controller's default bindings; GetHandSkeleton: splay, the four gaps between fingers; rcx 0.56: vr input: the app key claimed for the game's own process id, not 0 (VR-1); rcx 0.55: vr input: press types on a face button (single, double, long, held) in the default bindings with their delays, GetActionOrigins; the system keyboard: ShowKeyboard, HideKeyboard, IsKeyboardOpen, SetKeyboardPose and keyboard_char, keyboard_done and keyboard_closed events with text; vr input: its own app key gmod.resurgence.rcx through a temporary application manifest so SteamVR keeps the bindings apart from other gmod vr addons, action sets main, pointer, pipboy and weapon with priorities, display names for actions and sets, default bindings for windows mixed reality; ik: rcx.ik.SolveArm and SolveArms, a two-bone arm solver; GetDisplay: maxWidth and maxHeight, the largest eye texture the driver takes before downsampling; GetDisplay: tanUp and tanDown the right way round, OpenVR names top and bottom backwards; SetActionSets and RegisterAction read missing arguments right; vr session kept across lua states: a closing state fades to the grid and suspends rendering, the next WaitFrame resumes; vr input: actions with a manifest and default bindings the module writes, haptics, finger curl, play area, fade, events, a SteamVR quit ends the session; one submit per compositor wait; explicit timing: the app does the post-present handoff so WaitGetPoses never touches the vulkan queue; core; vr: openvr session and poses, d3d9 vtable hooks, eye texture capture, submit at present with pose through dxvk vulkan interop or a d3d11 shared texture), built " __DATE__ " " __TIME__;
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
	rcx::gpu::RegisterFeature();
	rcx::engine::RegisterFeatures();

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

	rcx::gpu::PushTable(LUA);
	LUA->SetField(-2, "gpu");

	rcx::engine::PushTable(LUA);
	LUA->SetField(-2, "engine");

	rcx::mesh::PushTable(LUA);
	LUA->SetField(-2, "mesh");

	rcx::sys::PushTable(LUA);
	LUA->SetField(-2, "sys");

	LUA->SetField(-2, "rcx");
	LUA->Pop();

	rcx::lua::FlushLog(LUA);
	return 0;
}

GMOD_MODULE_CLOSE()
{
	static_cast<void>(LUA);
	rcx::vr::OnLuaClose();
	rcx::mesh::OnLuaClose();
	rcx::engine::DisarmAll();
	return 0;
}
