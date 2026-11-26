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
	  (0.73: on every adapter, for laptops with two GPUs)
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
		uint64_t waitTimeouts = 0;
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

	// Lua thread, after each successful WaitGetPoses. The compositor takes one submit per wait.
	void NoteWait();

	bool QueueSubmit(uint32_t frameId, const ::vr::HmdMatrix34_t& headPose, std::string& reason);

	void SetBounds(int eye, const TextureBounds& bounds);

	Stats GetStats();
}
