#include "vr/d3d9_bridge.h"

#include <atomic>
#include <cstdio>
#include <chrono>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include <windows.h>
#include <d3d9.h>
#include <d3d12.h>
#include <d3d9on12.h>
#include <d3d11.h>
#include <d3d11_4.h>
#include <dxgi.h>

#include "core/core.h"
#include "core/detour.h"
#include "engine/engine_guard.h"
#include "vr/dxvk_interop.h"

namespace rcx::vr::bridge
{
	namespace
	{
		// IDirect3DDevice9Ex vtable slots, in d3d9.h declaration order (the COM ABI, fixed).
		constexpr int kSlotReset = 16;
		constexpr int kSlotPresent = 17;
		constexpr int kSlotCreateTexture = 23;
		constexpr int kSlotSetRenderTarget = 37;
		constexpr int kSlotPresentEx = 121;
		constexpr int kSlotResetEx = 132;

		constexpr size_t kMaxQueuedSubmits = 3;
		constexpr double kWaitTimeoutMs = 100.0;
		// Three, so the native path can have two copies in flight on the GPU while a third is written.
		constexpr int kCopyCount = 3;

		using Direct3DCreate9ExFunction = HRESULT(WINAPI*)(UINT, IDirect3D9Ex**);
		using Direct3DCreate9Function = IDirect3D9*(WINAPI*)(UINT);
		using CreateDXGIFactory1Function = HRESULT(WINAPI*)(REFIID, void**);
		using D3D11CreateDeviceFunction = decltype(&D3D11CreateDevice);

		using PresentFunction = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
		using PresentExFunction = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9Ex*, const RECT*, const RECT*, HWND, const RGNDATA*, DWORD);
		using CreateTextureFunction = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DTexture9**, HANDLE*);
		using SetRenderTargetFunction = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
		using ResetFunction = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
		using ResetExFunction = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9Ex*, D3DPRESENT_PARAMETERS*, D3DDISPLAYMODEEX*);

		/*
			How the copy reaches the compositor:
			- Vulkan: the game runs on DXVK. The copy is a plain Direct3D 9 texture; its Vulkan image is
			  submitted on DXVK's own queue through DXVK's interop interface. No CPU wait.
			- Shared: native Direct3D 9. The copy is a texture shared with a Direct3D 11 device, submitted
			  as a Direct3D 11 texture after an event query shows the copy finished.
		*/
		enum class Backend
		{
			Unknown,
			Vulkan,
			Shared,
			D3D12,
			Failed
		};

		const char* BackendName(Backend backend)
		{
			switch (backend)
			{
				case Backend::Vulkan:
					return "dxvk vulkan interop";
				case Backend::Shared:
					return "d3d11 shared texture";
				case Backend::D3D12:
					return "d3d12 through d3d9on12";
				case Backend::Failed:
					return "failed";
				default:
					return "none yet";
			}
		}

		// wait: the compositor wait (NoteWait's count) the frame was rendered for. With mat_queue_mode 2 the
		// render thread presents a frame behind, so a frame is usually submitted in the next wait; one two or
		// more waits behind is dropped as stale.
		struct PendingFrame
		{
			uint32_t frameId = 0;
			uint64_t wait = 0;
			::vr::HmdMatrix34_t headPose{};
		};

		// The native path's copies (rcx 0.76): a copy is written and its event query issued at a Present, which
		// since 0.79 waits a bounded time for it; one not done by then is submitted at a later Present.
		// pending: written, not yet submitted or dropped; sequence orders the copies; frame is its pose.
		struct CopyTexture
		{
			IDirect3DTexture9* texture = nullptr;
			IDirect3DSurface9* surface = nullptr;
			HANDLE handle = nullptr;
			ID3D11Texture2D* compositorTexture = nullptr;
			dxvk::ID3D9VkInteropTexture* interopTexture = nullptr;
			IDirect3DQuery9* query = nullptr;
			// The D3D12 path: a command allocator and list for its two barriers, and the fence value after its last use.
			ID3D12CommandAllocator* allocator = nullptr;
			ID3D12GraphicsCommandList* list = nullptr;
			uint64_t fenceDone = 0;
			bool pending = false;
			uint64_t sequence = 0;
			PendingFrame frame{};
			std::chrono::steady_clock::time_point written{};
		};

		/*
			One patched device vtable and the functions it held. d3d9.dll can give devices made in
			different ways (Ex or not, pure, a proxy's wrapper) different vtables, so each throwaway
			device's vtable is patched once and each hook calls the original of the device's own table.
			Entries are filled before they're counted and never change after, so the hooks read them
			without a lock.
		*/
		struct HookedVtable
		{
			void** vtable = nullptr;
			bool ex = false;
			std::string label;
			std::string owner;
			PresentFunction present = nullptr;
			PresentExFunction presentEx = nullptr;
			CreateTextureFunction createTexture = nullptr;
			SetRenderTargetFunction setRenderTarget = nullptr;
			ResetFunction reset = nullptr;
			ResetExFunction resetEx = nullptr;
		};

		constexpr int kMaxVtables = 16;
		HookedVtable vtables[kMaxVtables];
		std::atomic<int> vtableCount{0};

		// IDirect3DSwapChain9::Present (slot 3, after IUnknown): a game can present through its swap
		// chain instead of the device. Patched per distinct swap chain vtable, like the devices'.
		constexpr int kSlotSwapChainPresent = 3;
		using SwapChainPresentFunction = HRESULT(STDMETHODCALLTYPE*)(IDirect3DSwapChain9*, const RECT*, const RECT*, HWND, const RGNDATA*, DWORD);

		struct HookedSwapChain
		{
			void** vtable = nullptr;
			SwapChainPresentFunction present = nullptr;
		};

		HookedSwapChain swapChains[kMaxVtables];
		std::atomic<int> swapChainCount{0};
		std::atomic<uint64_t> swapChainPresents{0};

		// Set while a hooked device Present runs, so a swap chain Present it calls inside isn't counted twice.
		thread_local bool insideDevicePresent = false;
		thread_local bool insideReset = false;

		// rcx 0.74: the d3d9.dll function bodies are detoured (vr_detours, crash-latched) when the
		// feature is on; a vtable slot is patched only where its body couldn't be detoured.
		bool detoursActive = false;
		std::string detoursOff;
		std::vector<std::string> detourReport;
		int detoursInstalled = 0;
		int detoursChained = 0;
		int detoursRefused = 0;

		// The module the game device's vtable lives in, once found ("d3d9.dll", an overlay, or none: a heap copy).
		std::string gameVptrOwner;

		void HookVtable(IDirect3DDevice9* device, bool ex, const std::string& label);

		// The table the game's device presented through (-1 until it's found).
		std::atomic<int> gameVtable{-1};

		// The Direct3D 9 adapters ("0: NVIDIA GeForce ..."), read at InstallHooks on the Lua thread; the
		// adapter the headset wants (IVRSystem::GetD3D9AdapterIndex) and the game's device's own
		// (GetCreationParameters), -1 until known.
		std::vector<std::string> adapterNames;
		// Each adapter's GPU (its LUID, else vendor and device ids): Direct3D 9 has an adapter per monitor
		// output, so two adapters can be one GPU.
		std::vector<std::string> adapterGpus;
		std::atomic<int> headsetAdapter{-1};
		std::atomic<int> gameAdapter{-1};
		constexpr UINT kMaxAdapters = 4;
		constexpr const char* kDetourFeature = "vr_detours";
		constexpr const char* kScanFeature = "vr_device_scan";

		// How the game's device was found: "" (not yet), "its Present" or the scan's place in shaderapidx9.dll.
		std::string deviceFoundBy;

		// The detours' latch is disarmed once this many presents have run through the hooks (NoteWait checks).
		constexpr uint64_t kCleanPresents = 600;
		std::atomic<uint64_t> presentCount{0};
		bool detoursArmed = false;

		// D3D9On12: whether the game's device runs on it, and its Direct3D 12 device's GPU (DescribeHooks fills it).
		std::string gameOn12;
		std::string scanResult;

		const HookedVtable& TableFor(const void* device)
		{
			void** vtable = *reinterpret_cast<void** const*>(device);
			int count = vtableCount.load();

			for (int index = 0; index < count; index++)
			{
				if (vtables[index].vtable == vtable)
				{
					return vtables[index];
				}
			}

			return vtables[0];
		}

		int TableIndexFor(const void* device)
		{
			void** vtable = *reinterpret_cast<void** const*>(device);
			int count = vtableCount.load();

			for (int index = 0; index < count; index++)
			{
				if (vtables[index].vtable == vtable)
				{
					return index;
				}
			}

			return -1;
		}

		bool hooksInstalled = false;
		DWORD luaThreadId = 0;
		std::atomic<const PresentObserver*> presentObserver{nullptr};

		// Shared between the hooks (device thread) and Lua; atomics only.
		std::atomic<IDirect3DDevice9*> gameDevice{nullptr};

		// The device's texture size limit (D3DCAPS9), read once the game's device is found; 0 until then.
		std::atomic<uint32_t> maxTextureWidth{0};
		std::atomic<uint32_t> maxTextureHeight{0};

		// The last render-target texture creation seen while a capture was armed, for EndCapture's reason.
		struct CreateAttempt
		{
			bool seen = false;
			uint32_t width = 0;
			uint32_t height = 0;
			uint32_t actualWidth = 0;
			uint32_t actualHeight = 0;
			uint32_t format = 0;
			long result = 0;
			int count = 0;
		};

		std::mutex attemptMutex;
		CreateAttempt lastAttempt;

		// How the eye texture was last captured ("creation" or "binding"), and render targets bound while armed.
		std::atomic<const char*> capturedVia{""};
		std::atomic<uint32_t> armedBinds{0};
		std::string lastFailureKind;
		std::atomic<IDirect3DSurface9*> eyeSurface{nullptr};
		std::atomic<uint32_t> eyeGeneration{0};
		std::atomic<bool> eyeDrawn{false};
		std::atomic<bool> sessionActive{false};
		std::atomic<bool> releaseRequested{false};
		std::atomic<bool> captureArmed{false};
		std::atomic<bool> recaptureArmed{false};
		std::atomic<uint32_t> captureWidth{0};
		std::atomic<uint32_t> captureHeight{0};
		std::atomic<bool> backendFailed{false};
		std::atomic<uint64_t> queuedDropped{0};
		std::atomic<uint64_t> presentsWaitingForDraw{0};
		std::atomic<uint64_t> waitCount{0};
		std::atomic<uint64_t> nestedPresents{0};

		// The captured eye texture. Written by the CreateTexture and Reset hooks.
		std::mutex captureMutex;
		IDirect3DTexture9* eyeTexture = nullptr;
		uint32_t eyeWidth = 0;
		uint32_t eyeHeight = 0;
		uint32_t eyeFormat = 0;

		// Lua's side: the frames waiting for a Present and the texture bounds. Never held across a device call.
		std::mutex queueMutex;
		std::deque<PendingFrame> pending;
		TextureBounds bounds[2] = {{0.0f, 0.0f, 0.5f, 1.0f}, {0.5f, 0.0f, 1.0f, 1.0f}};

		// The device thread's side. Lua takes it only in EndSession.
		// Timed, so the Lua thread never waits forever for a render thread stuck in a driver call (kLuaLockLimit).
		std::timed_mutex deviceMutex;
		constexpr std::chrono::milliseconds kLuaLockLimit{2000};
		::vr::IVRCompositor* compositor = nullptr;
		int32_t adapterIndex = -1;
		Backend backend = Backend::Unknown;
		HMODULE dxgiModule = nullptr;
		HMODULE d3d11Module = nullptr;
		ID3D11Device* compositorDevice = nullptr;
		ID3D11DeviceContext* compositorContext = nullptr;
		dxvk::ID3D9VkInteropDevice* interopDevice = nullptr;
		CopyTexture copies[kCopyCount]{};
		uint64_t copySequence = 0;

		// The D3D12 path (rcx 0.78): the game's device on D3D9On12, its Direct3D 12 device, a queue of ours on it
		// and a fence. deviceMutex held.
		IDirect3DDevice9On12* on12Device = nullptr;
		ID3D12Device* device12 = nullptr;
		ID3D12CommandQueue* queue12 = nullptr;
		ID3D12Fence* fence12 = nullptr;
		uint64_t fenceValue = 0;

		// The render thread's watchdog: a hand-off to SteamVR slower than kSlowSubmitMs for kSlowSubmitFrames
		// frames in a row stops VR (Tripped), rather than letting the game crawl into a freeze.
		constexpr double kSlowSubmitMs = 100.0;
		constexpr int kSlowSubmitFrames = 10;
		int slowSubmits = 0;
		std::atomic<bool> tripped{false};
		std::mutex tripMutex;
		std::string tripReason;
		std::atomic<uint64_t> submittedCount{0};
		uint32_t copiesGeneration = 0;
		int nextCopy = 0;
		uint64_t lastSubmittedWait = 0;

		// The native path's wait at a Present (SetSubmitWait): asked (negative for one headset frame) and the
		// headset's frame time read at BeginSession.
		constexpr double kMaxSubmitWaitMs = 50.0;
		std::atomic<double> askedSubmitWaitMs{-1.0};
		double headsetFrameMs = 1000.0 / 90.0;

		// The pacing rates' window (OnPresent, deviceMutex held).
		std::chrono::steady_clock::time_point rateWindowStart{};
		uint64_t rateWindowPresents = 0;
		uint64_t rateWindowSubmits = 0;
		uint64_t rateWindowWaits = 0;
		Stats stats{};

		std::mutex statsMutex;
		Stats publishedStats{};

		thread_local bool insideBridge = false;

		struct BridgeScope
		{
			bool previous = false;

			BridgeScope()
			{
				previous = insideBridge;
				insideBridge = true;
			}

			~BridgeScope()
			{
				insideBridge = previous;
			}
		};

		template <typename Interface>
		void SafeRelease(Interface*& pointer)
		{
			if (pointer != nullptr)
			{
				pointer->Release();
				pointer = nullptr;
			}
		}

		std::string HresultText(HRESULT result)
		{
			char text[16]{};
			snprintf(text, sizeof(text), "0x%08lx", static_cast<unsigned long>(result));
			return text;
		}

		// deviceMutex held.
		void SetError(const std::string& error)
		{
			if (stats.lastError != error)
			{
				rcx::core::Log("vr: %s", error.c_str());
			}

			stats.lastError = error;
		}

		// deviceMutex held.
		void Fail(const std::string& error)
		{
			SetError(error);
			backend = Backend::Failed;
			backendFailed = true;
		}

		// deviceMutex held.
		void PublishStats()
		{
			std::lock_guard<std::mutex> lock(statsMutex);
			publishedStats = stats;
			publishedStats.backend = BackendName(backend);
		}

		// deviceMutex held. The Direct3D 11 objects can go on any thread.
		void ReleaseCompositorObjects()
		{
			for (CopyTexture& copy : copies)
			{
				SafeRelease(copy.compositorTexture);
			}

			SafeRelease(compositorContext);
			SafeRelease(compositorDevice);
		}

		// deviceMutex held, device thread. The interop device goes with the copies, so the backend is chosen again.
		void ReleaseDeviceResources()
		{
			if (fence12 != nullptr && fence12->GetCompletedValue() < fenceValue)
			{
				HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

				if (event != nullptr && SUCCEEDED(fence12->SetEventOnCompletion(fenceValue, event)))
				{
					WaitForSingleObject(event, 500);
				}

				if (event != nullptr)
				{
					CloseHandle(event);
				}
			}

			for (CopyTexture& copy : copies)
			{
				SafeRelease(copy.list);
				SafeRelease(copy.allocator);
				copy.fenceDone = 0;
				SafeRelease(copy.compositorTexture);
				SafeRelease(copy.interopTexture);
				SafeRelease(copy.surface);
				SafeRelease(copy.texture);
				SafeRelease(copy.query);
				copy.handle = nullptr;
				copy.pending = false;
			}

			SafeRelease(interopDevice);
			SafeRelease(fence12);
			SafeRelease(queue12);
			SafeRelease(device12);
			SafeRelease(on12Device);
			fenceValue = 0;

			if (backend != Backend::Failed)
			{
				backend = Backend::Unknown;
			}
		}

		bool PatchSlot(void** vtable, int index, void* hook, void** original)
		{
			void** slot = &vtable[index];
			DWORD oldProtection = 0;

			if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtection))
			{
				return false;
			}

			*original = InterlockedExchangePointer(slot, hook);
			DWORD unused = 0;
			VirtualProtect(slot, sizeof(void*), oldProtection, &unused);
			return true;
		}

		double MillisecondsSince(std::chrono::steady_clock::time_point start)
		{
			return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
		}

		// The headset's DXGI adapter (SteamVR's GetDXGIOutputInfo): its LUID and name, false when unknown.
		bool HeadsetAdapter(LUID& luid, std::string& name)
		{
			if (adapterIndex < 0)
			{
				return false;
			}

			HMODULE dxgi = LoadLibraryExW(L"dxgi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
			auto createFactory = dxgi != nullptr ? reinterpret_cast<CreateDXGIFactory1Function>(reinterpret_cast<void*>(GetProcAddress(dxgi, "CreateDXGIFactory1"))) : nullptr;
			IDXGIFactory1* factory = nullptr;
			bool known = false;

			if (createFactory != nullptr && SUCCEEDED(createFactory(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) && factory != nullptr)
			{
				IDXGIAdapter1* adapter = nullptr;

				if (SUCCEEDED(factory->EnumAdapters1(static_cast<UINT>(adapterIndex), &adapter)) && adapter != nullptr)
				{
					DXGI_ADAPTER_DESC1 description{};

					if (SUCCEEDED(adapter->GetDesc1(&description)))
					{
						luid = description.AdapterLuid;
						char narrow[256]{};
						WideCharToMultiByte(CP_UTF8, 0, description.Description, -1, narrow, sizeof(narrow) - 1, nullptr, nullptr);
						name = narrow;
						known = true;
					}

					adapter->Release();
				}

				factory->Release();
			}

			return known;
		}

		// deviceMutex held, device thread. For a game device on D3D9On12: its Direct3D 12 device, a direct queue
		// and a fence, and the D3D12 backend; false (backend unchanged) when the device isn't on D3D9On12, false
		// with the backend failed when its GPU isn't the headset's or setup fails.
		bool ChooseD3D12(IDirect3DDevice9* device)
		{
			if (FAILED(device->QueryInterface(__uuidof(IDirect3DDevice9On12), reinterpret_cast<void**>(&on12Device))) || on12Device == nullptr)
			{
				on12Device = nullptr;
				return false;
			}

			if (FAILED(on12Device->GetD3D12Device(__uuidof(ID3D12Device), reinterpret_cast<void**>(&device12))) || device12 == nullptr)
			{
				device12 = nullptr;
				SafeRelease(on12Device);
				return false;
			}

			LUID gameLuid = device12->GetAdapterLuid();
			std::string gameName = DxgiAdapterName(gameLuid);
			LUID headsetLuid{};
			std::string headsetName = "unknown";
			bool headsetKnown = HeadsetAdapter(headsetLuid, headsetName);
			stats.compositorAdapter = "Direct3D 12 through D3D9On12 on " + gameName + "; the headset's DXGI adapter " + std::to_string(adapterIndex) + " (" + headsetName + ")";

			if (headsetKnown && (headsetLuid.LowPart != gameLuid.LowPart || headsetLuid.HighPart != gameLuid.HighPart))
			{
				Fail("the game draws on " + gameName + " (Direct3D 9 on D3D9On12) but SteamVR's headset is on " + headsetName + ", so every eye frame would cross "
					"graphics cards. Set gmod.exe (garrysmod\\bin\\win64\\gmod.exe) to High performance in Windows Settings > System > Display > Graphics, or in "
					"the NVIDIA or AMD control panel, so it runs on the headset's card, then restart the game");
				return false;
			}

			D3D12_COMMAND_QUEUE_DESC queueDescription{};
			queueDescription.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

			if (FAILED(device12->CreateCommandQueue(&queueDescription, __uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&queue12))) || queue12 == nullptr ||
				FAILED(device12->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void**>(&fence12))) || fence12 == nullptr)
			{
				Fail("can't create a Direct3D 12 queue or fence on D3D9On12's device");
				return false;
			}

			fenceValue = 0;
			backend = Backend::D3D12;
			rcx::core::Log("vr: the game runs on D3D9On12; submitting its Direct3D 12 resources (%s)", stats.compositorAdapter.c_str());
			return true;
		}

		// deviceMutex held, device thread.
		bool ChooseBackend(IDirect3DDevice9* device)
		{
			if (backend == Backend::Vulkan || backend == Backend::Shared || backend == Backend::D3D12)
			{
				return true;
			}

			if (backend == Backend::Failed)
			{
				return false;
			}

			HRESULT result = device->QueryInterface(__uuidof(dxvk::ID3D9VkInteropDevice), reinterpret_cast<void**>(&interopDevice));

			if (SUCCEEDED(result) && interopDevice != nullptr)
			{
				// Otherwise WaitGetPoses, on the Lua thread, uses DXVK's queue while DXVK's own threads do.
				compositor->SetExplicitTimingMode(::vr::VRCompositorTimingMode_Explicit_ApplicationPerformsPostPresentHandoff);
				backend = Backend::Vulkan;
				rcx::core::Log("vr: the game runs on DXVK; submitting through its Vulkan interop");
				return true;
			}

			interopDevice = nullptr;

			if (ChooseD3D12(device))
			{
				return true;
			}

			if (backend == Backend::Failed)
			{
				return false;
			}

			// As on DXVK: otherwise WaitGetPoses, on the Lua thread, does the handoff on the Direct3D 11 context while
			// this thread submits on it, and both hang in the driver (rcx 0.79, VR-42).
			compositor->SetExplicitTimingMode(::vr::VRCompositorTimingMode_Explicit_ApplicationPerformsPostPresentHandoff);
			backend = Backend::Shared;
			rcx::core::Log("vr: native Direct3D 9; submitting through a Direct3D 11 shared texture");
			return true;
		}

		std::string Narrow(const wchar_t* text)
		{
			char narrow[256]{};
			WideCharToMultiByte(CP_UTF8, 0, text, -1, narrow, sizeof(narrow) - 1, nullptr, nullptr);
			return narrow;
		}

		// When the device runs on D3D9On12, the LUID of the GPU its Direct3D 12 device is on (else false).
		bool D3D12AdapterLuid(IDirect3DDevice9* device, LUID& luid)
		{
			IDirect3DDevice9On12* on12 = nullptr;

			if (FAILED(device->QueryInterface(__uuidof(IDirect3DDevice9On12), reinterpret_cast<void**>(&on12))) || on12 == nullptr)
			{
				return false;
			}

			ID3D12Device* d3d12 = nullptr;
			bool known = SUCCEEDED(on12->GetD3D12Device(__uuidof(ID3D12Device), reinterpret_cast<void**>(&d3d12))) && d3d12 != nullptr;

			if (known)
			{
				luid = d3d12->GetAdapterLuid();
				d3d12->Release();
			}

			on12->Release();
			return known;
		}

		// The game device's adapter: its name, and its LUID when the runtime is Direct3D 9Ex (else false).
		bool GameAdapterLuid(IDirect3DDevice9* device, LUID& luid, std::string& name)
		{
			D3DDEVICE_CREATION_PARAMETERS creation{};
			IDirect3D9* direct3d = nullptr;

			if (FAILED(device->GetCreationParameters(&creation)) || FAILED(device->GetDirect3D(&direct3d)) || direct3d == nullptr)
			{
				return false;
			}

			D3DADAPTER_IDENTIFIER9 identifier{};

			if (SUCCEEDED(direct3d->GetAdapterIdentifier(creation.AdapterOrdinal, 0, &identifier)))
			{
				name = identifier.Description;
			}

			IDirect3D9Ex* direct3dEx = nullptr;
			bool known = SUCCEEDED(direct3d->QueryInterface(__uuidof(IDirect3D9Ex), reinterpret_cast<void**>(&direct3dEx))) && direct3dEx != nullptr &&
				SUCCEEDED(direct3dEx->GetAdapterLUID(creation.AdapterOrdinal, &luid));
			SafeRelease(direct3dEx);
			direct3d->Release();
			return known;
		}

		// deviceMutex held. On the headset's adapter (SteamVR's GetDXGIOutputInfo), which must be the GPU the
		// game's device draws on: a shared texture between two GPUs is slow or broken, so that's refused.
		bool EnsureCompositorDevice(IDirect3DDevice9* device)
		{
			if (compositorDevice != nullptr)
			{
				return true;
			}

			if (dxgiModule == nullptr)
			{
				dxgiModule = LoadLibraryExW(L"dxgi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
			}

			if (d3d11Module == nullptr)
			{
				d3d11Module = LoadLibraryExW(L"d3d11.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
			}

			if (dxgiModule == nullptr || d3d11Module == nullptr)
			{
				Fail("can't load dxgi.dll or d3d11.dll");
				return false;
			}

			auto createFactory = reinterpret_cast<CreateDXGIFactory1Function>(reinterpret_cast<void*>(GetProcAddress(dxgiModule, "CreateDXGIFactory1")));
			auto createDevice = reinterpret_cast<D3D11CreateDeviceFunction>(reinterpret_cast<void*>(GetProcAddress(d3d11Module, "D3D11CreateDevice")));

			if (createFactory == nullptr || createDevice == nullptr)
			{
				Fail("dxgi.dll or d3d11.dll is missing exports");
				return false;
			}

			IDXGIFactory1* factory = nullptr;
			IDXGIAdapter1* adapter = nullptr;

			if (SUCCEEDED(createFactory(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) && factory != nullptr && adapterIndex >= 0)
			{
				if (FAILED(factory->EnumAdapters1(static_cast<UINT>(adapterIndex), &adapter)))
				{
					adapter = nullptr;
				}
			}

			LUID gameLuid{};
			std::string gameName = "unknown";
			bool gameKnown = GameAdapterLuid(device, gameLuid, gameName);
			LUID on12Luid{};
			bool on12 = D3D12AdapterLuid(device, on12Luid);

			// On D3D9On12 the GPU that draws is its Direct3D 12 device's, whatever Direct3D 9's adapter says.
			if (on12)
			{
				gameLuid = on12Luid;
				gameKnown = true;
			}
			std::string headsetName = "unknown";
			bool sameGpu = true;
			int gameDxgiIndex = -1;

			if (adapter != nullptr)
			{
				DXGI_ADAPTER_DESC1 description{};

				if (SUCCEEDED(adapter->GetDesc1(&description)))
				{
					headsetName = Narrow(description.Description);
					sameGpu = !gameKnown || (description.AdapterLuid.LowPart == gameLuid.LowPart && description.AdapterLuid.HighPart == gameLuid.HighPart);
				}
			}

			if (gameKnown && factory != nullptr)
			{
				IDXGIAdapter1* candidate = nullptr;

				for (UINT index = 0; gameDxgiIndex < 0 && SUCCEEDED(factory->EnumAdapters1(index, &candidate)); index++)
				{
					DXGI_ADAPTER_DESC1 description{};

					if (SUCCEEDED(candidate->GetDesc1(&description)) && description.AdapterLuid.LowPart == gameLuid.LowPart && description.AdapterLuid.HighPart == gameLuid.HighPart)
					{
						gameDxgiIndex = static_cast<int>(index);

						if (on12)
						{
							gameName = Narrow(description.Description) + " (through D3D9On12's Direct3D 12 device)";
						}
					}

					SafeRelease(candidate);
				}
			}

			stats.compositorAdapter = "the headset's DXGI adapter " + std::to_string(adapterIndex) + " (" + headsetName + "); the game's Direct3D 9 device on " + gameName +
				(gameKnown ? " (DXGI adapter " + std::to_string(gameDxgiIndex) + ")" : " (its LUID unknown: not a Direct3D 9Ex runtime)");

			if (!sameGpu)
			{
				SafeRelease(adapter);
				SafeRelease(factory);
				Fail("the game draws on " + gameName + " but SteamVR's headset is on " + headsetName + ", so every eye frame would cross graphics cards. Set gmod.exe "
					"(garrysmod\\bin\\win64\\gmod.exe) to High performance in Windows Settings > System > Display > Graphics, or in the NVIDIA or AMD control panel, "
					"so it runs on the headset's card, then restart the game");
				return false;
			}

			D3D_DRIVER_TYPE driverType = adapter != nullptr ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE;
			HRESULT result = createDevice(adapter, driverType, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &compositorDevice, nullptr, &compositorContext);
			SafeRelease(adapter);
			SafeRelease(factory);

			if (FAILED(result) || compositorDevice == nullptr)
			{
				compositorDevice = nullptr;
				compositorContext = nullptr;
				Fail("can't create the Direct3D 11 device (" + HresultText(result) + ")");
				return false;
			}

			// SteamVR uses the context from whichever thread calls it, so the context locks itself.
			ID3D11Multithread* multithread = nullptr;

			if (SUCCEEDED(compositorContext->QueryInterface(__uuidof(ID3D11Multithread), reinterpret_cast<void**>(&multithread))) && multithread != nullptr)
			{
				multithread->SetMultithreadProtected(TRUE);
				multithread->Release();
			}

			rcx::core::Log("vr: Direct3D 11 device ready on %s", stats.compositorAdapter.c_str());
			return true;
		}

		// Native Direct3D 9Ex exports a shared texture that Direct3D 11 opens; the other direction is the fallback.
		bool CreateSharedCopy(IDirect3DDevice9* device, UINT width, UINT height, CopyTexture& copy)
		{
			HANDLE handle = nullptr;
			HRESULT result = device->CreateTexture(width, height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &copy.texture, &handle);

			if (SUCCEEDED(result) && copy.texture != nullptr && handle != nullptr)
			{
				result = compositorDevice->OpenSharedResource(handle, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&copy.compositorTexture));

				if (SUCCEEDED(result) && copy.compositorTexture != nullptr)
				{
					copy.handle = handle;
					return true;
				}
			}

			copy.compositorTexture = nullptr;
			SafeRelease(copy.texture);

			D3D11_TEXTURE2D_DESC description{};
			description.Width = width;
			description.Height = height;
			description.MipLevels = 1;
			description.ArraySize = 1;
			description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
			description.SampleDesc.Count = 1;
			description.Usage = D3D11_USAGE_DEFAULT;
			description.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
			description.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

			result = compositorDevice->CreateTexture2D(&description, nullptr, &copy.compositorTexture);

			if (FAILED(result) || copy.compositorTexture == nullptr)
			{
				copy.compositorTexture = nullptr;
				return false;
			}

			IDXGIResource* resource = nullptr;
			handle = nullptr;
			bool haveHandle = SUCCEEDED(copy.compositorTexture->QueryInterface(__uuidof(IDXGIResource), reinterpret_cast<void**>(&resource))) && resource != nullptr
				&& SUCCEEDED(resource->GetSharedHandle(&handle)) && handle != nullptr;
			SafeRelease(resource);

			if (!haveHandle)
			{
				SafeRelease(copy.compositorTexture);
				return false;
			}

			result = device->CreateTexture(width, height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &copy.texture, &handle);

			if (FAILED(result) || copy.texture == nullptr)
			{
				copy.texture = nullptr;
				SafeRelease(copy.compositorTexture);
				return false;
			}

			copy.handle = handle;
			return true;
		}

		// deviceMutex held, device thread.
		bool EnsureCopies(IDirect3DDevice9* device)
		{
			IDirect3DSurface9* source = eyeSurface.load();

			if (source == nullptr)
			{
				SetError("no eye texture captured");
				return false;
			}

			D3DSURFACE_DESC sourceDescription{};

			if (FAILED(source->GetDesc(&sourceDescription)))
			{
				SetError("can't read the eye texture's description");
				return false;
			}

			uint32_t generation = eyeGeneration.load();

			if (copies[0].texture != nullptr && copiesGeneration != generation)
			{
				ReleaseDeviceResources();
			}

			copiesGeneration = generation;

			if (!ChooseBackend(device))
			{
				return false;
			}

			if (backend == Backend::Shared)
			{
				if (!EnsureCompositorDevice(device))
				{
					return false;
				}

				for (CopyTexture& copy : copies)
				{
					if (copy.query == nullptr && FAILED(device->CreateQuery(D3DQUERYTYPE_EVENT, &copy.query)))
					{
						copy.query = nullptr;
						Fail("the device can't create an event query");
						return false;
					}
				}
			}

			for (CopyTexture& copy : copies)
			{
				if (copy.texture != nullptr && (backend != Backend::Shared || copy.compositorTexture != nullptr))
				{
					continue;
				}

				if (backend == Backend::Shared && copy.texture != nullptr)
				{
					HRESULT result = compositorDevice->OpenSharedResource(copy.handle, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&copy.compositorTexture));

					if (FAILED(result) || copy.compositorTexture == nullptr)
					{
						copy.compositorTexture = nullptr;
						Fail("Direct3D 11 can't open the shared texture again (" + HresultText(result) + ")");
						return false;
					}

					continue;
				}

				if (backend == Backend::Vulkan)
				{
					HRESULT result = device->CreateTexture(sourceDescription.Width, sourceDescription.Height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
						D3DPOOL_DEFAULT, &copy.texture, nullptr);

					if (FAILED(result) || copy.texture == nullptr)
					{
						copy.texture = nullptr;
						Fail("can't create the copy texture (" + HresultText(result) + ")");
						return false;
					}

					result = copy.texture->QueryInterface(__uuidof(dxvk::ID3D9VkInteropTexture), reinterpret_cast<void**>(&copy.interopTexture));

					if (FAILED(result) || copy.interopTexture == nullptr)
					{
						copy.interopTexture = nullptr;
						Fail("the copy texture has no DXVK interop interface (" + HresultText(result) + ")");
						return false;
					}
				}
				else if (backend == Backend::D3D12)
				{
					HRESULT result = device->CreateTexture(sourceDescription.Width, sourceDescription.Height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
						D3DPOOL_DEFAULT, &copy.texture, nullptr);

					if (FAILED(result) || copy.texture == nullptr)
					{
						copy.texture = nullptr;
						Fail("can't create the copy texture (" + HresultText(result) + ")");
						return false;
					}

					bool listed = SUCCEEDED(device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), reinterpret_cast<void**>(&copy.allocator)))
						&& copy.allocator != nullptr && SUCCEEDED(device12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, copy.allocator, nullptr,
						__uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void**>(&copy.list))) && copy.list != nullptr && SUCCEEDED(copy.list->Close());

					if (!listed)
					{
						Fail("can't create the Direct3D 12 command list for a copy");
						return false;
					}
				}
				else if (!CreateSharedCopy(device, sourceDescription.Width, sourceDescription.Height, copy))
				{
					Fail("can't share a texture between the game's Direct3D 9 device and Direct3D 11");
					return false;
				}

				if (FAILED(copy.texture->GetSurfaceLevel(0, &copy.surface)) || copy.surface == nullptr)
				{
					copy.surface = nullptr;
					Fail("can't get the copy texture's surface");
					return false;
				}
			}

			return true;
		}

		bool SubmitEyes(const ::vr::Texture_t& texture, ::vr::EVRSubmitFlags flags)
		{
			TextureBounds eyeBounds[2]{};

			{
				std::lock_guard<std::mutex> lock(queueMutex);
				eyeBounds[0] = bounds[0];
				eyeBounds[1] = bounds[1];
			}

			for (int eye = 0; eye < 2; eye++)
			{
				::vr::VRTextureBounds_t submitBounds{};
				submitBounds.uMin = eyeBounds[eye].uMin;
				submitBounds.vMin = eyeBounds[eye].vMin;
				submitBounds.uMax = eyeBounds[eye].uMax;
				submitBounds.vMax = eyeBounds[eye].vMax;

				::vr::EVRCompositorError error = compositor->Submit(static_cast<::vr::EVREye>(eye), &texture, &submitBounds, flags);

				if (error != ::vr::VRCompositorError_None)
				{
					stats.submitErrors++;
					stats.alreadySubmitted += error == ::vr::VRCompositorError_AlreadySubmitted ? 1 : 0;
					stats.lastCompositorError = static_cast<int>(error);
					SetError("compositor submit error " + std::to_string(static_cast<int>(error)));
					return false;
				}
			}

			return true;
		}

		// deviceMutex held, device thread. Takes the current compositor wait for this submit, or false when a
		// submit already took it. A frame drawn for an earlier wait than the one it takes counts as late.
		bool ClaimWait(const PendingFrame& frame)
		{
			uint64_t waits = waitCount.load();

			if (waits == lastSubmittedWait)
			{
				stats.framesSkipped++;
				return false;
			}

			lastSubmittedWait = waits;
			stats.lateSubmits += frame.wait < waits ? 1 : 0;
			return true;
		}

		// deviceMutex held, device thread.
		void SubmitVulkan(CopyTexture& copy, const PendingFrame& frame)
		{
			dxvk::VkImage image = nullptr;
			dxvk::VkImageLayout layout = dxvk::VK_IMAGE_LAYOUT_MAX_ENUM;

			if (interopDevice == nullptr || copy.interopTexture == nullptr || copy.surface == nullptr)
			{
				stats.submitErrors++;
				SetError("the DXVK interop objects are gone");
				return;
			}

			if (FAILED(copy.interopTexture->GetVulkanImageInfo(&image, &layout, nullptr)) || image == nullptr)
			{
				stats.submitErrors++;
				SetError("DXVK gave no Vulkan image for the copy");
				return;
			}

			::vr::VRVulkanTextureData_t vulkanData{};
			uint32_t queueIndex = 0;
			interopDevice->GetVulkanHandles(&vulkanData.m_pInstance, &vulkanData.m_pPhysicalDevice, &vulkanData.m_pDevice);
			interopDevice->GetSubmissionQueue(&vulkanData.m_pQueue, &queueIndex, &vulkanData.m_nQueueFamilyIndex);
			vulkanData.m_nImage = reinterpret_cast<uint64_t>(image);

			D3DSURFACE_DESC description{};
			copy.surface->GetDesc(&description);
			vulkanData.m_nWidth = description.Width;
			vulkanData.m_nHeight = description.Height;
			vulkanData.m_nFormat = dxvk::kFormatB8G8R8A8Unorm;
			vulkanData.m_nSampleCount = 1;

			dxvk::VkImageSubresourceRange range{};
			range.aspectMask = dxvk::kImageAspectColor;
			range.levelCount = 1;
			range.layerCount = 1;

			::vr::VRTextureWithPose_t texture{};
			texture.handle = &vulkanData;
			texture.eType = ::vr::TextureType_Vulkan;
			texture.eColorSpace = ::vr::ColorSpace_Gamma;
			texture.mDeviceToAbsoluteTracking = frame.headPose;

			// Claimed before the layout change, so a refused claim leaves the image as it was.
			if (!ClaimWait(frame))
			{
				return;
			}

			auto flushStart = std::chrono::steady_clock::now();
			interopDevice->TransitionTextureLayout(copy.interopTexture, &range, layout, dxvk::VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
			interopDevice->FlushRenderingCommands();
			stats.lastWaitMs = MillisecondsSince(flushStart);
			stats.maxWaitMs = stats.lastWaitMs > stats.maxWaitMs ? stats.lastWaitMs : stats.maxWaitMs;
			stats.averageWaitMs = stats.framesSubmitted == 0 ? stats.lastWaitMs : stats.averageWaitMs * 0.95 + stats.lastWaitMs * 0.05;

			auto submitStart = std::chrono::steady_clock::now();
			interopDevice->LockSubmissionQueue();
			bool submitted = SubmitEyes(texture, ::vr::Submit_TextureWithPose);

			if (submitted)
			{
				compositor->PostPresentHandoff();
				compositor->SubmitExplicitTimingData();
			}

			interopDevice->ReleaseSubmissionQueue();
			stats.lastSubmitMs = MillisecondsSince(submitStart);

			interopDevice->TransitionTextureLayout(copy.interopTexture, &range, dxvk::VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, layout);

			if (submitted)
			{
				stats.framesSubmitted++;
			}
		}

		// deviceMutex held, device thread. Submits the newest copy the GPU has finished (older ones it
		// supersedes are dropped), never waiting: a copy not done yet stays for the next Present.
		void SubmitFinishedShared()
		{
			int best = -1;

			for (int index = 0; index < kCopyCount; index++)
			{
				CopyTexture& copy = copies[index];

				if (!copy.pending || copy.query == nullptr)
				{
					continue;
				}

				HRESULT result = copy.query->GetData(nullptr, 0, D3DGETDATA_FLUSH);

				if (result == S_FALSE)
				{
					continue;
				}

				if (result != S_OK)
				{
					// A lost device or a failed query: the copy can't be trusted.
					copy.pending = false;
					stats.submitErrors++;
					continue;
				}

				if (best < 0 || copy.sequence > copies[best].sequence)
				{
					best = index;
				}
			}

			if (best < 0)
			{
				return;
			}

			for (CopyTexture& copy : copies)
			{
				if (copy.pending && copy.sequence < copies[best].sequence)
				{
					copy.pending = false;
					stats.staleFrames++;
				}
			}

			CopyTexture& copy = copies[best];
			copy.pending = false;
			stats.lastWaitMs = MillisecondsSince(copy.written);
			stats.maxWaitMs = stats.lastWaitMs > stats.maxWaitMs ? stats.lastWaitMs : stats.maxWaitMs;
			stats.averageWaitMs = stats.framesSubmitted == 0 ? stats.lastWaitMs : stats.averageWaitMs * 0.95 + stats.lastWaitMs * 0.05;

			if (copy.frame.wait + 1 < waitCount.load())
			{
				stats.staleFrames++;
				return;
			}

			if (!ClaimWait(copy.frame))
			{
				return;
			}

			::vr::VRTextureWithPose_t texture{};
			texture.handle = copy.compositorTexture;
			texture.eType = ::vr::TextureType_DirectX;
			texture.eColorSpace = ::vr::ColorSpace_Gamma;
			texture.mDeviceToAbsoluteTracking = copy.frame.headPose;

			auto submitStart = std::chrono::steady_clock::now();

			if (SubmitEyes(texture, ::vr::Submit_TextureWithPose))
			{
				compositor->PostPresentHandoff();
				compositor->SubmitExplicitTimingData();
				stats.framesSubmitted++;
			}

			stats.lastSubmitMs = MillisecondsSince(submitStart);
		}

		// The native path's wait at a Present now: one headset frame unless SetSubmitWait asked otherwise.
		double SubmitWaitLimitMs()
		{
			double asked = askedSubmitWaitMs.load();
			double limit = asked < 0.0 ? headsetFrameMs : asked;
			return limit > kMaxSubmitWaitMs ? kMaxSubmitWaitMs : limit;
		}

		/*
			deviceMutex held, device thread. The native path at a Present: copy this frame into a free copy and
			mark it with an event query, wait up to SubmitWaitLimitMs for the GPU to finish it (Direct3D 11 can't
			wait for Direct3D 9's work itself), then submit the newest finished copy. A copy not done in time
			(waitTimeouts) goes out at a later Present, a frame late. With every copy still in flight the oldest is
			overwritten (framesNotReady). lastWaitMs is how long the copy took to be seen finished.
		*/
		void PipelineShared(IDirect3DDevice9* device, const PendingFrame& frame)
		{
			int slot = -1;

			for (int index = 0; index < kCopyCount && slot < 0; index++)
			{
				if (!copies[index].pending)
				{
					slot = index;
				}
			}

			if (slot < 0)
			{
				slot = 0;

				for (int index = 1; index < kCopyCount; index++)
				{
					slot = copies[index].sequence < copies[slot].sequence ? index : slot;
				}

				copies[slot].pending = false;
				stats.framesNotReady++;
			}

			CopyTexture& copy = copies[slot];
			HRESULT copyResult = device->StretchRect(eyeSurface.load(), nullptr, copy.surface, nullptr, D3DTEXF_NONE);

			if (FAILED(copyResult))
			{
				stats.submitErrors++;
				SetError("can't copy the eye texture (" + HresultText(copyResult) + ")");
				return;
			}

			copy.query->Issue(D3DISSUE_END);
			copy.frame = frame;
			copy.sequence = ++copySequence;
			copy.written = std::chrono::steady_clock::now();
			copy.pending = true;

			double limit = SubmitWaitLimitMs();
			stats.submitWaitLimitMs = limit;

			if (limit > 0.0)
			{
				HRESULT state = copy.query->GetData(nullptr, 0, D3DGETDATA_FLUSH);

				while (state == S_FALSE && MillisecondsSince(copy.written) < limit)
				{
					SwitchToThread();
					state = copy.query->GetData(nullptr, 0, D3DGETDATA_FLUSH);
				}

				stats.waitTimeouts += state == S_FALSE ? 1 : 0;
			}

			SubmitFinishedShared();
		}

		// Records one transition of resource and runs it on queue12 (the copy's list and allocator, reset by the caller).
		bool RunBarrier(CopyTexture& copy, ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
		{
			if (FAILED(copy.list->Reset(copy.allocator, nullptr)))
			{
				return false;
			}

			D3D12_RESOURCE_BARRIER barrier{};
			barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			barrier.Transition.pResource = resource;
			barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			barrier.Transition.StateBefore = before;
			barrier.Transition.StateAfter = after;
			copy.list->ResourceBarrier(1, &barrier);

			if (FAILED(copy.list->Close()))
			{
				return false;
			}

			ID3D12CommandList* lists[] = {copy.list};
			queue12->ExecuteCommandLists(1, lists);
			return true;
		}

		/*
			deviceMutex held, device thread. The D3D12 path: copy the eye into a copy whose last use the GPU has
			finished (else skip, framesNotReady), take the copy's Direct3D 12 resource from D3D9On12 against our
			queue (D3D9On12 makes the queue wait for its own work on it; it comes back in COMMON), make it a shader
			resource, submit it with that queue, make it COMMON again, signal the fence and give it back with that
			fence value. The CPU never waits for the GPU.
		*/
		void SubmitD3D12(IDirect3DDevice9* device, const PendingFrame& frame)
		{
			uint64_t waits = waitCount.load();

			if (frame.wait + 1 < waits)
			{
				stats.staleFrames++;
				return;
			}

			if (waits == lastSubmittedWait)
			{
				stats.framesSkipped++;
				return;
			}

			uint64_t completed = fence12->GetCompletedValue();
			int slot = -1;

			for (int step = 0; step < kCopyCount && slot < 0; step++)
			{
				int index = (nextCopy + step) % kCopyCount;
				slot = copies[index].fenceDone <= completed ? index : -1;
			}

			if (slot < 0)
			{
				stats.framesNotReady++;
				return;
			}

			nextCopy = (slot + 1) % kCopyCount;
			CopyTexture& copy = copies[slot];
			HRESULT result = device->StretchRect(eyeSurface.load(), nullptr, copy.surface, nullptr, D3DTEXF_NONE);

			if (FAILED(result))
			{
				stats.submitErrors++;
				SetError("can't copy the eye texture (" + HresultText(result) + ")");
				return;
			}

			ID3D12Resource* resource = nullptr;
			result = on12Device->UnwrapUnderlyingResource(copy.texture, queue12, __uuidof(ID3D12Resource), reinterpret_cast<void**>(&resource));

			if (FAILED(result) || resource == nullptr)
			{
				stats.submitErrors++;
				SetError("D3D9On12 wouldn't give the copy's Direct3D 12 resource (" + HresultText(result) + ")");
				return;
			}

			bool ready = SUCCEEDED(copy.allocator->Reset()) && RunBarrier(copy, resource, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

			if (ready && ClaimWait(frame))
			{
				::vr::D3D12TextureData_t data{};
				data.m_pResource = resource;
				data.m_pCommandQueue = queue12;
				data.m_nNodeMask = 0;

				::vr::VRTextureWithPose_t texture{};
				texture.handle = &data;
				texture.eType = ::vr::TextureType_DirectX12;
				texture.eColorSpace = ::vr::ColorSpace_Gamma;
				texture.mDeviceToAbsoluteTracking = frame.headPose;

				auto submitStart = std::chrono::steady_clock::now();

				if (SubmitEyes(texture, ::vr::Submit_TextureWithPose))
				{
					stats.framesSubmitted++;
				}

				stats.lastSubmitMs = MillisecondsSince(submitStart);
			}
			else if (!ready)
			{
				stats.submitErrors++;
				SetError("can't record the Direct3D 12 barrier for the copy");
			}

			if (ready)
			{
				RunBarrier(copy, resource, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
			}

			fenceValue++;
			queue12->Signal(fence12, fenceValue);
			copy.fenceDone = fenceValue;
			ID3D12Fence* fences[] = {fence12};
			UINT64 values[] = {fenceValue};
			on12Device->ReturnUnderlyingResource(copy.texture, 1, values, fences);
			resource->Release();
		}

		// deviceMutex held, device thread.
		void SubmitFrame(IDirect3DDevice9* device, const PendingFrame& frame)
		{
			if (compositor == nullptr || !EnsureCopies(device))
			{
				stats.submitErrors++;
				return;
			}

			if (backend == Backend::Shared)
			{
				PipelineShared(device, frame);
				return;
			}

			if (backend == Backend::D3D12)
			{
				SubmitD3D12(device, frame);
				return;
			}

			// One submit per compositor wait (a second is AlreadySubmitted): skip early when this wait has one
			// already; the wait is claimed right before the submit itself (ClaimWait).
			uint64_t waits = waitCount.load();

			if (frame.wait + 1 < waits)
			{
				stats.staleFrames++;
				return;
			}

			if (waits == lastSubmittedWait)
			{
				stats.framesSkipped++;
				return;
			}

			CopyTexture& copy = copies[nextCopy];
			nextCopy = (nextCopy + 1) % kCopyCount;

			HRESULT copyResult = device->StretchRect(eyeSurface.load(), nullptr, copy.surface, nullptr, D3DTEXF_NONE);

			if (FAILED(copyResult))
			{
				stats.submitErrors++;
				SetError("can't copy the eye texture (" + HresultText(copyResult) + ")");
				return;
			}

			SubmitVulkan(copy, frame);
		}

		void OnPresent(IDirect3DDevice9* device, bool isEx)
		{
			if (insideBridge)
			{
				nestedPresents++;
				return;
			}

			IDirect3DDevice9* known = gameDevice.load();

			if (known == nullptr)
			{
				gameDevice.compare_exchange_strong(known, device);
				known = gameDevice.load();

				D3DCAPS9 caps{};

				if (known == device && SUCCEEDED(device->GetDeviceCaps(&caps)))
				{
					maxTextureWidth = caps.MaxTextureWidth;
					maxTextureHeight = caps.MaxTextureHeight;
				}

				int table = TableIndexFor(device);

				if (known == device && deviceFoundBy.empty())
				{
					deviceFoundBy = "its Present";
				}

				if (known == device)
				{
					void* vptr = *reinterpret_cast<void**>(device);
					HMODULE owner = nullptr;
					DWORD ownerFlags = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;
					char ownerPath[MAX_PATH]{};

					if (GetModuleHandleExW(ownerFlags, reinterpret_cast<LPCWSTR>(vptr), &owner) && GetModuleFileNameA(owner, ownerPath, MAX_PATH) > 0)
					{
						std::string path(ownerPath);
						size_t slash = path.find_last_of("\\/");
						gameVptrOwner = slash == std::string::npos ? path : path.substr(slash + 1);
					}
					else
					{
						gameVptrOwner = "no module (a copy on the heap)";
					}
				}

				if (table < 0 && known == device && !detoursActive)
				{
					// Found through its swap chain on a vtable no throwaway had: patch the device's own.
					IDirect3DDevice9Ex* probe = nullptr;
					bool ex = isEx;

					if (!ex && SUCCEEDED(device->QueryInterface(__uuidof(IDirect3DDevice9Ex), reinterpret_cast<void**>(&probe))) && probe != nullptr)
					{
						ex = true;
						probe->Release();
					}

					HookVtable(device, ex, "the game's own");
					table = TableIndexFor(device);
				}

				gameVtable = table;
				D3DDEVICE_CREATION_PARAMETERS creation{};

				if (known == device && SUCCEEDED(device->GetCreationParameters(&creation)))
				{
					gameAdapter = static_cast<int>(creation.AdapterOrdinal);
				}

				rcx::core::Log("vr: found the game's Direct3D 9 device (Present%s, %s thread, the %s vtable)", isEx ? "Ex" : "",
					GetCurrentThreadId() == luaThreadId ? "Lua" : "another", table >= 0 ? vtables[table].label.c_str() : "unknown");
			}

			if (device != known)
			{
				return;
			}

			BridgeScope scope;
			bool drawn = eyeDrawn.exchange(false);
			bool haveFrame = false;
			PendingFrame frame{};

			{
				std::lock_guard<std::mutex> lock(queueMutex);

				if (!sessionActive.load())
				{
					pending.clear();
				}
				else if (drawn && !pending.empty())
				{
					frame = pending.front();
					pending.pop_front();
					haveFrame = true;
				}
				else if (!drawn && !pending.empty())
				{
					presentsWaitingForDraw++;
				}
			}

			std::lock_guard<std::timed_mutex> lock(deviceMutex);
			stats.presents++;
			presentCount++;
			auto now = std::chrono::steady_clock::now();
			double windowSeconds = std::chrono::duration<double>(now - rateWindowStart).count();

			if (windowSeconds >= 1.0)
			{
				bool first = rateWindowStart == std::chrono::steady_clock::time_point{} || windowSeconds > 5.0;
				uint64_t waits = waitCount.load();
				stats.presentsPerSecond = first ? 0.0 : (stats.presents - rateWindowPresents) / windowSeconds;
				stats.submitsPerSecond = first ? 0.0 : (stats.framesSubmitted - rateWindowSubmits) / windowSeconds;
				stats.waitsPerSecond = first ? 0.0 : (waits - rateWindowWaits) / windowSeconds;
				rateWindowStart = now;
				rateWindowPresents = stats.presents;
				rateWindowSubmits = stats.framesSubmitted;
				rateWindowWaits = waits;
			}

			stats.deviceIsEx = stats.deviceIsEx || isEx;
			stats.presentOnOtherThread = GetCurrentThreadId() != luaThreadId;

			if (releaseRequested.exchange(false))
			{
				ReleaseDeviceResources();
				backend = Backend::Unknown;
				backendFailed = false;
			}

			if (haveFrame && !tripped.load())
			{
				auto handOffStart = std::chrono::steady_clock::now();
				uint64_t before = stats.framesSubmitted;
				SubmitFrame(device, frame);
				double handOffMs = MillisecondsSince(handOffStart);
				submittedCount += stats.framesSubmitted - before;
				slowSubmits = handOffMs > kSlowSubmitMs ? slowSubmits + 1 : 0;

				if (slowSubmits >= kSlowSubmitFrames)
				{
					char text[256]{};
					std::snprintf(text, sizeof(text), "handing each frame to SteamVR took over %.0f ms for %d frames in a row (the last %.0f ms, on %s), so VR was stopped before the "
						"game froze", kSlowSubmitMs, kSlowSubmitFrames, handOffMs, BackendName(backend));
					std::lock_guard<std::mutex> tripLock(tripMutex);
					tripReason = text;
					tripped = true;
					SetError(text);
				}
			}
			else if (sessionActive.load() && drawn)
			{
				stats.drawsWithoutSubmit++;
			}

			PublishStats();
		}

		void OnReset(IDirect3DDevice9* device)
		{
			if (device != gameDevice.load())
			{
				return;
			}

			BridgeScope scope;

			{
				std::lock_guard<std::timed_mutex> lock(deviceMutex);
				ReleaseDeviceResources();
				backend = Backend::Unknown;
				backendFailed = false;
				stats.resets++;
				PublishStats();
			}

			std::lock_guard<std::mutex> lock(captureMutex);

			if (eyeTexture != nullptr)
			{
				// The game recreates its render targets after a reset; take the next one of the same size.
				captureWidth = eyeWidth;
				captureHeight = eyeHeight;
				recaptureArmed = true;
			}

			eyeTexture = nullptr;
			eyeSurface = nullptr;
			eyeGeneration++;
			rcx::core::Log("vr: device reset; the eye texture will be captured again");
		}

		// The observer only sees the game's own device, never a Present the bridge makes itself.
		const PresentObserver* ObserverFor(IDirect3DDevice9* device)
		{
			if (insideBridge || device == nullptr || device != gameDevice.load())
			{
				return nullptr;
			}

			return presentObserver.load();
		}

		HRESULT DoPresent(PresentFunction original, IDirect3DDevice9* device, const RECT* sourceRect, const RECT* destinationRect, HWND window, const RGNDATA* dirtyRegion)
		{
			if (insideDevicePresent)
			{
				return original(device, sourceRect, destinationRect, window, dirtyRegion);
			}

			OnPresent(device, false);
			const PresentObserver* observer = ObserverFor(device);

			if (observer != nullptr)
			{
				observer->beforePresent(device);
			}

			bool outer = insideDevicePresent;
			insideDevicePresent = true;
			HRESULT result = original(device, sourceRect, destinationRect, window, dirtyRegion);
			insideDevicePresent = outer;

			if (observer != nullptr)
			{
				observer->afterPresent(device);
			}

			return result;
		}

		HRESULT DoPresentEx(PresentExFunction original, IDirect3DDevice9Ex* device, const RECT* sourceRect, const RECT* destinationRect, HWND window, const RGNDATA* dirtyRegion,
			DWORD flags)
		{
			if (insideDevicePresent)
			{
				return original(device, sourceRect, destinationRect, window, dirtyRegion, flags);
			}

			OnPresent(device, true);
			const PresentObserver* observer = ObserverFor(device);

			if (observer != nullptr)
			{
				observer->beforePresent(device);
			}

			bool outer = insideDevicePresent;
			insideDevicePresent = true;
			HRESULT result = original(device, sourceRect, destinationRect, window, dirtyRegion, flags);
			insideDevicePresent = outer;

			if (observer != nullptr)
			{
				observer->afterPresent(device);
			}

			return result;
		}

		HRESULT DoSwapChainPresent(SwapChainPresentFunction original, IDirect3DSwapChain9* chain, const RECT* sourceRect, const RECT* destinationRect, HWND window,
			const RGNDATA* dirtyRegion, DWORD flags)
		{
			IDirect3DDevice9* device = nullptr;

			if (insideDevicePresent || insideBridge || FAILED(chain->GetDevice(&device)) || device == nullptr)
			{
				return original(chain, sourceRect, destinationRect, window, dirtyRegion, flags);
			}

			// GetDevice adds a reference; the device outlives its swap chain's Present.
			device->Release();
			swapChainPresents++;
			OnPresent(device, false);
			const PresentObserver* observer = ObserverFor(device);

			if (observer != nullptr)
			{
				observer->beforePresent(device);
			}

			HRESULT result = original(chain, sourceRect, destinationRect, window, dirtyRegion, flags);

			if (observer != nullptr)
			{
				observer->afterPresent(device);
			}

			return result;
		}

		HRESULT DoReset(ResetFunction original, IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* parameters)
		{
			if (insideReset)
			{
				return original(device, parameters);
			}

			OnReset(device);
			const PresentObserver* observer = ObserverFor(device);

			if (observer != nullptr)
			{
				observer->onReset(device);
			}

			insideReset = true;
			HRESULT result = original(device, parameters);
			insideReset = false;
			return result;
		}

		HRESULT DoResetEx(ResetExFunction original, IDirect3DDevice9Ex* device, D3DPRESENT_PARAMETERS* parameters, D3DDISPLAYMODEEX* displayMode)
		{
			if (insideReset)
			{
				return original(device, parameters, displayMode);
			}

			OnReset(device);
			const PresentObserver* observer = ObserverFor(device);

			if (observer != nullptr)
			{
				observer->onReset(device);
			}

			insideReset = true;
			HRESULT result = original(device, parameters, displayMode);
			insideReset = false;
			return result;
		}

		// Takes a render target texture as the eye texture. via: how it was seen, "creation" or "binding".
		void CaptureEye(IDirect3DTexture9* texture, IDirect3DSurface9* surface, uint32_t width, uint32_t height, uint32_t format, const char* via)
		{
			std::lock_guard<std::mutex> lock(captureMutex);
			eyeTexture = texture;
			eyeWidth = width;
			eyeHeight = height;
			eyeFormat = format;
			eyeSurface = surface;
			eyeGeneration++;
			captureArmed = false;
			recaptureArmed = false;
			capturedVia = via;
			rcx::core::Log("vr: captured the eye texture by its %s, %ux%u format %u", via, width, height, static_cast<unsigned>(format));
		}

		// While a capture is armed, a render target of the asked size bound as target 0 is taken as the
		// eye texture: the fallback for one made where the creation hook didn't see it (already made,
		// made on another path or later on the render thread).
		void CaptureBound(IDirect3DDevice9* device, IDirect3DSurface9* surface)
		{
			IDirect3DDevice9* known = gameDevice.load();

			if (known != nullptr && known != device)
			{
				return;
			}

			armedBinds++;
			D3DSURFACE_DESC desc{};

			if (FAILED(surface->GetDesc(&desc)) || (desc.Usage & D3DUSAGE_RENDERTARGET) == 0)
			{
				return;
			}

			if (desc.Width != captureWidth.load() || desc.Height != captureHeight.load())
			{
				return;
			}

			IDirect3DTexture9* texture = nullptr;

			if (FAILED(surface->GetContainer(__uuidof(IDirect3DTexture9), reinterpret_cast<void**>(&texture))) || texture == nullptr)
			{
				return;
			}

			// The texture outlives this binding as the game's render target; the bridge keeps no reference.
			texture->Release();
			CaptureEye(texture, surface, desc.Width, desc.Height, static_cast<uint32_t>(desc.Format), "binding");
		}

		HRESULT DoSetRenderTarget(SetRenderTargetFunction original, IDirect3DDevice9* device, DWORD index, IDirect3DSurface9* surface)
		{
			if (index == 0 && surface != nullptr && !insideBridge)
			{
				if (surface == eyeSurface.load())
				{
					eyeDrawn = true;
				}
				else if (captureArmed.load() || recaptureArmed.load())
				{
					CaptureBound(device, surface);
				}
			}

			return original(device, index, surface);
		}

		HRESULT DoCreateTexture(CreateTextureFunction original, IDirect3DDevice9* device, UINT width, UINT height, UINT levels, DWORD usage, D3DFORMAT format,
			D3DPOOL pool, IDirect3DTexture9** texture, HANDLE* sharedHandle)
		{
			HRESULT result = original(device, width, height, levels, usage, format, pool, texture, sharedHandle);

			if (!insideBridge && (usage & D3DUSAGE_RENDERTARGET) != 0 && (captureArmed.load() || recaptureArmed.load()))
			{
				CreateAttempt attempt{};
				attempt.seen = true;
				attempt.width = width;
				attempt.height = height;
				attempt.format = static_cast<uint32_t>(format);
				attempt.result = static_cast<long>(result);

				D3DSURFACE_DESC desc{};

				if (SUCCEEDED(result) && texture != nullptr && *texture != nullptr && SUCCEEDED((*texture)->GetLevelDesc(0, &desc)))
				{
					attempt.actualWidth = desc.Width;
					attempt.actualHeight = desc.Height;
				}

				std::lock_guard<std::mutex> lock(attemptMutex);
				attempt.count = lastAttempt.count + 1;
				lastAttempt = attempt;
			}

			if (FAILED(result) || insideBridge || texture == nullptr || *texture == nullptr)
			{
				return result;
			}

			if ((usage & D3DUSAGE_RENDERTARGET) == 0 || (!captureArmed.load() && !recaptureArmed.load()))
			{
				return result;
			}

			if (width != captureWidth.load() || height != captureHeight.load())
			{
				return result;
			}

			IDirect3DDevice9* known = gameDevice.load();

			if (known != nullptr && known != device)
			{
				return result;
			}

			IDirect3DSurface9* surface = nullptr;

			if (FAILED((*texture)->GetSurfaceLevel(0, &surface)) || surface == nullptr)
			{
				return result;
			}

			// The surface lives as long as its texture; the bridge keeps no reference to either.
			surface->Release();
			CaptureEye(*texture, surface, width, height, static_cast<uint32_t>(format), "creation");
			return result;
		}

		// The vtable route: a patched slot calls the original of the device's own table.
		HRESULT STDMETHODCALLTYPE HookPresent(IDirect3DDevice9* device, const RECT* sourceRect, const RECT* destinationRect, HWND window, const RGNDATA* dirtyRegion)
		{
			return DoPresent(TableFor(device).present, device, sourceRect, destinationRect, window, dirtyRegion);
		}

		HRESULT STDMETHODCALLTYPE HookPresentEx(IDirect3DDevice9Ex* device, const RECT* sourceRect, const RECT* destinationRect, HWND window, const RGNDATA* dirtyRegion, DWORD flags)
		{
			return DoPresentEx(TableFor(device).presentEx, device, sourceRect, destinationRect, window, dirtyRegion, flags);
		}

		HRESULT STDMETHODCALLTYPE HookSwapChainPresent(IDirect3DSwapChain9* chain, const RECT* sourceRect, const RECT* destinationRect, HWND window, const RGNDATA* dirtyRegion,
			DWORD flags)
		{
			void** vtable = *reinterpret_cast<void***>(chain);
			SwapChainPresentFunction original = swapChains[0].present;
			int count = swapChainCount.load();

			for (int index = 0; index < count; index++)
			{
				if (swapChains[index].vtable == vtable)
				{
					original = swapChains[index].present;
				}
			}

			return DoSwapChainPresent(original, chain, sourceRect, destinationRect, window, dirtyRegion, flags);
		}

		HRESULT STDMETHODCALLTYPE HookReset(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* parameters)
		{
			return DoReset(TableFor(device).reset, device, parameters);
		}

		HRESULT STDMETHODCALLTYPE HookResetEx(IDirect3DDevice9Ex* device, D3DPRESENT_PARAMETERS* parameters, D3DDISPLAYMODEEX* displayMode)
		{
			return DoResetEx(TableFor(device).resetEx, device, parameters, displayMode);
		}

		HRESULT STDMETHODCALLTYPE HookSetRenderTarget(IDirect3DDevice9* device, DWORD index, IDirect3DSurface9* surface)
		{
			return DoSetRenderTarget(TableFor(device).setRenderTarget, device, index, surface);
		}

		HRESULT STDMETHODCALLTYPE HookCreateTexture(IDirect3DDevice9* device, UINT width, UINT height, UINT levels, DWORD usage, D3DFORMAT format,
			D3DPOOL pool, IDirect3DTexture9** texture, HANDLE* sharedHandle)
		{
			return DoCreateTexture(TableFor(device).createTexture, device, width, height, levels, usage, format, pool, texture, sharedHandle);
		}

		/*
			The detour route: each distinct d3d9.dll body of a function gets its own entry point (index I)
			and trampoline, so a device whose vtable is a copy (an overlay's) still reaches the hooks
			through the body it calls.
		*/
		constexpr int kMaxBodies = 8;

		struct DetourSet
		{
			const char* name = "";
			void* bodies[kMaxBodies]{};
			void* trampolines[kMaxBodies]{};
			bool installed[kMaxBodies]{};
			int count = 0;
		};

		DetourSet presentDetours{"Present"};
		DetourSet presentExDetours{"PresentEx"};
		DetourSet swapChainPresentDetours{"the swap chain's Present"};
		DetourSet resetDetours{"Reset"};
		DetourSet resetExDetours{"ResetEx"};
		DetourSet setRenderTargetDetours{"SetRenderTarget"};
		DetourSet createTextureDetours{"CreateTexture"};

		template <int I>
		HRESULT STDMETHODCALLTYPE DetourPresent(IDirect3DDevice9* device, const RECT* sourceRect, const RECT* destinationRect, HWND window, const RGNDATA* dirtyRegion)
		{
			return DoPresent(reinterpret_cast<PresentFunction>(presentDetours.trampolines[I]), device, sourceRect, destinationRect, window, dirtyRegion);
		}

		template <int I>
		HRESULT STDMETHODCALLTYPE DetourPresentEx(IDirect3DDevice9Ex* device, const RECT* sourceRect, const RECT* destinationRect, HWND window, const RGNDATA* dirtyRegion, DWORD flags)
		{
			return DoPresentEx(reinterpret_cast<PresentExFunction>(presentExDetours.trampolines[I]), device, sourceRect, destinationRect, window, dirtyRegion, flags);
		}

		template <int I>
		HRESULT STDMETHODCALLTYPE DetourSwapChainPresent(IDirect3DSwapChain9* chain, const RECT* sourceRect, const RECT* destinationRect, HWND window, const RGNDATA* dirtyRegion,
			DWORD flags)
		{
			return DoSwapChainPresent(reinterpret_cast<SwapChainPresentFunction>(swapChainPresentDetours.trampolines[I]), chain, sourceRect, destinationRect, window, dirtyRegion,
				flags);
		}

		template <int I>
		HRESULT STDMETHODCALLTYPE DetourReset(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* parameters)
		{
			return DoReset(reinterpret_cast<ResetFunction>(resetDetours.trampolines[I]), device, parameters);
		}

		template <int I>
		HRESULT STDMETHODCALLTYPE DetourResetEx(IDirect3DDevice9Ex* device, D3DPRESENT_PARAMETERS* parameters, D3DDISPLAYMODEEX* displayMode)
		{
			return DoResetEx(reinterpret_cast<ResetExFunction>(resetExDetours.trampolines[I]), device, parameters, displayMode);
		}

		template <int I>
		HRESULT STDMETHODCALLTYPE DetourSetRenderTarget(IDirect3DDevice9* device, DWORD index, IDirect3DSurface9* surface)
		{
			return DoSetRenderTarget(reinterpret_cast<SetRenderTargetFunction>(setRenderTargetDetours.trampolines[I]), device, index, surface);
		}

		template <int I>
		HRESULT STDMETHODCALLTYPE DetourCreateTexture(IDirect3DDevice9* device, UINT width, UINT height, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool,
			IDirect3DTexture9** texture, HANDLE* sharedHandle)
		{
			return DoCreateTexture(reinterpret_cast<CreateTextureFunction>(createTextureDetours.trampolines[I]), device, width, height, levels, usage, format, pool, texture,
				sharedHandle);
		}

#define RCX_DETOUR_ENTRIES(entry) \
	{reinterpret_cast<void*>(&entry<0>), reinterpret_cast<void*>(&entry<1>), reinterpret_cast<void*>(&entry<2>), reinterpret_cast<void*>(&entry<3>), \
		reinterpret_cast<void*>(&entry<4>), reinterpret_cast<void*>(&entry<5>), reinterpret_cast<void*>(&entry<6>), reinterpret_cast<void*>(&entry<7>)}

		void* const presentEntries[kMaxBodies] = RCX_DETOUR_ENTRIES(DetourPresent);
		void* const presentExEntries[kMaxBodies] = RCX_DETOUR_ENTRIES(DetourPresentEx);
		void* const swapChainPresentEntries[kMaxBodies] = RCX_DETOUR_ENTRIES(DetourSwapChainPresent);
		void* const resetEntries[kMaxBodies] = RCX_DETOUR_ENTRIES(DetourReset);
		void* const resetExEntries[kMaxBodies] = RCX_DETOUR_ENTRIES(DetourResetEx);
		void* const setRenderTargetEntries[kMaxBodies] = RCX_DETOUR_ENTRIES(DetourSetRenderTarget);
		void* const createTextureEntries[kMaxBodies] = RCX_DETOUR_ENTRIES(DetourCreateTexture);

#undef RCX_DETOUR_ENTRIES
	}

	namespace
	{
		// The file name of the module an address lives in ("d3d9.dll"), or "unknown".
		std::string ModuleNameOf(const void* address)
		{
			HMODULE module = nullptr;
			DWORD flags = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;

			if (address == nullptr || !GetModuleHandleExW(flags, reinterpret_cast<LPCWSTR>(address), &module))
			{
				return "unknown";
			}

			char path[MAX_PATH]{};
			DWORD length = GetModuleFileNameA(module, path, MAX_PATH);

			if (length == 0 || length >= MAX_PATH)
			{
				return "unknown";
			}

			std::string text(path, length);
			size_t slash = text.find_last_of("\\/");
			return slash == std::string::npos ? text : text.substr(slash + 1);
		}

		// "d3d9.dll+0x1a2b0" for an address in a module, else the bare address.
		std::string DescribeAddress(const void* address)
		{
			HMODULE module = nullptr;
			DWORD flags = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;
			char text[64]{};

			if (GetModuleHandleExW(flags, reinterpret_cast<LPCWSTR>(address), &module))
			{
				std::snprintf(text, sizeof(text), "+0x%llx", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(address) - reinterpret_cast<uintptr_t>(module)));
				return ModuleNameOf(address) + text;
			}

			std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(address)));
			return text;
		}

		// Detours one body of a function once; true when it's detoured (now or before), false when it
		// was refused or the set is full, so the caller patches the vtable slot instead.
		bool DetourBody(DetourSet& set, void* const* entries, void* body)
		{
			if (!detoursActive || body == nullptr)
			{
				return false;
			}

			for (int index = 0; index < set.count; index++)
			{
				if (set.bodies[index] == body)
				{
					return set.installed[index];
				}
			}

			if (set.count >= kMaxBodies)
			{
				return false;
			}

			int index = set.count;
			set.bodies[index] = body;
			set.count = index + 1;
			std::string how;
			std::string reason;
			set.installed[index] = rcx::detour::Install(body, entries[index], &set.trampolines[index], how, reason);
			std::string line = std::string(set.name) + " at " + DescribeAddress(body) + ": " + (set.installed[index] ? how : "refused, " + reason + "; its vtable slots are patched instead");
			detourReport.push_back(line);
			rcx::core::Log("vr: %s", line.c_str());

			if (set.installed[index])
			{
				detoursInstalled++;
				detoursChained += how.find("chained") != std::string::npos ? 1 : 0;
			}
			else
			{
				detoursRefused++;
			}

			return set.installed[index];
		}

		// Patches a throwaway device's vtable unless one before it had the same. ex: the device is an
		// IDirect3DDevice9Ex, so the vtable has the Ex slots. The originals are read and counted before
		// any slot is patched, so a hook never runs without its table.
		void HookVtable(IDirect3DDevice9* device, bool ex, const std::string& label)
		{
			void** vtable = *reinterpret_cast<void***>(device);

			if (vtable == nullptr)
			{
				return;
			}

			int count = vtableCount.load();

			for (int index = 0; index < count; index++)
			{
				if (vtables[index].vtable == vtable)
				{
					return;
				}
			}

			if (count >= kMaxVtables)
			{
				return;
			}

			HookedVtable& table = vtables[count];
			table.vtable = vtable;
			table.ex = ex;
			table.label = label;
			table.owner = ModuleNameOf(vtable[kSlotCreateTexture]);
			table.present = reinterpret_cast<PresentFunction>(vtable[kSlotPresent]);
			table.createTexture = reinterpret_cast<CreateTextureFunction>(vtable[kSlotCreateTexture]);
			table.setRenderTarget = reinterpret_cast<SetRenderTargetFunction>(vtable[kSlotSetRenderTarget]);
			table.reset = reinterpret_cast<ResetFunction>(vtable[kSlotReset]);

			if (ex)
			{
				table.presentEx = reinterpret_cast<PresentExFunction>(vtable[kSlotPresentEx]);
				table.resetEx = reinterpret_cast<ResetExFunction>(vtable[kSlotResetEx]);
			}

			vtableCount = count + 1;

			// Each slot is caught by detouring its body, or else by patching the slot.
			struct SlotHook
			{
				int slot;
				void* hook;
				DetourSet* set;
				void* const* entries;
			};

			const SlotHook slots[] = {
				{kSlotCreateTexture, reinterpret_cast<void*>(&HookCreateTexture), &createTextureDetours, createTextureEntries},
				{kSlotSetRenderTarget, reinterpret_cast<void*>(&HookSetRenderTarget), &setRenderTargetDetours, setRenderTargetEntries},
				{kSlotReset, reinterpret_cast<void*>(&HookReset), &resetDetours, resetEntries},
				{kSlotPresent, reinterpret_cast<void*>(&HookPresent), &presentDetours, presentEntries},
				{kSlotResetEx, reinterpret_cast<void*>(&HookResetEx), &resetExDetours, resetExEntries},
				{kSlotPresentEx, reinterpret_cast<void*>(&HookPresentEx), &presentExDetours, presentExEntries},
			};

			void* unused = nullptr;
			bool patched = true;

			for (const SlotHook& entry : slots)
			{
				bool exSlot = entry.slot == kSlotResetEx || entry.slot == kSlotPresentEx;

				if (exSlot && !ex)
				{
					continue;
				}

				if (DetourBody(*entry.set, entry.entries, vtable[entry.slot]))
				{
					continue;
				}

				patched = PatchSlot(vtable, entry.slot, entry.hook, &unused) && patched;
			}

			rcx::core::Log("vr: %s the %s device's vtable (its functions in %s)", patched ? "hooked" : "couldn't fully hook", label.c_str(), table.owner.c_str());
		}

		// Patches a throwaway swap chain's Present unless one before it had the same vtable.
		void HookSwapChain(IDirect3DSwapChain9* chain, const std::string& label)
		{
			void** vtable = *reinterpret_cast<void***>(chain);

			if (vtable == nullptr)
			{
				return;
			}

			int count = swapChainCount.load();

			for (int index = 0; index < count; index++)
			{
				if (swapChains[index].vtable == vtable)
				{
					return;
				}
			}

			if (count >= kMaxVtables)
			{
				return;
			}

			swapChains[count].vtable = vtable;
			swapChains[count].present = reinterpret_cast<SwapChainPresentFunction>(vtable[kSlotSwapChainPresent]);
			swapChainCount = count + 1;

			void* unused = nullptr;
			bool patched = DetourBody(swapChainPresentDetours, swapChainPresentEntries, vtable[kSlotSwapChainPresent])
				|| PatchSlot(vtable, kSlotSwapChainPresent, reinterpret_cast<void*>(&HookSwapChainPresent), &unused);
			rcx::core::Log("vr: %s the %s device's swap chain Present (in %s)", patched ? "hooked" : "couldn't hook", label.c_str(),
				ModuleNameOf(reinterpret_cast<const void*>(swapChains[count].present)).c_str());
		}

		using EnumProcessModulesFunction = BOOL(WINAPI*)(HANDLE, HMODULE*, DWORD, LPDWORD);

		// Every loaded module whose file name is d3d9.dll (a proxy and the one it forwards to can both be loaded).
		std::vector<HMODULE> LoadedD3d9Modules()
		{
			std::vector<HMODULE> found;
			HMODULE first = GetModuleHandleW(L"d3d9.dll");

			if (first != nullptr)
			{
				found.push_back(first);
			}

			auto enumModules = reinterpret_cast<EnumProcessModulesFunction>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "K32EnumProcessModules")));

			if (enumModules == nullptr)
			{
				return found;
			}

			static HMODULE modules[1024];
			DWORD needed = 0;

			if (!enumModules(GetCurrentProcess(), modules, sizeof(modules), &needed))
			{
				return found;
			}

			DWORD moduleCount = needed / sizeof(HMODULE);

			for (DWORD index = 0; index < moduleCount && index < 1024; index++)
			{
				wchar_t path[MAX_PATH]{};
				DWORD length = GetModuleFileNameW(modules[index], path, MAX_PATH);

				if (length == 0 || length >= MAX_PATH)
				{
					continue;
				}

				const wchar_t* name = path;

				for (const wchar_t* cursor = path; *cursor != L'\0'; cursor++)
				{
					if (*cursor == L'\\' || *cursor == L'/')
					{
						name = cursor + 1;
					}
				}

				bool known = false;

				for (HMODULE module : found)
				{
					known = known || module == modules[index];
				}

				if (!known && _wcsicmp(name, L"d3d9.dll") == 0)
				{
					found.push_back(modules[index]);
				}
			}

			return found;
		}

		// Every loaded module as (lowercase file name, full path).
		std::vector<std::pair<std::string, std::string>> LoadedModules()
		{
			std::vector<std::pair<std::string, std::string>> found;
			auto enumModules = reinterpret_cast<EnumProcessModulesFunction>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "K32EnumProcessModules")));
			static HMODULE modules[1024];
			DWORD needed = 0;

			if (enumModules == nullptr || !enumModules(GetCurrentProcess(), modules, sizeof(modules), &needed))
			{
				return found;
			}

			for (DWORD index = 0; index < needed / sizeof(HMODULE) && index < 1024; index++)
			{
				char path[MAX_PATH]{};
				DWORD length = GetModuleFileNameA(modules[index], path, MAX_PATH);

				if (length == 0 || length >= MAX_PATH)
				{
					continue;
				}

				std::string full(path, length);
				size_t slash = full.find_last_of("\\/");
				std::string name = slash == std::string::npos ? full : full.substr(slash + 1);

				for (char& character : name)
				{
					character = static_cast<char>(tolower(static_cast<unsigned char>(character)));
				}

				found.emplace_back(name, full);
			}

			return found;
		}

		// A cached VirtualQuery: whether [address, address + size) is committed, readable memory, and whether it's
		// executable code of a loaded image.
		struct MemoryCheck
		{
			struct Region
			{
				uintptr_t start = 0;
				uintptr_t end = 0;
				bool readable = false;
				bool imageCode = false;
			};

			std::vector<Region> regions;

			const Region* Find(uintptr_t address)
			{
				for (const Region& region : regions)
				{
					if (address >= region.start && address < region.end)
					{
						return &region;
					}
				}

				MEMORY_BASIC_INFORMATION info{};

				if (address < 0x10000 || address > 0x7fffffffffffULL || VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info)) == 0)
				{
					return nullptr;
				}

				DWORD protect = info.Protect & 0xff;
				bool committed = info.State == MEM_COMMIT && (info.Protect & PAGE_GUARD) == 0 && protect != PAGE_NOACCESS;
				bool executable = protect == PAGE_EXECUTE_READ || protect == PAGE_EXECUTE_READWRITE || protect == PAGE_EXECUTE_WRITECOPY;
				Region region{};
				region.start = reinterpret_cast<uintptr_t>(info.BaseAddress);
				region.end = region.start + info.RegionSize;
				region.readable = committed && protect != PAGE_EXECUTE;
				region.imageCode = committed && executable && info.Type == MEM_IMAGE;

				if (regions.size() > 4096)
				{
					regions.clear();
				}

				regions.push_back(region);
				return &regions.back();
			}

			bool Readable(uintptr_t address, size_t size)
			{
				const Region* region = Find(address);
				return region != nullptr && region->readable && address + size <= region->end;
			}

			bool ImageCode(uintptr_t address)
			{
				const Region* region = Find(address);
				return region != nullptr && region->imageCode;
			}
		};

		// IDirect3DDevice9's slot count: a candidate's vtable must have this many entries, all into code of a loaded image.
		constexpr size_t kDeviceSlots = 119;
	}

	IDirect3DDevice9* ScanForDevice(void* module, std::string& where)
	{
		where.clear();

		if (module == nullptr)
		{
			return nullptr;
		}

		const uint8_t* base = reinterpret_cast<const uint8_t*>(module);
		const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);

		if (dos->e_magic != IMAGE_DOS_SIGNATURE)
		{
			return nullptr;
		}

		const IMAGE_NT_HEADERS64* headers = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);

		if (headers->Signature != IMAGE_NT_SIGNATURE)
		{
			return nullptr;
		}

		const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(headers);
		MemoryCheck memory;
		int candidates = 0;

		for (WORD index = 0; index < headers->FileHeader.NumberOfSections; index++, section++)
		{
			if ((section->Characteristics & IMAGE_SCN_MEM_WRITE) == 0)
			{
				continue;
			}

			uintptr_t start = reinterpret_cast<uintptr_t>(base + section->VirtualAddress);
			size_t size = section->Misc.VirtualSize & ~static_cast<size_t>(7);

			for (size_t offset = 0; offset + sizeof(uintptr_t) <= size; offset += sizeof(uintptr_t))
			{
				uintptr_t object = *reinterpret_cast<const uintptr_t*>(start + offset);

				if ((object & 7) != 0 || !memory.Readable(object, sizeof(uintptr_t)))
				{
					continue;
				}

				uintptr_t table = *reinterpret_cast<const uintptr_t*>(object);

				if ((table & 7) != 0 || !memory.Readable(table, kDeviceSlots * sizeof(uintptr_t)))
				{
					continue;
				}

				const uintptr_t* entries = reinterpret_cast<const uintptr_t*>(table);
				bool allCode = true;

				for (size_t slot = 0; slot < kDeviceSlots && allCode; slot++)
				{
					allCode = memory.ImageCode(entries[slot]);
				}

				if (!allCode)
				{
					continue;
				}

				// A COM object with a device-sized vtable in loaded code: ask it.
				candidates++;
				IUnknown* unknown = reinterpret_cast<IUnknown*>(object);
				IDirect3DDevice9* device = nullptr;

				if (SUCCEEDED(unknown->QueryInterface(__uuidof(IDirect3DDevice9), reinterpret_cast<void**>(&device))) && device != nullptr)
				{
					// The engine keeps its own reference; the bridge keeps none.
					device->Release();
					char text[96]{};
					std::snprintf(text, sizeof(text), "+0x%llx, after %d candidate(s)", static_cast<unsigned long long>(start + offset - reinterpret_cast<uintptr_t>(base)), candidates);
					where = ModuleNameOf(base) + text;
					return device;
				}
			}
		}

		where = "not found in " + ModuleNameOf(base) + " (" + std::to_string(candidates) + " COM object(s) with a device-sized vtable asked)";
		return nullptr;
	}

	namespace
	{
		// Finds the game's device in shaderapidx9.dll's data, independent of any hook, and hooks its own vtable.
		void FindGameDevice()
		{
			std::string why;

			if (rcx::core::IsFeatureDisabled(kScanFeature, &why))
			{
				scanResult = "off: " + why;
				return;
			}

			HMODULE shaderApi = GetModuleHandleW(L"shaderapidx9.dll");

			if (shaderApi == nullptr)
			{
				scanResult = "shaderapidx9.dll isn't loaded";
				return;
			}

			if (!rcx::engine::Arm(kScanFeature))
			{
				scanResult = "off: the crash latch couldn't be armed";
				return;
			}

			std::string where;
			IDirect3DDevice9* device = ScanForDevice(shaderApi, where);
			scanResult = where;

			if (device == nullptr)
			{
				rcx::engine::Disarm(kScanFeature);
				rcx::core::Log("vr: the game's device wasn't found by the scan: %s", where.c_str());
				return;
			}

			AdoptGameDevice(device, "the scan of " + where);
			rcx::engine::Disarm(kScanFeature);
		}
	}

	void AdoptGameDevice(IDirect3DDevice9* device, const std::string& how)
	{
		IDirect3DDevice9* known = nullptr;
		bool adopted = gameDevice.compare_exchange_strong(known, device);

		if (!adopted && known != device)
		{
			rcx::core::Log("vr: %s found a device, but another one already presented through the hooks; kept that one", how.c_str());
			return;
		}

		if (deviceFoundBy.empty() || deviceFoundBy == "its Present")
		{
			deviceFoundBy = adopted ? how : deviceFoundBy + " (and " + how + ")";
		}

		D3DCAPS9 caps{};

		if (maxTextureWidth.load() == 0 && SUCCEEDED(device->GetDeviceCaps(&caps)))
		{
			maxTextureWidth = caps.MaxTextureWidth;
			maxTextureHeight = caps.MaxTextureHeight;
		}

		D3DDEVICE_CREATION_PARAMETERS creation{};

		if (SUCCEEDED(device->GetCreationParameters(&creation)))
		{
			gameAdapter = static_cast<int>(creation.AdapterOrdinal);
		}

		void* vptr = *reinterpret_cast<void**>(device);
		std::string owner = ModuleNameOf(vptr);
		gameVptrOwner = owner == "unknown" ? "no module (a copy on the heap)" : owner;
		IDirect3DDevice9Ex* probe = nullptr;
		bool ex = false;

		if (SUCCEEDED(device->QueryInterface(__uuidof(IDirect3DDevice9Ex), reinterpret_cast<void**>(&probe))) && probe != nullptr)
		{
			ex = true;
			probe->Release();
		}

		// Its own table: each slot's body is detoured if it isn't yet, else the slot is patched.
		HookVtable(device, ex, "the game's own");
		int table = TableIndexFor(device);
		gameVtable = table;
		rcx::core::Log("vr: found the game's Direct3D 9 device by %s; its vtable is in %s, its functions in %s", how.c_str(), gameVptrOwner.c_str(),
			table >= 0 ? vtables[table].owner.c_str() : "unknown");
	}

	bool InstallHooks(std::string& reason)
	{
		if (hooksInstalled)
		{
			return true;
		}

		luaThreadId = GetCurrentThreadId();
		std::vector<HMODULE> d3d9Modules = LoadedD3d9Modules();

		if (d3d9Modules.empty())
		{
			reason = "d3d9.dll isn't loaded: the game isn't running on Direct3D 9";
			return false;
		}

		if (!rcx::core::PinModule())
		{
			reason = "can't pin the module in memory";
			return false;
		}

		HWND window = CreateWindowExW(0, L"STATIC", L"rcx", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, nullptr, nullptr);

		if (window == nullptr)
		{
			reason = "can't create a window for the throwaway device";
			return false;
		}

		// The detours are behind vr_detours: the policy file, the player's rcx_vr_detours and the crash
		// latch (armed before the first write) can each keep them off, and then only vtables are patched.
		std::string detoursReason;

		if (rcx::core::IsFeatureDisabled(kDetourFeature, &detoursReason))
		{
			detoursOff = detoursReason;
		}
		else if (!rcx::engine::Arm(kDetourFeature))
		{
			detoursOff = "the crash latch couldn't be armed";
		}
		else
		{
			detoursActive = true;
			detoursArmed = true;
		}

		struct ThrowawayKind
		{
			bool ex = false;
			DWORD vertexFlags = 0;
			const char* label = "";
		};

		// The game's device may be made any of these ways (-nod3d9ex gives a plain one), each possibly with its own vtable.
		const ThrowawayKind kinds[] = {
			{true, D3DCREATE_SOFTWARE_VERTEXPROCESSING, "Ex software"},
			{true, D3DCREATE_HARDWARE_VERTEXPROCESSING, "Ex hardware"},
			{true, D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_PUREDEVICE, "Ex pure"},
			{false, D3DCREATE_SOFTWARE_VERTEXPROCESSING, "plain software"},
			{false, D3DCREATE_HARDWARE_VERTEXPROCESSING, "plain hardware"},
			{false, D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_PUREDEVICE, "plain pure"},
		};

		HRESULT firstResult = E_FAIL;
		bool firstTried = false;

		for (HMODULE d3d9 : d3d9Modules)
		{
			char modulePath[MAX_PATH]{};
			GetModuleFileNameA(d3d9, modulePath, MAX_PATH);
			bool firstModule = d3d9 == d3d9Modules.front();
			auto create9Ex = reinterpret_cast<Direct3DCreate9ExFunction>(reinterpret_cast<void*>(GetProcAddress(d3d9, "Direct3DCreate9Ex")));
			auto create9 = reinterpret_cast<Direct3DCreate9Function>(reinterpret_cast<void*>(GetProcAddress(d3d9, "Direct3DCreate9")));
			IDirect3D9Ex* direct3dEx = nullptr;

			if (create9Ex == nullptr || FAILED(create9Ex(D3D_SDK_VERSION, &direct3dEx)))
			{
				direct3dEx = nullptr;
			}

			IDirect3D9* direct3d = create9 != nullptr ? create9(D3D_SDK_VERSION) : nullptr;
			IDirect3D9* enumerator = direct3dEx != nullptr ? static_cast<IDirect3D9*>(direct3dEx) : direct3d;
			UINT adapterCount = enumerator != nullptr ? enumerator->GetAdapterCount() : 0;

			if (firstModule && enumerator != nullptr)
			{
				adapterNames.clear();
				adapterGpus.clear();

				for (UINT adapter = 0; adapter < adapterCount; adapter++)
				{
					D3DADAPTER_IDENTIFIER9 identifier{};
					bool identified = SUCCEEDED(enumerator->GetAdapterIdentifier(adapter, 0, &identifier));
					std::string name = identified ? std::string(identifier.Description) : std::string("unknown");
					adapterNames.push_back(std::to_string(adapter) + ": " + name);
					char gpu[64]{};
					LUID luid{};

					if (direct3dEx != nullptr && SUCCEEDED(direct3dEx->GetAdapterLUID(adapter, &luid)))
					{
						std::snprintf(gpu, sizeof(gpu), "luid %08lx:%08lx", static_cast<unsigned long>(luid.HighPart), static_cast<unsigned long>(luid.LowPart));
					}
					else
					{
						std::snprintf(gpu, sizeof(gpu), "pci %04lx:%04lx", static_cast<unsigned long>(identifier.VendorId), static_cast<unsigned long>(identifier.DeviceId));
					}

					adapterGpus.push_back(gpu);
				}
			}

			// On a laptop with two GPUs each is its own adapter; the game may be on either.
			UINT adapterLimit = adapterCount < 1 ? 1 : (adapterCount > kMaxAdapters ? kMaxAdapters : adapterCount);

			for (UINT adapter = 0; adapter < adapterLimit; adapter++)
			{
				for (const ThrowawayKind& kind : kinds)
				{
					D3DPRESENT_PARAMETERS parameters{};
					parameters.Windowed = TRUE;
					parameters.SwapEffect = D3DSWAPEFFECT_DISCARD;
					parameters.BackBufferFormat = D3DFMT_UNKNOWN;
					parameters.BackBufferWidth = 1;
					parameters.BackBufferHeight = 1;
					parameters.hDeviceWindow = window;

					DWORD flags = kind.vertexFlags | D3DCREATE_NOWINDOWCHANGES | D3DCREATE_FPU_PRESERVE;
					IDirect3DDevice9* device = nullptr;
					HRESULT result = E_FAIL;

					if (kind.ex && direct3dEx != nullptr)
					{
						IDirect3DDevice9Ex* exDevice = nullptr;
						result = direct3dEx->CreateDeviceEx(adapter, D3DDEVTYPE_HAL, window, flags, &parameters, nullptr, &exDevice);
						device = exDevice;
					}
					else if (!kind.ex && direct3d != nullptr)
					{
						result = direct3d->CreateDevice(adapter, D3DDEVTYPE_HAL, window, flags, &parameters, &device);
					}

					if (!firstTried)
					{
						firstTried = true;
						firstResult = result;
					}

					if (FAILED(result) || device == nullptr)
					{
						continue;
					}

					bool ex = kind.ex;

					if (!ex)
					{
						IDirect3DDevice9Ex* probe = nullptr;

						if (SUCCEEDED(device->QueryInterface(__uuidof(IDirect3DDevice9Ex), reinterpret_cast<void**>(&probe))) && probe != nullptr)
						{
							ex = true;
							probe->Release();
						}
					}

					std::string label = std::string(kind.label) + (adapter > 0 ? " on adapter " + std::to_string(adapter) : std::string()) +
						(firstModule ? std::string() : " (" + std::string(modulePath) + ")");
					HookVtable(device, ex, label);
					IDirect3DSwapChain9* chain = nullptr;

					if (SUCCEEDED(device->GetSwapChain(0, &chain)) && chain != nullptr)
					{
						HookSwapChain(chain, label);
						chain->Release();
					}

					device->Release();
				}
			}

			SafeRelease(direct3dEx);
			SafeRelease(direct3d);
		}

		// D3D9On12 (Windows' Direct3D 9 on Direct3D 12) can give the game a device of its own class: its throwaways too.
		auto create9On12Ex = reinterpret_cast<PFN_Direct3DCreate9On12Ex>(reinterpret_cast<void*>(GetProcAddress(d3d9Modules.front(), "Direct3DCreate9On12Ex")));
		IDirect3D9Ex* direct3d9On12 = nullptr;
		D3D9ON12_ARGS on12Arguments{};
		on12Arguments.Enable9On12 = TRUE;

		if (create9On12Ex != nullptr && SUCCEEDED(create9On12Ex(D3D_SDK_VERSION, &on12Arguments, 1, &direct3d9On12)) && direct3d9On12 != nullptr)
		{
			const DWORD on12Flags[] = {D3DCREATE_SOFTWARE_VERTEXPROCESSING, D3DCREATE_HARDWARE_VERTEXPROCESSING};

			for (DWORD vertexFlags : on12Flags)
			{
				D3DPRESENT_PARAMETERS parameters{};
				parameters.Windowed = TRUE;
				parameters.SwapEffect = D3DSWAPEFFECT_DISCARD;
				parameters.BackBufferFormat = D3DFMT_UNKNOWN;
				parameters.BackBufferWidth = 1;
				parameters.BackBufferHeight = 1;
				parameters.hDeviceWindow = window;
				IDirect3DDevice9Ex* device = nullptr;

				if (FAILED(direct3d9On12->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window, vertexFlags | D3DCREATE_NOWINDOWCHANGES | D3DCREATE_FPU_PRESERVE, &parameters,
					nullptr, &device)) || device == nullptr)
				{
					continue;
				}

				std::string label = vertexFlags == D3DCREATE_SOFTWARE_VERTEXPROCESSING ? "D3D9On12 software" : "D3D9On12 hardware";
				HookVtable(device, true, label);
				IDirect3DSwapChain9* chain = nullptr;

				if (SUCCEEDED(device->GetSwapChain(0, &chain)) && chain != nullptr)
				{
					HookSwapChain(chain, label);
					chain->Release();
				}

				device->Release();
			}

			direct3d9On12->Release();
		}
		else
		{
			rcx::core::Log("vr: no D3D9On12 throwaway device (Direct3DCreate9On12Ex %s)", create9On12Ex != nullptr ? "failed" : "isn't exported");
		}

		DestroyWindow(window);

		if (vtableCount.load() == 0)
		{
			reason = "can't create the throwaway Direct3D 9 device (" + HresultText(firstResult) + ")";
			return false;
		}

		hooksInstalled = true;
		FindGameDevice();
		rcx::core::Log("vr: Direct3D 9 hooks installed (%s)", detoursActive ? (std::to_string(detoursInstalled) + " detours, " + std::to_string(detoursRefused) + " refused").c_str()
			: ("vtables only: " + detoursOff).c_str());
		return true;
	}

	void SetPresentObserver(const PresentObserver* observer)
	{
		presentObserver.store(observer);
	}

	void GetTextureLimit(uint32_t& width, uint32_t& height)
	{
		width = maxTextureWidth.load();
		height = maxTextureHeight.load();
	}

	bool IsDeviceFound()
	{
		return gameDevice.load() != nullptr;
	}

	bool BeginSession(::vr::IVRSystem* system, ::vr::IVRCompositor* newCompositor, std::string& reason)
	{
		if (system == nullptr || newCompositor == nullptr)
		{
			reason = "no OpenVR system or compositor";
			return false;
		}

		int32_t newAdapterIndex = -1;
		system->GetDXGIOutputInfo(&newAdapterIndex);
		headsetAdapter = system->GetD3D9AdapterIndex();
		::vr::ETrackedPropertyError propertyError = ::vr::TrackedProp_Success;
		float refresh = system->GetFloatTrackedDeviceProperty(::vr::k_unTrackedDeviceIndex_Hmd, ::vr::Prop_DisplayFrequency_Float, &propertyError);

		{
			std::unique_lock<std::timed_mutex> lock(deviceMutex, std::defer_lock);

			if (!lock.try_lock_for(kLuaLockLimit))
			{
				reason = "the game's render thread is stuck in the graphics driver (the VR hand-off couldn't be taken over)";
				return false;
			}

			compositor = newCompositor;
			adapterIndex = newAdapterIndex;
			stats.framesSubmitted = 0;
			stats.staleFrames = 0;
			stats.alreadySubmitted = 0;
			stats.framesNotReady = 0;
			stats.submitErrors = 0;
			stats.drawsWithoutSubmit = 0;
			stats.framesSkipped = 0;
			stats.waitTimeouts = 0;
			stats.lateSubmits = 0;
			stats.presentsPerSecond = 0.0;
			stats.submitsPerSecond = 0.0;
			stats.waitsPerSecond = 0.0;
			rateWindowStart = {};
			headsetFrameMs = propertyError == ::vr::TrackedProp_Success && refresh > 20.0f ? 1000.0 / refresh : 1000.0 / 90.0;
			stats.maxWaitMs = 0.0;
			stats.averageWaitMs = 0.0;
			stats.lastError.clear();
			PublishStats();
		}

		{
			std::lock_guard<std::mutex> lock(queueMutex);
			pending.clear();
		}

		queuedDropped = 0;
		presentsWaitingForDraw = 0;
		tripped = false;
		slowSubmits = 0;
		sessionActive = true;
		return true;
	}

	void EndSession()
	{
		sessionActive = false;

		{
			std::lock_guard<std::mutex> lock(queueMutex);
			pending.clear();
		}

		{
			std::lock_guard<std::mutex> lock(captureMutex);

			if (eyeTexture == nullptr)
			{
				recaptureArmed = false;
			}
		}

		std::unique_lock<std::timed_mutex> lock(deviceMutex, std::defer_lock);

		if (!lock.try_lock_for(kLuaLockLimit))
		{
			// The render thread is stuck in a driver call; with the session inactive it queues no frame, so it
			// never reaches the compositor again. Its objects are released when it next presents.
			releaseRequested = true;
			rcx::core::Log("vr: the render thread was busy for over %lld ms at the session end; its VR objects are released at its next present",
				static_cast<long long>(kLuaLockLimit.count()));
			return;
		}

		compositor = nullptr;
		ReleaseCompositorObjects();
		releaseRequested = true;
	}

	void BeginCapture(uint32_t width, uint32_t height)
	{
		{
			std::lock_guard<std::mutex> lock(attemptMutex);
			lastAttempt = CreateAttempt{};
		}

		captureWidth = width;
		captureHeight = height;
		recaptureArmed = false;
		captureArmed = true;
	}

	std::string DescribeGraphicsLayers()
	{
		std::string text;
		wchar_t systemFolder[MAX_PATH]{};
		UINT systemLength = GetSystemDirectoryW(systemFolder, MAX_PATH);
		HMODULE d3d9 = GetModuleHandleW(L"d3d9.dll");

		if (d3d9 != nullptr)
		{
			wchar_t path[MAX_PATH]{};
			DWORD length = GetModuleFileNameW(d3d9, path, MAX_PATH);
			bool system = systemLength > 0 && length > systemLength && _wcsnicmp(path, systemFolder, systemLength) == 0;
			char narrow[MAX_PATH]{};
			WideCharToMultiByte(CP_UTF8, 0, path, static_cast<int>(length), narrow, MAX_PATH - 1, nullptr, nullptr);
			text = std::string("d3d9.dll ") + (system ? "is Windows' own" : std::string("is not Windows' own (a proxy such as DXVK, ReShade or an ENB): ") + narrow);
		}

		struct Layer
		{
			const wchar_t* module;
			const char* name;
		};

		const Layer layers[] = {
			{L"vulkan-1.dll", "Vulkan (DXVK or an overlay)"},
			{L"d3d9on12.dll", "D3D9On12 (Windows can run Direct3D 9 on Direct3D 12; the game device's functions say whether it does here)"},
			{L"ReShade64.dll", "ReShade"},
			{L"dxgi.dll", "DXGI"},
			{L"GameOverlayRenderer64.dll", "Steam overlay"},
			{L"DiscordHook64.dll", "Discord overlay"},
			{L"RTSSHooks64.dll", "RivaTuner"},
			{L"graphics-hook64.dll", "OBS game capture"},
			{L"nvspcap64.dll", "NVIDIA capture (nvspcap64.dll: the driver loads it for ShadowPlay and the overlay, so it doesn't mean the overlay is on)"},
			{L"SpecialK64.dll", "Special K"},
		};

		std::string loaded;

		for (const Layer& layer : layers)
		{
			if (GetModuleHandleW(layer.module) != nullptr)
			{
				loaded += (loaded.empty() ? "" : ", ") + std::string(layer.name);
			}
		}

		text += "; loaded: " + (loaded.empty() ? std::string("no known overlay or graphics layer") : loaded);
		std::string modules;

		for (const auto& entry : LoadedModules())
		{
			const std::string& name = entry.first;
			bool graphics = name.compare(0, 2, "nv") == 0 || name.find("d3d9") != std::string::npos || name.find("optimus") != std::string::npos ||
				name.find("wrap") != std::string::npos || name.find("reshade") != std::string::npos || name.find("dxvk") != std::string::npos ||
				name.find("shaderapi") != std::string::npos || name.find("igd") == 0 || name.find("amd") == 0 || name.find("ati") == 0;

			if (graphics)
			{
				modules += (modules.empty() ? "" : ", ") + entry.second;
			}
		}

		text += "; graphics modules: " + (modules.empty() ? std::string("none") : modules);
		return text;
	}

	std::string DescribeAdapters()
	{
		std::string names;

		for (const std::string& name : adapterNames)
		{
			names += (names.empty() ? "" : ", ") + name;
		}

		int headset = headsetAdapter.load();
		int game = gameAdapter.load();
		std::string text = "adapters " + (names.empty() ? std::string("not read") : names) + "; the headset wants adapter " +
			(headset >= 0 ? std::to_string(headset) : std::string("unknown")) + ", the game's device is on " + (game >= 0 ? std::to_string(game) : std::string("unknown"));

		if (GpuMismatchLikely())
		{
			text += ". The game must run on the same graphics card as the headset: set gmod.exe (garrysmod\\bin\\win64\\gmod.exe) to High performance in Windows "
				"Settings > System > Display > Graphics, or in the NVIDIA or AMD control panel, then restart the game";
		}

		return text;
	}

	bool GpuMismatchLikely()
	{
		int headset = headsetAdapter.load();
		int game = gameAdapter.load();

		auto gpuOf = [](int adapter) -> std::string
		{
			return adapter >= 0 && static_cast<size_t>(adapter) < adapterGpus.size() ? adapterGpus[adapter] : std::string();
		};

		if (game >= 0 && headset >= 0)
		{
			return gpuOf(game) != gpuOf(headset);
		}

		size_t gpus = 0;

		for (size_t index = 0; index < adapterGpus.size(); index++)
		{
			bool seen = false;

			for (size_t before = 0; before < index; before++)
			{
				seen = seen || adapterGpus[before] == adapterGpus[index];
			}

			gpus += seen ? 0 : 1;
		}

		return gameDevice.load() == nullptr && gpus > 1;
	}

	void GetAdapters(int& headset, int& game, int& count)
	{
		headset = headsetAdapter.load();
		game = gameAdapter.load();
		count = static_cast<int>(adapterNames.size());
	}

	std::string DxgiAdapterName(const LUID& luid)
	{
		HMODULE dxgi = LoadLibraryExW(L"dxgi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
		using CreateFactoryFunction = HRESULT(WINAPI*)(REFIID, void**);
		auto createFactory = dxgi != nullptr ? reinterpret_cast<CreateFactoryFunction>(reinterpret_cast<void*>(GetProcAddress(dxgi, "CreateDXGIFactory1"))) : nullptr;
		IDXGIFactory1* factory = nullptr;
		std::string name = "an unknown GPU";

		if (createFactory != nullptr && SUCCEEDED(createFactory(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) && factory != nullptr)
		{
			IDXGIAdapter1* adapter = nullptr;

			for (UINT index = 0; SUCCEEDED(factory->EnumAdapters1(index, &adapter)); index++)
			{
				DXGI_ADAPTER_DESC1 description{};

				if (SUCCEEDED(adapter->GetDesc1(&description)) && description.AdapterLuid.LowPart == luid.LowPart && description.AdapterLuid.HighPart == luid.HighPart)
				{
					name = Narrow(description.Description) + " (DXGI adapter " + std::to_string(index) + ")";
				}

				adapter->Release();
			}

			factory->Release();
		}

		return name;
	}

	std::string DescribeDetours()
	{
		if (!detoursActive)
		{
			return "no detours (" + (detoursOff.empty() ? std::string("hooks not installed") : detoursOff) + ")";
		}

		return std::to_string(detoursInstalled) + " detours (" + std::to_string(detoursChained) + " chained onto another program's), " + std::to_string(detoursRefused) +
			" refused";
	}

	std::string DetourReport()
	{
		std::string text;

		for (const std::string& line : detourReport)
		{
			text += (text.empty() ? "" : "; ") + line;
		}

		return text;
	}

	std::string DescribeHooks()
	{
		std::string tables;
		int count = vtableCount.load();

		for (int index = 0; index < count; index++)
		{
			tables += (index == 0 ? "" : ", ") + std::string(vtables[index].label) + " in " + vtables[index].owner;
		}

		int game = gameVtable.load();
		uint64_t presents = 0;

		{
			std::lock_guard<std::mutex> lock(statsMutex);
			presents = publishedStats.presents;
		}

		std::string device = gameDevice.load() == nullptr
			? "the game's device hasn't presented through any of the " + std::to_string(count) + " hooked device vtables (" + tables + ") or " +
				std::to_string(swapChainCount.load()) + " swap chain vtables"
			: "the game's device, found by " + deviceFoundBy + ", is on the " + (game >= 0 ? vtables[game].label + " vtable, its functions in " + vtables[game].owner : std::string("unknown vtable")) + " (" + std::to_string(presents) +
				" presents, " + std::to_string(swapChainPresents.load()) + " through its swap chain)";
		device += "; the device scan: " + (scanResult.empty() ? std::string("not run") : scanResult);
		IDirect3DDevice9* found = gameDevice.load();

		if (found != nullptr && gameOn12.empty())
		{
			LUID luid{};

			if (!D3D12AdapterLuid(found, luid))
			{
				gameOn12 = "not on D3D9On12";
			}
			else
			{
				gameOn12 = "on D3D9On12, its Direct3D 12 device on " + DxgiAdapterName(luid);
			}
		}

		if (!gameOn12.empty())
		{
			device += "; the game's device is " + gameOn12;
		}

		char limit[64]{};
		std::snprintf(limit, sizeof(limit), "%ux%u", maxTextureWidth.load(), maxTextureHeight.load());
		return device + "; texture limit " + (maxTextureWidth.load() > 0 ? std::string(limit) : std::string("not read")) + "; " +
			std::to_string(armedBinds.load()) + " render targets bound while armed; " + DescribeDetours() +
			(gameVptrOwner.empty() ? std::string() : "; the device's vtable is in " + gameVptrOwner) + "; " + DescribeAdapters() + "; " + DescribeGraphicsLayers();
	}

	bool EndCapture(std::string& reason, std::string& kind)
	{
		uint32_t width = captureWidth.load();
		uint32_t height = captureHeight.load();
		captureArmed = false;
		kind.clear();

		if (!hooksInstalled)
		{
			reason = "VR isn't started";
			kind = "not_started";
			return false;
		}

		std::lock_guard<std::mutex> lock(captureMutex);

		if (eyeTexture == nullptr || eyeSurface.load() == nullptr)
		{
			CreateAttempt attempt{};

			{
				std::lock_guard<std::mutex> attemptLock(attemptMutex);
				attempt = lastAttempt;
			}

			char detail[320]{};

			if (!attempt.seen)
			{
				kind = gameDevice.load() == nullptr ? "no_device" : "not_seen";
				std::snprintf(detail, sizeof(detail), "no render target was created or bound at that size while capturing");

				// A creation the engine made later (on the render thread) is still taken.
				recaptureArmed = true;
			}
			else if (attempt.result < 0)
			{
				kind = "refused";
				std::snprintf(detail, sizeof(detail), "Direct3D refused the render target: %ux%u format %u failed with HRESULT 0x%08lx", attempt.width, attempt.height,
					attempt.format, static_cast<unsigned long>(attempt.result));
			}
			else
			{
				kind = "other_size";
				std::snprintf(detail, sizeof(detail), "the last render target made was %ux%u (asked %ux%u) format %u, not the eye size", attempt.actualWidth,
					attempt.actualHeight, attempt.width, attempt.height, attempt.format);
			}

			lastFailureKind = kind;
			reason = "no eye texture of " + std::to_string(width) + "x" + std::to_string(height) + " was captured: " + detail + "; " + DescribeHooks();
			rcx::core::LogToFile("EndTarget failed (%s): %s", kind.c_str(), reason.c_str());
			return false;
		}

		if (eyeWidth != width || eyeHeight != height)
		{
			kind = "other_size";
			lastFailureKind = kind;
			reason = "the captured eye texture is " + std::to_string(eyeWidth) + "x" + std::to_string(eyeHeight) + ", not the asked size";
			return false;
		}

		lastFailureKind.clear();
		return true;
	}

	bool Tripped(std::string& reason)
	{
		if (!tripped.load())
		{
			return false;
		}

		std::lock_guard<std::mutex> lock(tripMutex);
		reason = tripReason;
		return true;
	}

	uint64_t SubmittedFrames()
	{
		return submittedCount.load();
	}

	void NoteWait()
	{
		waitCount++;

		if (detoursArmed && presentCount.load() >= kCleanPresents)
		{
			detoursArmed = false;
			rcx::engine::Disarm(kDetourFeature);
			rcx::core::Log("vr: the detours ran %llu presents cleanly; their crash latch is cleared", static_cast<unsigned long long>(presentCount.load()));
		}
	}

	bool QueueSubmit(uint32_t frameId, const ::vr::HmdMatrix34_t& headPose, std::string& reason)
	{
		if (!sessionActive.load())
		{
			reason = "VR isn't started";
			return false;
		}

		if (eyeSurface.load() == nullptr)
		{
			reason = "no eye texture captured";
			return false;
		}

		if (backendFailed.load())
		{
			std::lock_guard<std::mutex> lock(statsMutex);
			reason = publishedStats.lastError;
			return false;
		}

		std::lock_guard<std::mutex> lock(queueMutex);

		while (pending.size() >= kMaxQueuedSubmits)
		{
			pending.pop_front();
			queuedDropped++;
		}

		PendingFrame frame{};
		frame.frameId = frameId;
		frame.wait = waitCount.load();
		frame.headPose = headPose;
		pending.push_back(frame);
		return true;
	}

	void SetSubmitWait(double milliseconds)
	{
		askedSubmitWaitMs = milliseconds < 0.0 ? -1.0 : milliseconds;
	}

	void SetBounds(int eye, const TextureBounds& newBounds)
	{
		if (eye < 0 || eye > 1)
		{
			return;
		}

		std::lock_guard<std::mutex> lock(queueMutex);
		bounds[eye] = newBounds;
	}

	Stats GetStats()
	{
		Stats copy{};

		{
			std::lock_guard<std::mutex> lock(statsMutex);
			copy = publishedStats;
		}

		copy.queuedDropped = queuedDropped.load();
		copy.presentsWaitingForDraw = presentsWaitingForDraw.load();
		copy.hooksInstalled = hooksInstalled;
		copy.deviceFound = gameDevice.load() != nullptr;
		copy.eyeCaptured = eyeSurface.load() != nullptr;

		std::lock_guard<std::mutex> lock(captureMutex);
		copy.eyeWidth = eyeWidth;
		copy.eyeHeight = eyeHeight;
		copy.eyeFormat = eyeFormat;
		copy.capturedVia = capturedVia.load();
		copy.nestedPresents = nestedPresents.load();
		copy.lastCaptureFailure = lastFailureKind;
		copy.hooks = DescribeHooks();
		return copy;
	}
}
