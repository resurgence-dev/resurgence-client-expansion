#include "vr/vr_lua.h"

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "core/lua_util.h"
#include "vr/d3d9_bridge.h"
#include "vr/vr_input.h"
#include "vr/vr_session.h"

using namespace GarrysMod::Lua;

namespace rcx
{
	bool IsFeatureOff(ILuaBase* lua, const char* feature, std::string& reason);
}

namespace rcx::vr
{
	namespace
	{
		constexpr double kRadiansToDegrees = 57.29577951308232;
		constexpr int64_t kMinTargetSize = 16;
		constexpr int64_t kMaxTargetSize = 16384;
		constexpr unsigned int kMaxStringLength = 64;
		constexpr int kMaxActionSets = 8;
		constexpr double kMaxHapticSeconds = 5.0;
		constexpr double kMaxHapticFrequency = 320.0;
		constexpr double kMaxFadeSeconds = 10.0;
		constexpr unsigned int kMaxKeyboardTextLength = 4096;
		constexpr double kMaxKeyboardCharacters = 1024.0;
		constexpr double kMaxKeyboardDistance = 100.0;

		// A string of at most kMaxStringLength bytes at stackPosition, or false.
		bool ReadShortString(ILuaBase* lua, int stackPosition, std::string& value)
		{
			if (!lua->IsType(stackPosition, Type::String))
			{
				return false;
			}

			unsigned int length = 0;
			const char* text = lua->GetString(stackPosition, &length);

			if (text == nullptr || length > kMaxStringLength)
			{
				return false;
			}

			value.assign(text, length);
			return true;
		}

		// Whether an argument is missing: past the last one Lua passed, or nil.
		bool IsAbsent(ILuaBase* lua, int stackPosition)
		{
			return stackPosition > lua->Top() || lua->IsType(stackPosition, Type::Nil);
		}

		bool ReadHand(ILuaBase* lua, int stackPosition, rcx::vr::input::Hand& hand)
		{
			std::string name;

			if (!ReadShortString(lua, stackPosition, name))
			{
				return false;
			}

			if (name == "left")
			{
				hand = rcx::vr::input::Hand::Left;
				return true;
			}

			if (name == "right")
			{
				hand = rcx::vr::input::Hand::Right;
				return true;
			}

			return false;
		}

		// OpenVR's tracking space (right handed, +Y up, -Z forward, meters) to Source's axes (+X forward, +Y left, +Z up).
		Vector ToSource(float x, float y, float z)
		{
			return rcx::lua::MakeVector(-z, -x, y);
		}

		Vector ColumnToSource(const ::vr::HmdMatrix34_t& matrix, int column, float sign)
		{
			return ToSource(sign * matrix.m[0][column], sign * matrix.m[1][column], sign * matrix.m[2][column]);
		}

		// Source's MatrixAngles, from the forward, left and up axes.
		QAngle AnglesFromAxes(const Vector& forward, const Vector& left, const Vector& up)
		{
			QAngle angles{};
			double xyDistance = std::sqrt(static_cast<double>(forward.x) * forward.x + static_cast<double>(forward.y) * forward.y);

			if (xyDistance > 0.001)
			{
				angles.x = static_cast<float>(std::atan2(-forward.z, xyDistance) * kRadiansToDegrees);
				angles.y = static_cast<float>(std::atan2(forward.y, forward.x) * kRadiansToDegrees);
				angles.z = static_cast<float>(std::atan2(left.z, up.z) * kRadiansToDegrees);
			}
			else
			{
				angles.x = static_cast<float>(std::atan2(-forward.z, xyDistance) * kRadiansToDegrees);
				angles.y = static_cast<float>(std::atan2(-left.x, left.y) * kRadiansToDegrees);
				angles.z = 0.0f;
			}

			return angles;
		}

		void PushTransform(ILuaBase* lua, const ::vr::HmdMatrix34_t& matrix)
		{
			// The device's forward is -Z, its left -X, its up +Y.
			Vector forward = ColumnToSource(matrix, 2, -1.0f);
			Vector left = ColumnToSource(matrix, 0, -1.0f);
			Vector up = ColumnToSource(matrix, 1, 1.0f);

			lua->PushVector(ColumnToSource(matrix, 3, 1.0f));
			lua->SetField(-2, "pos");
			lua->PushAngle(AnglesFromAxes(forward, left, up));
			lua->SetField(-2, "ang");
		}

		void PushPose(ILuaBase* lua, const DevicePose& pose)
		{
			lua->CreateTable();
			lua->PushBool(pose.valid);
			lua->SetField(-2, "valid");
			PushTransform(lua, pose.matrix);
			lua->PushVector(ToSource(pose.velocity.v[0], pose.velocity.v[1], pose.velocity.v[2]));
			lua->SetField(-2, "vel");

			Vector angularVelocity = ToSource(pose.angularVelocity.v[0], pose.angularVelocity.v[1], pose.angularVelocity.v[2]);
			angularVelocity.x = static_cast<float>(angularVelocity.x * kRadiansToDegrees);
			angularVelocity.y = static_cast<float>(angularVelocity.y * kRadiansToDegrees);
			angularVelocity.z = static_cast<float>(angularVelocity.z * kRadiansToDegrees);
			lua->PushVector(angularVelocity);
			lua->SetField(-2, "angvel");
		}

		void PushEye(ILuaBase* lua, const EyeDisplay& eye)
		{
			lua->CreateTable();
			lua->PushNumber(eye.tanLeft);
			lua->SetField(-2, "tanLeft");
			lua->PushNumber(eye.tanRight);
			lua->SetField(-2, "tanRight");
			lua->PushNumber(eye.tanUp);
			lua->SetField(-2, "tanUp");
			lua->PushNumber(eye.tanDown);
			lua->SetField(-2, "tanDown");
			PushTransform(lua, eye.eyeToHead);
		}

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

		RCX_FUNCTION(IsHeadsetPresent)
		{
			std::string reason;

			if (rcx::IsFeatureOff(lua, "vr", reason))
			{
				return rcx::lua::PushFailure(lua, reason);
			}

			if (!rcx::vr::IsHeadsetPresent(reason))
			{
				return rcx::lua::PushFailure(lua, reason);
			}

			lua->PushBool(true);
			return 1;
		}

		RCX_FUNCTION(Start)
		{
			std::string reason;

			if (rcx::IsFeatureOff(lua, "vr", reason))
			{
				return rcx::lua::PushFailure(lua, reason);
			}

			if (!rcx::vr::Start(reason))
			{
				return rcx::lua::PushFailure(lua, reason);
			}

			lua->PushBool(true);
			return 1;
		}

		RCX_FUNCTION(Stop)
		{
			rcx::vr::Stop();
			lua->PushBool(true);
			return 1;
		}

		RCX_FUNCTION(IsRunning)
		{
			lua->PushBool(rcx::vr::IsRunning());
			return 1;
		}

		RCX_FUNCTION(GetDisplay)
		{
			Display display{};
			std::string reason;

			if (!rcx::vr::GetDisplay(display, reason))
			{
				return rcx::lua::PushNilReason(lua, reason);
			}

			lua->CreateTable();
			SetNumber(lua, "width", display.width);
			SetNumber(lua, "height", display.height);

			if (display.maxWidth > 0)
			{
				SetNumber(lua, "maxWidth", display.maxWidth);
				SetNumber(lua, "maxHeight", display.maxHeight);
			}

			SetNumber(lua, "refreshRate", display.refreshRate);
			PushEye(lua, display.eyes[0]);
			lua->SetField(-2, "left");
			PushEye(lua, display.eyes[1]);
			lua->SetField(-2, "right");
			return 1;
		}

		RCX_FUNCTION(BeginTarget)
		{
			int64_t width = 0;
			int64_t height = 0;

			if (!rcx::lua::ReadInteger(lua, 1, kMinTargetSize, kMaxTargetSize, width) || !rcx::lua::ReadInteger(lua, 2, kMinTargetSize, kMaxTargetSize, height))
			{
				return rcx::lua::PushFailure(lua, "width and height must be whole numbers from 16 to 16384");
			}

			if (!rcx::vr::IsRunning())
			{
				return rcx::lua::PushFailure(lua, "VR isn't started");
			}

			rcx::vr::bridge::BeginCapture(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
			lua->PushBool(true);
			return 1;
		}

		RCX_FUNCTION(EndTarget)
		{
			std::string reason;

			if (!rcx::vr::bridge::EndCapture(reason))
			{
				return rcx::lua::PushFailure(lua, reason);
			}

			lua->PushBool(true);
			return 1;
		}

		RCX_FUNCTION(SetTextureBounds)
		{
			int eye = -1;

			if (lua->IsType(1, Type::String))
			{
				const char* name = lua->GetString(1);

				if (name != nullptr && std::strcmp(name, "left") == 0)
				{
					eye = 0;
				}
				else if (name != nullptr && std::strcmp(name, "right") == 0)
				{
					eye = 1;
				}
			}

			if (eye < 0)
			{
				return rcx::lua::PushFailure(lua, "eye must be \"left\" or \"right\"");
			}

			double values[4]{};

			for (int index = 0; index < 4; index++)
			{
				if (!rcx::lua::ReadNumber(lua, 2 + index, 0.0, 1.0, values[index]))
				{
					return rcx::lua::PushFailure(lua, "bounds must be numbers from 0 to 1: uMin, vMin, uMax, vMax");
				}
			}

			rcx::vr::bridge::TextureBounds bounds{};
			bounds.uMin = static_cast<float>(values[0]);
			bounds.vMin = static_cast<float>(values[1]);
			bounds.uMax = static_cast<float>(values[2]);
			bounds.vMax = static_cast<float>(values[3]);
			rcx::vr::bridge::SetBounds(eye, bounds);
			lua->PushBool(true);
			return 1;
		}

		RCX_FUNCTION(WaitFrame)
		{
			std::string reason;
			uint32_t frameId = rcx::vr::WaitFrame(reason);

			if (frameId == 0)
			{
				return rcx::lua::PushNilReason(lua, reason);
			}

			lua->PushNumber(frameId);
			return 1;
		}

		RCX_FUNCTION(GetPoses)
		{
			DevicePose poses[static_cast<int>(Device::Count)]{};
			uint32_t frameId = 0;
			std::string reason;

			if (!rcx::vr::GetPoses(poses, frameId, reason))
			{
				return rcx::lua::PushNilReason(lua, reason);
			}

			lua->CreateTable();
			SetNumber(lua, "frame", frameId);
			PushPose(lua, poses[static_cast<int>(Device::Head)]);
			lua->SetField(-2, "head");
			PushPose(lua, poses[static_cast<int>(Device::LeftHand)]);
			lua->SetField(-2, "left");
			PushPose(lua, poses[static_cast<int>(Device::RightHand)]);
			lua->SetField(-2, "right");
			return 1;
		}

		RCX_FUNCTION(Submit)
		{
			int64_t frameId = 0;

			if (!rcx::lua::ReadInteger(lua, 1, 1, 0x7fffffff, frameId))
			{
				return rcx::lua::PushFailure(lua, "frame id must be the number WaitFrame returned");
			}

			std::string reason;

			if (!rcx::vr::Submit(static_cast<uint32_t>(frameId), reason))
			{
				return rcx::lua::PushFailure(lua, reason);
			}

			lua->PushBool(true);
			return 1;
		}

		RCX_FUNCTION(GetStats)
		{
			rcx::vr::bridge::Stats stats = rcx::vr::bridge::GetStats();
			lua->CreateTable();
			SetBool(lua, "running", rcx::vr::IsRunning());
			SetBool(lua, "detached", rcx::vr::IsDetached());
			lua->PushString(rcx::vr::input::GetAppKey());
			lua->SetField(-2, "appKey");
			lua->PushString(rcx::vr::input::GetIdentity().c_str());
			lua->SetField(-2, "appIdentity");
			SetBool(lua, "hooksInstalled", stats.hooksInstalled);
			SetBool(lua, "deviceFound", stats.deviceFound);
			SetBool(lua, "deviceIsEx", stats.deviceIsEx);
			SetBool(lua, "presentOnOtherThread", stats.presentOnOtherThread);
			SetBool(lua, "eyeCaptured", stats.eyeCaptured);
			SetNumber(lua, "eyeWidth", stats.eyeWidth);
			SetNumber(lua, "eyeHeight", stats.eyeHeight);
			SetNumber(lua, "eyeFormat", stats.eyeFormat);
			SetNumber(lua, "presents", static_cast<double>(stats.presents));
			SetNumber(lua, "framesSubmitted", static_cast<double>(stats.framesSubmitted));
			SetNumber(lua, "submitErrors", static_cast<double>(stats.submitErrors));
			SetNumber(lua, "queuedDropped", static_cast<double>(stats.queuedDropped));
			SetNumber(lua, "presentsWaitingForDraw", static_cast<double>(stats.presentsWaitingForDraw));
			SetNumber(lua, "drawsWithoutSubmit", static_cast<double>(stats.drawsWithoutSubmit));
			SetNumber(lua, "framesSkipped", static_cast<double>(stats.framesSkipped));
			SetNumber(lua, "waitTimeouts", static_cast<double>(stats.waitTimeouts));
			SetNumber(lua, "resets", static_cast<double>(stats.resets));
			SetNumber(lua, "lastWaitMs", stats.lastWaitMs);
			SetNumber(lua, "maxWaitMs", stats.maxWaitMs);
			SetNumber(lua, "averageWaitMs", stats.averageWaitMs);
			SetNumber(lua, "lastSubmitMs", stats.lastSubmitMs);
			SetNumber(lua, "lastCompositorError", stats.lastCompositorError);
			lua->PushString(stats.backend.c_str());
			lua->SetField(-2, "backend");
			lua->PushString(stats.lastError.c_str());
			lua->SetField(-2, "lastError");
			return 1;
		}

		RCX_FUNCTION(RegisterAction)
		{
			std::string name;
			std::string kind;
			std::string binding;
			std::string label;
			std::string set;

			if (!ReadShortString(lua, 1, name) || !ReadShortString(lua, 2, kind))
			{
				return rcx::lua::PushFailure(lua, "RegisterAction(name, kind, binding, label, set): name and kind must be short strings");
			}

			if (!IsAbsent(lua, 3) && !ReadShortString(lua, 3, binding))
			{
				return rcx::lua::PushFailure(lua, "binding must be a short string or nil");
			}

			if (!IsAbsent(lua, 4) && !ReadShortString(lua, 4, label))
			{
				return rcx::lua::PushFailure(lua, "label must be a short string or nil");
			}

			if (!IsAbsent(lua, 5) && !ReadShortString(lua, 5, set))
			{
				return rcx::lua::PushFailure(lua, "set must be a short string or nil");
			}

			std::string reason;

			if (!rcx::vr::input::RegisterAction(name, kind, binding, label, set, reason))
			{
				return rcx::lua::PushFailure(lua, reason);
			}

			lua->PushBool(true);
			return 1;
		}

		RCX_FUNCTION(SetActionSets)
		{
			std::vector<std::string> sets;

			for (int index = 1; index <= kMaxActionSets && !IsAbsent(lua, index); index++)
			{
				std::string set;

				if (!ReadShortString(lua, index, set))
				{
					return rcx::lua::PushFailure(lua, "action sets are names");
				}

				sets.push_back(set);
			}

			std::string reason;

			if (!rcx::vr::input::SetActionSets(sets, reason))
			{
				return rcx::lua::PushFailure(lua, reason);
			}

			lua->PushBool(true);
			return 1;
		}

		RCX_FUNCTION(GetActions)
		{
			std::vector<rcx::vr::input::ActionValue> values;
			std::string reason;

			if (!rcx::vr::input::GetActions(values, reason))
			{
				return rcx::lua::PushNilReason(lua, reason);
			}

			lua->CreateTable();

			for (const rcx::vr::input::ActionValue& value : values)
			{
				switch (value.kind)
				{
				case rcx::vr::input::ActionKind::Boolean:
					lua->PushBool(value.state);
					break;
				case rcx::vr::input::ActionKind::Vector1:
					lua->PushNumber(value.x);
					break;
				case rcx::vr::input::ActionKind::Vector2:
					lua->CreateTable();
					SetNumber(lua, "x", value.x);
					SetNumber(lua, "y", value.y);
					break;
				}

				lua->SetField(-2, value.name.c_str());
			}

			return 1;
		}

		RCX_FUNCTION(Haptic)
		{
			rcx::vr::input::Hand hand = rcx::vr::input::Hand::Left;
			double seconds = 0.0;
			double frequency = 0.0;
			double amplitude = 0.0;

			if (!ReadHand(lua, 1, hand))
			{
				return rcx::lua::PushFailure(lua, "hand must be \"left\" or \"right\"");
			}

			bool valid = rcx::lua::ReadNumber(lua, 2, 0.0, kMaxHapticSeconds, seconds)
				&& rcx::lua::ReadNumber(lua, 3, 0.0, kMaxHapticFrequency, frequency)
				&& rcx::lua::ReadNumber(lua, 4, 0.0, 1.0, amplitude);

			if (!valid)
			{
				return rcx::lua::PushFailure(lua, "Haptic(hand, seconds 0-5, frequency 0-320, amplitude 0-1)");
			}

			std::string reason;

			if (!rcx::vr::input::Haptic(hand, static_cast<float>(seconds), static_cast<float>(frequency), static_cast<float>(amplitude), reason))
			{
				return rcx::lua::PushFailure(lua, reason);
			}

			lua->PushBool(true);
			return 1;
		}

		RCX_FUNCTION(GetHandSkeleton)
		{
			rcx::vr::input::Hand hand = rcx::vr::input::Hand::Left;

			if (!ReadHand(lua, 1, hand))
			{
				return rcx::lua::PushNilReason(lua, "hand must be \"left\" or \"right\"");
			}

			rcx::vr::input::HandSkeleton skeleton{};
			std::string reason;

			if (!rcx::vr::input::GetHandSkeleton(hand, skeleton, reason))
			{
				return rcx::lua::PushNilReason(lua, reason);
			}

			const char* fingers[5] = {"thumb", "index", "middle", "ring", "pinky"};
			lua->CreateTable();

			for (int finger = 0; finger < 5; finger++)
			{
				SetNumber(lua, fingers[finger], skeleton.curl[finger]);
			}

			lua->CreateTable();

			for (int gap = 0; gap < 4; gap++)
			{
				lua->PushNumber(gap + 1);
				lua->PushNumber(skeleton.splay[gap]);
				lua->SetTable(-3);
			}

			lua->SetField(-2, "splay");

			return 1;
		}

		RCX_FUNCTION(GetPlayArea)
		{
			PlayArea playArea{};
			std::string reason;

			if (!rcx::vr::GetPlayArea(playArea, reason))
			{
				return rcx::lua::PushNilReason(lua, reason);
			}

			lua->CreateTable();
			SetNumber(lua, "width", playArea.width);
			SetNumber(lua, "depth", playArea.depth);
			return 1;
		}

		RCX_FUNCTION(Fade)
		{
			double values[5]{};
			const double maximums[5] = {kMaxFadeSeconds, 1.0, 1.0, 1.0, 1.0};

			for (int index = 0; index < 5; index++)
			{
				if (!rcx::lua::ReadNumber(lua, 1 + index, 0.0, maximums[index], values[index]))
				{
					return rcx::lua::PushFailure(lua, "Fade(seconds 0-10, red, green, blue, alpha 0-1)");
				}
			}

			std::string reason;
			bool faded = rcx::vr::Fade(static_cast<float>(values[0]), static_cast<float>(values[1]), static_cast<float>(values[2]),
				static_cast<float>(values[3]), static_cast<float>(values[4]), reason);

			if (!faded)
			{
				return rcx::lua::PushFailure(lua, reason);
			}

			lua->PushBool(true);
			return 1;
		}

		RCX_FUNCTION(GetActionOrigins)
		{
			std::string name;

			if (!ReadShortString(lua, 1, name))
			{
				return rcx::lua::PushNilReason(lua, "GetActionOrigins(name): name must be a short string");
			}

			uint32_t count = 0;
			std::string reason;

			if (!rcx::vr::input::GetActionOrigins(name, count, reason))
			{
				return rcx::lua::PushNilReason(lua, reason);
			}

			lua->PushNumber(static_cast<double>(count));
			return 1;
		}

		// A string of at most maximum bytes at stackPosition, or false.
		bool ReadString(ILuaBase* lua, int stackPosition, unsigned int maximum, std::string& value)
		{
			if (!lua->IsType(stackPosition, Type::String))
			{
				return false;
			}

			unsigned int length = 0;
			const char* text = lua->GetString(stackPosition, &length);

			if (text == nullptr || length > maximum)
			{
				return false;
			}

			value.assign(text, length);
			return true;
		}

		RCX_FUNCTION(ShowKeyboard)
		{
			KeyboardRequest request{};
			double maxCharacters = 0.0;

			if (!IsAbsent(lua, 1) && !ReadString(lua, 1, kMaxKeyboardTextLength, request.text))
			{
				return rcx::lua::PushFailure(lua, "ShowKeyboard(text, maxCharacters, multiLine, minimal, description): text must be a string of at most 4096 bytes or nil");
			}

			if (!IsAbsent(lua, 2) && !rcx::lua::ReadNumber(lua, 2, 0.0, kMaxKeyboardCharacters, maxCharacters))
			{
				return rcx::lua::PushFailure(lua, "maxCharacters must be 0 to 1024 (0: 1024)");
			}

			request.maxCharacters = static_cast<uint32_t>(maxCharacters);
			request.multiLine = lua->IsType(3, Type::Bool) && lua->GetBool(3);
			request.minimal = !lua->IsType(4, Type::Bool) || lua->GetBool(4);

			if (!IsAbsent(lua, 5) && !ReadShortString(lua, 5, request.description))
			{
				return rcx::lua::PushFailure(lua, "description must be a short string or nil");
			}

			std::string reason;

			if (!rcx::vr::ShowKeyboard(request, reason))
			{
				return rcx::lua::PushFailure(lua, reason);
			}

			lua->PushBool(true);
			return 1;
		}

		RCX_FUNCTION(HideKeyboard)
		{
			rcx::vr::HideKeyboard();
			lua->PushBool(true);
			return 1;
		}

		RCX_FUNCTION(IsKeyboardOpen)
		{
			lua->PushBool(rcx::vr::IsKeyboardOpen());
			return 1;
		}

		RCX_FUNCTION(SetKeyboardPose)
		{
			double values[5]{};
			const double minimums[5] = {-kMaxKeyboardDistance, -kMaxKeyboardDistance, -kMaxKeyboardDistance, -360.0, -90.0};
			const double maximums[5] = {kMaxKeyboardDistance, kMaxKeyboardDistance, kMaxKeyboardDistance, 360.0, 90.0};

			for (int index = 0; index < 5; index++)
			{
				if (!rcx::lua::ReadNumber(lua, 1 + index, minimums[index], maximums[index], values[index]))
				{
					return rcx::lua::PushFailure(lua, "SetKeyboardPose(x, y, z meters in standing space, yaw -360 to 360, pitch -90 to 90 degrees)");
				}
			}

			std::string reason;
			bool placed = rcx::vr::SetKeyboardPose(static_cast<float>(values[0]), static_cast<float>(values[1]), static_cast<float>(values[2]),
				static_cast<float>(values[3]), static_cast<float>(values[4]), reason);

			if (!placed)
			{
				return rcx::lua::PushFailure(lua, reason);
			}

			lua->PushBool(true);
			return 1;
		}

		RCX_FUNCTION(PollEvents)
		{
			std::vector<Event> events = rcx::vr::TakeEvents();
			lua->CreateTable();

			for (size_t index = 0; index < events.size(); index++)
			{
				lua->PushNumber(static_cast<double>(index + 1));
				lua->CreateTable();
				lua->PushString(events[index].type.c_str());
				lua->SetField(-2, "type");
				SetNumber(lua, "value", events[index].value);

				if (!events[index].text.empty())
				{
					lua->PushString(events[index].text.c_str(), static_cast<unsigned int>(events[index].text.size()));
					lua->SetField(-2, "text");
				}

				lua->SetTable(-3);
			}

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
		SetFunction(lua, "IsHeadsetPresent", IsHeadsetPresent);
		SetFunction(lua, "Start", Start);
		SetFunction(lua, "Stop", Stop);
		SetFunction(lua, "IsRunning", IsRunning);
		SetFunction(lua, "GetDisplay", GetDisplay);
		SetFunction(lua, "BeginTarget", BeginTarget);
		SetFunction(lua, "EndTarget", EndTarget);
		SetFunction(lua, "SetTextureBounds", SetTextureBounds);
		SetFunction(lua, "WaitFrame", WaitFrame);
		SetFunction(lua, "GetPoses", GetPoses);
		SetFunction(lua, "Submit", Submit);
		SetFunction(lua, "GetStats", GetStats);
		SetFunction(lua, "RegisterAction", RegisterAction);
		SetFunction(lua, "SetActionSets", SetActionSets);
		SetFunction(lua, "GetActions", GetActions);
		SetFunction(lua, "Haptic", Haptic);
		SetFunction(lua, "GetHandSkeleton", GetHandSkeleton);
		SetFunction(lua, "GetPlayArea", GetPlayArea);
		SetFunction(lua, "Fade", Fade);
		SetFunction(lua, "PollEvents", PollEvents);
		SetFunction(lua, "GetActionOrigins", GetActionOrigins);
		SetFunction(lua, "ShowKeyboard", ShowKeyboard);
		SetFunction(lua, "HideKeyboard", HideKeyboard);
		SetFunction(lua, "IsKeyboardOpen", IsKeyboardOpen);
		SetFunction(lua, "SetKeyboardPose", SetKeyboardPose);
	}

	void OnLuaClose()
	{
		rcx::vr::Detach();
	}
}
