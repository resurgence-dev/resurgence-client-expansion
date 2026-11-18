#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "openvr.h"

/*
	Controller input through OpenVR's action system. Lua declares its actions (a name, a kind and
	optionally a suggested binding from a fixed list, with a press type on a face button: single,
	double, long or held, so one button carries several actions); the module writes the action manifest and
	the default bindings for the Index, Touch and Vive controllers itself, into a fixed folder next
	to the DLL, and hands OpenVR that path. Nothing from Lua becomes a path: names are checked to
	[a-z0-9_] and bindings come from the list.

	The module adds its own actions beside Lua's: a skeleton per hand (finger curl) and one haptic
	output. Lua thread only.
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

	constexpr size_t kMaxActions = 48;

	// label: the name SteamVR's binding screen shows ("" shows the name). set: "main" (also ""),
	// "pointer", "pipboy" or "weapon".
	bool RegisterAction(const std::string& name, const std::string& kind, const std::string& binding, const std::string& label, const std::string& set, std::string& reason);

	// Writes the manifest and bindings when they changed, loads them into OpenVR, and makes the named
	// sets the active ones, the first with the highest priority; an empty list means "main". The
	// module's own set (hand skeletons, haptics) is always active.
	bool SetActionSets(const std::vector<std::string>& sets, std::string& reason);

	// The session's IVRInput (or nullptr when the runtime has none) at start, and its end. The first
	// start in the process also claims the game's own app key (Identify), so SteamVR keeps its
	// bindings apart from other Garry's Mod VR addons'.
	void BeginSession(::vr::IVRInput* input, ::vr::IVRApplications* applications);
	void EndSession();

	// Once per frame, after WaitGetPoses.
	void Update();

	bool GetActions(std::vector<ActionValue>& values, std::string& reason);
	bool Haptic(Hand hand, float seconds, float frequency, float amplitude, std::string& reason);
	bool GetHandSkeleton(Hand hand, HandSkeleton& skeleton, std::string& reason);

	// How many inputs the loaded binding puts on a registered action (0 when none), or false with reason set.
	bool GetActionOrigins(const std::string& name, uint32_t& count, std::string& reason);

	// Writes the action manifest, the default bindings and the application manifest without a session,
	// for tests/input_files_test.cpp. Not reachable from Lua.
	bool WriteFilesForTest(std::string& reason);

	// What claiming the app key did: "identified as ...", the failure, or "not tried".
	std::string GetIdentity();
	const char* GetAppKey();
}
