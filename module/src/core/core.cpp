#include "core/core.h"

#include <cstdarg>
#include <cstdio>
#include <mutex>

#include <windows.h>

namespace rcx::core
{
	namespace
	{
		constexpr size_t kMaxQueuedLogLines = 256;

		std::mutex logMutex;
		std::vector<std::string> logLines;

		std::mutex featureMutex;
		std::vector<FeatureState> features;

		FeatureState* FindFeature(const char* name)
		{
			for (FeatureState& feature : features)
			{
				if (feature.name == name)
				{
					return &feature;
				}
			}

			return nullptr;
		}
	}

	void Log(const char* format, ...)
	{
		char buffer[1024]{};
		va_list arguments;
		va_start(arguments, format);
		vsnprintf(buffer, sizeof(buffer), format, arguments);
		va_end(arguments);

		std::string line = std::string("[rcx] ") + buffer;
		OutputDebugStringA((line + "\n").c_str());

		std::lock_guard<std::mutex> lock(logMutex);

		if (logLines.size() >= kMaxQueuedLogLines)
		{
			return;
		}

		logLines.push_back(std::move(line));
	}

	std::vector<std::string> TakeLogLines()
	{
		std::lock_guard<std::mutex> lock(logMutex);
		std::vector<std::string> taken;
		taken.swap(logLines);
		return taken;
	}

	void RegisterFeature(const char* name)
	{
		std::lock_guard<std::mutex> lock(featureMutex);
		FeatureState* feature = FindFeature(name);

		if (feature == nullptr)
		{
			FeatureState added{};
			added.name = name;
			features.push_back(added);
			feature = &features.back();
		}

		feature->built = true;
	}

	void DisableFeature(const char* name, const std::string& reason)
	{
		std::lock_guard<std::mutex> lock(featureMutex);
		FeatureState* feature = FindFeature(name);

		if (feature == nullptr)
		{
			return;
		}

		if (feature->disabledReason.empty())
		{
			feature->disabledReason = reason;
		}
	}

	bool IsFeatureDisabled(const char* name, std::string* reason)
	{
		std::lock_guard<std::mutex> lock(featureMutex);
		FeatureState* feature = FindFeature(name);

		if (feature == nullptr || !feature->built)
		{
			if (reason != nullptr)
			{
				*reason = "not built into this module";
			}

			return true;
		}

		if (feature->disabledReason.empty())
		{
			return false;
		}

		if (reason != nullptr)
		{
			*reason = feature->disabledReason;
		}

		return true;
	}

	std::vector<FeatureState> GetFeatures()
	{
		std::lock_guard<std::mutex> lock(featureMutex);
		return features;
	}

	std::wstring GetModuleFolder()
	{
		HMODULE module = nullptr;
		DWORD flags = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;

		if (!GetModuleHandleExW(flags, reinterpret_cast<LPCWSTR>(&GetModuleFolder), &module))
		{
			return std::wstring();
		}

		wchar_t path[MAX_PATH]{};
		DWORD length = GetModuleFileNameW(module, path, MAX_PATH);

		if (length == 0 || length >= MAX_PATH)
		{
			return std::wstring();
		}

		std::wstring folder(path, length);
		size_t slash = folder.find_last_of(L"\\/");

		if (slash == std::wstring::npos)
		{
			return std::wstring();
		}

		return folder.substr(0, slash + 1);
	}

	bool PinModule()
	{
		static bool pinned = false;

		if (pinned)
		{
			return true;
		}

		HMODULE module = nullptr;
		DWORD flags = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN;
		pinned = GetModuleHandleExW(flags, reinterpret_cast<LPCWSTR>(&PinModule), &module) != FALSE;
		return pinned;
	}
}
