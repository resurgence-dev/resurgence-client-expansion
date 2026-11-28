#pragma once

#include <cstdint>
#include <string>

#include "openvr.h"

struct IDirect3DDevice9;

/*
	The bridge between the game's Direct3D 9 device and the compositor.

	- The device is found through vtable hooks: a throwaway device of the same class gives the
	  vtable, its Present, CreateTexture, SetRenderTarget and Reset slots are patched, and the
	  game's device shows up as `this` on its first Present. No machine code is decoded.
	- The eye texture is the first render target texture of the asked size created, or bound as
	  render target 0 (rcx 0.71: one that already existed or was made where the creation hook
	  missed it), between BeginCapture and EndCapture. Every distinct vtable a throwaway device of
	  each kind (Ex or plain, software, hardware or pure) has is patched, each hook calling its own
	  table's original. Since 0.72 throwaways are made through every loaded d3d9.dll, their swap
	  chains' Present is patched too, and the game device's own vtable is patched when it's found
	  (0.73: on every adapter, for laptops with two GPUs). Since 0.74 the d3d9.dll bodies of those
	  functions are detoured (core/detour.h, behind vr_detours), so a device whose vptr an overlay
	  moved to its own copy of the vtable is caught too; a slot is patched only where its body's
	  detour is refused.
	  on one no throwaway had. The game's texture is left as it is: at Present the bridge copies
	  it into one of two textures of its own and hands that to the compositor, so the game never
	  draws into a texture the compositor is still reading.
	- On DXVK the copy's Vulkan image goes to the compositor on DXVK's own queue, through DXVK's
	  interop interface (DXVK can't share Direct3D 9 textures with Direct3D 11). On native Direct3D 9
	  the copy is shared with a Direct3D 11 device, and an event query shows the copy finished first.
	- Submit happens at the game's Present, on whichever thread presents (the render thread with
	  mat_queue_mode 2), with the head pose the frame was rendered from. Frames are paired with poses
	  in order: each Present that follows a draw into the eye texture takes the oldest queued submit.
	- Lua never waits on the device thread except in EndSession.
*/
struct _LUID;

namespace rcx::vr::bridge
{
	struct TextureBounds
	{
		float uMin = 0.0f;
		float vMin = 0.0f;
		float uMax = 1.0f;
		float vMax = 1.0f;
	};

	struct Stats
	{
		bool hooksInstalled = false;
		bool deviceFound = false;
		bool deviceIsEx = false;
		bool presentOnOtherThread = false;
		bool eyeCaptured = false;
		uint32_t eyeWidth = 0;
		uint32_t eyeHeight = 0;
		uint32_t eyeFormat = 0;
		uint64_t presents = 0;
		uint64_t framesSubmitted = 0;
		uint64_t submitErrors = 0;
		uint64_t queuedDropped = 0;
		uint64_t presentsWaitingForDraw = 0;
		uint64_t drawsWithoutSubmit = 0;
		uint64_t framesSkipped = 0;
		// rcx 0.75: frames dropped because Lua had waited again before they could be submitted, compositor
		// AlreadySubmitted errors, Presents that arrived inside the bridge's own handling (counted, not handled).
		uint64_t staleFrames = 0;
		// rcx 0.76, the native path: frames whose copy was overwritten while the GPU still had every copy in flight.
		uint64_t framesNotReady = 0;
		uint64_t alreadySubmitted = 0;
		uint64_t nestedPresents = 0;
		// The Direct3D 11 device's adapter and the game device's, by name (the shared texture path only).
		std::string compositorAdapter;
		// rcx 0.79, the native path: Presents whose copy wasn't done within the submit wait (SetSubmitWait), so it
		// went out at a later Present; the wait limit in ms in use.
		uint64_t waitTimeouts = 0;
		double submitWaitLimitMs = 0.0;
		// rcx 0.79, frame pacing: frames submitted under a later compositor wait than the one they were drawn for
		// (SteamVR sees them as late and throttles the game), and per-second rates over the last second: the
		// game's Presents, the frames handed to SteamVR, and Lua's compositor waits.
		uint64_t lateSubmits = 0;
		double presentsPerSecond = 0.0;
		double submitsPerSecond = 0.0;
		double waitsPerSecond = 0.0;
		uint64_t resets = 0;
		double lastWaitMs = 0.0;
		double maxWaitMs = 0.0;
		double averageWaitMs = 0.0;
		double lastSubmitMs = 0.0;
		int lastCompositorError = 0;
		std::string backend;
		std::string lastError;
		// how the eye texture was captured ("creation", "binding", ""), the last EndCapture failure kind, DescribeHooks()
		std::string capturedVia;
		std::string lastCaptureFailure;
		std::string hooks;
	};

	// Main thread. Installs the hooks once per process; they stay for the process's life and do
	// nothing while no session runs. Other components (rcx.gpu) install them too.
	bool InstallHooks(std::string& reason);

	/*
		Another component's view of the game's Present and Reset, called on the presenting thread
		(the render thread with mat_queue_mode 2) with the game's own device: beforePresent just before
		the real Present, afterPresent right after it returns, onReset before a Reset. The functions
		must stay valid for the process's life (the module is pinned once the hooks are in); pass
		nullptr to stop the calls.
	*/
	struct PresentObserver
	{
		void (*beforePresent)(IDirect3DDevice9* device) = nullptr;
		void (*afterPresent)(IDirect3DDevice9* device) = nullptr;
		void (*onReset)(IDirect3DDevice9* device) = nullptr;
	};

	void SetPresentObserver(const PresentObserver* observer);

	// True once the game's device has presented through the hooks.
	bool IsDeviceFound();

	// The device's texture size limit (D3DCAPS9 MaxTextureWidth and Height), 0 until the device is found.
	void GetTextureLimit(uint32_t& width, uint32_t& height);

	// Main thread. The submit path is chosen and built on the device thread at the first submit.
	bool BeginSession(::vr::IVRSystem* system, ::vr::IVRCompositor* compositor, std::string& reason);

	// Main thread. Waits for a Present in progress, then drops everything the session owns. The
	// Direct3D 9 textures are released at the next Present, on the device's own thread.
	void EndSession();

	void BeginCapture(uint32_t width, uint32_t height);

	/*
		kind says why it failed: "refused" (Direct3D refused the creation), "other_size" (a render
		target of another size was made or captured), the two a smaller size can help; "not_seen"
		(the game's device is hooked but no creation or binding of that size was seen); "no_device"
		(the game's device never presented through a hooked vtable); "not_started". After not_seen or
		no_device the capture stays armed for the rest of the session, so a creation or binding seen
		later is still taken. reason ends with DescribeHooks().
	*/
	bool EndCapture(std::string& reason, std::string& kind);

	// What the hooks see, for failure reasons and GetStats: the hooked vtables and the modules their
	// functions live in, whether and through which the game's device presented, the texture limit
	// read, render targets bound while armed, and DescribeGraphicsLayers().
	std::string DescribeHooks();

	// Whether d3d9.dll is Windows' own (else its path), and the known overlays and graphics layers loaded.
	std::string DescribeGraphicsLayers();

	// The Direct3D 9 adapters, the one the headset wants and the game's device's, and when
	// GpuMismatchLikely() the advice to run the game on the headset's graphics card (rcx 0.73).
	std::string DescribeAdapters();

	// The game's device is on another adapter than the headset's, or wasn't found on a system with
	// more than one adapter (a laptop with two GPUs).
	bool GpuMismatchLikely();

	// The headset's and the game's adapter (-1 unknown) and how many adapters there are.
	void GetAdapters(int& headset, int& game, int& count);

	// rcx 0.74: the detours on d3d9.dll's bodies (Present, PresentEx, the swap chain's Present,
	// Reset, ResetEx, SetRenderTarget, CreateTexture). DescribeDetours: how many, chained and
	// refused, or why they're off; DetourReport: one line per body with its address, first bytes and
	// what was done.
	std::string DescribeDetours();
	std::string DetourReport();

	// The name and DXGI index of the GPU with that LUID ("an unknown GPU" when none matches).
	std::string DxgiAdapterName(const ::_LUID& luid);

	/*
		rcx 0.74: finding the game's device without any hook. Source keeps its device pointer in
		shaderapidx9.dll's data: ScanForDevice reads a module's writable sections for an object whose vtable
		has IDirect3DDevice9's 119 slots, all into executable code of loaded images, and asks each such
		object for IDirect3DDevice9. No engine offset is used, so no build clearance; it's behind
		vr_device_scan (policy file and crash latch). where says where it was found, or how many were asked.
		AdoptGameDevice takes a device found that way as the game's: its texture limit, adapter and vtable
		owner, and its own vtable hooked (each slot's body detoured, else the slot patched).
	*/
	IDirect3DDevice9* ScanForDevice(void* module, std::string& where);
	void AdoptGameDevice(IDirect3DDevice9* device, const std::string& how);

	// Lua thread, after each successful WaitGetPoses. The compositor takes one submit per wait.
	void NoteWait();

	// rcx 0.78: the render thread's watchdog tripped (each frame's hand-off to SteamVR too slow for too long), with
	// why; WaitFrame then stops VR. Frames handed to the compositor so far this process, for WaitFrame's check.
	bool Tripped(std::string& reason);
	uint64_t SubmittedFrames();

	bool QueueSubmit(uint32_t frameId, const ::vr::HmdMatrix34_t& headPose, std::string& reason);

	/*
		rcx 0.79, the native path: how long a Present waits for the GPU to finish the frame's copy before handing
		it to SteamVR in the same Present. Direct3D 11 can't wait on Direct3D 9's work on the GPU (no implicit
		ordering of the shared texture, tests/shared_sync_test.cpp), so without the wait every frame went out a
		Present late and SteamVR throttled the game. Negative: one headset frame (the default); 0: never wait
		(0.76-0.78's frame-late pipeline). Capped at 50 ms.
	*/
	void SetSubmitWait(double milliseconds);

	void SetBounds(int eye, const TextureBounds& bounds);

	Stats GetStats();
}
