#include "sys/sys_memory.h"

#include <windows.h>
#include <psapi.h>
#include <dxgi1_4.h>

#include <chrono>
#include <mutex>

/*
	Process memory from K32GetProcessMemoryInfo, the machine's from GlobalMemoryStatusEx, the graphics
	card's from IDXGIAdapter3::QueryVideoMemoryInfo (WDDM's per-process budget and usage on that adapter,
	which counts the game's Direct3D 9 allocations too). dxgi.dll is loaded by hand from System32 at the
	first call, as the VR bridge does, so loading rcx loads nothing new.
*/
namespace rcx::sys
{
	namespace
	{
		using CreateFactoryFunction = HRESULT(WINAPI*)(REFIID, void**);

		std::mutex mutex;
		bool adapterTried = false;
		IDXGIAdapter3* adapter = nullptr;
		std::string adapterReason;
		std::string adapterName;
		uint64_t adapterDedicated = 0;
		bool haveLast = false;
		uint64_t lastFaults = 0;
		std::chrono::steady_clock::time_point lastAt{};

		std::string Narrow(const wchar_t* text)
		{
			int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);

			if (size <= 1)
			{
				return std::string();
			}

			std::string result(static_cast<size_t>(size - 1), '\0');
			WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), size, nullptr, nullptr);
			return result;
		}

		void FindAdapter()
		{
			adapterTried = true;
			HMODULE module = LoadLibraryExW(L"dxgi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);

			if (module == nullptr)
			{
				adapterReason = "dxgi.dll didn't load";
				return;
			}

			auto create = reinterpret_cast<CreateFactoryFunction>(reinterpret_cast<void*>(GetProcAddress(module, "CreateDXGIFactory1")));

			if (create == nullptr)
			{
				adapterReason = "no CreateDXGIFactory1 in dxgi.dll";
				return;
			}

			IDXGIFactory1* factory = nullptr;

			if (FAILED(create(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) || factory == nullptr)
			{
				adapterReason = "CreateDXGIFactory1 failed";
				return;
			}

			IDXGIAdapter1* candidate = nullptr;

			for (UINT index = 0; factory->EnumAdapters1(index, &candidate) != DXGI_ERROR_NOT_FOUND; index++)
			{
				DXGI_ADAPTER_DESC1 description{};
				IDXGIAdapter3* version3 = nullptr;
				bool usable = SUCCEEDED(candidate->GetDesc1(&description)) && (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0;

				if (usable && description.DedicatedVideoMemory > adapterDedicated
					&& SUCCEEDED(candidate->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&version3))))
				{
					if (adapter != nullptr)
					{
						adapter->Release();
					}

					adapter = version3;
					adapterDedicated = description.DedicatedVideoMemory;
					adapterName = Narrow(description.Description);
				}

				candidate->Release();
				candidate = nullptr;
			}

			factory->Release();

			if (adapter == nullptr)
			{
				adapterReason = "no hardware adapter with DXGI 1.4";
			}
		}
	}

	bool ReadMemory(MemoryInfo& info, std::string& reason)
	{
		std::lock_guard<std::mutex> lock(mutex);
		info = MemoryInfo{};

		PROCESS_MEMORY_COUNTERS_EX counters{};
		counters.cb = sizeof(counters);

		if (!K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters)))
		{
			reason = "GetProcessMemoryInfo failed (" + std::to_string(GetLastError()) + ")";
			return false;
		}

		MEMORYSTATUSEX status{};
		status.dwLength = sizeof(status);

		if (!GlobalMemoryStatusEx(&status))
		{
			reason = "GlobalMemoryStatusEx failed (" + std::to_string(GetLastError()) + ")";
			return false;
		}

		info.workingSet = counters.WorkingSetSize;
		info.peakWorkingSet = counters.PeakWorkingSetSize;
		info.privateBytes = counters.PrivateUsage;
		info.pageFaults = counters.PageFaultCount;
		info.totalPhysical = status.ullTotalPhys;
		info.availablePhysical = status.ullAvailPhys;
		info.memoryLoad = status.dwMemoryLoad;
		info.commitLimit = status.ullTotalPageFile;
		info.commitCharge = status.ullTotalPageFile - status.ullAvailPageFile;

		auto now = std::chrono::steady_clock::now();

		if (haveLast)
		{
			double seconds = std::chrono::duration<double>(now - lastAt).count();

			if (seconds > 0.0 && info.pageFaults >= lastFaults)
			{
				info.pageFaultsPerSecond = static_cast<double>(info.pageFaults - lastFaults) / seconds;
			}
		}

		haveLast = true;
		lastFaults = info.pageFaults;
		lastAt = now;

		if (!adapterTried)
		{
			FindAdapter();
		}

		if (adapter == nullptr)
		{
			info.vramReason = adapterReason;
			return true;
		}

		DXGI_QUERY_VIDEO_MEMORY_INFO local{};
		DXGI_QUERY_VIDEO_MEMORY_INFO nonLocal{};

		if (FAILED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local))
			|| FAILED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonLocal)))
		{
			info.vramReason = "QueryVideoMemoryInfo failed";
			return true;
		}

		info.vramValid = true;
		info.adapterName = adapterName;
		info.vramDedicated = adapterDedicated;
		info.vramBudget = local.Budget;
		info.vramUsage = local.CurrentUsage;
		info.sharedBudget = nonLocal.Budget;
		info.sharedUsage = nonLocal.CurrentUsage;
		return true;
	}
}
