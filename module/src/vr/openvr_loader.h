#pragma once

#include <string>

#include "openvr.h"

/*
	openvr_api.dll is loaded by hand, from the module's own folder, the first time VR is asked for
	something. Loading rcx alone never loads it. The path is built here from the module's location
	and a fixed file name; nothing from Lua reaches it.
*/
namespace rcx::vr
{
	struct OpenVRApi
	{
		uint32_t (*InitInternal2)(::vr::EVRInitError* error, ::vr::EVRApplicationType type, const char* startupInfo) = nullptr;
		void (*ShutdownInternal)() = nullptr;
		bool (*IsHmdPresent)() = nullptr;
		bool (*IsRuntimeInstalled)() = nullptr;
		void* (*GetGenericInterface)(const char* version, ::vr::EVRInitError* error) = nullptr;
		const char* (*GetInitErrorDescription)(::vr::EVRInitError error) = nullptr;
	};

	// The loaded API, or nullptr with reason set.
	const OpenVRApi* LoadOpenVR(std::string& reason);

	bool IsOpenVRFilePresent();

	std::string DescribeInitError(::vr::EVRInitError error);
}
