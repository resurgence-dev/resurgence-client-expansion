/*
	The x64 detour (src/core/detour.cpp) outside the game: the instruction decoder against known
	encodings, and real detours of hand-written functions in executable memory (a prologue moved
	whole, a RIP-relative load rewritten, chaining onto an E9 and an FF 25 jump, a refused call).
	Exits non-zero on a failure.
*/
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <windows.h>
#include <d3d9.h>

#include "core/detour.h"

namespace
{
	int failures = 0;

	void Check(bool condition, const char* label)
	{
		std::printf("%s   %s\n", condition ? "ok" : "FAIL", label);

		if (!condition)
		{
			failures++;
		}
	}

	struct DecodeCase
	{
		const char* label;
		std::vector<uint8_t> bytes;
		bool known;
		size_t length;
		size_t ripOffset;
		bool jump;
	};

	using AddFunction = int (*)(int, int);
	using OneFunction = int (*)(int);

	AddFunction originalAdd = nullptr;
	AddFunction originalChain = nullptr;
	AddFunction originalIndirect = nullptr;
	AddFunction originalThunk = nullptr;
	OneFunction originalLoad = nullptr;

	int HookAdd(int a, int b)
	{
		return originalAdd(a, b) + 100;
	}

	int HookChain(int a, int b)
	{
		return originalChain(a, b) + 10;
	}

	int HookIndirect(int a, int b)
	{
		return originalIndirect(a, b) + 20;
	}

	int HookThunk(int a, int b)
	{
		return originalThunk(a, b) + 30;
	}

	int HookLoad(int a)
	{
		return originalLoad(a) + 1000;
	}

	int Refused(int a, int b)
	{
		return a - b;
	}

	void Put(uint8_t* at, std::initializer_list<uint8_t> bytes)
	{
		std::memcpy(at, bytes.begin(), bytes.size());
	}

	// This machine's d3d9.dll: the bodies the VR bridge detours (rcx 0.74), detoured for real on a
	// throwaway device and called through it. Skipped with a note when no device can be made.
	using PresentFunction = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
	using CreateTextureFunction = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DTexture9**, HANDLE*);
	using SetRenderTargetFunction = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
	using SwapChainPresentFunction = HRESULT(STDMETHODCALLTYPE*)(IDirect3DSwapChain9*, const RECT*, const RECT*, HWND, const RGNDATA*, DWORD);

	PresentFunction originalPresent = nullptr;
	CreateTextureFunction originalCreateTexture = nullptr;
	SetRenderTargetFunction originalSetRenderTarget = nullptr;
	SwapChainPresentFunction originalSwapChainPresent = nullptr;
	int presents = 0;
	int creations = 0;
	int bindings = 0;
	int swapChainPresents = 0;

	HRESULT STDMETHODCALLTYPE HookPresent(IDirect3DDevice9* device, const RECT* source, const RECT* destination, HWND window, const RGNDATA* dirty)
	{
		presents++;
		return originalPresent(device, source, destination, window, dirty);
	}

	HRESULT STDMETHODCALLTYPE HookCreateTexture(IDirect3DDevice9* device, UINT width, UINT height, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool,
		IDirect3DTexture9** texture, HANDLE* shared)
	{
		creations++;
		return originalCreateTexture(device, width, height, levels, usage, format, pool, texture, shared);
	}

	HRESULT STDMETHODCALLTYPE HookSetRenderTarget(IDirect3DDevice9* device, DWORD index, IDirect3DSurface9* surface)
	{
		bindings++;
		return originalSetRenderTarget(device, index, surface);
	}

	HRESULT STDMETHODCALLTYPE HookSwapChainPresent(IDirect3DSwapChain9* chain, const RECT* source, const RECT* destination, HWND window, const RGNDATA* dirty, DWORD flags)
	{
		swapChainPresents++;
		return originalSwapChainPresent(chain, source, destination, window, dirty, flags);
	}

	void DetourSlot(const char* name, void** vtable, int slot, void* hook, void** original)
	{
		std::string how;
		std::string reason;
		std::string bytes = rcx::detour::HexBytes(vtable[slot], 16);
		bool installed = rcx::detour::Install(vtable[slot], hook, original, how, reason);
		Check(installed, (std::string("d3d9.dll's ") + name + " is detoured: " + (installed ? how : reason + " [" + bytes + "]")).c_str());
	}

	void TestD3d9()
	{
		HMODULE d3d9 = LoadLibraryW(L"d3d9.dll");
		using CreateFunction = IDirect3D9*(WINAPI*)(UINT);
		auto create = d3d9 != nullptr ? reinterpret_cast<CreateFunction>(reinterpret_cast<void*>(GetProcAddress(d3d9, "Direct3DCreate9"))) : nullptr;
		IDirect3D9* direct3d = create != nullptr ? create(D3D_SDK_VERSION) : nullptr;
		HWND window = CreateWindowExW(0, L"STATIC", L"rcx_detour_test", WS_POPUP, 0, 0, 64, 64, nullptr, nullptr, nullptr, nullptr);
		IDirect3DDevice9* device = nullptr;

		if (direct3d != nullptr && window != nullptr)
		{
			D3DPRESENT_PARAMETERS parameters{};
			parameters.Windowed = TRUE;
			parameters.SwapEffect = D3DSWAPEFFECT_DISCARD;
			parameters.BackBufferFormat = D3DFMT_UNKNOWN;
			parameters.hDeviceWindow = window;
			direct3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window, D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &parameters, &device);
		}

		if (device == nullptr)
		{
			std::printf("note no Direct3D 9 device here: the d3d9.dll detours are skipped\n");
			return;
		}

		void** vtable = *reinterpret_cast<void***>(device);
		IDirect3DSwapChain9* chain = nullptr;
		device->GetSwapChain(0, &chain);
		DetourSlot("Present", vtable, 17, reinterpret_cast<void*>(&HookPresent), reinterpret_cast<void**>(&originalPresent));
		DetourSlot("CreateTexture", vtable, 23, reinterpret_cast<void*>(&HookCreateTexture), reinterpret_cast<void**>(&originalCreateTexture));
		DetourSlot("SetRenderTarget", vtable, 37, reinterpret_cast<void*>(&HookSetRenderTarget), reinterpret_cast<void**>(&originalSetRenderTarget));

		if (chain != nullptr)
		{
			void** chainTable = *reinterpret_cast<void***>(chain);
			DetourSlot("the swap chain's Present", chainTable, 3, reinterpret_cast<void*>(&HookSwapChainPresent), reinterpret_cast<void**>(&originalSwapChainPresent));
		}

		IDirect3DTexture9* texture = nullptr;
		HRESULT created = device->CreateTexture(64, 32, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &texture, nullptr);
		Check(SUCCEEDED(created) && texture != nullptr && creations == 1, "a render target made through the device reaches the CreateTexture detour once");

		IDirect3DSurface9* surface = nullptr;

		if (texture != nullptr && SUCCEEDED(texture->GetSurfaceLevel(0, &surface)))
		{
			Check(SUCCEEDED(device->SetRenderTarget(0, surface)) && bindings >= 1, "binding it reaches the SetRenderTarget detour");
			surface->Release();
		}

		HRESULT presented = device->Present(nullptr, nullptr, nullptr, nullptr);
		Check(SUCCEEDED(presented) && presents == 1, "a device Present reaches the Present detour once");

		if (chain != nullptr)
		{
			int before = swapChainPresents;
			chain->Present(nullptr, nullptr, nullptr, nullptr, 0);
			Check(swapChainPresents == before + 1, "a swap chain Present reaches its detour");
			std::printf("note the device's Present went through the swap chain's body %d time(s)\n", before);
			chain->Release();
		}

		if (texture != nullptr)
		{
			texture->Release();
		}

		device->Release();
		direct3d->Release();
		DestroyWindow(window);
	}
}

int main()
{
	const DecodeCase cases[] = {
		{"mov [rsp+8], rbx", {0x48, 0x89, 0x5c, 0x24, 0x08}, true, 5, 0, false},
		{"push rbx (REX)", {0x40, 0x53}, true, 2, 0, false},
		{"sub rsp, 0x28", {0x48, 0x83, 0xec, 0x28}, true, 4, 0, false},
		{"mov rax, [rip+x]", {0x48, 0x8b, 0x05, 0x10, 0x20, 0x30, 0x00}, true, 7, 3, false},
		{"nop dword [rax+rax]", {0x0f, 0x1f, 0x44, 0x00, 0x00}, true, 5, 0, false},
		{"66 nop word", {0x66, 0x0f, 0x1f, 0x44, 0x00, 0x00}, true, 6, 0, false},
		{"mov rax, imm64", {0x48, 0xb8, 1, 2, 3, 4, 5, 6, 7, 8}, true, 10, 0, false},
		{"mov dword [rsp+0x10], imm32", {0xc7, 0x44, 0x24, 0x10, 1, 2, 3, 4}, true, 8, 0, false},
		{"test cl, 1", {0xf6, 0xc1, 0x01}, true, 3, 0, false},
		{"lea rcx, [rip+x]", {0x48, 0x8d, 0x0d, 1, 2, 3, 4}, true, 7, 3, false},
		{"jmp rel32", {0xe9, 1, 2, 3, 4}, true, 5, 0, true},
		{"jmp [rip+x]", {0xff, 0x25, 0, 0, 0, 0}, true, 6, 2, true},
		{"jmp rax", {0xff, 0xe0}, true, 2, 0, true},
		{"call rel32 is refused", {0xe8, 1, 2, 3, 4}, false, 0, 0, false},
		{"jcc rel8 is refused", {0x74, 0x05}, false, 0, 0, false},
		{"call [rip+x] is refused", {0xff, 0x15, 0, 0, 0, 0}, false, 0, 0, false},
		{"a cut instruction is refused", {0x48, 0x89}, false, 0, 0, false},
	};

	for (const DecodeCase& entry : cases)
	{
		rcx::detour::Decoded decoded{};
		bool known = rcx::detour::Decode(entry.bytes.data(), entry.bytes.size(), decoded);
		bool right = known == entry.known && (!known || (decoded.length == entry.length && decoded.ripDisplacementOffset == entry.ripOffset && decoded.jump == entry.jump));
		Check(right, entry.label);
	}

	uint8_t* page = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));

	if (page == nullptr)
	{
		std::printf("FAIL no executable page\n");
		return 1;
	}

	std::memset(page, 0xcc, 4096);

	// add: mov [rsp+8], rbx; push rdi; sub rsp, 0x20; lea eax, [rcx+rdx]; add rsp, 0x20; pop rdi; ret
	uint8_t* add = page;
	Put(add, {0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20, 0x8d, 0x04, 0x11, 0x48, 0x83, 0xc4, 0x20, 0x5f, 0xc3});

	// load: mov eax, [rip+disp] (the int at page+0x400); add eax, ecx; ret
	uint8_t* load = page + 0x100;
	int32_t displacement = static_cast<int32_t>((page + 0x400) - (load + 6));
	Put(load, {0x8b, 0x05});
	std::memcpy(load + 2, &displacement, sizeof(displacement));
	Put(load + 6, {0x03, 0xc1, 0xc3});
	int32_t stored = 7;
	std::memcpy(page + 0x400, &stored, sizeof(stored));

	// target: lea eax, [rcx+rdx]; ret; chain: jmp rel32 to target; indirect: jmp [rip+0]; dq target
	uint8_t* target = page + 0x200;
	Put(target, {0x8d, 0x04, 0x11, 0xc3});
	uint8_t* chain = page + 0x240;
	int32_t relative = static_cast<int32_t>(target - (chain + 5));
	chain[0] = 0xe9;
	std::memcpy(chain + 1, &relative, sizeof(relative));
	uint8_t* indirect = page + 0x280;
	Put(indirect, {0xff, 0x25, 0, 0, 0, 0});
	uint64_t targetAddress = reinterpret_cast<uint64_t>(target);
	std::memcpy(indirect + 6, &targetAddress, sizeof(targetAddress));

	// thunk: add rcx, 0 (an adjustment); jmp rel32 to target, as d3d9.dll's swap chain Present is
	uint8_t* thunk = page + 0x2c0;
	Put(thunk, {0x48, 0x83, 0xc1, 0x00, 0xe9});
	int32_t thunkRelative = static_cast<int32_t>(target - (thunk + 9));
	std::memcpy(thunk + 5, &thunkRelative, sizeof(thunkRelative));

	// refused: call rel32 first
	uint8_t* refused = page + 0x300;
	Put(refused, {0xe8, 0, 0, 0, 0, 0xc3});
	FlushInstructionCache(GetCurrentProcess(), page, 4096);

	Check(reinterpret_cast<AddFunction>(add)(2, 3) == 5, "add(2, 3) is 5 before");
	Check(reinterpret_cast<OneFunction>(load)(1) == 8, "load(1) is 8 before");

	std::string how;
	std::string reason;
	bool installed = rcx::detour::Install(add, reinterpret_cast<void*>(&HookAdd), reinterpret_cast<void**>(&originalAdd), how, reason);
	Check(installed && originalAdd != nullptr, ("a prologue is detoured: " + (installed ? how : reason)).c_str());
	Check(installed && reinterpret_cast<AddFunction>(add)(2, 3) == 105, "add(2, 3) is 105 through the hook");

	installed = rcx::detour::Install(load, reinterpret_cast<void*>(&HookLoad), reinterpret_cast<void**>(&originalLoad), how, reason);
	Check(installed, ("a RIP-relative load is detoured: " + (installed ? how : reason)).c_str());
	Check(installed && reinterpret_cast<OneFunction>(load)(1) == 1008, "load(1) is 1008: the moved load still reads its int");

	installed = rcx::detour::Install(chain, reinterpret_cast<void*>(&HookChain), reinterpret_cast<void**>(&originalChain), how, reason);
	Check(installed && how.find("chained") != std::string::npos, ("an E9 jump is chained: " + (installed ? how : reason)).c_str());
	Check(installed && reinterpret_cast<AddFunction>(chain)(2, 3) == 15, "chain(2, 3) is 15: the hook, then the jump's target");

	installed = rcx::detour::Install(indirect, reinterpret_cast<void*>(&HookIndirect), reinterpret_cast<void**>(&originalIndirect), how, reason);
	Check(installed && how.find("chained") != std::string::npos, ("an FF 25 jump is chained: " + (installed ? how : reason)).c_str());
	Check(installed && reinterpret_cast<AddFunction>(indirect)(2, 3) == 25, "indirect(2, 3) is 25");

	installed = rcx::detour::Install(thunk, reinterpret_cast<void*>(&HookThunk), reinterpret_cast<void**>(&originalThunk), how, reason);
	Check(installed, ("a thunk (an adjustment, then a relative jump) is detoured: " + (installed ? how : reason)).c_str());
	Check(installed && reinterpret_cast<AddFunction>(thunk)(2, 3) == 35, "thunk(2, 3) is 35");

	void* unused = nullptr;
	std::string before = rcx::detour::HexBytes(refused, 6);
	installed = rcx::detour::Install(refused, reinterpret_cast<void*>(&Refused), &unused, how, reason);
	Check(!installed && unused == nullptr, ("a call at the start is refused: " + reason).c_str());
	Check(rcx::detour::HexBytes(refused, 6) == before, "and the refused function is left as it was");

	TestD3d9();
	std::printf("%d failure(s)\n", failures);
	return failures == 0 ? 0 : 1;
}
