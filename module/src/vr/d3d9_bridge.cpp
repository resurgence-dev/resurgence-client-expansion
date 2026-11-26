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
#include <d3d11.h>
#include <dxgi.h>

#include "core/core.h"
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
		constexpr int kCopyCount = 2;

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
				case Backend::Failed:
					return "failed";
				default:
					return "none yet";
			}
		}

		struct PendingFrame
		{
			uint32_t frameId = 0;
			::vr::HmdMatrix34_t headPose{};
		};

		struct CopyTexture
		{
			IDirect3DTexture9* texture = nullptr;
			IDirect3DSurface9* surface = nullptr;
			HANDLE handle = nullptr;
			ID3D11Texture2D* compositorTexture = nullptr;
			dxvk::ID3D9VkInteropTexture* interopTexture = nullptr;
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

		void HookVtable(IDirect3DDevice9* device, bool ex, const std::string& label);

		// The table the game's device presented through (-1 until it's found).
		std::atomic<int> gameVtable{-1};

		// The Direct3D 9 adapters ("0: NVIDIA GeForce ..."), read at InstallHooks on the Lua thread; the
		// adapter the headset wants (IVRSystem::GetD3D9AdapterIndex) and the game's device's own
		// (GetCreationParameters), -1 until known.
		std::vector<std::string> adapterNames;
		std::atomic<int> headsetAdapter{-1};
		std::atomic<int> gameAdapter{-1};
		constexpr UINT kMaxAdapters = 4;

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
		std::mutex deviceMutex;
		::vr::IVRCompositor* compositor = nullptr;
		int32_t adapterIndex = -1;
		Backend backend = Backend::Unknown;
		HMODULE dxgiModule = nullptr;
		HMODULE d3d11Module = nullptr;
		ID3D11Device* compositorDevice = nullptr;
		ID3D11DeviceContext* compositorContext = nullptr;
		dxvk::ID3D9VkInteropDevice* interopDevice = nullptr;
		CopyTexture copies[kCopyCount]{};
		IDirect3DQuery9* drawQuery = nullptr;
		uint32_t copiesGeneration = 0;
		int nextCopy = 0;
		uint64_t lastSubmittedWait = 0;
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
			for (CopyTexture& copy : copies)
			{
				SafeRelease(copy.compositorTexture);
				SafeRelease(copy.interopTexture);
				SafeRelease(copy.surface);
				SafeRelease(copy.texture);
				copy.handle = nullptr;
			}

			SafeRelease(drawQuery);
			SafeRelease(interopDevice);

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

		// deviceMutex held, device thread.
		bool ChooseBackend(IDirect3DDevice9* device)
		{
			if (backend == Backend::Vulkan || backend == Backend::Shared)
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
			backend = Backend::Shared;
			rcx::core::Log("vr: native Direct3D 9; submitting through a Direct3D 11 shared texture");
			return true;
		}

		// deviceMutex held.
		bool EnsureCompositorDevice()
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

			if (adapterIndex >= 0 && SUCCEEDED(createFactory(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) && factory != nullptr)
			{
				if (FAILED(factory->EnumAdapters1(static_cast<UINT>(adapterIndex), &adapter)))
				{
					adapter = nullptr;
				}
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

			rcx::core::Log("vr: Direct3D 11 device ready on adapter %d", adapterIndex);
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
				if (!EnsureCompositorDevice())
				{
					return false;
				}

				if (drawQuery == nullptr && FAILED(device->CreateQuery(D3DQUERYTYPE_EVENT, &drawQuery)))
				{
					drawQuery = nullptr;
					Fail("the device can't create an event query");
					return false;
				}
			}

			for (CopyTexture& copy : copies)
			{
				if (copy.texture != nullptr && (backend == Backend::Vulkan || copy.compositorTexture != nullptr))
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
					stats.lastCompositorError = static_cast<int>(error);
					SetError("compositor submit error " + std::to_string(static_cast<int>(error)));
					return false;
				}
			}

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

		// deviceMutex held, device thread.
		void SubmitShared(CopyTexture& copy, const PendingFrame& frame)
		{
			auto waitStart = std::chrono::steady_clock::now();
			drawQuery->Issue(D3DISSUE_END);
			bool done = false;

			while (!done)
			{
				HRESULT queryResult = drawQuery->GetData(nullptr, 0, D3DGETDATA_FLUSH);

				if (queryResult == S_OK)
				{
					done = true;
					break;
				}

				if (queryResult != S_FALSE || MillisecondsSince(waitStart) > kWaitTimeoutMs)
				{
					break;
				}

				YieldProcessor();
			}

			stats.lastWaitMs = MillisecondsSince(waitStart);
			stats.maxWaitMs = stats.lastWaitMs > stats.maxWaitMs ? stats.lastWaitMs : stats.maxWaitMs;
			stats.averageWaitMs = stats.framesSubmitted == 0 ? stats.lastWaitMs : stats.averageWaitMs * 0.95 + stats.lastWaitMs * 0.05;

			if (!done)
			{
				stats.waitTimeouts++;
				stats.submitErrors++;
				SetError("the eye draws didn't finish within the wait limit");
				return;
			}

			::vr::VRTextureWithPose_t texture{};
			texture.handle = copy.compositorTexture;
			texture.eType = ::vr::TextureType_DirectX;
			texture.eColorSpace = ::vr::ColorSpace_Gamma;
			texture.mDeviceToAbsoluteTracking = frame.headPose;

			auto submitStart = std::chrono::steady_clock::now();

			if (SubmitEyes(texture, ::vr::Submit_TextureWithPose))
			{
				stats.framesSubmitted++;
			}

			stats.lastSubmitMs = MillisecondsSince(submitStart);
		}

		// deviceMutex held, device thread.
		void SubmitFrame(IDirect3DDevice9* device, const PendingFrame& frame)
		{
			if (compositor == nullptr || !EnsureCopies(device))
			{
				stats.submitErrors++;
				return;
			}

			// The render thread can fall a frame behind the Lua thread; a second submit after one wait is refused (AlreadySubmitted).
			uint64_t waits = waitCount.load();

			if (waits == lastSubmittedWait)
			{
				stats.framesSkipped++;
				return;
			}

			lastSubmittedWait = waits;

			CopyTexture& copy = copies[nextCopy];
			nextCopy = (nextCopy + 1) % kCopyCount;

			HRESULT copyResult = device->StretchRect(eyeSurface.load(), nullptr, copy.surface, nullptr, D3DTEXF_NONE);

			if (FAILED(copyResult))
			{
				stats.submitErrors++;
				SetError("can't copy the eye texture (" + HresultText(copyResult) + ")");
				return;
			}

			if (backend == Backend::Vulkan)
			{
				SubmitVulkan(copy, frame);
			}
			else
			{
				SubmitShared(copy, frame);
			}
		}

		void OnPresent(IDirect3DDevice9* device, bool isEx)
		{
			if (insideBridge)
			{
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

				if (table < 0 && known == device)
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

			std::lock_guard<std::mutex> lock(deviceMutex);
			stats.presents++;
			stats.deviceIsEx = stats.deviceIsEx || isEx;
			stats.presentOnOtherThread = GetCurrentThreadId() != luaThreadId;

			if (releaseRequested.exchange(false))
			{
				ReleaseDeviceResources();
				backend = Backend::Unknown;
				backendFailed = false;
			}

			if (haveFrame)
			{
				SubmitFrame(device, frame);
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
				std::lock_guard<std::mutex> lock(deviceMutex);
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

		HRESULT STDMETHODCALLTYPE HookPresent(IDirect3DDevice9* device, const RECT* sourceRect, const RECT* destinationRect, HWND window, const RGNDATA* dirtyRegion)
		{
			OnPresent(device, false);
			const PresentObserver* observer = ObserverFor(device);

			if (observer != nullptr)
			{
				observer->beforePresent(device);
			}

			bool outer = insideDevicePresent;
			insideDevicePresent = true;
			HRESULT result = TableFor(device).present(device, sourceRect, destinationRect, window, dirtyRegion);
			insideDevicePresent = outer;

			if (observer != nullptr)
			{
				observer->afterPresent(device);
			}

			return result;
		}

		HRESULT STDMETHODCALLTYPE HookPresentEx(IDirect3DDevice9Ex* device, const RECT* sourceRect, const RECT* destinationRect, HWND window, const RGNDATA* dirtyRegion, DWORD flags)
		{
			OnPresent(device, true);
			const PresentObserver* observer = ObserverFor(device);

			if (observer != nullptr)
			{
				observer->beforePresent(device);
			}

			bool outer = insideDevicePresent;
			insideDevicePresent = true;
			HRESULT result = TableFor(device).presentEx(device, sourceRect, destinationRect, window, dirtyRegion, flags);
			insideDevicePresent = outer;

			if (observer != nullptr)
			{
				observer->afterPresent(device);
			}

			return result;
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

		HRESULT STDMETHODCALLTYPE HookReset(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* parameters)
		{
			OnReset(device);
			const PresentObserver* observer = ObserverFor(device);

			if (observer != nullptr)
			{
				observer->onReset(device);
			}

			return TableFor(device).reset(device, parameters);
		}

		HRESULT STDMETHODCALLTYPE HookResetEx(IDirect3DDevice9Ex* device, D3DPRESENT_PARAMETERS* parameters, D3DDISPLAYMODEEX* displayMode)
		{
			OnReset(device);
			const PresentObserver* observer = ObserverFor(device);

			if (observer != nullptr)
			{
				observer->onReset(device);
			}

			return TableFor(device).resetEx(device, parameters, displayMode);
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

		HRESULT STDMETHODCALLTYPE HookSetRenderTarget(IDirect3DDevice9* device, DWORD index, IDirect3DSurface9* surface)
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

			return TableFor(device).setRenderTarget(device, index, surface);
		}

		HRESULT STDMETHODCALLTYPE HookCreateTexture(IDirect3DDevice9* device, UINT width, UINT height, UINT levels, DWORD usage, D3DFORMAT format,
			D3DPOOL pool, IDirect3DTexture9** texture, HANDLE* sharedHandle)
		{
			HRESULT result = TableFor(device).createTexture(device, width, height, levels, usage, format, pool, texture, sharedHandle);

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

			void* unused = nullptr;
			bool patched = PatchSlot(vtable, kSlotCreateTexture, reinterpret_cast<void*>(&HookCreateTexture), &unused)
				&& PatchSlot(vtable, kSlotSetRenderTarget, reinterpret_cast<void*>(&HookSetRenderTarget), &unused)
				&& PatchSlot(vtable, kSlotReset, reinterpret_cast<void*>(&HookReset), &unused)
				&& PatchSlot(vtable, kSlotPresent, reinterpret_cast<void*>(&HookPresent), &unused);

			if (patched && ex)
			{
				patched = PatchSlot(vtable, kSlotResetEx, reinterpret_cast<void*>(&HookResetEx), &unused)
					&& PatchSlot(vtable, kSlotPresentEx, reinterpret_cast<void*>(&HookPresentEx), &unused);
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
			bool patched = PatchSlot(vtable, kSlotSwapChainPresent, reinterpret_cast<void*>(&HookSwapChainPresent), &unused);
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

				for (UINT adapter = 0; adapter < adapterCount; adapter++)
				{
					D3DADAPTER_IDENTIFIER9 identifier{};
					std::string name = SUCCEEDED(enumerator->GetAdapterIdentifier(adapter, 0, &identifier)) ? std::string(identifier.Description) : std::string("unknown");
					adapterNames.push_back(std::to_string(adapter) + ": " + name);
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

		DestroyWindow(window);

		if (vtableCount.load() == 0)
		{
			reason = "can't create the throwaway Direct3D 9 device (" + HresultText(firstResult) + ")";
			return false;
		}

		hooksInstalled = true;
		rcx::core::Log("vr: Direct3D 9 hooks installed");
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

		{
			std::lock_guard<std::mutex> lock(deviceMutex);
			compositor = newCompositor;
			adapterIndex = newAdapterIndex;
			stats.framesSubmitted = 0;
			stats.submitErrors = 0;
			stats.drawsWithoutSubmit = 0;
			stats.framesSkipped = 0;
			stats.waitTimeouts = 0;
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

		std::lock_guard<std::mutex> lock(deviceMutex);
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
			{L"ReShade64.dll", "ReShade"},
			{L"dxgi.dll", "DXGI"},
			{L"GameOverlayRenderer64.dll", "Steam overlay"},
			{L"DiscordHook64.dll", "Discord overlay"},
			{L"RTSSHooks64.dll", "RivaTuner"},
			{L"graphics-hook64.dll", "OBS game capture"},
			{L"nvspcap64.dll", "NVIDIA overlay"},
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

		if (game >= 0 && headset >= 0)
		{
			return game != headset;
		}

		return gameDevice.load() == nullptr && adapterNames.size() > 1;
	}

	void GetAdapters(int& headset, int& game, int& count)
	{
		headset = headsetAdapter.load();
		game = gameAdapter.load();
		count = static_cast<int>(adapterNames.size());
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
			: "the game's device presents through the " + (game >= 0 ? vtables[game].label : std::string("unknown")) + " vtable (" + std::to_string(presents) + " presents, " +
				std::to_string(swapChainPresents.load()) + " through its swap chain)";

		char limit[64]{};
		std::snprintf(limit, sizeof(limit), "%ux%u", maxTextureWidth.load(), maxTextureHeight.load());
		return device + "; texture limit " + (maxTextureWidth.load() > 0 ? std::string(limit) : std::string("not read")) + "; " +
			std::to_string(armedBinds.load()) + " render targets bound while armed; " + DescribeAdapters() + "; " + DescribeGraphicsLayers();
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

	void NoteWait()
	{
		waitCount++;
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
		frame.headPose = headPose;
		pending.push_back(frame);
		return true;
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
		copy.lastCaptureFailure = lastFailureKind;
		copy.hooks = DescribeHooks();
		return copy;
	}
}
