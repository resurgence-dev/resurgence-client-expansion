/*
	Writes the VR input files (action manifest, default bindings, application manifest) from the
	actions Resurgence registers, into rcx_vr_input/ next to this exe (builds/win64/tests/), with no
	SteamVR. tools/check_input_files.py then parses and checks them. Exits non-zero when a write or
	a registration fails.
*/
#include <cstdio>
#include <string>
#include <vector>

#include "vr/vr_input.h"

namespace
{
	struct TestAction
	{
		const char* name = nullptr;
		const char* kind = nullptr;
		const char* binding = nullptr;
		const char* label = nullptr;
		const char* set = nullptr;
		// the frame_controller override (nullptr: none, the shared binding holds there)
		const char* frame = nullptr;
	};

	// gamemode/plugins/vr/cl_input.lua's list as a 0.69 module gets it: the press types as binding suffixes, the chord
	// as both_, and the Steam Frame layout of vr-design.md's "Steam Frame defaults and D-pad hotkeys" as overrides.
	const TestAction kActions[] = {
		{"move", "vector2", "left_stick", "Move", "main"},
		{"turn", "vector2", "right_stick", "Turn", "main"},
		{"sprint", "boolean", "left_stick_click", "Sprint", "main"},
		{"crouch", "boolean", "right_stick_click", "Crouch", "main"},
		{"jump", "boolean", "right_a", "Jump", "main"},
		{"pipboy", "boolean", "left_b", "Open Pip-Boy", "main", "right_b"},
		{"menu", "boolean", "left_a_single", "Game menu", "main", "left_view_single"},
		{"weapon_menu", "boolean", "left_a_long", "Weapon menu", "main", "left_view_long"},
		{"ping", "boolean", "left_a_double", "Ping", "main", "left_view_double"},
		{"favorites_left", "boolean", "", "Favorites, left hand", "main"},
		{"favorites_right", "boolean", "right_b_long", "Favorites, right hand", "main", "right_x_long"},
		{"ui_toggle", "boolean", "right_b_single", "Show or hide the UI window", "main", "right_x_single"},
		{"use", "boolean", "left_trigger", "Use (what the hand points at)", "main"},
		{"trigger_left", "vector1", "left_trigger", "Left trigger", "main"},
		{"trigger_right", "vector1", "right_trigger", "Right trigger", "main"},
		{"grip_left", "vector1", "left_grip", "Left grip", "main"},
		{"grip_right", "vector1", "right_grip", "Right grip", "main"},
		{"recenter", "boolean", "both_stick_click", "Recenter the view", "main"},
		{"voice_toggle", "boolean", "", "Voice on or off", "main", "right_y"},
		{"hotkey_1", "boolean", "", "Hotkey 1", "main", "left_dpad_up+left_dpad_left"},
		{"hotkey_2", "boolean", "", "Hotkey 2", "main", "left_dpad_up"},
		{"hotkey_3", "boolean", "", "Hotkey 3", "main", "left_dpad_up+left_dpad_right"},
		{"hotkey_4", "boolean", "", "Hotkey 4", "main", "left_dpad_right"},
		{"hotkey_5", "boolean", "", "Hotkey 5", "main", "left_dpad_down+left_dpad_right"},
		{"hotkey_6", "boolean", "", "Hotkey 6", "main", "left_dpad_down"},
		{"hotkey_7", "boolean", "", "Hotkey 7", "main", "left_dpad_down+left_dpad_left"},
		{"hotkey_8", "boolean", "", "Hotkey 8", "main", "left_dpad_left"},
		{"reload", "boolean", "right_b_single", "Reload", "weapon", "right_x_single"},
		{"ammo_next", "boolean", "right_b_double", "Next ammo type", "weapon", "right_x_double"},
		{"favorites_weapon", "boolean", "right_b_long", "Favorites, weapon hand", "weapon", "right_x_long"},
		{"click_left", "boolean", "left_trigger", "Left click", "pointer"},
		{"click_right", "boolean", "right_trigger", "Right click", "pointer"},
		{"pipboy_drop", "boolean", "right_stick_click", "Pip-Boy: drop the item", "pipboy"},
		{"turn_pipboy", "vector2", "right_stick", "Turn in the Pip-Boy", "pipboy"},
		{"pipboy_save", "boolean", "right_a", "Pip-Boy: save the item", "pipboy"},
	};

	struct BadOverride
	{
		const char* label = nullptr;
		const char* kind = nullptr;
		const char* type = nullptr;
		const char* text = nullptr;
	};

	// Overrides RegisterAction must turn down.
	const BadOverride kBadOverrides[] = {
		{"an override on a controller that takes none", "boolean", "knuckles", "right_a"},
		{"an unknown controller type", "boolean", "nonsense", "right_a"},
		{"an input the hand lacks", "boolean", "frame_controller", "right_dpad_up"},
		{"an unknown input", "boolean", "frame_controller", "left_c"},
		{"the system button", "boolean", "frame_controller", "left_system"},
		{"a press type in a chord", "boolean", "frame_controller", "left_dpad_up_long+left_dpad_left"},
		{"a chord of three", "boolean", "frame_controller", "left_dpad_up+left_dpad_left+left_view"},
		{"a chord of one input twice", "boolean", "frame_controller", "left_dpad_up+left_dpad_up"},
		{"a chord on a stick", "boolean", "frame_controller", "left_stick+left_view"},
		{"a vector2 action on a button", "vector2", "frame_controller", "right_x"},
		{"a press type on a trigger", "boolean", "frame_controller", "right_trigger_long"},
		{"a press type for a vector1 action", "vector1", "frame_controller", "right_x_long"},
	};
}

int main()
{
	int failures = 0;

	for (const TestAction& action : kActions)
	{
		std::string reason;
		std::vector<std::pair<std::string, std::string>> overrides;

		if (action.frame != nullptr)
		{
			overrides.emplace_back("frame_controller", action.frame);
		}

		if (!rcx::vr::input::RegisterAction(action.name, action.kind, action.binding, action.label, action.set, overrides, reason))
		{
			std::printf("FAIL register %s: %s\n", action.name, reason.c_str());
			failures++;
		}
	}

	for (const BadOverride& bad : kBadOverrides)
	{
		std::string reason;

		if (rcx::vr::input::RegisterAction("bad_override", bad.kind, "", "", "", {{bad.type, bad.text}}, reason))
		{
			std::printf("FAIL %s was taken\n", bad.label);
			failures++;
		}
	}

	std::string reason;

	if (rcx::vr::input::RegisterAction("bad_label", "boolean", "", "has \"quotes\"", "", reason))
	{
		std::printf("FAIL a label with quotes was taken\n");
		failures++;
	}

	if (rcx::vr::input::RegisterAction("bad_set", "boolean", "", "", "nonsense", reason))
	{
		std::printf("FAIL an unknown set was taken\n");
		failures++;
	}

	if (rcx::vr::input::RegisterAction("bad_press", "vector1", "left_trigger_long", "", "", reason))
	{
		std::printf("FAIL a press type on a trigger was taken\n");
		failures++;
	}

	if (rcx::vr::input::RegisterAction("bad_press", "boolean", "left_a_triple", "", "", reason))
	{
		std::printf("FAIL an unknown press type was taken\n");
		failures++;
	}

	if (rcx::vr::input::RegisterAction("bad_chord", "vector2", "both_stick", "", "", reason))
	{
		std::printf("FAIL a chord on a stick was taken\n");
		failures++;
	}

	if (rcx::vr::input::RegisterAction("bad_chord", "boolean", "both_a_long", "", "", reason))
	{
		std::printf("FAIL a press type on a chord was taken\n");
		failures++;
	}

	if (!rcx::vr::input::WriteFilesForTest(reason))
	{
		std::printf("FAIL write: %s\n", reason.c_str());
		failures++;
	}

	// GetActionOrigins pairs each origin with its binding info by hand, else by index when the counts agree
	struct MatchCase
	{
		const char* label;
		std::vector<std::string> hands;
		std::vector<std::string> paths;
		std::vector<int> expected;
	};

	const MatchCase matchCases[] = {
		{"both hands in reverse order", {"left", "right"}, {"/user/hand/right", "/user/hand/left"}, {1, 0}},
		{"one hand twice", {"right", "right"}, {"/user/hand/right", "/user/hand/right"}, {0, 1}},
		{"no hand, counts agree", {"", "left"}, {"/user/head", "/user/hand/left"}, {0, 1}},
		{"no hand, counts differ", {"", "left"}, {"/user/hand/left"}, {-1, 0}},
		{"no binding info", {"left"}, {}, {-1}},
		{"no origins", {}, {"/user/hand/left"}, {}},
	};

	for (const MatchCase& match : matchCases)
	{
		std::vector<int> got = rcx::vr::input::MatchBindingInfo(match.hands, match.paths);

		if (got != match.expected)
		{
			std::printf("FAIL binding info match: %s\n", match.label);
			failures++;
		}
	}

	std::printf("%s\n", failures == 0 ? "input files written" : "input files test failed");
	return failures == 0 ? 0 : 1;
}
