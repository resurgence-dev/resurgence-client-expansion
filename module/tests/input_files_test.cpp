/*
	Writes the VR input files (action manifest, default bindings, application manifest) from the
	actions Resurgence registers, into rcx_vr_input/ next to this exe (builds/win64/tests/), with no
	SteamVR. tools/check_input_files.py then parses and checks them. Exits non-zero when a write or
	a registration fails.
*/
#include <cstdio>
#include <string>

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
	};

	// gamemode/plugins/vr/cl_input.lua's list as a 0.57 module gets it: the press types as binding suffixes, the chord as both_.
	const TestAction kActions[] = {
		{"move", "vector2", "left_stick", "Move", "main"},
		{"turn", "vector2", "right_stick", "Turn", "main"},
		{"sprint", "boolean", "left_stick_click", "Sprint", "main"},
		{"crouch", "boolean", "right_stick_click", "Crouch", "main"},
		{"jump", "boolean", "right_a", "Jump", "main"},
		{"pipboy", "boolean", "left_b", "Open Pip-Boy", "main"},
		{"menu", "boolean", "left_a_single", "Game menu", "main"},
		{"weapon_menu", "boolean", "left_a_long", "Weapon menu", "main"},
		{"quick_select_left", "boolean", "", "Favorites, left hand", "main"},
		{"quick_select_right", "boolean", "right_b_long", "Favorites, right hand", "main"},
		{"ui_toggle", "boolean", "right_b_single", "Show or hide the UI window", "main"},
		{"use", "boolean", "left_trigger", "Use (what the hand points at)", "main"},
		{"trigger_left", "vector1", "left_trigger", "Left trigger", "main"},
		{"trigger_right", "vector1", "right_trigger", "Right trigger", "main"},
		{"grip_left", "vector1", "left_grip", "Left grip", "main"},
		{"grip_right", "vector1", "right_grip", "Right grip", "main"},
		{"recenter", "boolean", "", "Recenter the view", "main"},
		{"tuning_menu", "boolean", "both_stick_click", "Tuning menu", "main"},
		{"reload", "boolean", "right_b_single", "Reload", "weapon"},
		{"ammo_next", "boolean", "right_b_double", "Next ammo type", "weapon"},
		{"quick_select_weapon", "boolean", "right_b_long", "Favorites, weapon hand", "weapon"},
		{"click_left", "boolean", "left_trigger", "Left click", "pointer"},
		{"click_right", "boolean", "right_trigger", "Right click", "pointer"},
	};
}

int main()
{
	int failures = 0;

	for (const TestAction& action : kActions)
	{
		std::string reason;

		if (!rcx::vr::input::RegisterAction(action.name, action.kind, action.binding, action.label, action.set, reason))
		{
			std::printf("FAIL register %s: %s\n", action.name, reason.c_str());
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

	std::printf("%s\n", failures == 0 ? "input files written" : "input files test failed");
	return failures == 0 ? 0 : 1;
}
