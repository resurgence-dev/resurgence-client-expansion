#include "vr/vr_session.h"

#include <cmath>
#include <cstring>

#include "core/core.h"
#include "vr/d3d9_bridge.h"
#include "vr/openvr_loader.h"
#include "vr/vr_input.h"

namespace rcx::vr
{
	namespace
	{
		constexpr const char* kFeature = "vr";
		constexpr int kFrameHistory = 8;
		constexpr size_t kMaxQueuedEvents = 64;
		constexpr int kMaxEventsPerFrame = 64;
		constexpr uint64_t kKeyboardToken = 0x52435831;
		constexpr uint32_t kMaxKeyboardCharacters = 1024;
		constexpr double kDegreesToRadians = 0.017453292519943295;

		struct FrameRecord
		{
			uint32_t frameId = 0;
			::vr::HmdMatrix34_t headPose{};
		};

		const OpenVRApi* api = nullptr;
		::vr::IVRSystem* vrSystem = nullptr;
		::vr::IVRCompositor* vrCompositor = nullptr;
		::vr::IVRChaperone* vrChaperone = nullptr;
		::vr::IVROverlay* vrOverlay = nullptr;
		bool keyboardOpen = false;
		bool running = false;
		bool detached = false;
		constexpr float kGridFadeSeconds = 0.4f;
		std::vector<Event> queuedEvents;

		uint32_t lastFrameId = 0;
		FrameRecord frames[kFrameHistory]{};
		DevicePose latestPoses[static_cast<int>(Device::Count)]{};

		bool IsOff(std::string& reason)
		{
			return rcx::core::IsFeatureDisabled(kFeature, &reason);
		}

		void ShutdownRuntime()
		{
			rcx::vr::input::EndSession();
			vrSystem = nullptr;
			vrCompositor = nullptr;
			vrChaperone = nullptr;
			vrOverlay = nullptr;
			keyboardOpen = false;

			if (api != nullptr)
			{
				api->ShutdownInternal();
			}
		}

		void QueueEvent(const char* type, float value, const std::string& text = std::string())
		{
			if (queuedEvents.size() >= kMaxQueuedEvents)
			{
				queuedEvents.erase(queuedEvents.begin());
			}

			Event event{};
			event.type = type;
			event.value = value;
			event.text = text;
			queuedEvents.push_back(event);
		}

		// A keyboard event of ours: the scene application's keyboard, opened by ShowKeyboard.
		bool IsOurKeyboard(const ::vr::VREvent_t& event)
		{
			return keyboardOpen && event.data.keyboard.overlayHandle == ::vr::k_ulOverlayHandleInvalid;
		}

		std::string KeyboardInput(const ::vr::VREvent_t& event)
		{
			const char* input = event.data.keyboard.cNewInput;
			size_t length = 0;

			while (length < sizeof(event.data.keyboard.cNewInput) && input[length] != '\0')
			{
				length++;
			}

			return std::string(input, length);
		}

		std::string KeyboardText()
		{
			if (vrOverlay == nullptr)
			{
				return std::string();
			}

			std::vector<char> buffer(kMaxKeyboardCharacters * 4 + 1, '\0');
			vrOverlay->GetKeyboardText(buffer.data(), static_cast<uint32_t>(buffer.size()));
			buffer.back() = '\0';
			return std::string(buffer.data());
		}

		// Collects the runtime's events. Returns false when SteamVR asked the app to quit.
		bool PollRuntimeEvents()
		{
			::vr::VREvent_t event{};

			for (int count = 0; count < kMaxEventsPerFrame && vrSystem->PollNextEvent(&event, sizeof(event)); count++)
			{
				switch (event.eventType)
				{
				case ::vr::VREvent_Quit:
					QueueEvent("quit", 0.0f);
					return false;
				case ::vr::VREvent_DashboardActivated:
					QueueEvent("dashboard_open", 0.0f);
					break;
				case ::vr::VREvent_DashboardDeactivated:
					QueueEvent("dashboard_close", 0.0f);
					break;
				case ::vr::VREvent_IpdChanged:
					QueueEvent("ipd_changed", event.data.ipd.ipdMeters);
					break;
				case ::vr::VREvent_ChaperoneUniverseHasChanged:
				case ::vr::VREvent_ChaperoneDataHasChanged:
					QueueEvent("play_area_changed", 0.0f);
					break;
				case ::vr::VREvent_TrackedDeviceRoleChanged:
					QueueEvent("controllers_changed", 0.0f);
					break;
				case ::vr::VREvent_InputFocusChanged:
					QueueEvent("input_focus_changed", 0.0f);
					break;
				case ::vr::VREvent_KeyboardCharInput:
					if (IsOurKeyboard(event))
					{
						QueueEvent("keyboard_char", 0.0f, KeyboardInput(event));
					}

					break;
				case ::vr::VREvent_KeyboardDone:
					if (IsOurKeyboard(event))
					{
						QueueEvent("keyboard_done", 0.0f, KeyboardText());
					}

					break;
				case ::vr::VREvent_KeyboardClosed:
				case ::vr::VREvent_KeyboardClosed_Global:
					if (IsOurKeyboard(event))
					{
						keyboardOpen = false;
						QueueEvent("keyboard_closed", 0.0f);
					}

					break;
				case ::vr::VREvent_TrackedDeviceDeactivated:
					if (event.trackedDeviceIndex == ::vr::k_unTrackedDeviceIndex_Hmd)
					{
						QueueEvent("headset_disconnected", 0.0f);
					}

					break;
				default:
					break;
				}
			}

			return true;
		}

		DevicePose ReadPose(const ::vr::TrackedDevicePose_t* poses, ::vr::TrackedDeviceIndex_t index)
		{
			DevicePose pose{};

			if (index >= ::vr::k_unMaxTrackedDeviceCount)
			{
				return pose;
			}

			const ::vr::TrackedDevicePose_t& source = poses[index];
			pose.valid = source.bPoseIsValid && source.bDeviceIsConnected;
			pose.matrix = source.mDeviceToAbsoluteTracking;
			pose.velocity = source.vVelocity;
			pose.angularVelocity = source.vAngularVelocity;
			return pose;
		}
	}

	void RegisterFeature()
	{
		rcx::core::RegisterFeature(kFeature);

		if (!IsOpenVRFilePresent())
		{
			rcx::core::DisableFeature(kFeature, "missing dependency: openvr_api.dll next to the module");
		}
	}

	bool IsHeadsetPresent(std::string& reason)
	{
		if (IsOff(reason))
		{
			return false;
		}

		if (running)
		{
			return true;
		}

		const OpenVRApi* loadedApi = LoadOpenVR(reason);

		if (loadedApi == nullptr)
		{
			return false;
		}

		if (!loadedApi->IsRuntimeInstalled())
		{
			reason = "SteamVR isn't installed";
			return false;
		}

		if (!loadedApi->IsHmdPresent())
		{
			reason = "no headset found";
			return false;
		}

		return true;
	}

	bool Start(std::string& reason)
	{
		if (running)
		{
			return true;
		}

		if (IsOff(reason))
		{
			return false;
		}

		api = LoadOpenVR(reason);

		if (api == nullptr)
		{
			rcx::core::DisableFeature(kFeature, reason);
			return false;
		}

		if (!rcx::vr::bridge::InstallHooks(reason))
		{
			rcx::core::DisableFeature(kFeature, "failed start: " + reason);
			return false;
		}

		if (!api->IsRuntimeInstalled())
		{
			reason = "SteamVR isn't installed";
			return false;
		}

		::vr::EVRInitError error = ::vr::VRInitError_None;
		api->InitInternal2(&error, ::vr::VRApplication_Scene, nullptr);

		if (error != ::vr::VRInitError_None)
		{
			reason = DescribeInitError(error);
			return false;
		}

		vrSystem = static_cast<::vr::IVRSystem*>(api->GetGenericInterface(::vr::IVRSystem_Version, &error));

		if (vrSystem == nullptr || error != ::vr::VRInitError_None)
		{
			reason = "no " + std::string(::vr::IVRSystem_Version) + ": " + DescribeInitError(error);
			ShutdownRuntime();
			return false;
		}

		vrCompositor = static_cast<::vr::IVRCompositor*>(api->GetGenericInterface(::vr::IVRCompositor_Version, &error));

		if (vrCompositor == nullptr || error != ::vr::VRInitError_None)
		{
			reason = "no " + std::string(::vr::IVRCompositor_Version) + ": " + DescribeInitError(error);
			ShutdownRuntime();
			return false;
		}

		vrCompositor->SetTrackingSpace(::vr::TrackingUniverseStanding);
		vrChaperone = static_cast<::vr::IVRChaperone*>(api->GetGenericInterface(::vr::IVRChaperone_Version, &error));

		::vr::EVRInitError inputError = ::vr::VRInitError_None;
		auto* vrInput = static_cast<::vr::IVRInput*>(api->GetGenericInterface(::vr::IVRInput_Version, &inputError));

		if (vrInput == nullptr || inputError != ::vr::VRInitError_None)
		{
			rcx::core::Log("vr: no %s, so no controller input: %s", ::vr::IVRInput_Version, DescribeInitError(inputError).c_str());
			vrInput = nullptr;
		}

		::vr::EVRInitError overlayError = ::vr::VRInitError_None;
		vrOverlay = static_cast<::vr::IVROverlay*>(api->GetGenericInterface(::vr::IVROverlay_Version, &overlayError));

		if (overlayError != ::vr::VRInitError_None)
		{
			rcx::core::Log("vr: no %s, so no system keyboard: %s", ::vr::IVROverlay_Version, DescribeInitError(overlayError).c_str());
			vrOverlay = nullptr;
		}

		::vr::EVRInitError applicationsError = ::vr::VRInitError_None;
		auto* vrApplications = static_cast<::vr::IVRApplications*>(api->GetGenericInterface(::vr::IVRApplications_Version, &applicationsError));

		if (applicationsError != ::vr::VRInitError_None)
		{
			vrApplications = nullptr;
		}

		rcx::vr::input::BeginSession(vrInput, vrApplications);
		queuedEvents.clear();

		if (!rcx::vr::bridge::BeginSession(vrSystem, vrCompositor, reason))
		{
			ShutdownRuntime();
			return false;
		}

		for (FrameRecord& frame : frames)
		{
			frame = FrameRecord{};
		}

		for (DevicePose& pose : latestPoses)
		{
			pose = DevicePose{};
		}

		running = true;
		rcx::core::Log("vr: session started");
		return true;
	}

	void Stop()
	{
		if (!running)
		{
			return;
		}

		running = false;
		detached = false;
		rcx::vr::bridge::EndSession();
		ShutdownRuntime();
		rcx::core::Log("vr: session stopped");
	}

	bool IsRunning()
	{
		return running;
	}

	void Detach()
	{
		if (!running || detached || vrCompositor == nullptr)
		{
			return;
		}

		vrCompositor->FadeGrid(kGridFadeSeconds, true);
		vrCompositor->SuspendRendering(true);
		detached = true;
		rcx::core::Log("vr: the lua state closed; the session stays, rendering suspended until the next WaitFrame");
	}

	bool IsDetached()
	{
		return detached;
	}

	// The headset's integer property, or 0 when it's missing, unreadable or not positive.
	static uint32_t ReadPositiveIntProperty(::vr::ETrackedDeviceProperty property)
	{
		if (vrSystem == nullptr)
		{
			return 0;
		}

		::vr::ETrackedPropertyError propertyError = ::vr::TrackedProp_Success;
		int32_t value = vrSystem->GetInt32TrackedDeviceProperty(::vr::k_unTrackedDeviceIndex_Hmd, property, &propertyError);

		if (propertyError != ::vr::TrackedProp_Success || value <= 0)
		{
			return 0;
		}

		return static_cast<uint32_t>(value);
	}

	bool GetDisplay(Display& display, std::string& reason)
	{
		if (!running || vrSystem == nullptr)
		{
			reason = "VR isn't started";
			return false;
		}

		vrSystem->GetRecommendedRenderTargetSize(&display.width, &display.height);

		::vr::ETrackedPropertyError propertyError = ::vr::TrackedProp_Success;
		display.refreshRate = vrSystem->GetFloatTrackedDeviceProperty(::vr::k_unTrackedDeviceIndex_Hmd, ::vr::Prop_DisplayFrequency_Float, &propertyError);

		display.maxWidth = ReadPositiveIntProperty(::vr::Prop_Hmd_MaxDistortedTextureWidth_Int32);
		display.maxHeight = ReadPositiveIntProperty(::vr::Prop_Hmd_MaxDistortedTextureHeight_Int32);

		if (display.maxWidth == 0 || display.maxHeight == 0)
		{
			display.maxWidth = 0;
			display.maxHeight = 0;
		}

		for (int eye = 0; eye < 2; eye++)
		{
			float left = 0.0f;
			float right = 0.0f;
			float top = 0.0f;
			float bottom = 0.0f;
			vrSystem->GetProjectionRaw(static_cast<::vr::EVREye>(eye), &left, &right, &top, &bottom);

			// OpenVR gives signed tangents and names two of them backwards: "bottom" runs from the center of
			// projection to the display's top edge and "top" to its bottom edge (the note on GetProjectionRaw
			// in the Driver API documentation). These are the extents on each side.
			EyeDisplay& eyeDisplay = display.eyes[eye];
			eyeDisplay.tanLeft = -left;
			eyeDisplay.tanRight = right;
			eyeDisplay.tanUp = bottom;
			eyeDisplay.tanDown = -top;
			eyeDisplay.eyeToHead = vrSystem->GetEyeToHeadTransform(static_cast<::vr::EVREye>(eye));
		}

		return true;
	}

	uint32_t WaitFrame(std::string& reason)
	{
		if (!running || vrCompositor == nullptr || vrSystem == nullptr)
		{
			reason = "VR isn't started";
			return 0;
		}

		if (detached)
		{
			vrCompositor->SuspendRendering(false);
			vrCompositor->FadeGrid(kGridFadeSeconds, false);
			detached = false;
			rcx::core::Log("vr: a lua state took the session over");
		}

		::vr::TrackedDevicePose_t renderPoses[::vr::k_unMaxTrackedDeviceCount]{};
		::vr::EVRCompositorError error = vrCompositor->WaitGetPoses(renderPoses, ::vr::k_unMaxTrackedDeviceCount, nullptr, 0);

		if (error != ::vr::VRCompositorError_None && error != ::vr::VRCompositorError_DoNotHaveFocus)
		{
			reason = "compositor wait error " + std::to_string(static_cast<int>(error));
			return 0;
		}

		::vr::TrackedDeviceIndex_t leftHand = vrSystem->GetTrackedDeviceIndexForControllerRole(::vr::TrackedControllerRole_LeftHand);
		::vr::TrackedDeviceIndex_t rightHand = vrSystem->GetTrackedDeviceIndexForControllerRole(::vr::TrackedControllerRole_RightHand);

		latestPoses[static_cast<int>(Device::Head)] = ReadPose(renderPoses, ::vr::k_unTrackedDeviceIndex_Hmd);
		latestPoses[static_cast<int>(Device::LeftHand)] = ReadPose(renderPoses, leftHand);
		latestPoses[static_cast<int>(Device::RightHand)] = ReadPose(renderPoses, rightHand);

		rcx::vr::bridge::NoteWait();
		rcx::vr::input::Update();

		if (!PollRuntimeEvents())
		{
			Stop();
			reason = "SteamVR is quitting";
			return 0;
		}

		lastFrameId++;

		if (lastFrameId == 0 || lastFrameId > 0x7fffffffu)
		{
			lastFrameId = 1;
		}

		FrameRecord& record = frames[lastFrameId % kFrameHistory];
		record.frameId = lastFrameId;
		record.headPose = latestPoses[static_cast<int>(Device::Head)].matrix;
		return lastFrameId;
	}

	bool GetPoses(DevicePose (&poses)[static_cast<int>(Device::Count)], uint32_t& frameId, std::string& reason)
	{
		if (!running)
		{
			reason = "VR isn't started";
			return false;
		}

		if (lastFrameId == 0)
		{
			reason = "no frame yet: call WaitFrame first";
			return false;
		}

		for (int device = 0; device < static_cast<int>(Device::Count); device++)
		{
			poses[device] = latestPoses[device];
		}

		frameId = lastFrameId;
		return true;
	}

	bool Submit(uint32_t frameId, std::string& reason)
	{
		if (!running)
		{
			reason = "VR isn't started";
			return false;
		}

		const FrameRecord& record = frames[frameId % kFrameHistory];

		if (frameId == 0 || record.frameId != frameId)
		{
			reason = "unknown or old frame id";
			return false;
		}

		return rcx::vr::bridge::QueueSubmit(frameId, record.headPose, reason);
	}
}

namespace rcx::vr
{
	bool GetPlayArea(PlayArea& playArea, std::string& reason)
	{
		if (!running || vrChaperone == nullptr)
		{
			reason = running ? "the runtime has no chaperone" : "VR isn't started";
			return false;
		}

		if (!vrChaperone->GetPlayAreaSize(&playArea.width, &playArea.depth))
		{
			reason = "no play area is set up";
			return false;
		}

		return true;
	}

	bool Fade(float seconds, float red, float green, float blue, float alpha, std::string& reason)
	{
		if (!running || vrCompositor == nullptr)
		{
			reason = "VR isn't started";
			return false;
		}

		vrCompositor->FadeToColor(seconds, red, green, blue, alpha, false);
		return true;
	}

	std::vector<Event> TakeEvents()
	{
		std::vector<Event> events;
		events.swap(queuedEvents);
		return events;
	}

	bool ShowKeyboard(const KeyboardRequest& request, std::string& reason)
	{
		if (!running || vrOverlay == nullptr)
		{
			reason = running ? "the runtime has no overlay interface" : "VR isn't started";
			return false;
		}

		uint32_t maxCharacters = request.maxCharacters == 0 || request.maxCharacters > kMaxKeyboardCharacters ? kMaxKeyboardCharacters : request.maxCharacters;
		uint32_t flags = ::vr::KeyboardFlag_Modal;

		if (request.minimal)
		{
			flags |= ::vr::KeyboardFlag_Minimal;
		}

		::vr::EGamepadTextInputLineMode lineMode = request.multiLine ? ::vr::k_EGamepadTextInputLineModeMultipleLines : ::vr::k_EGamepadTextInputLineModeSingleLine;
		::vr::EVROverlayError error = vrOverlay->ShowKeyboard(::vr::k_EGamepadTextInputModeNormal, lineMode, flags, request.description.c_str(), maxCharacters, request.text.c_str(), kKeyboardToken);

		if (error != ::vr::VROverlayError_None)
		{
			reason = std::string("ShowKeyboard failed: ") + vrOverlay->GetOverlayErrorNameFromEnum(error);
			return false;
		}

		keyboardOpen = true;
		return true;
	}

	void HideKeyboard()
	{
		if (vrOverlay == nullptr || !keyboardOpen)
		{
			return;
		}

		vrOverlay->HideKeyboard();
	}

	bool IsKeyboardOpen()
	{
		return keyboardOpen;
	}

	bool SetKeyboardPose(float x, float y, float z, float yawDegrees, float pitchDegrees, std::string& reason)
	{
		if (!running || vrOverlay == nullptr)
		{
			reason = running ? "the runtime has no overlay interface" : "VR isn't started";
			return false;
		}

		double yaw = yawDegrees * kDegreesToRadians;
		double pitch = pitchDegrees * kDegreesToRadians;
		double cosYaw = std::cos(yaw);
		double sinYaw = std::sin(yaw);
		double cosPitch = std::cos(pitch);
		double sinPitch = std::sin(pitch);

		// R = yaw about +Y, then pitch about the turned +X; columns are the keyboard's axes.
		::vr::HmdMatrix34_t transform{};
		transform.m[0][0] = static_cast<float>(cosYaw);
		transform.m[0][1] = static_cast<float>(sinYaw * sinPitch);
		transform.m[0][2] = static_cast<float>(sinYaw * cosPitch);
		transform.m[1][0] = 0.0f;
		transform.m[1][1] = static_cast<float>(cosPitch);
		transform.m[1][2] = static_cast<float>(-sinPitch);
		transform.m[2][0] = static_cast<float>(-sinYaw);
		transform.m[2][1] = static_cast<float>(cosYaw * sinPitch);
		transform.m[2][2] = static_cast<float>(cosYaw * cosPitch);
		transform.m[0][3] = x;
		transform.m[1][3] = y;
		transform.m[2][3] = z;

		vrOverlay->SetKeyboardTransformAbsolute(::vr::TrackingUniverseStanding, &transform);
		return true;
	}
}
