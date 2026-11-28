/*
	The D3D12 hand-off the VR bridge uses for a game device on D3D9On12 (rcx 0.78), outside the game and
	without SteamVR: a D3D9On12 device, a render target copy, and for each of 120 frames the bridge's
	sequence (StretchRect into the copy, UnwrapUnderlyingResource against a queue of our own, COMMON to
	PIXEL_SHADER_RESOURCE and back, Signal, ReturnUnderlyingResource with that fence value). The D3D12
	debug layer, when this machine has it, checks the resource states. Only SteamVR's Submit between the two
	transitions isn't run. Skipped with a note when no D3D9On12 device can be made. Exits non-zero on a failure.
*/
#include <cstdio>
#include <string>

#include <windows.h>
#include <d3d9.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <d3d9on12.h>

namespace
{
	int failures = 0;

	void Check(bool condition, const std::string& label)
	{
		std::printf("%s   %s\n", condition ? "ok" : "FAIL", label.c_str());

		if (!condition)
		{
			failures++;
		}
	}

	bool Transition(ID3D12CommandAllocator* allocator, ID3D12GraphicsCommandList* list, ID3D12CommandQueue* queue, ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
		D3D12_RESOURCE_STATES after)
	{
		if (FAILED(list->Reset(allocator, nullptr)))
		{
			return false;
		}

		D3D12_RESOURCE_BARRIER barrier{};
		barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barrier.Transition.pResource = resource;
		barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		barrier.Transition.StateBefore = before;
		barrier.Transition.StateAfter = after;
		list->ResourceBarrier(1, &barrier);

		if (FAILED(list->Close()))
		{
			return false;
		}

		ID3D12CommandList* lists[] = {list};
		queue->ExecuteCommandLists(1, lists);
		return true;
	}

	// The debug layer's errors since the last call (a wrong resource state is one), the first few printed; warnings
	// are D3D9On12's own (its ColorFill clears without a clear value) and only counted in a note.
	int TakeDebugMessages(ID3D12InfoQueue* info)
	{
		if (info == nullptr)
		{
			return 0;
		}

		int problems = 0;
		int warnings = 0;
		UINT64 count = info->GetNumStoredMessages();

		for (UINT64 index = 0; index < count; index++)
		{
			SIZE_T length = 0;
			info->GetMessage(index, nullptr, &length);
			D3D12_MESSAGE* message = static_cast<D3D12_MESSAGE*>(malloc(length));

			if (message != nullptr && SUCCEEDED(info->GetMessage(index, message, &length)))
			{
				warnings += message->Severity == D3D12_MESSAGE_SEVERITY_WARNING ? 1 : 0;

				if (message->Severity < D3D12_MESSAGE_SEVERITY_WARNING)
				{
					if (problems < 5)
					{
						std::printf("note debug layer: %s\n", message->pDescription);
					}

					problems++;
				}
			}

			free(message);
		}

		info->ClearStoredMessages();
		std::printf("note the debug layer gave %d warning(s), all D3D9On12's own\n", warnings);
		return problems;
	}
}

int main()
{
	HMODULE d3d12Module = LoadLibraryW(L"d3d12.dll");
	using DebugFunction = HRESULT(WINAPI*)(REFIID, void**);
	auto getDebug = d3d12Module != nullptr ? reinterpret_cast<DebugFunction>(reinterpret_cast<void*>(GetProcAddress(d3d12Module, "D3D12GetDebugInterface"))) : nullptr;
	ID3D12Debug* debug = nullptr;
	bool debugLayer = getDebug != nullptr && SUCCEEDED(getDebug(__uuidof(ID3D12Debug), reinterpret_cast<void**>(&debug))) && debug != nullptr;

	if (debugLayer)
	{
		debug->EnableDebugLayer();
	}

	std::printf("note the D3D12 debug layer is %s\n", debugLayer ? "on" : "not installed here (resource states aren't checked)");

	HMODULE d3d9 = LoadLibraryW(L"d3d9.dll");
	auto create9On12Ex = d3d9 != nullptr ? reinterpret_cast<PFN_Direct3DCreate9On12Ex>(reinterpret_cast<void*>(GetProcAddress(d3d9, "Direct3DCreate9On12Ex"))) : nullptr;
	D3D9ON12_ARGS arguments{};
	arguments.Enable9On12 = TRUE;
	IDirect3D9Ex* direct3d = nullptr;
	HWND window = CreateWindowExW(0, L"STATIC", L"rcx_d3d9on12_test", WS_POPUP, 0, 0, 64, 64, nullptr, nullptr, nullptr, nullptr);
	IDirect3DDevice9Ex* device = nullptr;

	if (create9On12Ex != nullptr && SUCCEEDED(create9On12Ex(D3D_SDK_VERSION, &arguments, 1, &direct3d)) && direct3d != nullptr && window != nullptr)
	{
		D3DPRESENT_PARAMETERS parameters{};
		parameters.Windowed = TRUE;
		parameters.SwapEffect = D3DSWAPEFFECT_DISCARD;
		parameters.BackBufferFormat = D3DFMT_UNKNOWN;
		parameters.hDeviceWindow = window;
		direct3d->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &parameters, nullptr, &device);
	}

	if (device == nullptr)
	{
		std::printf("note no D3D9On12 device here: skipped\n");
		return 0;
	}

	IDirect3DDevice9On12* on12 = nullptr;
	ID3D12Device* device12 = nullptr;
	Check(SUCCEEDED(device->QueryInterface(__uuidof(IDirect3DDevice9On12), reinterpret_cast<void**>(&on12))) && on12 != nullptr, "the device answers IDirect3DDevice9On12");
	Check(on12 != nullptr && SUCCEEDED(on12->GetD3D12Device(__uuidof(ID3D12Device), reinterpret_cast<void**>(&device12))) && device12 != nullptr, "and gives its Direct3D 12 device");

	if (device12 == nullptr)
	{
		std::printf("%d failure(s)\n", failures);
		return 1;
	}

	ID3D12InfoQueue* info = nullptr;

	if (debugLayer && SUCCEEDED(device12->QueryInterface(__uuidof(ID3D12InfoQueue), reinterpret_cast<void**>(&info))))
	{
		info->ClearStoredMessages();
	}

	ID3D12CommandQueue* queue = nullptr;
	ID3D12Fence* fence = nullptr;
	ID3D12CommandAllocator* allocator = nullptr;
	ID3D12GraphicsCommandList* list = nullptr;
	D3D12_COMMAND_QUEUE_DESC queueDescription{};
	queueDescription.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	bool made = SUCCEEDED(device12->CreateCommandQueue(&queueDescription, __uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&queue))) &&
		SUCCEEDED(device12->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void**>(&fence))) &&
		SUCCEEDED(device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), reinterpret_cast<void**>(&allocator))) &&
		SUCCEEDED(device12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, __uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void**>(&list))) &&
		SUCCEEDED(list->Close());
	Check(made, "a queue, fence, allocator and list on D3D9On12's Direct3D 12 device");

	IDirect3DTexture9* eye = nullptr;
	IDirect3DTexture9* copy = nullptr;
	IDirect3DSurface9* eyeSurface = nullptr;
	IDirect3DSurface9* copySurface = nullptr;
	made = made && SUCCEEDED(device->CreateTexture(256, 128, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &eye, nullptr)) &&
		SUCCEEDED(device->CreateTexture(256, 128, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &copy, nullptr)) &&
		SUCCEEDED(eye->GetSurfaceLevel(0, &eyeSurface)) && SUCCEEDED(copy->GetSurfaceLevel(0, &copySurface));
	Check(made, "an eye texture and a copy");

	UINT64 value = 0;
	int unwrapFailures = 0;
	int barrierFailures = 0;
	int returnFailures = 0;
	ID3D12Resource* first = nullptr;
	bool sameResource = true;
	ULONGLONG start = GetTickCount64();

	for (int frame = 0; made && frame < 120; frame++)
	{
		// As the bridge does: the copy is only reused once the GPU has finished its last use.
		if (fence->GetCompletedValue() < value)
		{
			HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
			fence->SetEventOnCompletion(value, event);
			WaitForSingleObject(event, 2000);
			CloseHandle(event);
		}

		device->ColorFill(eyeSurface, nullptr, D3DCOLOR_ARGB(255, frame, 0, 0));
		device->StretchRect(eyeSurface, nullptr, copySurface, nullptr, D3DTEXF_NONE);
		ID3D12Resource* resource = nullptr;

		if (FAILED(on12->UnwrapUnderlyingResource(copy, queue, __uuidof(ID3D12Resource), reinterpret_cast<void**>(&resource))) || resource == nullptr)
		{
			unwrapFailures++;
			continue;
		}

		first = first == nullptr ? resource : first;
		sameResource = sameResource && resource == first;
		bool ready = SUCCEEDED(allocator->Reset()) && Transition(allocator, list, queue, resource, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		barrierFailures += ready && Transition(allocator, list, queue, resource, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON) ? 0 : 1;
		value++;
		queue->Signal(fence, value);
		ID3D12Fence* fences[] = {fence};
		UINT64 values[] = {value};
		returnFailures += SUCCEEDED(on12->ReturnUnderlyingResource(copy, 1, values, fences)) ? 0 : 1;
		resource->Release();
		device->PresentEx(nullptr, nullptr, nullptr, nullptr, 0);
	}

	HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	fence->SetEventOnCompletion(value, event);
	bool finished = WaitForSingleObject(event, 5000) == WAIT_OBJECT_0;
	CloseHandle(event);
	ULONGLONG elapsed = GetTickCount64() - start;

	Check(unwrapFailures == 0, "every frame's copy unwraps to a Direct3D 12 resource (" + std::to_string(unwrapFailures) + " failed)");
	Check(barrierFailures == 0, "and takes both transitions (" + std::to_string(barrierFailures) + " failed)");
	Check(returnFailures == 0, "and goes back with the fence value (" + std::to_string(returnFailures) + " failed)");
	Check(finished && fence->GetCompletedValue() == value, "the GPU finishes all " + std::to_string(value) + " frames, in " + std::to_string(elapsed) + " ms");
	Check(sameResource, "the copy unwraps to the same resource each frame");
	int problems = TakeDebugMessages(info);
	Check(problems == 0, "the debug layer reports no errors, so the resource states are right (" + std::to_string(problems) + ")");

	std::printf("%d failure(s)\n", failures);
	return failures == 0 ? 0 : 1;
}
