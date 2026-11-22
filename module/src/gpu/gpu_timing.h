#pragma once

#include <cstdint>
#include <string>

/*
	rcx.gpu: render-thread and GPU timing, measurement only (docs/rcx-engine-hooks-design.md, candidate 12).

	It watches the game's Present through the VR bridge's Direct3D 9 hooks (a public COM interface;
	no engine internals) and times each frame on the thread that presents, which is the render thread
	with mat_queue_mode 2. A frame runs from one Present's return to the next Present's call.

	- presentInterval: from one Present call to the next, the rate the game puts frames out.
	- renderWall: from a Present's return to the next Present's call: the presenting thread's work for
	  the frame plus any wait for the main thread's queued work.
	- renderBusy: the presenting thread's own CPU time over renderWall (its cycle count, converted to
	  ms with the TSC rate), so renderWall - renderBusy is roughly the time it waited.
	- presentBlock: time inside the real Present. Long when the GPU or the swap chain holds the frame.
	- gpuSpan: GPU timestamps at the frame's first and last command, read a few frames late. Includes
	  any time the GPU sat idle waiting for commands between them.
	- gpuInterval: from one frame's first GPU timestamp to the next's, the GPU's own frame period.

	Device work (queries) happens only on the presenting thread, inside the hooks. Lua reads copies
	under a mutex that the device thread holds only to append a sample.
*/
namespace rcx::gpu
{
	struct Summary
	{
		uint32_t count = 0;
		double mean = 0.0;
		double p50 = 0.0;
		double p95 = 0.0;
		double max = 0.0;
	};

	struct Window
	{
		double seconds = 0.0;
		uint64_t frames = 0;
		uint64_t gpuFrames = 0;
		uint64_t gpuDropped = 0;
		uint64_t disjoint = 0;
		bool truncated = false;
		Summary presentInterval;
		Summary presentBlock;
		Summary renderWall;
		Summary renderBusy;
		Summary gpuSpan;
		Summary gpuInterval;
	};

	struct Last
	{
		uint64_t frame = 0;
		double presentInterval = 0.0;
		double presentBlock = 0.0;
		double renderWall = 0.0;
		double renderBusy = 0.0;
		uint64_t gpuFrame = 0;
		double gpuSpan = 0.0;
		double gpuInterval = 0.0;
	};

	struct Status
	{
		bool running = false;
		bool deviceFound = false;
		bool queriesReady = false;
		bool presentOnMainThread = false;
		bool busyCalibrated = false;
		uint64_t frames = 0;
		uint64_t gpuFrames = 0;
		uint64_t gpuDropped = 0;
		uint64_t disjoint = 0;
		uint64_t resets = 0;
		uint64_t threadChanges = 0;
		double cyclesPerMs = 0.0;
		std::string queryError;
	};

	void RegisterFeature();

	// Main thread. Installs the Direct3D 9 hooks if they aren't in yet and starts timing.
	bool Start(std::string& reason);

	// Main thread. Stops timing; the queries are released at the next Present, on the device's thread.
	void Stop();

	Status GetStatus();
	Last GetLast();

	// Clears the window's samples; GetWindow summarizes everything since.
	void BeginWindow();
	Window GetWindow();
}
