#include "gpu/gpu_timing.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <deque>
#include <mutex>
#include <vector>

#include <intrin.h>
#include <windows.h>
#include <d3d9.h>

#include "core/core.h"
#include "vr/d3d9_bridge.h"

namespace rcx::gpu
{
	namespace
	{
		constexpr const char* kFeature = "gpu";

		// Query sets in flight. A set is read once all four queries are done, normally 2-3 frames later.
		constexpr int kQuerySets = 8;

		// Samples kept per metric in a window: about 5 minutes at 60 frames a second.
		constexpr size_t kMaxSamples = 20000;

		// The TSC rate is measured over at least this long before renderBusy is reported.
		constexpr double kCalibrationMs = 250.0;

		struct QuerySet
		{
			IDirect3DQuery9* disjoint = nullptr;
			IDirect3DQuery9* frequency = nullptr;
			IDirect3DQuery9* start = nullptr;
			IDirect3DQuery9* end = nullptr;
			bool issued = false;
			uint64_t frame = 0;
		};

		struct Samples
		{
			std::vector<double> presentInterval;
			std::vector<double> presentBlock;
			std::vector<double> renderWall;
			std::vector<double> renderBusy;
			std::vector<double> gpuSpan;
			std::vector<double> gpuInterval;
		};

		std::atomic<bool> wanted{false};
		std::atomic<bool> registeredObserver{false};
		std::atomic<DWORD> mainThread{0};

		// The presenting thread's side; touched only inside the hooks.
		QuerySet sets[kQuerySets]{};
		std::deque<int> issuedOrder;
		int openSet = -1;
		int nextSet = 0;
		bool queriesMade = false;
		bool queriesFailed = false;
		DWORD presentThread = 0;
		bool haveAfter = false;
		bool haveBefore = false;
		bool blockPending = false;
		double lastAfterMs = 0.0;
		uint64_t lastAfterCycles = 0;
		double lastBeforeMs = 0.0;
		uint64_t frameCounter = 0;
		bool havePreviousStart = false;
		uint64_t previousStartFrame = 0;
		uint64_t previousStartTicks = 0;
		bool calibrationStarted = false;
		double calibrationStartMs = 0.0;
		uint64_t calibrationStartTsc = 0;

		// Shared with Lua.
		std::mutex dataMutex;
		Samples samples;
		Last last;
		Status status;
		double windowStartMs = 0.0;
		uint64_t windowFrames = 0;
		uint64_t windowGpuFrames = 0;
		uint64_t windowGpuDropped = 0;
		uint64_t windowDisjoint = 0;
		bool windowTruncated = false;

		double NowMs()
		{
			static const double ticksPerMs = []()
			{
				LARGE_INTEGER frequency{};
				QueryPerformanceFrequency(&frequency);
				return static_cast<double>(frequency.QuadPart) / 1000.0;
			}();

			LARGE_INTEGER counter{};
			QueryPerformanceCounter(&counter);
			return static_cast<double>(counter.QuadPart) / ticksPerMs;
		}

		uint64_t ThreadCycles()
		{
			ULONG64 cycles = 0;
			QueryThreadCycleTime(GetCurrentThread(), &cycles);
			return cycles;
		}

		void ReleaseQuery(IDirect3DQuery9*& query)
		{
			if (query != nullptr)
			{
				query->Release();
				query = nullptr;
			}
		}

		void ReleaseQueries()
		{
			for (QuerySet& set : sets)
			{
				ReleaseQuery(set.disjoint);
				ReleaseQuery(set.frequency);
				ReleaseQuery(set.start);
				ReleaseQuery(set.end);
				set.issued = false;
			}

			issuedOrder.clear();
			openSet = -1;
			nextSet = 0;
			queriesMade = false;
			havePreviousStart = false;
		}

		void ResetBaseline()
		{
			haveAfter = false;
			haveBefore = false;
			blockPending = false;
		}

		bool MakeQueries(IDirect3DDevice9* device)
		{
			if (queriesMade)
			{
				return true;
			}

			if (queriesFailed)
			{
				return false;
			}

			for (QuerySet& set : sets)
			{
				HRESULT results[4] = {
					device->CreateQuery(D3DQUERYTYPE_TIMESTAMPDISJOINT, &set.disjoint),
					device->CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ, &set.frequency),
					device->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &set.start),
					device->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &set.end)
				};

				for (HRESULT result : results)
				{
					if (FAILED(result))
					{
						ReleaseQueries();
						queriesFailed = true;

						char text[96]{};
						snprintf(text, sizeof(text), "the device has no timestamp queries (0x%08lx)", static_cast<unsigned long>(result));
						rcx::core::Log("gpu: %s; only the CPU side is timed", text);

						std::lock_guard<std::mutex> lock(dataMutex);
						status.queryError = text;
						return false;
					}
				}
			}

			queriesMade = true;

			std::lock_guard<std::mutex> lock(dataMutex);
			status.queriesReady = true;
			return true;
		}

		void Append(std::vector<double>& list, double value)
		{
			if (list.size() >= kMaxSamples)
			{
				windowTruncated = true;
				return;
			}

			list.push_back(value);
		}

		// Reads every finished set, oldest first, and stops at the first one the GPU hasn't reached.
		void ReadFinished()
		{
			while (!issuedOrder.empty())
			{
				QuerySet& set = sets[issuedOrder.front()];
				BOOL disjoint = FALSE;
				UINT64 frequency = 0;
				UINT64 start = 0;
				UINT64 end = 0;

				HRESULT results[4] = {
					set.disjoint->GetData(&disjoint, sizeof(disjoint), 0),
					set.frequency->GetData(&frequency, sizeof(frequency), 0),
					set.start->GetData(&start, sizeof(start), 0),
					set.end->GetData(&end, sizeof(end), 0)
				};

				bool pending = false;
				bool failed = false;

				for (HRESULT result : results)
				{
					pending = pending || result == S_FALSE;
					failed = failed || FAILED(result);
				}

				if (pending && !failed)
				{
					return;
				}

				issuedOrder.pop_front();
				set.issued = false;

				if (failed)
				{
					havePreviousStart = false;
					continue;
				}

				std::lock_guard<std::mutex> lock(dataMutex);

				if (disjoint || frequency == 0 || end < start)
				{
					status.disjoint++;
					windowDisjoint++;
					havePreviousStart = false;
					continue;
				}

				double ticksPerMs = static_cast<double>(frequency) / 1000.0;
				double span = static_cast<double>(end - start) / ticksPerMs;
				double interval = 0.0;
				bool haveInterval = havePreviousStart && set.frame == previousStartFrame + 1 && start >= previousStartTicks;

				if (haveInterval)
				{
					interval = static_cast<double>(start - previousStartTicks) / ticksPerMs;
				}

				havePreviousStart = true;
				previousStartFrame = set.frame;
				previousStartTicks = start;

				status.gpuFrames++;
				windowGpuFrames++;
				last.gpuFrame = set.frame;
				last.gpuSpan = span;
				Append(samples.gpuSpan, span);

				if (haveInterval)
				{
					last.gpuInterval = interval;
					Append(samples.gpuInterval, interval);
				}
			}
		}

		void BeforePresent(IDirect3DDevice9* device)
		{
			static_cast<void>(device);

			if (!wanted.load() || GetCurrentThreadId() != presentThread)
			{
				return;
			}

			double now = NowMs();
			uint64_t cycles = ThreadCycles();
			uint64_t tsc = __rdtsc();

			if (openSet >= 0)
			{
				QuerySet& set = sets[openSet];
				set.end->Issue(D3DISSUE_END);
				set.frequency->Issue(D3DISSUE_END);
				set.disjoint->Issue(D3DISSUE_END);
				set.issued = true;
				set.frame = frameCounter;
				issuedOrder.push_back(openSet);
				openSet = -1;
			}

			if (!calibrationStarted)
			{
				calibrationStarted = true;
				calibrationStartMs = now;
				calibrationStartTsc = tsc;
			}

			double calibratedMs = now - calibrationStartMs;
			double cyclesPerMs = calibratedMs >= kCalibrationMs ? static_cast<double>(tsc - calibrationStartTsc) / calibratedMs : 0.0;

			if (queriesMade)
			{
				ReadFinished();
			}

			std::lock_guard<std::mutex> lock(dataMutex);
			status.cyclesPerMs = cyclesPerMs;
			status.busyCalibrated = cyclesPerMs > 0.0;
			last.frame = frameCounter;

			if (haveBefore)
			{
				last.presentInterval = now - lastBeforeMs;
				Append(samples.presentInterval, last.presentInterval);
			}

			if (haveAfter)
			{
				last.renderWall = now - lastAfterMs;
				Append(samples.renderWall, last.renderWall);

				if (cyclesPerMs > 0.0)
				{
					last.renderBusy = static_cast<double>(cycles - lastAfterCycles) / cyclesPerMs;
					Append(samples.renderBusy, last.renderBusy);
				}
			}

			status.frames++;
			windowFrames++;
			frameCounter++;
			lastBeforeMs = now;
			haveBefore = true;
			blockPending = true;
		}

		void AfterPresent(IDirect3DDevice9* device)
		{
			if (!wanted.load())
			{
				if (queriesMade)
				{
					ReleaseQueries();
				}

				ResetBaseline();
				presentThread = 0;
				return;
			}

			DWORD thread = GetCurrentThreadId();

			if (thread != presentThread)
			{
				bool changed = presentThread != 0;
				presentThread = thread;
				ResetBaseline();

				if (queriesMade)
				{
					ReleaseQueries();
				}

				std::lock_guard<std::mutex> lock(dataMutex);
				status.threadChanges += changed ? 1 : 0;
				status.presentOnMainThread = thread == mainThread.load();
				return;
			}

			double now = NowMs();

			if (blockPending)
			{
				std::lock_guard<std::mutex> lock(dataMutex);
				last.presentBlock = now - lastBeforeMs;
				Append(samples.presentBlock, last.presentBlock);
				blockPending = false;
			}

			if (MakeQueries(device))
			{
				QuerySet& set = sets[nextSet];

				if (set.issued)
				{
					// The GPU is more than kQuerySets frames behind: reuse the oldest set unread.
					issuedOrder.erase(std::remove(issuedOrder.begin(), issuedOrder.end(), nextSet), issuedOrder.end());
					set.issued = false;
					havePreviousStart = false;

					std::lock_guard<std::mutex> lock(dataMutex);
					status.gpuDropped++;
					windowGpuDropped++;
				}

				set.disjoint->Issue(D3DISSUE_BEGIN);
				set.start->Issue(D3DISSUE_END);
				openSet = nextSet;
				nextSet = (nextSet + 1) % kQuerySets;
			}

			lastAfterMs = NowMs();
			lastAfterCycles = ThreadCycles();
			haveAfter = true;
		}

		void OnReset(IDirect3DDevice9* device)
		{
			static_cast<void>(device);
			ReleaseQueries();
			ResetBaseline();
			queriesFailed = false;

			std::lock_guard<std::mutex> lock(dataMutex);
			status.resets++;
			status.queriesReady = false;
		}

		const rcx::vr::bridge::PresentObserver observer{&BeforePresent, &AfterPresent, &OnReset};

		Summary Summarize(std::vector<double> values)
		{
			Summary summary{};

			if (values.empty())
			{
				return summary;
			}

			std::sort(values.begin(), values.end());
			double total = 0.0;

			for (double value : values)
			{
				total += value;
			}

			size_t count = values.size();
			summary.count = static_cast<uint32_t>(count);
			summary.mean = total / static_cast<double>(count);
			summary.p50 = values[(count - 1) / 2];
			summary.p95 = values[std::min(count - 1, static_cast<size_t>(static_cast<double>(count - 1) * 0.95 + 0.5))];
			summary.max = values.back();
			return summary;
		}
	}

	void RegisterFeature()
	{
		rcx::core::RegisterFeature(kFeature);
	}

	bool Start(std::string& reason)
	{
		if (!rcx::vr::bridge::InstallHooks(reason))
		{
			return false;
		}

		if (!registeredObserver.exchange(true))
		{
			rcx::vr::bridge::SetPresentObserver(&observer);
		}

		mainThread = GetCurrentThreadId();
		BeginWindow();

		if (!wanted.exchange(true))
		{
			rcx::core::Log("gpu: timing started");
		}

		return true;
	}

	void Stop()
	{
		if (wanted.exchange(false))
		{
			rcx::core::Log("gpu: timing stopped");
		}
	}

	Status GetStatus()
	{
		std::lock_guard<std::mutex> lock(dataMutex);
		Status copy = status;
		copy.running = wanted.load();
		copy.deviceFound = rcx::vr::bridge::IsDeviceFound();
		return copy;
	}

	Last GetLast()
	{
		std::lock_guard<std::mutex> lock(dataMutex);
		return last;
	}

	void BeginWindow()
	{
		std::lock_guard<std::mutex> lock(dataMutex);
		samples = Samples{};
		windowStartMs = NowMs();
		windowFrames = 0;
		windowGpuFrames = 0;
		windowGpuDropped = 0;
		windowDisjoint = 0;
		windowTruncated = false;
	}

	Window GetWindow()
	{
		Samples copy;
		Window window{};

		{
			std::lock_guard<std::mutex> lock(dataMutex);
			copy = samples;
			window.seconds = (NowMs() - windowStartMs) / 1000.0;
			window.frames = windowFrames;
			window.gpuFrames = windowGpuFrames;
			window.gpuDropped = windowGpuDropped;
			window.disjoint = windowDisjoint;
			window.truncated = windowTruncated;
		}

		window.presentInterval = Summarize(std::move(copy.presentInterval));
		window.presentBlock = Summarize(std::move(copy.presentBlock));
		window.renderWall = Summarize(std::move(copy.renderWall));
		window.renderBusy = Summarize(std::move(copy.renderBusy));
		window.gpuSpan = Summarize(std::move(copy.gpuSpan));
		window.gpuInterval = Summarize(std::move(copy.gpuInterval));
		return window;
	}
}
