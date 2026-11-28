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

		constexpr size_t kMaxSessionLogBytes = 2 * 1024 * 1024;
		std::mutex sessionLogMutex;
		FILE* sessionLog = nullptr;
		size_t sessionLogBytes = 0;
		bool sessionLogFull = false;

		void WriteSessionLine(const std::string& line)
		{
			std::lock_guard<std::mutex> lock(sessionLogMutex);

			if (sessionLog == nullptr || sessionLogFull)
			{
				return;
			}

			SYSTEMTIME now{};
			GetLocalTime(&now);
			char stamp[32]{};
			std::snprintf(stamp, sizeof(stamp), "%02u:%02u:%02u.%03u ", now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);
			std::string text = std::string(stamp) + line + "\n";

			if (sessionLogBytes + text.size() > kMaxSessionLogBytes)
			{
				std::fputs("the log is full; nothing more is written this session\n", sessionLog);
				std::fflush(sessionLog);
				sessionLogFull = true;
				return;
			}

			std::fputs(text.c_str(), sessionLog);
			std::fflush(sessionLog);
			sessionLogBytes += text.size();
		}

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
		WriteSessionLine(line);

		std::lock_guard<std::mutex> lock(logMutex);

		if (logLines.size() >= kMaxQueuedLogLines)
		{
			return;
		}

		logLines.push_back(std::move(line));
	}

	bool OpenSessionLog(const char* version, std::string& path)
	{
		std::wstring folder = GetModuleFolder();

		if (folder.empty())
		{
			return false;
		}

		folder += L"rcx_state\\";
		CreateDirectoryW(folder.c_str(), nullptr);
		std::wstring file = folder + L"vr_last.log";
		char narrow[MAX_PATH * 3]{};
		WideCharToMultiByte(CP_UTF8, 0, file.c_str(), -1, narrow, sizeof(narrow) - 1, nullptr, nullptr);
		path = narrow;

		{
			std::lock_guard<std::mutex> lock(sessionLogMutex);

			if (sessionLog != nullptr)
			{
				std::fclose(sessionLog);
			}

			sessionLog = _wfopen(file.c_str(), L"wb");
			sessionLogBytes = 0;
			sessionLogFull = false;
		}

		if (sessionLog == nullptr)
		{
			return false;
		}

		SYSTEMTIME now{};
		GetLocalTime(&now);
		char header[96]{};
		std::snprintf(header, sizeof(header), "VR session log, %04u-%02u-%02u", now.wYear, now.wMonth, now.wDay);
		WriteSessionLine(header);
		WriteSessionLine(version);
		return true;
	}

	void CloseSessionLog()
	{
		std::lock_guard<std::mutex> lock(sessionLogMutex);

		if (sessionLog != nullptr)
		{
			std::fclose(sessionLog);
			sessionLog = nullptr;
		}
	}

	void LogToFile(const char* format, ...)
	{
		char buffer[8192]{};
		va_list arguments;
		va_start(arguments, format);
		vsnprintf(buffer, sizeof(buffer), format, arguments);
		va_end(arguments);
		WriteSessionLine(buffer);
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

	void ResetFeature(const char* name)
	{
		std::lock_guard<std::mutex> lock(featureMutex);
		FeatureState* feature = FindFeature(name);

		if (feature != nullptr)
		{
			feature->disabledReason.clear();
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
