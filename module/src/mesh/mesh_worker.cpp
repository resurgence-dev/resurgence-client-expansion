#include "mesh/mesh_worker.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

#include <windows.h>

#include "core/core.h"

namespace rcx::mesh
{
	namespace
	{
		struct Job
		{
			uint32_t id = 0;
			MergeInput input;
		};

		std::mutex mutex;
		std::condition_variable wake;
		std::deque<Job> queue;
		std::vector<BuildResult> finished;
		std::thread worker;
		bool stopping = false;
		uint32_t nextId = 1;
		uint32_t buildingId = 0;
		std::vector<uint32_t> canceledWhileBuilding;
		QueueStats stats;

		std::filesystem::path Folder()
		{
			std::wstring module = rcx::core::GetModuleFolder();

			if (module.empty())
			{
				return std::filesystem::path();
			}

			// the module's folder is garrysmod/lua/bin/: the models folder is two up
			return std::filesystem::path(module).parent_path().parent_path().parent_path() / L"models" / L"rcx_merged";
		}

		bool ValidName(const std::string& name)
		{
			if (name.size() != 32)
			{
				return false;
			}

			return std::all_of(name.begin(), name.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
		}

		bool WriteFile(const std::filesystem::path& path, const Bytes& data)
		{
			std::filesystem::path temporary = path;
			temporary += L".tmp";

			{
				std::ofstream file(temporary, std::ios::binary | std::ios::trunc);

				if (!file)
				{
					return false;
				}

				file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));

				if (!file)
				{
					return false;
				}
			}

			return MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE;
		}

		void Run()
		{
			for (;;)
			{
				Job job;

				{
					std::unique_lock<std::mutex> lock(mutex);
					wake.wait(lock, [] { return stopping || !queue.empty(); });

					if (stopping)
					{
						return;
					}

					job = std::move(queue.front());
					queue.pop_front();
					buildingId = job.id;
					stats.building = true;
				}

				auto start = std::chrono::steady_clock::now();
				BuildResult result;
				result.id = job.id;
				result.name = job.input.name;
				result.ok = Merge(job.input, result.stats, result.error);

				if (result.ok)
				{
					std::filesystem::path folder = Folder();
					std::filesystem::path stem = folder / std::filesystem::path(job.input.name);
					std::error_code ignored;
					std::filesystem::create_directories(folder, ignored);

					// the .mdl last: the engine looks for it, and finds the other two already in place
					result.ok = WriteFile(stem.wstring() + L".vvd", result.stats.vvd) && WriteFile(stem.wstring() + L".dx90.vtx", result.stats.vtx) &&
						WriteFile(stem.wstring() + L".mdl", result.stats.mdl);

					if (!result.ok)
					{
						result.error = "the merged model's files couldn't be written";
					}
				}

				result.milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
				result.stats.mdl.clear();
				result.stats.vvd.clear();
				result.stats.vtx.clear();

				std::lock_guard<std::mutex> lock(mutex);
				stats.building = false;
				buildingId = 0;
				auto canceled = std::find(canceledWhileBuilding.begin(), canceledWhileBuilding.end(), job.id);

				if (canceled != canceledWhileBuilding.end())
				{
					canceledWhileBuilding.erase(canceled);
					continue;
				}

				(result.ok ? stats.done : stats.failed) += 1;
				finished.push_back(std::move(result));
			}
		}
	}

	bool Submit(MergeInput&& input, uint32_t& id, std::string& reason)
	{
		if (!ValidName(input.name))
		{
			reason = "the name must be 32 lowercase hex digits";
			return false;
		}

		if (Folder().empty())
		{
			reason = "the module's folder can't be read";
			return false;
		}

		std::lock_guard<std::mutex> lock(mutex);

		if (queue.size() >= kMaxQueued)
		{
			reason = "the build queue is full";
			return false;
		}

		if (!worker.joinable())
		{
			stopping = false;
			worker = std::thread(Run);
		}

		id = nextId++;
		queue.push_back(Job{id, std::move(input)});
		wake.notify_one();
		return true;
	}

	std::vector<BuildResult> Poll()
	{
		std::lock_guard<std::mutex> lock(mutex);
		std::vector<BuildResult> taken;
		taken.swap(finished);
		return taken;
	}

	bool Cancel(uint32_t id)
	{
		std::lock_guard<std::mutex> lock(mutex);

		for (auto it = queue.begin(); it != queue.end(); ++it)
		{
			if (it->id == id)
			{
				queue.erase(it);
				stats.canceled += 1;
				return true;
			}
		}

		if (buildingId == id)
		{
			canceledWhileBuilding.push_back(id);
			stats.canceled += 1;
			return true;
		}

		return false;
	}

	QueueStats GetStats()
	{
		std::lock_guard<std::mutex> lock(mutex);
		QueueStats copy = stats;
		copy.queued = static_cast<int>(queue.size());
		return copy;
	}

	bool Exists(const std::string& name)
	{
		if (!ValidName(name))
		{
			return false;
		}

		std::filesystem::path stem = Folder() / std::filesystem::path(name);
		std::error_code error;
		return std::filesystem::exists(stem.wstring() + L".mdl", error) && std::filesystem::exists(stem.wstring() + L".vvd", error) &&
			std::filesystem::exists(stem.wstring() + L".dx90.vtx", error);
	}

	void PruneFolder(uint64_t maxBytes)
	{
		std::filesystem::path folder = Folder();
		std::error_code error;

		if (folder.empty() || !std::filesystem::is_directory(folder, error))
		{
			return;
		}

		struct Entry
		{
			std::filesystem::path path;
			uint64_t size;
			std::filesystem::file_time_type time;
		};

		std::vector<Entry> ours;

		for (const auto& entry : std::filesystem::directory_iterator(folder, error))
		{
			if (!entry.is_regular_file(error))
			{
				continue;
			}

			std::string file = entry.path().filename().string();
			size_t dot = file.find('.');
			std::string stem = dot == std::string::npos ? file : file.substr(0, dot);
			std::string ext = dot == std::string::npos ? std::string() : file.substr(dot);

			if (!ValidName(stem) || (ext != ".mdl" && ext != ".vvd" && ext != ".dx90.vtx"))
			{
				std::filesystem::remove(entry.path(), error);
				continue;
			}

			ours.push_back({entry.path(), entry.file_size(error), entry.last_write_time(error)});
		}

		uint64_t total = 0;

		for (const Entry& entry : ours)
		{
			total += entry.size;
		}

		std::sort(ours.begin(), ours.end(), [](const Entry& a, const Entry& b) { return a.time < b.time; });

		for (const Entry& entry : ours)
		{
			if (total <= maxBytes)
			{
				break;
			}

			std::filesystem::remove(entry.path, error);
			total -= entry.size;
		}
	}

	void Shutdown()
	{
		{
			std::lock_guard<std::mutex> lock(mutex);
			stopping = true;
			queue.clear();
		}

		wake.notify_all();

		if (worker.joinable())
		{
			worker.join();
		}

		std::lock_guard<std::mutex> lock(mutex);
		stopping = false;
	}
}
