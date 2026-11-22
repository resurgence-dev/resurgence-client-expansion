#include "vr/d3d9_bridge.h"

#include <atomic>
#include <cstdio>
#include <chrono>
#include <deque>
#include <mutex>

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

		PresentFunction originalPresent = nullptr;
		PresentExFunction originalPresentEx = nullptr;
		CreateTextureFunction originalCreateTexture = nullptr;
		SetRenderTargetFunction originalSetRenderTarget = nullptr;
		ResetFunction originalReset = nullptr;
		ResetExFunction originalResetEx = nullptr;

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

				rcx::core::Log("vr: found the game's Direct3D 9 device (Present%s, %s thread)", isEx ? "Ex" : "",
					GetCurrentThreadId() == luaThreadId ? "Lua" : "another");
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

			HRESULT result = originalPresent(device, sourceRect, destinationRect, window, dirtyRegion);

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

			HRESULT result = originalPresentEx(device, sourceRect, destinationRect, window, dirtyRegion, flags);

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

			return originalReset(device, parameters);
		}

		HRESULT STDMETHODCALLTYPE HookResetEx(IDirect3DDevice9Ex* device, D3DPRESENT_PARAMETERS* parameters, D3DDISPLAYMODEEX* displayMode)
		{
			OnReset(device);
			const PresentObserver* observer = ObserverFor(device);

			if (observer != nullptr)
			{
				observer->onReset(device);
			}

			return originalResetEx(device, parameters, displayMode);
		}

		HRESULT STDMETHODCALLTYPE HookSetRenderTarget(IDirect3DDevice9* device, DWORD index, IDirect3DSurface9* surface)
		{
			if (index == 0 && surface != nullptr && surface == eyeSurface.load() && !insideBridge)
			{
				eyeDrawn = true;
			}

			return originalSetRenderTarget(device, index, surface);
		}

		HRESULT STDMETHODCALLTYPE HookCreateTexture(IDirect3DDevice9* device, UINT width, UINT height, UINT levels, DWORD usage, D3DFORMAT format,
			D3DPOOL pool, IDirect3DTexture9** texture, HANDLE* sharedHandle)
		{
			HRESULT result = originalCreateTexture(device, width, height, levels, usage, format, pool, texture, sharedHandle);

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

			std::lock_guard<std::mutex> lock(captureMutex);
			eyeTexture = *texture;
			eyeWidth = width;
			eyeHeight = height;
			eyeFormat = static_cast<uint32_t>(format);
			eyeSurface = surface;
			eyeGeneration++;
			captureArmed = false;
			recaptureArmed = false;
			rcx::core::Log("vr: captured the eye texture, %ux%u format %u", width, height, static_cast<unsigned>(format));
			return result;
		}
	}

	bool InstallHooks(std::string& reason)
	{
		if (hooksInstalled)
		{
			return true;
		}

		luaThreadId = GetCurrentThreadId();
		HMODULE d3d9 = GetModuleHandleW(L"d3d9.dll");

		if (d3d9 == nullptr)
		{
			reason = "d3d9.dll isn't loaded: the game isn't running on Direct3D 9";
			return false;
		}

		auto create9Ex = reinterpret_cast<Direct3DCreate9ExFunction>(reinterpret_cast<void*>(GetProcAddress(d3d9, "Direct3DCreate9Ex")));

		if (create9Ex == nullptr)
		{
			reason = "the game's d3d9.dll has no Direct3DCreate9Ex";
			return false;
		}

		IDirect3D9Ex* direct3d = nullptr;

		if (FAILED(create9Ex(D3D_SDK_VERSION, &direct3d)) || direct3d == nullptr)
		{
			reason = "Direct3DCreate9Ex failed";
			return false;
		}

		HWND window = CreateWindowExW(0, L"STATIC", L"rcx", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, nullptr, nullptr);

		if (window == nullptr)
		{
			direct3d->Release();
			reason = "can't create a window for the throwaway device";
			return false;
		}

		D3DPRESENT_PARAMETERS parameters{};
		parameters.Windowed = TRUE;
		parameters.SwapEffect = D3DSWAPEFFECT_DISCARD;
		parameters.BackBufferFormat = D3DFMT_UNKNOWN;
		parameters.BackBufferWidth = 1;
		parameters.BackBufferHeight = 1;
		parameters.hDeviceWindow = window;

		DWORD flags = D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_NOWINDOWCHANGES | D3DCREATE_FPU_PRESERVE;
		IDirect3DDevice9Ex* device = nullptr;
		HRESULT result = direct3d->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window, flags, &parameters, nullptr, &device);

		if (FAILED(result) || device == nullptr)
		{
			direct3d->Release();
			DestroyWindow(window);
			reason = "can't create the throwaway Direct3D 9 device (" + HresultText(result) + ")";
			return false;
		}

		void** vtable = *reinterpret_cast<void***>(device);
		device->Release();
		direct3d->Release();
		DestroyWindow(window);

		if (vtable == nullptr)
		{
			reason = "the throwaway device has no vtable";
			return false;
		}

		if (!rcx::core::PinModule())
		{
			reason = "can't pin the module in memory";
			return false;
		}

		bool patched = PatchSlot(vtable, kSlotCreateTexture, reinterpret_cast<void*>(&HookCreateTexture), reinterpret_cast<void**>(&originalCreateTexture))
			&& PatchSlot(vtable, kSlotSetRenderTarget, reinterpret_cast<void*>(&HookSetRenderTarget), reinterpret_cast<void**>(&originalSetRenderTarget))
			&& PatchSlot(vtable, kSlotReset, reinterpret_cast<void*>(&HookReset), reinterpret_cast<void**>(&originalReset))
			&& PatchSlot(vtable, kSlotResetEx, reinterpret_cast<void*>(&HookResetEx), reinterpret_cast<void**>(&originalResetEx))
			&& PatchSlot(vtable, kSlotPresentEx, reinterpret_cast<void*>(&HookPresentEx), reinterpret_cast<void**>(&originalPresentEx))
			&& PatchSlot(vtable, kSlotPresent, reinterpret_cast<void*>(&HookPresent), reinterpret_cast<void**>(&originalPresent));

		if (!patched)
		{
			reason = "can't patch the Direct3D 9 device's vtable";
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

	bool EndCapture(std::string& reason)
	{
		uint32_t width = captureWidth.load();
		uint32_t height = captureHeight.load();
		captureArmed = false;

		if (!hooksInstalled)
		{
			reason = "VR isn't started";
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
				std::snprintf(detail, sizeof(detail), "no render target was created at all while capturing (a target that already existed isn't created again: use a new name)");
			}
			else if (attempt.result < 0)
			{
				std::snprintf(detail, sizeof(detail), "Direct3D refused the render target: %ux%u format %u failed with HRESULT 0x%08lx", attempt.width, attempt.height,
					attempt.format, static_cast<unsigned long>(attempt.result));
			}
			else
			{
				std::snprintf(detail, sizeof(detail), "the last render target made was %ux%u (asked %ux%u) format %u, not the eye size", attempt.actualWidth,
					attempt.actualHeight, attempt.width, attempt.height, attempt.format);
			}

			char limit[96]{};
			std::snprintf(limit, sizeof(limit), "; the device's texture limit is %ux%u", maxTextureWidth.load(), maxTextureHeight.load());
			reason = "no eye texture of " + std::to_string(width) + "x" + std::to_string(height) + " was captured: " + detail +
				(maxTextureWidth.load() > 0 ? limit : "");
			return false;
		}

		if (eyeWidth != width || eyeHeight != height)
		{
			reason = "the captured eye texture is " + std::to_string(eyeWidth) + "x" + std::to_string(eyeHeight) + ", not the asked size";
			return false;
		}

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
		return copy;
	}
}
