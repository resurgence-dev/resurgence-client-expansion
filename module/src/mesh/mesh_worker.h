#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "mesh/mesh_merge.h"

/*
	The merged-mesh build queue: one worker thread runs Merge and writes the three files into the fixed folder
	garrysmod/models/rcx_merged/ (next to the module's lua/bin), each through a temporary name and a rename, the .mdl
	last. The main thread submits jobs and polls finished ones; nothing here touches the engine.
*/
namespace rcx::mesh
{
	struct BuildResult
	{
		uint32_t id = 0;
		bool ok = false;
		std::string name;
		std::string error;
		MergeOutput stats;
		double milliseconds = 0.0;
	};

	struct QueueStats
	{
		int queued = 0;
		bool building = false;
		uint64_t done = 0;
		uint64_t failed = 0;
		uint64_t canceled = 0;
	};

	constexpr size_t kMaxQueued = 64;

	// Queues a job; false and a reason when the queue is full or the folder can't be made.
	bool Submit(MergeInput&& input, uint32_t& id, std::string& reason);
	std::vector<BuildResult> Poll();
	bool Cancel(uint32_t id);
	QueueStats GetStats();
	// Whether name's three files are already in the folder (a model built in an earlier session).
	bool Exists(const std::string& name);
	// Deletes the oldest files past maxBytes and any file that isn't one of the module's own names.
	void PruneFolder(uint64_t maxBytes);
	// Stops the worker after its current job (the module's close).
	void Shutdown();
}
