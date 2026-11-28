/*
	The native path's hand-off (rcx 0.79): a Direct3D 9Ex device draws slow work, writes a color unique to the
	frame and copies it into a texture shared with a Direct3D 11 device, issues an event query, and Direct3D 11
	reads the shared texture. Read at once, it can see an older frame (on an RTX 4090 every read did: nothing
	orders Direct3D 9's writes before the other device's read), which is noted; after the bridge's wait for the
	query, every read must be the frame's own color. Only SteamVR's own copy (on the same Direct3D 11 device)
	isn't run. Skipped with a note when no Direct3D 9Ex or Direct3D 11 device can be made. Exits non-zero on a failure.
*/
#include <cstdio>
#include <string>

#include <windows.h>
#include <d3d9.h>
#include <d3d11.h>

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

	constexpr UINT kHeavySize = 4096;
	constexpr UINT kEyeSize = 1024;
	constexpr int kHeavyCopies = 40;
	constexpr int kFrames = 120;
}

int main()
{
	HMODULE d3d9Module = LoadLibraryW(L"d3d9.dll");
	HMODULE d3d11Module = LoadLibraryW(L"d3d11.dll");
	using Create9ExFunction = HRESULT(WINAPI*)(UINT, IDirect3D9Ex**);
	auto create9Ex = d3d9Module != nullptr ? reinterpret_cast<Create9ExFunction>(reinterpret_cast<void*>(GetProcAddress(d3d9Module, "Direct3DCreate9Ex"))) : nullptr;
	auto create11 = d3d11Module != nullptr ? reinterpret_cast<PFN_D3D11_CREATE_DEVICE>(reinterpret_cast<void*>(GetProcAddress(d3d11Module, "D3D11CreateDevice"))) : nullptr;
	IDirect3D9Ex* direct3d = nullptr;
	IDirect3DDevice9Ex* device = nullptr;
	HWND window = CreateWindowExW(0, L"STATIC", L"rcx_shared_sync_test", WS_POPUP, 0, 0, 64, 64, nullptr, nullptr, nullptr, nullptr);

	if (create9Ex != nullptr && window != nullptr && SUCCEEDED(create9Ex(D3D_SDK_VERSION, &direct3d)) && direct3d != nullptr)
	{
		D3DPRESENT_PARAMETERS parameters{};
		parameters.Windowed = TRUE;
		parameters.SwapEffect = D3DSWAPEFFECT_DISCARD;
		parameters.BackBufferFormat = D3DFMT_UNKNOWN;
		parameters.hDeviceWindow = window;
		direct3d->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &parameters, nullptr, &device);
	}

	ID3D11Device* device11 = nullptr;
	ID3D11DeviceContext* context = nullptr;

	if (create11 != nullptr)
	{
		create11(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device11, nullptr, &context);
	}

	if (device == nullptr || device11 == nullptr)
	{
		std::printf("note no Direct3D 9Ex or Direct3D 11 device here: skipped\n");
		return 0;
	}

	D3DADAPTER_IDENTIFIER9 identifier{};
	direct3d->GetAdapterIdentifier(D3DADAPTER_DEFAULT, 0, &identifier);
	std::printf("note on %s\n", identifier.Description);

	IDirect3DSurface9* heavyA = nullptr;
	IDirect3DSurface9* heavyB = nullptr;
	IDirect3DSurface9* eye = nullptr;
	IDirect3DTexture9* shared = nullptr;
	IDirect3DSurface9* sharedSurface = nullptr;
	IDirect3DQuery9* query = nullptr;
	HANDLE handle = nullptr;
	bool made = SUCCEEDED(device->CreateRenderTarget(kHeavySize, kHeavySize, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &heavyA, nullptr)) &&
		SUCCEEDED(device->CreateRenderTarget(kHeavySize, kHeavySize, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &heavyB, nullptr)) &&
		SUCCEEDED(device->CreateRenderTarget(kEyeSize, kEyeSize, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &eye, nullptr)) &&
		SUCCEEDED(device->CreateTexture(kEyeSize, kEyeSize, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &shared, &handle)) && handle != nullptr &&
		SUCCEEDED(shared->GetSurfaceLevel(0, &sharedSurface)) && SUCCEEDED(device->CreateQuery(D3DQUERYTYPE_EVENT, &query));
	Check(made, "the Direct3D 9Ex targets, a shared texture and an event query");

	ID3D11Texture2D* opened = nullptr;
	ID3D11Texture2D* staging = nullptr;

	if (made)
	{
		made = SUCCEEDED(device11->OpenSharedResource(handle, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&opened))) && opened != nullptr;
		D3D11_TEXTURE2D_DESC description{};
		description.Width = 4;
		description.Height = 4;
		description.MipLevels = 1;
		description.ArraySize = 1;
		description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		description.SampleDesc.Count = 1;
		description.Usage = D3D11_USAGE_STAGING;
		description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		made = made && SUCCEEDED(device11->CreateTexture2D(&description, nullptr, &staging));
		Check(made, "Direct3D 11 opens the shared texture");
	}

	for (int pass = 0; made && pass < 2; pass++)
	{
		// Pass 0 reads at once; pass 1 waits for the event query first, as the bridge's Present does (up to 50 ms).
		bool wait = pass == 1;
		int unfinished = 0;
		int timedOut = 0;
		int wrong = 0;
		int firstWrong = -1;
		unsigned int firstWrongColor = 0;
		ULONGLONG start = GetTickCount64();

		for (int frame = 0; frame < kFrames; frame++)
		{
			for (int heavy = 0; heavy < kHeavyCopies; heavy++)
			{
				device->StretchRect(heavy % 2 == 0 ? heavyA : heavyB, nullptr, heavy % 2 == 0 ? heavyB : heavyA, nullptr, D3DTEXF_LINEAR);
			}

			// Slow work into the eye, then the frame's own color over all of it.
			device->StretchRect(heavyA, nullptr, eye, nullptr, D3DTEXF_LINEAR);
			unsigned int color = D3DCOLOR_ARGB(255, (frame * 37 + 11 + pass * 101) & 0xff, (frame * 91 + 5) & 0xff, (frame + 1) & 0xff);
			device->ColorFill(eye, nullptr, color);
			device->StretchRect(eye, nullptr, sharedSurface, nullptr, D3DTEXF_NONE);
			query->Issue(D3DISSUE_END);
			ULONGLONG issued = GetTickCount64();
			HRESULT state = query->GetData(nullptr, 0, D3DGETDATA_FLUSH);
			unfinished += state == S_FALSE ? 1 : 0;

			while (wait && state == S_FALSE && GetTickCount64() - issued < 50)
			{
				SwitchToThread();
				state = query->GetData(nullptr, 0, D3DGETDATA_FLUSH);
			}

			timedOut += wait && state == S_FALSE ? 1 : 0;
			D3D11_BOX box{kEyeSize / 2, kEyeSize / 2, 0, kEyeSize / 2 + 4, kEyeSize / 2 + 4, 1};
			context->CopySubresourceRegion(staging, 0, 0, 0, 0, opened, 0, &box);
			D3D11_MAPPED_SUBRESOURCE mapped{};
			unsigned int read = 0;

			if (SUCCEEDED(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped)))
			{
				read = *static_cast<unsigned int*>(mapped.pData);
				context->Unmap(staging, 0);
			}

			if (read != color)
			{
				firstWrongColor = firstWrong < 0 ? read : firstWrongColor;
				firstWrong = firstWrong < 0 ? frame : firstWrong;
				wrong++;
			}

			device->PresentEx(nullptr, nullptr, nullptr, nullptr, 0);
		}

		ULONGLONG elapsed = GetTickCount64() - start;
		std::printf("note %s: %d of %d frames' queries weren't done when issued, %d wrong reads, in %llu ms\n", wait ? "waiting" : "not waiting", unfinished, kFrames, wrong,
			static_cast<unsigned long long>(elapsed));

		if (firstWrong >= 0)
		{
			std::printf("note %s: frame %d read 0x%08x\n", wait ? "waiting" : "not waiting", firstWrong, firstWrongColor);
		}

		if (!wait)
		{
			Check(unfinished > 0, "the reads come before the Direct3D 9 work is done (else the test proves nothing)");
			std::printf("note without a wait Direct3D 11 %s\n", wrong > 0 ? "reads before Direct3D 9's writes land, so the bridge must wait" : "happened to read every frame's color");
			continue;
		}

		Check(timedOut == 0, "the wait sees every copy done within 50 ms (" + std::to_string(timedOut) + " timed out)");
		Check(wrong == 0, "after the wait Direct3D 11 always reads the frame's own color (" + std::to_string(wrong) + " wrong)");
	}

	std::printf("%d failure(s)\n", failures);
	return failures == 0 ? 0 : 1;
}
