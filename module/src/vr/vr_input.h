#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "openvr.h"

/*
	Controller input through OpenVR's action system. Lua declares its actions (a name, a kind and
	optionally a suggested binding from a fixed list, with a press type on a face button: single,
	double, long or held, so one button carries several actions); the module writes the action manifest and
	the default bindings for the Index, Touch and Vive controllers itself, into a fixed folder next
	to the DLL, and hands OpenVR that path. Nothing from Lua becomes a path: names are checked to
	[a-z0-9_] and bindings come from the list.

	The module adds its own actions beside Lua's: a skeleton per hand (finger curl), one haptic
	output and the eye gaze (an eyetracking action, with default bindings for the headsets that have
	eye tracking). Lua thread only.
*/
namespace rcx::vr::input
{
	enum class ActionKind
	{
		Boolean,
		Vector1,
		Vector2
	};

	enum class Hand
	{
		Left = 0,
		Right = 1
	};

	struct ActionValue
	{
		std::string name;
		ActionKind kind = ActionKind::Boolean;
		bool active = false;
		bool state = false;
		float x = 0.0f;
		float y = 0.0f;
	};

	struct HandSkeleton
	{
		// thumb, index, middle, ring, pinky: 0 straight, 1 fully curled
		float curl[5]{};
		// the gaps thumb-index, index-middle, middle-ring, ring-pinky: 0 together, 1 fully spread
		float splay[4]{};
	};

	// The headset's eye gaze from the module's eyetracking action (bound to /user/head/eyetracking in
	// the headset bindings), in the standing tracking space: OpenVR's axes, meters.
	struct EyeGaze
	{
		// bActive: the action is bound to a source that's there. bValid: the data can be used.
		// bTracked: the eyes are tracked right now (false through a blink or a lost eye).
		bool active = false;
		bool valid = false;
		bool tracked = false;
		::vr::HmdVector3_t origin{};
		::vr::HmdVector3_t target{};
	};

	// One input the loaded binding puts on an action. hand: "left", "right" or "" (not on a hand).
	// name: SteamVR's localized name with the hand, controller type and input source; shortName: the
	// input source alone ("A Button"). devicePath, inputPath, mode, slot and sourceType come from
	// GetActionBindingInfo ("" when it gave none for this origin); controllerType is the device's
	// Prop_ControllerType_String ("knuckles").
	struct ActionOrigin
	{
		std::string hand;
		std::string name;
		std::string shortName;
		std::string controllerType;
		std::string devicePath;
		std::string inputPath;
		std::string mode;
		std::string slot;
		std::string sourceType;
	};

	constexpr size_t kMaxActions = 48;

	// label: the name SteamVR's binding screen shows ("" shows the name). set: "main" (also ""),
	// "pointer", "pipboy" or "weapon".
	bool RegisterAction(const std::string& name, const std::string& kind, const std::string& binding, const std::string& label, const std::string& set, std::string& reason);

	// overrides: per controller type, that controller's own default in place of the shared binding, in
	// its own input names. Only frame_controller takes one (its inputs: stick, stick_click, trigger,
	// grip and bumper on both hands; a, b, x, y and menu on the right; view and dpad_up, dpad_right,
	// dpad_down, dpad_left on the left). A value is "" (no default there), "<hand>_<input>" with an
	// optional press type on a button, or "<hand>_<input>+<hand>_<input>", a chord of two buttons'
	// clicks for a boolean action.
	bool RegisterAction(const std::string& name, const std::string& kind, const std::string& binding, const std::string& label, const std::string& set, const std::vector<std::pair<std::string, std::string>>& overrides, std::string& reason);

	// The controller types the module writes default bindings for, the keys an override can use.
	std::vector<std::string> GetControllerTypes();

	// Writes the manifest and bindings when they changed, loads them into OpenVR, and makes the named
	// sets the active ones, the first with the highest priority; an empty list means "main". The
	// module's own set (hand skeletons, haptics) is always active.
	bool SetActionSets(const std::vector<std::string>& sets, std::string& reason);

	// The session's IVRInput (or nullptr when the runtime has none) at start, and its end. The first
	// start in the process also claims the game's own app key (Identify), so SteamVR keeps its
	// bindings apart from other Garry's Mod VR addons'.
	void BeginSession(::vr::IVRInput* input, ::vr::IVRApplications* applications, ::vr::IVRSystem* system);
	void EndSession();

	// Once per frame, after WaitGetPoses.
	void Update();

	bool GetActions(std::vector<ActionValue>& values, std::string& reason);
	bool Haptic(Hand hand, float seconds, float frequency, float amplitude, std::string& reason);
	bool GetHandSkeleton(Hand hand, HandSkeleton& skeleton, std::string& reason);

	// The gaze Update read this frame (GetEyeTrackingDataForNextFrame), or false with reason set when
	// there's no action set, the read failed or the action isn't active (no eye tracker, or SteamVR
	// doesn't share its data).
	bool GetEyeGaze(EyeGaze& gaze, std::string& reason);

	// The inputs the loaded binding puts on a registered action (none when it puts none), or false with
	// reason set. Read once per action and kept until ForgetOrigins; an empty read isn't kept, so the
	// next call asks SteamVR again.
	bool GetActionOrigins(const std::string& name, std::vector<ActionOrigin>& origins, std::string& reason);

	// Drops the kept origins: the session calls it when SteamVR reloads the bindings or a controller
	// changes, and loading new handles (SetActionSets, a session start) drops them too.
	void ForgetOrigins();

	// Highlights the action's inputs in SteamVR's overlay (IVRInput::ShowActionOrigins).
	bool ShowActionOrigins(const std::string& name, std::string& reason);

	// For each origin's hand, the index of the binding info it takes (-1 for none): the first unused
	// entry whose device path is that hand's (/user/hand/left or /user/hand/right), else, when the
	// counts agree, the entry at the origin's own index. Pure, for tests/input_files_test.cpp.
	std::vector<int> MatchBindingInfo(const std::vector<std::string>& originHands, const std::vector<std::string>& bindingDevicePaths);

	// Writes the action manifest, the default bindings and the application manifest without a session,
	// for tests/input_files_test.cpp. Not reachable from Lua.
	bool WriteFilesForTest(std::string& reason);

	// What claiming the app key did: "identified as ...", the failure, or "not tried".
	std::string GetIdentity();
	const char* GetAppKey();
}
