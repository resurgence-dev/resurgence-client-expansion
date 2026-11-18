#pragma once

#include <string>
#include <vector>

/*
	The Client Expansion's core: the version string, the feature registry, and logging.
	Components register what they build with and switch themselves off with a reason; Lua reads
	both through rcx.Features() and rcx.Disabled().
*/
namespace rcx::core
{
	extern const char* const kVersion;

	struct FeatureState
	{
		std::string name;
		bool built = false;
		std::string disabledReason;
	};

	// Thread safe. Lines are printed to the game console the next time Lua calls into the module.
	void Log(const char* format, ...);
	std::vector<std::string> TakeLogLines();

	void RegisterFeature(const char* name);
	void DisableFeature(const char* name, const std::string& reason);
	bool IsFeatureDisabled(const char* name, std::string* reason = nullptr);
	std::vector<FeatureState> GetFeatures();

	// The folder this DLL was loaded from, with a trailing backslash. Empty if it can't be read.
	std::wstring GetModuleFolder();

	// Keeps the DLL loaded for the rest of the process. Needed before patching anything that
	// points back into it, since GMod frees a module when its Lua state closes.
	bool PinModule();
}
