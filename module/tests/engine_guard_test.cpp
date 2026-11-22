/*
	The engine safety layer's checks outside the game (src/engine/engine_guard.cpp): the build check refuses a DLL
	that isn't loaded and a feature with no clearance, clears this game's DLLs (mapped from bin/win64 without running
	them), and the policy file, a revoked build key and the crash latch each turn the feature off; a new load resets
	what the last one found. Uses rcx_policy.json and rcx_state/ next to this exe (builds/win64/tests/) and removes them.
	Exits non-zero on any failure. Pass the game's bin/win64 folder as the first argument.
*/
#include <cstdio>
#include <fstream>
#include <string>

#include <windows.h>

#include "core/core.h"
#include "engine/engine_guard.h"

namespace rcx::core
{
	const char* const kVersion = "engine_guard_test";
}

namespace
{
	int failures = 0;

	void Check(bool passed, const char* what, const std::string& detail = std::string())
	{
		std::printf("%s %s%s%s\n", passed ? "ok  " : "FAIL", what, detail.empty() ? "" : ": ", detail.c_str());

		if (!passed)
		{
			failures += 1;
		}
	}

	std::string Reason(const char* feature)
	{
		std::string reason;
		return rcx::core::IsFeatureDisabled(feature, &reason) ? reason : std::string();
	}

	rcx::engine::FeatureBuild Build(const char* feature)
	{
		for (const rcx::engine::FeatureBuild& build : rcx::engine::GetBuilds())
		{
			if (build.feature == feature)
			{
				return build;
			}
		}

		return rcx::engine::FeatureBuild{};
	}

	void Register()
	{
		rcx::engine::RegisterEngineFeature("mesh_combine", {"engine.dll", "datacache.dll", "studiorender.dll"});
	}

	void WritePolicy(const std::wstring& path, const std::string& text)
	{
		std::ofstream file(path, std::ios::binary | std::ios::trunc);
		file << text;
	}
}

int wmain(int argc, wchar_t** argv)
{
	if (argc < 2)
	{
		std::printf("usage: rcx_engine_guard_test <game bin/win64 folder>\n");
		return 2;
	}

	std::wstring bin = argv[1];
	std::wstring folder = rcx::core::GetModuleFolder();
	std::wstring policy = folder + L"rcx_policy.json";
	std::wstring latch = folder + L"rcx_state\\armed_mesh_combine";
	DeleteFileW(policy.c_str());
	DeleteFileW(latch.c_str());

	Register();
	Check(Reason("mesh_combine").find("not loaded") != std::string::npos, "off while the game's DLLs aren't loaded", Reason("mesh_combine"));
	Check(!Build("mesh_combine").cleared, "not cleared while the game's DLLs aren't loaded");

	for (const wchar_t* name : {L"engine.dll", L"datacache.dll", L"studiorender.dll"})
	{
		std::wstring path = bin + L"\\" + name;
		HMODULE module = LoadLibraryExW(path.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
		Check(module != nullptr, "mapped a game DLL without running it");
	}

	Register();
	rcx::engine::FeatureBuild build = Build("mesh_combine");
	Check(build.cleared, "cleared on this game build", build.key);
	Check(Reason("mesh_combine").empty(), "on after a new load resets the last one's reason", Reason("mesh_combine"));
	Check(build.dlls.size() == 3 && build.dlls[0].textHash.size() == 64, "three fingerprints with full hashes");

	rcx::engine::RegisterEngineFeature("guard_test_uncleared", {"engine.dll"});
	Check(Reason("guard_test_uncleared").find("wasn't cleared") != std::string::npos, "a feature with no clearance stays off", Reason("guard_test_uncleared"));

	WritePolicy(policy, "{\"disabled\": [\"other\", \"mesh_combine\"], \"revoked\": []}");
	Register();
	Check(Reason("mesh_combine").find("policy") != std::string::npos, "the policy file's disabled list turns it off", Reason("mesh_combine"));

	WritePolicy(policy, "{\"disabled\": [\"all\"]}");
	Register();
	Check(Reason("mesh_combine").find("policy") != std::string::npos, "the policy file's all turns it off");

	WritePolicy(policy, "{\"disabled\": [], \"revoked\": [\"" + build.key + "\"]}");
	Register();
	Check(Reason("mesh_combine").find("revoked") != std::string::npos, "a revoked build key turns it off", Reason("mesh_combine"));

	WritePolicy(policy, "{\"revoked\": [\"engine.dll@00000000-0000000000000000\"]}");
	Register();
	Check(Reason("mesh_combine").empty(), "another build key's revocation leaves it on", Reason("mesh_combine"));
	DeleteFileW(policy.c_str());

	Check(rcx::engine::Arm("mesh_combine"), "arms the crash latch");
	Check(GetFileAttributesW(latch.c_str()) != INVALID_FILE_ATTRIBUTES, "the latch file is written");

	std::string reason;
	Check(!rcx::engine::ClearLatch("mesh_combine", reason), "the latch can't be cleared while the feature is in use", reason);

	rcx::engine::DisarmAll();
	Check(GetFileAttributesW(latch.c_str()) == INVALID_FILE_ATTRIBUTES, "a clean close removes the latch file");

	Register();
	Check(Reason("mesh_combine").empty(), "on after a clean close");

	rcx::engine::Arm("mesh_combine");
	Register();
	Check(Reason("mesh_combine").find("clean close") != std::string::npos, "a latch left by a crash keeps it off at the next load", Reason("mesh_combine"));
	Check(Build("mesh_combine").latched, "the build reports the latch");

	rcx::engine::DisarmAll();
	WritePolicy(latch, build.key);
	Register();
	Check(!Reason("mesh_combine").empty(), "still off until the latch is cleared");
	Check(rcx::engine::ClearLatch("mesh_combine", reason), "ClearLatch removes it", reason);
	Register();
	Check(Reason("mesh_combine").empty(), "on at the load after ClearLatch", Reason("mesh_combine"));
	Check(!rcx::engine::ClearLatch("not_a_feature", reason), "ClearLatch refuses a name that isn't an engine feature");

	RemoveDirectoryW((folder + L"rcx_state").c_str());
	std::printf("%d failure(s)\n", failures);
	return failures == 0 ? 0 : 1;
}
