#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "openvr.h"

/*
	The OpenVR session: start and stop, the display, the frame wait and the poses. Lua thread only.
	Every function is safe in any order; a call that needs a session it doesn't have fails with a
	reason.
*/
namespace rcx::vr
{
	enum class Device
	{
		Head = 0,
		LeftHand = 1,
		RightHand = 2,
		Count = 3
	};

	struct DevicePose
	{
		bool valid = false;
		::vr::HmdMatrix34_t matrix{};
		::vr::HmdVector3_t velocity{};
		::vr::HmdVector3_t angularVelocity{};
	};

	struct EyeDisplay
	{
		float tanLeft = 0.0f;
		float tanRight = 0.0f;
		float tanUp = 0.0f;
		float tanDown = 0.0f;
		::vr::HmdMatrix34_t eyeToHead{};
	};

	struct Display
	{
		uint32_t width = 0;
		uint32_t height = 0;

		// The largest eye texture the driver takes before the compositor downsamples it, or 0 when
		// the driver doesn't say (Prop_Hmd_MaxDistortedTexture*).
		uint32_t maxWidth = 0;
		uint32_t maxHeight = 0;
		float refreshRate = 0.0f;
		EyeDisplay eyes[2]{};
	};

	/*
		rcx 0.79, frame pacing from SteamVR's side, updated once a second from WaitFrame: the headset's refresh
		rate, the average WaitGetPoses time, the compositor's new frames, reprojected and dropped frames per
		second (GetCumulativeStats), and from the last frame's timing the frames SteamVR is throttling the game
		by (1: every other headset frame) and predicting ahead.
	*/
	struct Pacing
	{
		double headsetHz = 0.0;
		double waitPosesMs = 0.0;
		double compositorFramesPerSecond = 0.0;
		double reprojectedPerSecond = 0.0;
		double droppedPerSecond = 0.0;
		uint32_t throttledFrames = 0;
		uint32_t predictedFrames = 0;
	};

	Pacing GetPacing();

	void RegisterFeature();

	bool IsHeadsetPresent(std::string& reason);
	bool Start(std::string& reason);
	void Stop();
	bool IsRunning();

	// The Lua state driving the session is closing (a disconnect or a server change). The session
	// stays: the compositor fades to its grid and rendering is suspended until the next WaitFrame,
	// from the next Lua state, resumes it.
	void Detach();
	bool IsDetached();

	bool GetDisplay(Display& display, std::string& reason);

	// Blocks until the compositor wants the next frame and reads the poses. Returns the frame id, or 0.
	uint32_t WaitFrame(std::string& reason);

	bool GetPoses(DevicePose (&poses)[static_cast<int>(Device::Count)], uint32_t& frameId, std::string& reason);

	bool Submit(uint32_t frameId, std::string& reason);

	struct ControllerInfo
	{
		// A controller holds the hand's role.
		bool connected = false;

		// Prop_ControllerType_String ("knuckles", "oculus_touch", ...) and Prop_RenderModelName_String;
		// empty when the driver doesn't say.
		std::string controllerType;
		std::string renderModel;

		// The render model's "tip" component, SteamVR's pointing pose (what /pose/tip is bound to), as
		// the transform from the tip's space to the controller's tracked space, -Z out of the tip.
		// False when the model has none.
		bool tipValid = false;
		::vr::HmdMatrix34_t tip{};
	};

	// The controller in a hand's role (Device::LeftHand or Device::RightHand). Read from the runtime
	// when the role's device changes or an event says the controller did (activated, updated,
	// deactivated, a role or its model or type property changed) and kept until then. A tip the
	// runtime can't give yet is asked for again at most once a second.
	bool GetControllerInfo(Device hand, ControllerInfo& info, std::string& reason);

	struct PlayArea
	{
		float width = 0.0f;
		float depth = 0.0f;
	};

	// The play area's size in meters, from the chaperone.
	bool GetPlayArea(PlayArea& playArea, std::string& reason);

	// The compositor's fade to a color over seconds; alpha 0 fades back in.
	bool Fade(float seconds, float red, float green, float blue, float alpha, std::string& reason);

	struct Event
	{
		std::string type;
		float value = 0.0f;
		std::string text;
	};

	// The events WaitFrame collected since the last call, oldest first: quit, dashboard_open,
	// dashboard_close, ipd_changed (value: meters), play_area_changed, controllers_changed,
	// input_focus_changed, headset_disconnected; and the system keyboard's keyboard_char (text: what
	// was typed, "\b" for backspace, "\n" for enter, an escape sequence for an arrow key),
	// keyboard_done (text: SteamVR's copy of the text, empty in minimal mode) and keyboard_closed.
	std::vector<Event> TakeEvents();

	struct KeyboardRequest
	{
		std::string text;
		std::string description;
		uint32_t maxCharacters = 0;
		bool multiLine = false;

		// Minimal: every key is sent at once as keyboard_char and SteamVR keeps no text of its own.
		// Otherwise SteamVR edits its own copy and sends it with keyboard_done.
		bool minimal = true;
	};

	// Opens SteamVR's system keyboard for the scene application (only one at a time).
	bool ShowKeyboard(const KeyboardRequest& request, std::string& reason);
	void HideKeyboard();
	bool IsKeyboardOpen();

	// Places the keyboard in the standing tracking space: a position in meters and a turn about the
	// vertical (yaw) and then the keyboard's own sideways axis (pitch), in degrees.
	bool SetKeyboardPose(float x, float y, float z, float yawDegrees, float pitchDegrees, std::string& reason);
}
