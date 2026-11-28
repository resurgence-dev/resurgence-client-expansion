/*
	VR-42's case outside the game: a device whose vptr an overlay swapped to its own copy of the
	vtable. The VR bridge's hooks (rcx 0.74, detours on d3d9.dll's bodies) must still find the device
	on its Present and capture a render target by its creation, although no vtable it patched is the
	device's. Skipped with a note when no Direct3D 9 device can be made. Exits non-zero on a failure.
*/
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <windows.h>
#include <d3d9.h>

#include "core/core.h"
#include "engine/engine_guard.h"
#include "vr/d3d9_bridge.h"

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

	// IDirect3DDevice9's slots (the plain interface); a copy this long covers every call made here.
	constexpr size_t kDeviceSlots = 119;

	// Where the test keeps its device, as shaderapidx9.dll keeps the game's: in the module's data.
	IDirect3DDevice9* volatile storedDevice = nullptr;
}

int main()
{
	HMODULE d3d9 = LoadLibraryW(L"d3d9.dll");
	using CreateFunction = IDirect3D9*(WINAPI*)(UINT);
	auto create = d3d9 != nullptr ? reinterpret_cast<CreateFunction>(reinterpret_cast<void*>(GetProcAddress(d3d9, "Direct3DCreate9"))) : nullptr;
	IDirect3D9* direct3d = create != nullptr ? create(D3D_SDK_VERSION) : nullptr;
	HWND window = CreateWindowExW(0, L"STATIC", L"rcx_bridge_vptr_test", WS_POPUP, 0, 0, 64, 64, nullptr, nullptr, nullptr, nullptr);
	IDirect3DDevice9* device = nullptr;

	if (direct3d != nullptr && window != nullptr)
	{
		D3DPRESENT_PARAMETERS parameters{};
		parameters.Windowed = TRUE;
		parameters.SwapEffect = D3DSWAPEFFECT_DISCARD;
		parameters.BackBufferFormat = D3DFMT_UNKNOWN;
		parameters.hDeviceWindow = window;
		direct3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &parameters, &device);
	}

	if (device == nullptr)
	{
		std::printf("note no Direct3D 9 device here: skipped\n");
		return 0;
	}

	// The overlay: the device's vptr now points at a heap copy of its vtable, as before any hook.
	void** original = *reinterpret_cast<void***>(device);
	void** copy = static_cast<void**>(HeapAlloc(GetProcessHeap(), 0, kDeviceSlots * sizeof(void*)));
	std::memcpy(copy, original, kDeviceSlots * sizeof(void*));
	*reinterpret_cast<void***>(device) = copy;

	// The session log: Log lines and LogToFile lines land in rcx_state/vr_last.log, flushed as written.
	std::string logPath;
	Check(rcx::core::OpenSessionLog("rcx test", logPath), "the session log opens: " + logPath);
	rcx::core::Log("a console line %d", 1);
	rcx::core::LogToFile("a file-only line %d", 2);
	{
		FILE* file = std::fopen(logPath.c_str(), "rb");
		char text[4096]{};
		size_t read = file != nullptr ? std::fread(text, 1, sizeof(text) - 1, file) : 0;

		if (file != nullptr)
		{
			std::fclose(file);
		}

		std::string content(text, read);
		Check(content.find("rcx test") != std::string::npos && content.find("[rcx] a console line 1") != std::string::npos &&
			content.find("a file-only line 2") != std::string::npos, "and holds the version, a Log line and a LogToFile line before it's closed");
	}
	rcx::core::TakeLogLines();

	// As the module's open does (engine_lua.cpp).
	rcx::engine::RegisterSystemFeature("vr_detours");
	std::string reason;
	bool installed = rcx::vr::bridge::InstallHooks(reason);
	Check(installed, "the hooks install: " + (installed ? rcx::vr::bridge::DescribeDetours() : reason));

	for (const std::string& line : rcx::core::TakeLogLines())
	{
		std::printf("log  %s\n", line.c_str());
	}

	Check(rcx::vr::bridge::DescribeDetours().find(" 0 refused") != std::string::npos, "no detour was refused on this machine's d3d9.dll");

	device->Present(nullptr, nullptr, nullptr, nullptr);
	Check(rcx::vr::bridge::IsDeviceFound(), "the device is found on its Present through the copied vtable");

	rcx::vr::bridge::BeginCapture(96, 48);
	IDirect3DTexture9* texture = nullptr;
	HRESULT created = device->CreateTexture(96, 48, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &texture, nullptr);
	std::string kind;
	bool captured = rcx::vr::bridge::EndCapture(reason, kind);
	Check(SUCCEEDED(created) && captured, "a render target made through the copied vtable is captured" + (captured ? std::string() : ": " + kind + ", " + reason));

	rcx::vr::bridge::Stats stats = rcx::vr::bridge::GetStats();
	Check(stats.capturedVia == "creation", "by its creation (capturedVia " + stats.capturedVia + ")");
	Check(stats.hooks.find("the device's vtable is in no module") != std::string::npos, "the hooks line says the device's vtable is a heap copy");
	Check(stats.hooks.find("same graphics card") == std::string::npos, "and gives no GPU advice on a machine where the device is found");

	// The scan, as on the game's shaderapidx9.dll: the test's own module data holds the device pointer.
	storedDevice = device;
	std::string where;
	IDirect3DDevice9* scanned = rcx::vr::bridge::ScanForDevice(GetModuleHandleW(nullptr), where);
	char expected[32]{};
	std::snprintf(expected, sizeof(expected), "+0x%llx,", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(&storedDevice) - reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr))));
	Check(scanned == device && where.find(expected) != std::string::npos, "the scan finds the device (vptr on a heap copy) where the module keeps it: " + where);
	storedDevice = nullptr;
	std::printf("note hooks: %s\n", stats.hooks.c_str());

	for (const std::string& line : rcx::core::TakeLogLines())
	{
		std::printf("log  %s\n", line.c_str());
	}

	if (texture != nullptr)
	{
		texture->Release();
	}

	*reinterpret_cast<void***>(device) = original;
	device->Release();
	direct3d->Release();
	DestroyWindow(window);
	rcx::engine::DisarmAll();
	rcx::core::CloseSessionLog();
	std::printf("%d failure(s)\n", failures);
	return failures == 0 ? 0 : 1;
}
