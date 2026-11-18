#include "vr/openvr_loader.h"

#include <windows.h>

#include "core/core.h"

namespace rcx::vr
{
	namespace
	{
		const wchar_t* const kOpenVRFileName = L"openvr_api.dll";

		OpenVRApi api{};
		bool loaded = false;

		template <typename Function>
		bool Resolve(HMODULE module, const char* name, Function& function)
		{
			function = reinterpret_cast<Function>(reinterpret_cast<void*>(GetProcAddress(module, name)));
			return function != nullptr;
		}
	}

	bool IsOpenVRFilePresent()
	{
		std::wstring folder = rcx::core::GetModuleFolder();

		if (folder.empty())
		{
			return false;
		}

		DWORD attributes = GetFileAttributesW((folder + kOpenVRFileName).c_str());
		return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
	}

	const OpenVRApi* LoadOpenVR(std::string& reason)
	{
		if (loaded)
		{
			return &api;
		}

		std::wstring folder = rcx::core::GetModuleFolder();

		if (folder.empty())
		{
			reason = "can't find the module's own folder";
			return nullptr;
		}

		// The DLL's own folder first for its dependencies; never the working directory.
		DWORD flags = LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32;
		HMODULE module = LoadLibraryExW((folder + kOpenVRFileName).c_str(), nullptr, flags);

		if (module == nullptr)
		{
			reason = "missing dependency: openvr_api.dll next to the module (error " + std::to_string(GetLastError()) + ")";
			return nullptr;
		}

		OpenVRApi resolved{};
		bool complete = Resolve(module, "VR_InitInternal2", resolved.InitInternal2)
			&& Resolve(module, "VR_ShutdownInternal", resolved.ShutdownInternal)
			&& Resolve(module, "VR_IsHmdPresent", resolved.IsHmdPresent)
			&& Resolve(module, "VR_IsRuntimeInstalled", resolved.IsRuntimeInstalled)
			&& Resolve(module, "VR_GetGenericInterface", resolved.GetGenericInterface)
			&& Resolve(module, "VR_GetVRInitErrorAsEnglishDescription", resolved.GetInitErrorDescription);

		if (!complete)
		{
			FreeLibrary(module);
			reason = "openvr_api.dll is missing exports: wrong or old version";
			return nullptr;
		}

		api = resolved;
		loaded = true;
		rcx::core::Log("loaded openvr_api.dll");
		return &api;
	}

	std::string DescribeInitError(::vr::EVRInitError error)
	{
		std::string text = "openvr error " + std::to_string(static_cast<int>(error));

		if (loaded && api.GetInitErrorDescription != nullptr)
		{
			const char* description = api.GetInitErrorDescription(error);

			if (description != nullptr)
			{
				text += ": ";
				text += description;
			}
		}

		return text;
	}
}
