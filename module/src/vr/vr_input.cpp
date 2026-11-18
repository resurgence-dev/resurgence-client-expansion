#include "vr/vr_input.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "core/core.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace rcx::vr::input
{
	namespace
	{
		// The action sets Lua can put an action in, with the name SteamVR's binding screen shows.
		struct SetInfo
		{
			const char* name = nullptr;
			const char* label = nullptr;
		};

		const SetInfo kSets[] = {
			{"main", "Gameplay"},
			{"pointer", "Menu pointer"},
			{"pipboy", "Pip-Boy"},
			{"weapon", "Weapon held"},
		};
		constexpr int kSetCount = static_cast<int>(sizeof(kSets) / sizeof(kSets[0]));

		// The module's own set: the hand skeletons and the haptic output, always active, below Lua's sets.
		constexpr const char* kModuleSet = "/actions/rcx";
		constexpr const char* kModuleSetLabel = "Hands and haptics";
		constexpr const char* kSkeletonActions[2] = {"/actions/rcx/in/skeleton_left", "/actions/rcx/in/skeleton_right"};
		constexpr const char* kSkeletonLabels[2] = {"Left hand skeleton", "Right hand skeleton"};
		constexpr const char* kHapticAction = "/actions/rcx/out/haptic";
		constexpr const char* kHapticLabel = "Vibration";
		constexpr const char* kHandPaths[2] = {"/user/hand/left", "/user/hand/right"};
		constexpr const wchar_t* kFolderName = L"rcx_vr_input";
		constexpr size_t kMaxNameLength = 32;
		constexpr size_t kMaxLabelLength = 48;

		// The application identity SteamVR keeps bindings under, instead of Garry's Mod's steam.app.4000 (VR-1).
		constexpr const char* kAppKey = "gmod.resurgence.rcx";
		constexpr const char* kAppName = "Resurgence (Garry's Mod)";

		// Where a suggested binding sits on a controller: a side and one of these inputs.
		enum class Slot
		{
			None,
			Stick,
			StickClick,
			Trigger,
			Grip,
			A,
			B
		};

		// Which of SteamVR's button inputs an action takes: the plain click, or one of the press types
		// a "button" mode source tells apart, so one button can carry two or three actions.
		enum class Press
		{
			Click,
			Single,
			Double,
			Long,
			Held
		};

		struct Binding
		{
			int side = 0;
			Slot slot = Slot::None;
			Press press = Press::Click;
		};

		// The press timings written on a source that has a double, long or held input (seconds).
		constexpr const char* kLongPressDelay = "0.3";
		constexpr const char* kDoublePressDelay = "0.2";

		struct Action
		{
			std::string name;
			std::string label;
			int set = 0;
			ActionKind kind = ActionKind::Boolean;
			Binding binding{};
			::vr::VRActionHandle_t handle = ::vr::k_ulInvalidActionHandle;
			ActionValue value{};
		};

		// How one controller type spells a slot: the source path under the hand, its mode, and the
		// component an action of each kind binds to ("" when that kind can't go there).
		struct SlotSpelling
		{
			const char* path = nullptr;
			const char* mode = nullptr;
			const char* booleanComponent = "";
			const char* vector1Component = "";
			const char* vector2Component = "";
		};

		struct ControllerProfile
		{
			const char* controllerType = nullptr;
			const char* name = nullptr;
			SlotSpelling stick{};
			SlotSpelling stickClick{};
			SlotSpelling trigger{};
			SlotSpelling grip{};
			SlotSpelling a[2]{};
			SlotSpelling b[2]{};
		};

		const ControllerProfile kProfiles[] = {
			{
				"knuckles",
				"Valve Index",
				{"/input/thumbstick", "joystick", "", "", "position"},
				{"/input/thumbstick", "joystick", "click", "", ""},
				{"/input/trigger", "trigger", "click", "pull", ""},
				{"/input/grip", "trigger", "click", "pull", ""},
				{{"/input/a", "button", "click", "", ""}, {"/input/a", "button", "click", "", ""}},
				{{"/input/b", "button", "click", "", ""}, {"/input/b", "button", "click", "", ""}},
			},
			{
				"oculus_touch",
				"Oculus Touch (also Quest and Quest Pro controllers)",
				{"/input/joystick", "joystick", "", "", "position"},
				{"/input/joystick", "joystick", "click", "", ""},
				{"/input/trigger", "trigger", "click", "pull", ""},
				{"/input/grip", "trigger", "click", "pull", ""},
				{{"/input/x", "button", "click", "", ""}, {"/input/a", "button", "click", "", ""}},
				{{"/input/y", "button", "click", "", ""}, {"/input/b", "button", "click", "", ""}},
			},
			{
				"vive_controller",
				"Vive wand",
				{"/input/trackpad", "joystick", "", "", "position"},
				{"/input/trackpad", "joystick", "click", "", ""},
				{"/input/trigger", "trigger", "click", "pull", ""},
				{"/input/grip", "button", "click", "", ""},
				{{"/input/trackpad", "joystick", "click", "", ""}, {"/input/trackpad", "joystick", "click", "", ""}},
				{{"/input/application_menu", "button", "click", "", ""}, {"/input/application_menu", "button", "click", "", ""}},
			},
			{
				// Windows Mixed Reality: a thumbstick and a trackpad, no face buttons; a is the trackpad's click, b the menu button.
				"holographic_controller",
				"Windows Mixed Reality",
				{"/input/joystick", "joystick", "", "", "position"},
				{"/input/joystick", "joystick", "click", "", ""},
				{"/input/trigger", "trigger", "click", "pull", ""},
				{"/input/grip", "button", "click", "", ""},
				{{"/input/trackpad", "trackpad", "click", "", ""}, {"/input/trackpad", "trackpad", "click", "", ""}},
				{{"/input/application_menu", "button", "click", "", ""}, {"/input/application_menu", "button", "click", "", ""}},
			},
		};

		std::vector<Action> actions;
		::vr::IVRInput* vrInput = nullptr;
		bool setsReady = false;
		bool manifestDirty = true;
		::vr::VRActionSetHandle_t setHandles[kSetCount]{};
		::vr::VRActionSetHandle_t moduleSetHandle = ::vr::k_ulInvalidActionSetHandle;
		std::vector<int> activeSets;
		std::string identity = "not tried";
		bool identityTried = false;
		::vr::VRActionHandle_t skeletonHandles[2] = {::vr::k_ulInvalidActionHandle, ::vr::k_ulInvalidActionHandle};
		::vr::VRActionHandle_t hapticHandle = ::vr::k_ulInvalidActionHandle;
		::vr::VRInputValueHandle_t handSources[2] = {::vr::k_ulInvalidInputValueHandle, ::vr::k_ulInvalidInputValueHandle};
		HandSkeleton skeletons[2]{};
		bool skeletonValid[2] = {false, false};

		bool IsValidName(const std::string& name)
		{
			if (name.empty() || name.size() > kMaxNameLength || name.compare(0, 4, "rcx_") == 0)
			{
				return false;
			}

			if (name[0] < 'a' || name[0] > 'z')
			{
				return false;
			}

			for (char character : name)
			{
				bool letter = character >= 'a' && character <= 'z';
				bool digit = character >= '0' && character <= '9';

				if (!letter && !digit && character != '_')
				{
					return false;
				}
			}

			return true;
		}

		bool ParseKind(const std::string& text, ActionKind& kind)
		{
			if (text == "boolean")
			{
				kind = ActionKind::Boolean;
				return true;
			}

			if (text == "vector1")
			{
				kind = ActionKind::Vector1;
				return true;
			}

			if (text == "vector2")
			{
				kind = ActionKind::Vector2;
				return true;
			}

			return false;
		}

		// A display name: printable, no quote or backslash (it goes into JSON as is), at most kMaxLabelLength bytes. UTF-8 passes.
		bool IsValidLabel(const std::string& label)
		{
			if (label.size() > kMaxLabelLength)
			{
				return false;
			}

			for (char character : label)
			{
				unsigned char byte = static_cast<unsigned char>(character);

				if (byte < 0x20 || byte == 0x7f || character == '"' || character == '\\')
				{
					return false;
				}
			}

			return true;
		}

		bool ParseSet(const std::string& text, int& set)
		{
			for (int index = 0; index < kSetCount; index++)
			{
				if (text == kSets[index].name)
				{
					set = index;
					return true;
				}
			}

			return false;
		}

		std::string SetPath(int set)
		{
			return std::string("/actions/") + kSets[set].name;
		}

		bool ParseBinding(const std::string& text, Binding& binding)
		{
			binding = Binding{};

			if (text.empty())
			{
				return true;
			}

			std::string slotName;
			std::string body = text;

			struct PressName
			{
				const char* suffix = nullptr;
				Press press = Press::Click;
			};

			const PressName pressNames[] = {
				{"_single", Press::Single},
				{"_double", Press::Double},
				{"_long", Press::Long},
				{"_held", Press::Held},
			};

			for (const PressName& entry : pressNames)
			{
				size_t length = std::strlen(entry.suffix);

				if (body.size() > length && body.compare(body.size() - length, length, entry.suffix) == 0)
				{
					binding.press = entry.press;
					body = body.substr(0, body.size() - length);
					break;
				}
			}

			if (body.compare(0, 5, "left_") == 0)
			{
				binding.side = 0;
				slotName = body.substr(5);
			}
			else if (body.compare(0, 6, "right_") == 0)
			{
				binding.side = 1;
				slotName = body.substr(6);
			}
			else
			{
				return false;
			}

			struct SlotName
			{
				const char* name;
				Slot slot;
			};

			const SlotName slotNames[] = {
				{"stick", Slot::Stick},
				{"stick_click", Slot::StickClick},
				{"trigger", Slot::Trigger},
				{"grip", Slot::Grip},
				{"a", Slot::A},
				{"b", Slot::B},
			};

			for (const SlotName& entry : slotNames)
			{
				if (slotName == entry.name)
				{
					binding.slot = entry.slot;
					return true;
				}
			}

			return false;
		}

		bool KindFitsSlot(ActionKind kind, Slot slot)
		{
			switch (slot)
			{
			case Slot::None:
				return true;
			case Slot::Stick:
				return kind == ActionKind::Vector2;
			case Slot::Trigger:
			case Slot::Grip:
				return kind == ActionKind::Boolean || kind == ActionKind::Vector1;
			case Slot::StickClick:
			case Slot::A:
			case Slot::B:
				return kind == ActionKind::Boolean;
			}

			return false;
		}

		// A press type goes only on a face button, for a boolean action.
		bool PressFitsSlot(ActionKind kind, const Binding& binding)
		{
			if (binding.press == Press::Click)
			{
				return true;
			}

			return kind == ActionKind::Boolean && (binding.slot == Slot::A || binding.slot == Slot::B);
		}

		const char* PressComponent(Press press)
		{
			switch (press)
			{
			case Press::Click:
				return "click";
			case Press::Single:
				return "single";
			case Press::Double:
				return "double";
			case Press::Long:
				return "long";
			case Press::Held:
				return "held";
			}

			return "click";
		}

		const char* KindName(ActionKind kind)
		{
			switch (kind)
			{
			case ActionKind::Boolean:
				return "boolean";
			case ActionKind::Vector1:
				return "vector1";
			case ActionKind::Vector2:
				return "vector2";
			}

			return "boolean";
		}

		const SlotSpelling* Spell(const ControllerProfile& profile, const Binding& binding)
		{
			switch (binding.slot)
			{
			case Slot::None:
				return nullptr;
			case Slot::Stick:
				return &profile.stick;
			case Slot::StickClick:
				return &profile.stickClick;
			case Slot::Trigger:
				return &profile.trigger;
			case Slot::Grip:
				return &profile.grip;
			case Slot::A:
				return &profile.a[binding.side];
			case Slot::B:
				return &profile.b[binding.side];
			}

			return nullptr;
		}

		const char* ComponentFor(const SlotSpelling& spelling, ActionKind kind)
		{
			switch (kind)
			{
			case ActionKind::Boolean:
				return spelling.booleanComponent;
			case ActionKind::Vector1:
				return spelling.vector1Component;
			case ActionKind::Vector2:
				return spelling.vector2Component;
			}

			return "";
		}

		std::string ActionPath(const Action& action)
		{
			return SetPath(action.set) + "/in/" + action.name;
		}

		// A JSON string's contents: quotes, backslashes and control characters escaped.
		std::string EscapeJson(const std::string& text)
		{
			std::string escaped;

			for (char character : text)
			{
				unsigned char byte = static_cast<unsigned char>(character);

				if (character == '"' || character == '\\')
				{
					escaped += '\\';
					escaped += character;
				}
				else if (byte < 0x20)
				{
					char buffer[8]{};
					std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned int>(byte));
					escaped += buffer;
				}
				else
				{
					escaped += character;
				}
			}

			return escaped;
		}

		struct Source
		{
			std::string path;
			std::string mode;
			std::vector<std::pair<std::string, std::string>> inputs;
			bool timed = false;
		};

		// The component an action binds to on a slot: a press type's own input on a "button" mode source;
		// elsewhere a single press falls back to the plain click and the other press types to nothing.
		std::string ComponentForAction(const SlotSpelling& spelling, const Action& action)
		{
			std::string component = ComponentFor(spelling, action.kind);

			if (action.binding.press == Press::Click || component.empty())
			{
				return component;
			}

			if (std::string(spelling.mode) == "button")
			{
				return PressComponent(action.binding.press);
			}

			return action.binding.press == Press::Single ? component : std::string();
		}

		// One set's sources on one controller. Two actions of the set that land on the same component keep the first.
		std::vector<Source> BuildSources(const ControllerProfile& profile, int set)
		{
			std::vector<Source> sources;

			for (const Action& action : actions)
			{
				if (action.set != set)
				{
					continue;
				}

				const SlotSpelling* spelling = Spell(profile, action.binding);

				if (spelling == nullptr || spelling->path == nullptr)
				{
					continue;
				}

				std::string component = ComponentForAction(*spelling, action);

				if (component.empty())
				{
					continue;
				}

				std::string path = std::string(kHandPaths[action.binding.side]) + spelling->path;
				Source* source = nullptr;

				for (Source& existing : sources)
				{
					if (existing.path == path)
					{
						source = &existing;
					}
				}

				if (source == nullptr)
				{
					sources.push_back(Source{path, spelling->mode, {}});
					source = &sources.back();
				}

				bool taken = false;

				for (const auto& input : source->inputs)
				{
					taken = taken || input.first == component;
				}

				if (!taken)
				{
					source->inputs.emplace_back(component, ActionPath(action));
					source->timed = source->timed || component == "double" || component == "long" || component == "held";
				}
			}

			return sources;
		}

		std::string SourcesJson(const std::vector<Source>& sources)
		{
			std::string json = "\"sources\": [";

			for (size_t index = 0; index < sources.size(); index++)
			{
				const Source& source = sources[index];
				json += index == 0 ? "\n" : ",\n";
				json += "\t\t\t\t{\"path\": \"" + source.path + "\", \"mode\": \"" + source.mode + "\", \"inputs\": {";

				for (size_t inputIndex = 0; inputIndex < source.inputs.size(); inputIndex++)
				{
					json += inputIndex == 0 ? "" : ", ";
					json += "\"" + source.inputs[inputIndex].first + "\": {\"output\": \"" + source.inputs[inputIndex].second + "\"}";
				}

				json += "}";

				if (source.timed)
				{
					json += ", \"parameters\": {\"long_press_delay\": " + std::string(kLongPressDelay) + ", \"double_press_delay\": " + std::string(kDoublePressDelay) + "}";
				}

				json += "}";
			}

			json += "\n\t\t\t]";
			return json;
		}

		// One controller's default bindings: each of Lua's sets, then the module's skeletons and haptics.
		std::string BuildBindings(const ControllerProfile& profile)
		{
			std::string json = "{\n\t\"controller_type\": \"" + std::string(profile.controllerType) + "\",\n";
			json += "\t\"name\": \"Resurgence defaults\",\n";
			json += "\t\"description\": \"Written by the Resurgence Client Expansion from the game's suggested bindings (" + std::string(profile.name) + ")\",\n";
			json += "\t\"bindings\": {\n";

			for (int set = 0; set < kSetCount; set++)
			{
				json += "\t\t\"" + SetPath(set) + "\": {\n\t\t\t" + SourcesJson(BuildSources(profile, set)) + "\n\t\t},\n";
			}

			json += "\t\t\"" + std::string(kModuleSet) + "\": {\n\t\t\t\"skeleton\": [\n";
			json += "\t\t\t\t{\"output\": \"" + std::string(kSkeletonActions[0]) + "\", \"path\": \"/user/hand/left/input/skeleton/left\"},\n";
			json += "\t\t\t\t{\"output\": \"" + std::string(kSkeletonActions[1]) + "\", \"path\": \"/user/hand/right/input/skeleton/right\"}\n";
			json += "\t\t\t],\n\t\t\t\"haptics\": [\n";
			json += "\t\t\t\t{\"output\": \"" + std::string(kHapticAction) + "\", \"path\": \"/user/hand/left/output/haptic\"},\n";
			json += "\t\t\t\t{\"output\": \"" + std::string(kHapticAction) + "\", \"path\": \"/user/hand/right/output/haptic\"}\n";
			json += "\t\t\t]\n\t\t}\n\t}\n}\n";
			return json;
		}

		std::string BuildManifest()
		{
			std::string json = "{\n\t\"default_bindings\": [";
			std::string localization = "\t\t{\"language_tag\": \"en_US\"";

			for (size_t index = 0; index < sizeof(kProfiles) / sizeof(kProfiles[0]); index++)
			{
				std::string type = kProfiles[index].controllerType;
				json += index == 0 ? "\n" : ",\n";
				json += "\t\t{\"controller_type\": \"" + type + "\", \"binding_url\": \"bindings_" + type + ".json\"}";
			}

			json += "\n\t],\n\t\"actions\": [\n";

			for (const Action& action : actions)
			{
				std::string path = ActionPath(action);
				json += "\t\t{\"name\": \"" + path + "\", \"type\": \"" + KindName(action.kind) + "\", \"requirement\": \"optional\"},\n";
				localization += ",\n\t\t\t\"" + path + "\": \"" + EscapeJson(action.label.empty() ? action.name : action.label) + "\"";
			}

			json += "\t\t{\"name\": \"" + std::string(kSkeletonActions[0]) + "\", \"type\": \"skeleton\", \"skeleton\": \"/skeleton/hand/left\"},\n";
			json += "\t\t{\"name\": \"" + std::string(kSkeletonActions[1]) + "\", \"type\": \"skeleton\", \"skeleton\": \"/skeleton/hand/right\"},\n";
			json += "\t\t{\"name\": \"" + std::string(kHapticAction) + "\", \"type\": \"vibration\"}\n";
			json += "\t],\n\t\"action_sets\": [\n";

			for (int set = 0; set < kSetCount; set++)
			{
				json += "\t\t{\"name\": \"" + SetPath(set) + "\", \"usage\": \"single\"},\n";
				localization += ",\n\t\t\t\"" + SetPath(set) + "\": \"" + kSets[set].label + "\"";
			}

			json += "\t\t{\"name\": \"" + std::string(kModuleSet) + "\", \"usage\": \"single\"}\n\t],\n";

			localization += ",\n\t\t\t\"" + std::string(kModuleSet) + "\": \"" + kModuleSetLabel + "\"";

			for (int hand = 0; hand < 2; hand++)
			{
				localization += ",\n\t\t\t\"" + std::string(kSkeletonActions[hand]) + "\": \"" + kSkeletonLabels[hand] + "\"";
			}

			localization += ",\n\t\t\t\"" + std::string(kHapticAction) + "\": \"" + kHapticLabel + "\"\n\t\t}";
			json += "\t\"localization\": [\n" + localization + "\n\t]\n}\n";
			return json;
		}

		bool WriteFile(const std::filesystem::path& path, const std::string& contents, std::string& reason)
		{
			std::ofstream stream(path, std::ios::binary | std::ios::trunc);

			if (!stream)
			{
				reason = "can't write the action manifest folder next to the module";
				return false;
			}

			stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));

			if (!stream)
			{
				reason = "writing the action manifest failed";
				return false;
			}

			return true;
		}

		std::string ToUtf8(const std::wstring& text)
		{
#ifdef _WIN32
			if (text.empty())
			{
				return std::string();
			}

			int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);

			if (size <= 0)
			{
				return std::string();
			}

			std::string result(static_cast<size_t>(size), '\0');
			WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), result.data(), size, nullptr, nullptr);
			return result;
#else
			return std::string(text.begin(), text.end());
#endif
		}

		// The fixed folder next to the DLL, made if missing, or false with reason set.
		bool InputFolder(std::filesystem::path& folder, std::string& reason)
		{
			std::wstring moduleFolder = rcx::core::GetModuleFolder();

			if (moduleFolder.empty())
			{
				reason = "can't find the module's folder";
				return false;
			}

			folder = std::filesystem::path(moduleFolder) / kFolderName;
			std::error_code error;
			std::filesystem::create_directories(folder, error);

			if (error)
			{
				reason = "can't make the action manifest folder next to the module: " + error.message();
				return false;
			}

			return true;
		}

		// The manifest's absolute path, after writing it and the bindings, or "" with reason set.
		std::string WriteManifest(std::string& reason)
		{
			std::filesystem::path folder;

			if (!InputFolder(folder, reason))
			{
				return std::string();
			}

			for (const ControllerProfile& profile : kProfiles)
			{
				std::filesystem::path bindingPath = folder / (std::string("bindings_") + profile.controllerType + ".json");

				if (!WriteFile(bindingPath, BuildBindings(profile), reason))
				{
					return std::string();
				}
			}

			std::filesystem::path manifestPath = folder / "actions.json";

			if (!WriteFile(manifestPath, BuildManifest(), reason))
			{
				return std::string();
			}

			return ToUtf8(manifestPath.wstring());
		}

		std::string DescribeInputError(const char* step, ::vr::EVRInputError error)
		{
			return std::string(step) + " failed: input error " + std::to_string(static_cast<int>(error));
		}

		// The application manifest that gives the game its own app key, written next to the action manifest.
		// Its absolute path, or "" with reason set.
		std::string WriteAppManifest(std::string& reason)
		{
			std::filesystem::path folder;

			if (!InputFolder(folder, reason))
			{
				return std::string();
			}

			std::string binaryPath;
#ifdef _WIN32
			wchar_t buffer[MAX_PATH]{};
			DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);

			if (length > 0 && length < MAX_PATH)
			{
				binaryPath = ToUtf8(std::wstring(buffer, length));
			}
#endif

			std::string actionManifest = ToUtf8((folder / "actions.json").wstring());
			std::string json = "{\n\t\"source\": \"builtin\",\n\t\"applications\": [{\n";
			json += "\t\t\"app_key\": \"" + std::string(kAppKey) + "\",\n";
			json += "\t\t\"launch_type\": \"binary\",\n";
			json += "\t\t\"binary_path_windows\": \"" + EscapeJson(binaryPath) + "\",\n";
			json += "\t\t\"is_dashboard_overlay\": false,\n";
			json += "\t\t\"action_manifest_path\": \"" + EscapeJson(actionManifest) + "\",\n";
			json += "\t\t\"strings\": {\"en_us\": {\"name\": \"" + EscapeJson(kAppName) + "\", \"description\": \"Resurgence through the Resurgence Client Expansion\"}}\n";
			json += "\t}]\n}\n";

			std::filesystem::path manifestPath = folder / "resurgence.vrmanifest";

			if (!WriteFile(manifestPath, json, reason))
			{
				return std::string();
			}

			return ToUtf8(manifestPath.wstring());
		}

		// Registers the application manifest as temporary (SteamVR forgets it when it quits) and claims its
		// app key for this process. Once per process; a failure leaves Garry's Mod's own identity.
		void Identify(::vr::IVRApplications* applications)
		{
			if (identityTried)
			{
				return;
			}

			identityTried = true;

			if (applications == nullptr)
			{
				identity = "no IVRApplications";
				return;
			}

			std::string reason;
			std::string manifestText = WriteAppManifest(reason);

			if (manifestText.empty())
			{
				identity = reason;
				return;
			}

			::vr::EVRApplicationError error = applications->AddApplicationManifest(manifestText.c_str(), true);

			if (error != ::vr::VRApplicationError_None)
			{
				identity = std::string("AddApplicationManifest failed: ") + applications->GetApplicationsErrorNameFromEnum(error);
				rcx::core::Log("vr: %s; bindings stay Garry's Mod's", identity.c_str());
				return;
			}

			// The game's own process id: given 0, SteamVR files the key under process 0, not this one, which it already knows as Steam's app.
			uint32_t processId = 0;
#ifdef _WIN32
			processId = static_cast<uint32_t>(GetCurrentProcessId());
#endif

			error = applications->IdentifyApplication(processId, kAppKey);

			if (error != ::vr::VRApplicationError_None)
			{
				identity = std::string("IdentifyApplication failed: ") + applications->GetApplicationsErrorNameFromEnum(error);
				rcx::core::Log("vr: %s; bindings stay Garry's Mod's", identity.c_str());
				return;
			}

			char key[::vr::k_unMaxApplicationKeyLength]{};
			applications->GetApplicationKeyByProcessId(processId, key, sizeof(key));
			identity = std::string("identified as ") + kAppKey + " (the runtime reports " + key + ")";
			rcx::core::Log("vr: %s", identity.c_str());
		}

		void ResetHandles()
		{
			setsReady = false;
			moduleSetHandle = ::vr::k_ulInvalidActionSetHandle;

			for (::vr::VRActionSetHandle_t& handle : setHandles)
			{
				handle = ::vr::k_ulInvalidActionSetHandle;
			}

			hapticHandle = ::vr::k_ulInvalidActionHandle;

			for (int hand = 0; hand < 2; hand++)
			{
				skeletonHandles[hand] = ::vr::k_ulInvalidActionHandle;
				handSources[hand] = ::vr::k_ulInvalidInputValueHandle;
				skeletonValid[hand] = false;
			}

			for (Action& action : actions)
			{
				action.handle = ::vr::k_ulInvalidActionHandle;
				action.value = ActionValue{action.name, action.kind};
			}
		}
	}

	bool RegisterAction(const std::string& name, const std::string& kindText, const std::string& bindingText, const std::string& label, const std::string& setText, std::string& reason)
	{
		ActionKind kind = ActionKind::Boolean;
		Binding binding{};
		int set = 0;

		if (!setText.empty() && !ParseSet(setText, set))
		{
			reason = "set must be \"main\", \"pointer\", \"pipboy\" or \"weapon\"";
			return false;
		}

		if (!IsValidLabel(label))
		{
			reason = "a label is at most 48 bytes, printable, without quotes or backslashes";
			return false;
		}

		if (!IsValidName(name))
		{
			reason = "action names are 1 to 32 characters of a-z, 0-9 and _, start with a letter and don't start with rcx_";
			return false;
		}

		if (!ParseKind(kindText, kind))
		{
			reason = "kind must be \"boolean\", \"vector1\" or \"vector2\"";
			return false;
		}

		if (!ParseBinding(bindingText, binding))
		{
			reason = "binding must be left_ or right_ followed by stick, stick_click, trigger, grip, a or b, and optionally _single, _double, _long or _held";
			return false;
		}

		if (!PressFitsSlot(kind, binding))
		{
			reason = "a press type (_single, _double, _long, _held) goes only on a or b, for a boolean action";
			return false;
		}

		if (!KindFitsSlot(kind, binding.slot))
		{
			reason = "a stick takes a vector2 action; a trigger or grip a boolean or vector1; stick_click, a and b a boolean";
			return false;
		}

		for (Action& action : actions)
		{
			if (action.name != name)
			{
				continue;
			}

			bool sameBinding = action.binding.side == binding.side && action.binding.slot == binding.slot && action.binding.press == binding.press;

			if (action.kind != kind || !sameBinding || action.label != label || action.set != set)
			{
				action.kind = kind;
				action.binding = binding;
				action.label = label;
				action.set = set;
				action.value = ActionValue{name, kind};
				manifestDirty = true;
			}

			return true;
		}

		if (actions.size() >= kMaxActions)
		{
			reason = "at most " + std::to_string(kMaxActions) + " actions";
			return false;
		}

		Action action{};
		action.name = name;
		action.label = label;
		action.set = set;
		action.kind = kind;
		action.binding = binding;
		action.value = ActionValue{name, kind};
		actions.push_back(action);
		manifestDirty = true;
		return true;
	}

	bool SetActionSets(const std::vector<std::string>& sets, std::string& reason)
	{
		std::vector<int> wanted;

		for (const std::string& name : sets)
		{
			int set = 0;

			if (!ParseSet(name, set))
			{
				reason = "action sets are \"main\", \"pointer\", \"pipboy\" and \"weapon\"";
				return false;
			}

			bool repeated = false;

			for (int existing : wanted)
			{
				repeated = repeated || existing == set;
			}

			if (!repeated)
			{
				wanted.push_back(set);
			}
		}

		if (wanted.empty())
		{
			wanted.push_back(0);
		}

		if (vrInput == nullptr)
		{
			reason = "VR isn't started, or the runtime has no input interface";
			return false;
		}

		if (actions.empty())
		{
			reason = "register actions first";
			return false;
		}

		if (setsReady && !manifestDirty)
		{
			activeSets = wanted;
			return true;
		}

		ResetHandles();

		std::string manifestPath = WriteManifest(reason);

		if (manifestPath.empty())
		{
			return false;
		}

		::vr::EVRInputError error = vrInput->SetActionManifestPath(manifestPath.c_str());

		if (error != ::vr::VRInputError_None)
		{
			reason = DescribeInputError("SetActionManifestPath", error);
			return false;
		}

		for (int set = 0; set < kSetCount; set++)
		{
			error = vrInput->GetActionSetHandle(SetPath(set).c_str(), &setHandles[set]);

			if (error != ::vr::VRInputError_None)
			{
				reason = DescribeInputError(("GetActionSetHandle " + SetPath(set)).c_str(), error);
				return false;
			}
		}

		vrInput->GetActionSetHandle(kModuleSet, &moduleSetHandle);

		for (Action& action : actions)
		{
			error = vrInput->GetActionHandle(ActionPath(action).c_str(), &action.handle);

			if (error != ::vr::VRInputError_None)
			{
				reason = DescribeInputError(("GetActionHandle " + action.name).c_str(), error);
				return false;
			}
		}

		for (int hand = 0; hand < 2; hand++)
		{
			vrInput->GetActionHandle(kSkeletonActions[hand], &skeletonHandles[hand]);
			vrInput->GetInputSourceHandle(kHandPaths[hand], &handSources[hand]);
		}

		vrInput->GetActionHandle(kHapticAction, &hapticHandle);

		manifestDirty = false;
		setsReady = true;
		activeSets = wanted;
		rcx::core::Log("vr: %d actions set, manifest %s", static_cast<int>(actions.size()), manifestPath.c_str());
		return true;
	}

	void BeginSession(::vr::IVRInput* input, ::vr::IVRApplications* applications)
	{
		Identify(applications);
		vrInput = input;
		ResetHandles();
		manifestDirty = true;
	}

	void EndSession()
	{
		vrInput = nullptr;
		ResetHandles();
		manifestDirty = true;
	}

	void Update()
	{
		if (vrInput == nullptr || !setsReady)
		{
			return;
		}

		// The first set Lua named has the highest priority: where two active sets bind one input, it wins.
		::vr::VRActiveActionSet_t active[kSetCount + 1]{};
		uint32_t count = 0;

		for (size_t index = 0; index < activeSets.size() && count < static_cast<uint32_t>(kSetCount); index++)
		{
			::vr::VRActiveActionSet_t& entry = active[count];
			entry.ulActionSet = setHandles[activeSets[index]];
			entry.ulRestrictedToDevice = ::vr::k_ulInvalidInputValueHandle;
			entry.nPriority = static_cast<int32_t>(kSetCount - index);
			count++;
		}

		if (moduleSetHandle != ::vr::k_ulInvalidActionSetHandle)
		{
			::vr::VRActiveActionSet_t& entry = active[count];
			entry.ulActionSet = moduleSetHandle;
			entry.ulRestrictedToDevice = ::vr::k_ulInvalidInputValueHandle;
			entry.nPriority = 0;
			count++;
		}

		if (count == 0 || vrInput->UpdateActionState(active, sizeof(active[0]), count) != ::vr::VRInputError_None)
		{
			return;
		}

		for (Action& action : actions)
		{
			ActionValue& value = action.value;
			value = ActionValue{action.name, action.kind};

			if (action.kind == ActionKind::Boolean)
			{
				::vr::InputDigitalActionData_t data{};

				if (vrInput->GetDigitalActionData(action.handle, &data, sizeof(data), ::vr::k_ulInvalidInputValueHandle) == ::vr::VRInputError_None)
				{
					value.active = data.bActive;
					value.state = data.bActive && data.bState;
				}

				continue;
			}

			::vr::InputAnalogActionData_t data{};

			if (vrInput->GetAnalogActionData(action.handle, &data, sizeof(data), ::vr::k_ulInvalidInputValueHandle) == ::vr::VRInputError_None)
			{
				value.active = data.bActive;
				value.x = data.bActive ? data.x : 0.0f;
				value.y = data.bActive ? data.y : 0.0f;
			}
		}

		for (int hand = 0; hand < 2; hand++)
		{
			::vr::VRSkeletalSummaryData_t summary{};
			::vr::EVRInputError error = vrInput->GetSkeletalSummaryData(skeletonHandles[hand], ::vr::VRSummaryType_FromDevice, &summary);
			skeletonValid[hand] = error == ::vr::VRInputError_None;

			if (!skeletonValid[hand])
			{
				continue;
			}

			for (int finger = 0; finger < 5; finger++)
			{
				skeletons[hand].curl[finger] = summary.flFingerCurl[finger];
			}
		}
	}

	bool GetActions(std::vector<ActionValue>& values, std::string& reason)
	{
		if (vrInput == nullptr || !setsReady)
		{
			reason = "no action sets: call SetActionSets after registering actions";
			return false;
		}

		values.clear();

		for (const Action& action : actions)
		{
			values.push_back(action.value);
		}

		return true;
	}

	bool Haptic(Hand hand, float seconds, float frequency, float amplitude, std::string& reason)
	{
		if (vrInput == nullptr || !setsReady || hapticHandle == ::vr::k_ulInvalidActionHandle)
		{
			reason = "no action sets: call SetActionSets first";
			return false;
		}

		int index = static_cast<int>(hand);
		::vr::EVRInputError error = vrInput->TriggerHapticVibrationAction(hapticHandle, 0.0f, seconds, frequency, amplitude, handSources[index]);

		if (error != ::vr::VRInputError_None)
		{
			reason = DescribeInputError("TriggerHapticVibrationAction", error);
			return false;
		}

		return true;
	}

	bool GetActionOrigins(const std::string& name, uint32_t& count, std::string& reason)
	{
		count = 0;

		if (vrInput == nullptr || !setsReady)
		{
			reason = "no action sets: call SetActionSets first";
			return false;
		}

		for (const Action& action : actions)
		{
			if (action.name != name)
			{
				continue;
			}

			::vr::VRInputValueHandle_t origins[::vr::k_unMaxActionOriginCount]{};
			::vr::EVRInputError error = vrInput->GetActionOrigins(setHandles[action.set], action.handle, origins, ::vr::k_unMaxActionOriginCount);

			if (error != ::vr::VRInputError_None)
			{
				reason = DescribeInputError("GetActionOrigins", error);
				return false;
			}

			for (::vr::VRInputValueHandle_t origin : origins)
			{
				if (origin != ::vr::k_ulInvalidInputValueHandle)
				{
					count++;
				}
			}

			return true;
		}

		reason = "no action of that name";
		return false;
	}

	bool WriteFilesForTest(std::string& reason)
	{
		return !WriteManifest(reason).empty() && !WriteAppManifest(reason).empty();
	}

	std::string GetIdentity()
	{
		return identity;
	}

	const char* GetAppKey()
	{
		return kAppKey;
	}

	bool GetHandSkeleton(Hand hand, HandSkeleton& skeleton, std::string& reason)
	{
		int index = static_cast<int>(hand);

		if (vrInput == nullptr || !setsReady)
		{
			reason = "no action sets: call SetActionSets first";
			return false;
		}

		if (!skeletonValid[index])
		{
			reason = "no skeletal data for that hand";
			return false;
		}

		skeleton = skeletons[index];
		return true;
	}
}
