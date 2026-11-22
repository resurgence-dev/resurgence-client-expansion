#pragma once

#include <cstdint>
#include <string>
#include <vector>

/*
	The safety layer for components that rely on engine internals (docs/rcx-engine-hooks-design.md, The safety
	layer). Each such feature names the game DLLs it depends on; at load the module fingerprints them from their
	files (PE TimeDateStamp, SizeOfImage and a SHA-256 of .text) and keeps the feature off, with a reason in
	rcx.Disabled(), unless the fingerprints match a clearance compiled into this build. Kill switches on top:
	the launcher's policy file (lua/bin/rcx_policy.json), the crash latch (rcx_state/armed_<feature>), any
	server's Lua (off only, for the session) and the player's rcx_<feature> convar (in the core).
*/
namespace rcx::engine
{
	struct DllFingerprint
	{
		std::string name;
		uint32_t timeDateStamp = 0;
		uint32_t sizeOfImage = 0;
		std::string textHash;
		std::string error;
	};

	struct FeatureBuild
	{
		std::string feature;
		std::string key;
		bool cleared = false;
		bool latched = false;
		std::vector<DllFingerprint> dlls;
	};

	// Registers an engine feature with the core and runs its build check, the policy file and the crash latch.
	// `dlls` are file names in the game's bin/win64. Call once per feature, at module open.
	void RegisterEngineFeature(const char* feature, const std::vector<std::string>& dlls);

	std::vector<FeatureBuild> GetBuilds();
	bool IsEngineFeature(const std::string& feature);

	// The crash latch: Arm writes rcx_state/armed_<feature> before the feature first touches the engine, and
	// DisarmAll removes every file this session wrote, at the module's clean close. A file found at the next load
	// keeps the feature off until ClearLatch (the player's options) removes it.
	bool Arm(const char* feature);
	void DisarmAll();
	bool ClearLatch(const std::string& feature, std::string& reason);
}
