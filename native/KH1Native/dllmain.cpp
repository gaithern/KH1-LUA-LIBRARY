#include "pch.h"
#include <tlhelp32.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>
#include <d3d11.h>

#include "imgui/imgui.h"
#include "imgui/imgui_impl_win32.h"
#include "imgui/imgui_impl_dx11.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// --- DEBUG LOGGING ---
static char g_dllDir[MAX_PATH] = "";

static void LogDebug(const char* msg) {
    if (!g_dllDir[0]) return;
    char path[MAX_PATH];
    snprintf(path, MAX_PATH, "%skh1_native_debug.log", g_dllDir);
    FILE* f = nullptr;
    if (fopen_s(&f, path, "a") == 0 && f) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(f, "[%02d:%02d:%02d] %s\n", st.wHour, st.wMinute, st.wSecond, msg);
        fclose(f);
    }
}

// --- LUA FUNCTION POINTERS ---
// Resolved against whichever already-loaded module in the host process exports
// the Lua C API (no Lua headers needed) -- see FindLuaModule(). Same technique
// as KH1Overlay's dllmain.cpp in the randomizer repo; duplicated here rather
// than shared because this module is intentionally independent of it.
typedef int          (__cdecl* t_lua_gettop)(void* L);
typedef long long    (__cdecl* t_lua_tointegerx)(void* L, int idx, int* isnum);
typedef double       (__cdecl* t_lua_tonumberx)(void* L, int idx, int* isnum);
typedef void         (__cdecl* t_lua_pushinteger)(void* L, long long n);
typedef void         (__cdecl* t_lua_pushboolean)(void* L, int b);
typedef const char*  (__cdecl* t_lua_pushstring)(void* L, const char* s);
typedef void         (__cdecl* t_luaL_setfuncs)(void* L, const void* l, int nup);
typedef void         (__cdecl* t_lua_createtable)(void* L, int narr, int nrec);
typedef void         (__cdecl* t_lua_setfield)(void* L, int idx, const char* k);
typedef const char*  (__cdecl* t_lua_tolstring)(void* L, int idx, size_t* len);
typedef unsigned long long (__cdecl* t_lua_rawlen)(void* L, int idx);
typedef int          (__cdecl* t_lua_rawgeti)(void* L, int idx, long long n);
typedef void         (__cdecl* t_lua_settop)(void* L, int idx);

static t_lua_gettop       p_lua_gettop       = nullptr;
static t_lua_tointegerx   p_lua_tointegerx   = nullptr;
static t_lua_tonumberx    p_lua_tonumberx    = nullptr;
static t_lua_pushinteger  p_lua_pushinteger  = nullptr;
static t_lua_pushboolean  p_lua_pushboolean  = nullptr;
static t_lua_pushstring   p_lua_pushstring   = nullptr;
static t_luaL_setfuncs    p_luaL_setfuncs    = nullptr;
static t_lua_createtable  p_lua_createtable  = nullptr;
static t_lua_setfield     p_lua_setfield     = nullptr;
static t_lua_tolstring    p_lua_tolstring    = nullptr;
static t_lua_rawlen       p_lua_rawlen       = nullptr;
static t_lua_rawgeti      p_lua_rawgeti      = nullptr;
static t_lua_settop       p_lua_settop       = nullptr;

struct luaL_Reg { const char* name; void* func; };

// --- CALL BRIDGE ---
// Windows x64 has a single calling convention (first four integer/pointer args
// in RCX,RDX,R8,R9, remainder on the stack, 32-byte shadow space). Casting a
// raw address to a fixed-arity function pointer type of the right shape and
// calling it normally produces exactly that ABI -- no inline asm needed (x64
// MSVC doesn't support __asm anyway). Only integer/pointer args are supported;
// float/double args go in XMM registers under this ABI and are not marshaled
// here.
typedef unsigned long long(__fastcall* Func0)();
typedef unsigned long long(__fastcall* Func1)(unsigned long long);
typedef unsigned long long(__fastcall* Func2)(unsigned long long, unsigned long long);
typedef unsigned long long(__fastcall* Func3)(unsigned long long, unsigned long long, unsigned long long);
typedef unsigned long long(__fastcall* Func4)(unsigned long long, unsigned long long, unsigned long long, unsigned long long);
typedef unsigned long long(__fastcall* Func5)(unsigned long long, unsigned long long, unsigned long long, unsigned long long, unsigned long long);
typedef unsigned long long(__fastcall* Func6)(unsigned long long, unsigned long long, unsigned long long, unsigned long long, unsigned long long, unsigned long long);

static const int MAX_CALL_ARGS = 6;

// Kept separate from l_call_function so the __try block doesn't have to share
// a stack frame with any C++ object that has a destructor (MSVC forbids that).
//
// __except(EXCEPTION_EXECUTE_HANDLER) turns a hardware fault (e.g. an access
// violation from a bad address or a wrong argument) into a normal false
// return instead of taking down the whole game process -- this is the only
// safety net a generic "call anything" bridge can offer. It does not protect
// against a call that "succeeds" but corrupts game state because the address,
// argument count, or argument types were wrong for that function.
static bool SafeCall(unsigned long long address, const unsigned long long* args, int argCount, unsigned long long& outResult) {
    __try {
        switch (argCount) {
        case 0: outResult = ((Func0)address)(); break;
        case 1: outResult = ((Func1)address)(args[0]); break;
        case 2: outResult = ((Func2)address)(args[0], args[1]); break;
        case 3: outResult = ((Func3)address)(args[0], args[1], args[2]); break;
        case 4: outResult = ((Func4)address)(args[0], args[1], args[2], args[3]); break;
        case 5: outResult = ((Func5)address)(args[0], args[1], args[2], args[3], args[4]); break;
        default: outResult = ((Func6)address)(args[0], args[1], args[2], args[3], args[4], args[5]); break;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// --- DEBUG OVERLAY <-> LUA SHARED STATE ---
// Bridges the overlay's own UI thread with Lua's _OnFrame. Guarded by g_lock
// since the two run on different threads. The overlay can't call game
// functions directly from its own thread -- we proved earlier that calling
// game code from any thread other than Lua's (which runs on the game's own
// main thread) is unreliable, so button clicks only ever queue a request;
// the actual call happens in poll_debug_action, invoked from Lua's _OnFrame.
static SRWLOCK g_lock = SRWLOCK_INIT;

static bool g_debugActionPending = false;
static char g_debugAction[32] = "";
static long long g_debugParam1 = 0;
static char g_debugParamText[256] = "";

static char g_debugResult[256] = "";

// --- STANDALONE IMGUI WINDOW ---
// Its own window, own D3D11 device, own swap chain, own message/render loop --
// completely independent of the game's window and renderer, so this can't
// destabilize the game the way hooking its renderer would. Same approach as
// KH1Overlay in the randomizer repo (duplicated rather than shared, since
// this module is intentionally independent of it).
static HWND g_hwnd = nullptr;
static ID3D11Device* g_device = nullptr;
static ID3D11DeviceContext* g_context = nullptr;
static IDXGISwapChain* g_swapChain = nullptr;
static ID3D11RenderTargetView* g_rtv = nullptr;
static LONG g_formThreadStarted = 0;
static bool g_formVisible = false;
static volatile bool g_shuttingDown = false;

static void CreateRenderTarget() {
    ID3D11Texture2D* backBuffer = nullptr;
    g_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer));
    g_device->CreateRenderTargetView(backBuffer, nullptr, &g_rtv);
    backBuffer->Release();
}

static LRESULT CALLBACK FormWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam)) {
        return true;
    }
    switch (msg) {
    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        g_formVisible = false;
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_SIZE:
        if (g_swapChain && wParam != SIZE_MINIMIZED) {
            if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
            g_swapChain->ResizeBuffers(0, LOWORD(lParam), HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
            CreateRenderTarget();
        }
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

// Only known/named actions are exposed here (not a raw address+args form) --
// each button queues a specific, already-vetted request for Lua to dispatch
// through the real named Lua function (e.g. spawn_prize), which is what picks
// the correct Steam/EGS address.
static void DrawForm() {
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(420, 220), ImGuiCond_FirstUseEver);
    ImGui::Begin("KH1Native Debug", nullptr, ImGuiWindowFlags_NoCollapse);

    static int itemId = 1;
    ImGui::InputInt("Item ID", &itemId);
    if (itemId < 1) itemId = 1;

    if (ImGui::Button("Spawn Prize", ImVec2(160, 0))) {
        AcquireSRWLockExclusive(&g_lock);
        strncpy_s(g_debugAction, "spawn_prize", _TRUNCATE);
        g_debugParam1 = itemId;
        g_debugActionPending = true;
        ReleaseSRWLockExclusive(&g_lock);
    }

    ImGui::Separator();
    static char customText[128] = "TEST";
    ImGui::InputText("Popup Text", customText, sizeof(customText));

    if (ImGui::Button("Show Popup", ImVec2(160, 0))) {
        AcquireSRWLockExclusive(&g_lock);
        strncpy_s(g_debugAction, "show_custom_popup", _TRUNCATE);
        strncpy_s(g_debugParamText, customText, _TRUNCATE);
        g_debugActionPending = true;
        ReleaseSRWLockExclusive(&g_lock);
    }

    char result[256];
    AcquireSRWLockExclusive(&g_lock);
    strncpy_s(result, g_debugResult, _TRUNCATE);
    ReleaseSRWLockExclusive(&g_lock);

    ImGui::Separator();
    ImGui::TextWrapped("Last result: %s", result[0] ? result : "(none yet)");

    ImGui::End();
}

static DWORD WINAPI FormThread(LPVOID) {
    WNDCLASSEXA wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = FormWndProc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "KH1NativeDebugWndClass";
    wc.hCursor = LoadCursorA(nullptr, reinterpret_cast<LPCSTR>(IDC_ARROW));
    RegisterClassExA(&wc);

    g_hwnd = CreateWindowExA(WS_EX_TOPMOST, wc.lpszClassName, "KH1Native Debug",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 460, 280, nullptr, nullptr, wc.hInstance, nullptr);

    DXGI_SWAP_CHAIN_DESC scd = {};
    scd.BufferCount = 2;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = g_hwnd;
    scd.SampleDesc.Count = 1;
    scd.Windowed = TRUE;
    scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL level;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION, &scd, &g_swapChain, &g_device, &level, &g_context);

    char msg[128];
    snprintf(msg, sizeof(msg), "Form window D3D11CreateDeviceAndSwapChain hr=0x%08lX", static_cast<unsigned long>(hr));
    LogDebug(msg);
    if (FAILED(hr)) return 0;

    CreateRenderTarget();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;

    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);

    LogDebug("Debug form window + standalone ImGui context ready");

    while (!g_shuttingDown) {
        MSG msg2;
        while (PeekMessageA(&msg2, nullptr, 0, 0, PM_REMOVE)) {
            if (msg2.message == WM_QUIT) {
                g_shuttingDown = true;
                break;
            }
            TranslateMessage(&msg2);
            DispatchMessageA(&msg2);
        }
        if (g_shuttingDown) break;

        if (!g_formVisible) {
            Sleep(50);
            continue;
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        DrawForm();
        ImGui::Render();

        const float clearColor[4] = { 0.10f, 0.10f, 0.12f, 1.0f };
        g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_context->ClearRenderTargetView(g_rtv, clearColor);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        g_swapChain->Present(1, 0);
        Sleep(16);
    }

    // Reached on WM_QUIT or when DllMain(DLL_PROCESS_DETACH) asks us to stop --
    // either way, fully tear down before this thread returns so nothing is left
    // executing inside this DLL's code if/when it gets unloaded out from under us.
    LogDebug("Debug form window thread shutting down");
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
    if (g_swapChain) { g_swapChain->Release(); g_swapChain = nullptr; }
    if (g_context) { g_context->Release(); g_context = nullptr; }
    if (g_device) { g_device->Release(); g_device = nullptr; }
    if (g_hwnd) { DestroyWindow(g_hwnd); g_hwnd = nullptr; }
    return 0;
}

static void EnsureFormThreadStarted() {
    if (InterlockedCompareExchange(&g_formThreadStarted, 1, 0) == 0) {
        LogDebug("Spawning debug form window thread");
        CreateThread(nullptr, 0, FormThread, nullptr, 0, nullptr);
    }
}

static void ToggleFormVisibility() {
    EnsureFormThreadStarted();

    // First press only: the form thread needs a moment to create the window.
    for (int i = 0; i < 50 && !g_hwnd; ++i) {
        Sleep(10);
    }
    if (!g_hwnd) return;

    g_formVisible = !g_formVisible;
    if (g_formVisible) {
        ShowWindow(g_hwnd, SW_SHOW);
        SetForegroundWindow(g_hwnd);
    } else {
        ShowWindow(g_hwnd, SW_HIDE);
    }
}

// --- LUA-CALLABLE FUNCTIONS ---

// call_function(rva, arg1, arg2, ...) -> ok(boolean), result(integer) | errorMessage(string)
//
// `rva` is an offset from the main module's current runtime base (the same
// convention this library's existing ReadByte/WriteByte-style addresses use),
// not an absolute address -- this keeps call sites stable across an
// ASLR-relocated base. Up to 6 integer/pointer arguments are supported and
// are passed through to the target function unchanged (they are NOT treated
// as RVAs -- e.g. an already-dereferenced pointer from ReadLong() should be
// passed as-is).
//
// This executes arbitrary code in the game process. SafeCall guards against a
// hard crash, but a wrong address/argument can still corrupt game state even
// when the call itself doesn't fault. Prefer wrapping a specific, tested call
// sequence in a named Lua function (see kh1_lua_library.lua) rather than
// calling this directly from gameplay-facing code.
extern "C" int l_call_function(void* L) {
    int nargs = p_lua_gettop(L);
    if (nargs < 1) {
        p_lua_pushboolean(L, 0);
        p_lua_pushstring(L, "call_function requires at least an address");
        return 2;
    }

    unsigned long long rva = (unsigned long long)p_lua_tointegerx(L, 1, nullptr);
    int argCount = nargs - 1;
    if (argCount > MAX_CALL_ARGS) argCount = MAX_CALL_ARGS;

    unsigned long long args[MAX_CALL_ARGS] = {};
    for (int i = 0; i < argCount; ++i) {
        args[i] = (unsigned long long)p_lua_tointegerx(L, i + 2, nullptr);
    }

    unsigned long long base = (unsigned long long)GetModuleHandleA(nullptr);
    unsigned long long address = base + rva;

    unsigned long long result = 0;
    bool ok = SafeCall(address, args, argCount, result);

    if (!ok) {
        char msg[128];
        snprintf(msg, sizeof(msg), "call_function crashed: rva=0x%llx argCount=%d", rva, argCount);
        LogDebug(msg);
        p_lua_pushboolean(L, 0);
        p_lua_pushstring(L, "call_function: exception during call (bad address or arguments)");
        return 2;
    }

    p_lua_pushboolean(L, 1);
    p_lua_pushinteger(L, (long long)result);
    return 2;
}

// get_module_base() -> integer
// The main module's current runtime base address, for scripts that need to
// reason about/log absolute addresses rather than just supplying an RVA to
// call_function.
extern "C" int l_get_module_base(void* L) {
    p_lua_pushinteger(L, (long long)(unsigned long long)GetModuleHandleA(nullptr));
    return 1;
}

// write_floats(f1, f2, ...) -> address
//
// Lua has no way to take the address of a local value, but some game
// functions expect a pointer to a small packed struct/vector (e.g. an {x,y,z}
// position) rather than a plain integer/pointer argument. This writes up to
// 16 Lua numbers as packed 32-bit floats into a static scratch buffer and
// returns its address, for use as an argument to call_function.
//
// The buffer is a single static instance, overwritten by every call and only
// valid until the next write_floats call -- pass the returned address into
// call_function immediately, don't hold onto it.
static const int MAX_SCRATCH_FLOATS = 16;
static float g_scratchFloats[MAX_SCRATCH_FLOATS];

extern "C" int l_write_floats(void* L) {
    int nargs = p_lua_gettop(L);
    if (nargs > MAX_SCRATCH_FLOATS) nargs = MAX_SCRATCH_FLOATS;
    for (int i = 0; i < nargs; ++i) {
        g_scratchFloats[i] = (float)p_lua_tonumberx(L, i + 1, nullptr);
    }
    p_lua_pushinteger(L, (long long)(unsigned long long)(void*)g_scratchFloats);
    return 1;
}

// --- POPUP TEXT HOOK ---
// Persistent, in-process equivalent of the Cheat Engine prototype that
// proved this works (see kh1-widget-prize-system-findings memory / project
// history): patches fnc_draw_item_popup_entry right after it resolves the
// real item name into RDI, redirecting RDI to g_customTextBuffer instead
// whenever g_customTextActive is set. No Cheat Engine involved -- this DLL
// does its own nearby-memory allocation and (unlike CE's external
// pause_process) suspends every other thread in this process itself while
// patching the live instruction stream, then resumes them.
static unsigned char g_customTextBuffer[512] = {};
static volatile unsigned char g_customTextActive = 0;
static bool g_popupHookInstalled = false;

// Windows only relocates a VirtualAlloc'd region to the exact address you
// ask for (or fails) -- it never silently moves it elsewhere -- so probing
// outward from the target in fixed steps is a standard, reliable way to land
// a code cave within jmp/call rel32 range (matches the technique CE's own
// "preferred address" allocation uses under the hood).
static void* AllocateNear(void* target, size_t size) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    uintptr_t granularity = si.dwAllocationGranularity ? si.dwAllocationGranularity : 0x10000;
    uintptr_t targetAddr = (uintptr_t)target;
    const uintptr_t maxRange = 0x70000000; // stay well inside +/-2GB for rel32 safety margin

    for (uintptr_t offset = 0; offset < maxRange; offset += granularity) {
        uintptr_t candidates[2] = { targetAddr + offset, (targetAddr > offset) ? (targetAddr - offset) : 0 };
        for (int i = 0; i < 2; ++i) {
            uintptr_t addr = candidates[i];
            if (addr == 0) continue;
            addr -= addr % granularity;
            void* p = VirtualAlloc((void*)addr, size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
            if (p) return p;
        }
    }
    return nullptr;
}

// Suspends every thread in this process except the caller, so patching a
// live instruction stream can't race a torn fetch on another thread -- the
// same principle as always pause_process()-ing before a Cheat Engine patch,
// just done from inside the process instead of from an external debugger.
static std::vector<HANDLE> SuspendOtherThreads() {
    std::vector<HANDLE> handles;
    DWORD selfTid = GetCurrentThreadId();
    DWORD pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return handles;

    THREADENTRY32 te = {};
    te.dwSize = sizeof(te);
    if (Thread32First(snap, &te)) {
        do {
            if (te.dwSize >= (FIELD_OFFSET(THREADENTRY32, th32OwnerProcessID) + sizeof(te.th32OwnerProcessID))) {
                if (te.th32OwnerProcessID == pid && te.th32ThreadID != selfTid) {
                    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
                    if (h) {
                        SuspendThread(h);
                        handles.push_back(h);
                    }
                }
            }
            te.dwSize = sizeof(te);
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    return handles;
}

static void ResumeThreads(std::vector<HANDLE>& handles) {
    for (HANDLE h : handles) {
        ResumeThread(h);
        CloseHandle(h);
    }
    handles.clear();
}

// Builds and installs the hook stub. hookAddr/resumeAddr/callTargetAddr are
// absolute addresses (base + RVA), resolved by the caller from the
// version-correct SteamGlobal/EGSGlobal address tables -- this function
// itself has no version-specific knowledge at all.
//
// Original bytes at hookAddr are expected to be exactly:
//   mov rdi,rax   (3 bytes: 48 8B F8 or 48 89 C7)
//   call rel32    (5 bytes: E8 xx xx xx xx)
// i.e. fnc_draw_item_popup_entry stashing the resolved item-name pointer
// into RDI right before calling fnc_queue_item_popup_text. Verified via
// live Cheat Engine testing on both Steam and EGS builds before this was
// written -- see native/KH1Native and the project memory for the writeup.
//
// Stub layout (all in the allocated cave):
//   push rax
//   mov rax, &g_customTextActive
//   cmp byte ptr [rax],1
//   jne useOriginal
//   mov byte ptr [rax],0    ; one-shot: consuming the custom text clears it
//                           ; immediately, so it can never apply to a later,
//                           ; unrelated popup (the rendering itself happens
//                           ; on a later frame via the always-ticking queue
//                           ; consumer, so clearing from Lua synchronously
//                           ; right after triggering would clear it before
//                           ; this hook ever runs -- this is the only point
//                           ; that's actually safe to clear it).
//   mov rdi, &g_customTextBuffer
//   pop rax
//   jmp continueCall
//   useOriginal:
//   pop rax
//   mov rdi,rax
//   continueCall:
//   call callTargetAddr
//   jmp resumeAddr
static bool InstallPopupTextHook(unsigned long long hookAddr, unsigned long long resumeAddr, unsigned long long callTargetAddr) {
    if (g_popupHookInstalled) return true;

    unsigned char* hookPtr = (unsigned char*)(uintptr_t)hookAddr;
    bool movRdiRax = (hookPtr[0] == 0x48) &&
        ((hookPtr[1] == 0x8B && hookPtr[2] == 0xF8) || (hookPtr[1] == 0x89 && hookPtr[2] == 0xC7));
    bool callRel32 = (hookPtr[3] == 0xE8);
    if (!movRdiRax || !callRel32) {
        LogDebug("InstallPopupTextHook: unexpected original bytes at hook address, aborting");
        return false;
    }

    void* cave = AllocateNear((void*)(uintptr_t)hookAddr, 4096);
    if (!cave) {
        LogDebug("InstallPopupTextHook: failed to allocate a nearby code cave");
        return false;
    }

    unsigned char stub[64] = {};
    size_t off = 0;
    uint64_t flagAddr = (uint64_t)(uintptr_t)&g_customTextActive;
    uint64_t bufAddr = (uint64_t)(uintptr_t)&g_customTextBuffer;

    stub[off++] = 0x50; // push rax

    stub[off++] = 0x48; stub[off++] = 0xB8; // mov rax, flagAddr
    memcpy(stub + off, &flagAddr, 8); off += 8;

    stub[off++] = 0x80; stub[off++] = 0x38; stub[off++] = 0x01; // cmp byte ptr [rax],1

    stub[off++] = 0x0F; stub[off++] = 0x85; // jne rel32 (patched below)
    size_t jneOperand = off; off += 4;

    stub[off++] = 0xC6; stub[off++] = 0x00; stub[off++] = 0x00; // mov byte ptr [rax],0 (one-shot consume)

    stub[off++] = 0x48; stub[off++] = 0xBF; // mov rdi, bufAddr
    memcpy(stub + off, &bufAddr, 8); off += 8;

    stub[off++] = 0x58; // pop rax

    stub[off++] = 0xE9; // jmp rel32 (patched below) -> continueCall
    size_t jmpOperand = off; off += 4;

    size_t useOriginalPos = off;
    stub[off++] = 0x58; // pop rax
    stub[off++] = 0x48; stub[off++] = 0x8B; stub[off++] = 0xF8; // mov rdi,rax

    size_t continueCallPos = off;
    stub[off++] = 0xE8; // call rel32 (patched below)
    size_t callOperand = off; off += 4;

    stub[off++] = 0xE9; // jmp rel32 (patched below) -> resumeAddr
    size_t jmpBackOperand = off; off += 4;

    size_t stubLen = off;
    uintptr_t caveBase = (uintptr_t)cave;

    int32_t jneRel = (int32_t)((int64_t)useOriginalPos - (int64_t)(jneOperand + 4));
    memcpy(stub + jneOperand, &jneRel, 4);

    int32_t jmpRel = (int32_t)((int64_t)continueCallPos - (int64_t)(jmpOperand + 4));
    memcpy(stub + jmpOperand, &jmpRel, 4);

    int32_t callRel = (int32_t)((int64_t)callTargetAddr - (int64_t)(caveBase + callOperand + 4));
    memcpy(stub + callOperand, &callRel, 4);

    int32_t jmpBackRel = (int32_t)((int64_t)resumeAddr - (int64_t)(caveBase + jmpBackOperand + 4));
    memcpy(stub + jmpBackOperand, &jmpBackRel, 4);

    unsigned char patch[8];
    patch[0] = 0xE9;
    int32_t hookRel = (int32_t)((int64_t)caveBase - (int64_t)(hookAddr + 5));
    memcpy(patch + 1, &hookRel, 4);
    patch[5] = 0x90; patch[6] = 0x90; patch[7] = 0x90; // NOP-pad the leftover 3 bytes of the 8-byte window

    std::vector<HANDLE> threads = SuspendOtherThreads();

    memcpy(cave, stub, stubLen);

    DWORD oldProtect = 0;
    VirtualProtect((void*)(uintptr_t)hookAddr, 8, PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((void*)(uintptr_t)hookAddr, patch, 8);
    VirtualProtect((void*)(uintptr_t)hookAddr, 8, oldProtect, &oldProtect);

    FlushInstructionCache(GetCurrentProcess(), (void*)(uintptr_t)hookAddr, 8);
    FlushInstructionCache(GetCurrentProcess(), cave, stubLen);

    ResumeThreads(threads);

    g_popupHookInstalled = true;
    LogDebug("InstallPopupTextHook: installed successfully");
    return true;
}

// install_popup_text_hook(hookRva, resumeRva, callTargetRva) -> ok(boolean)
// Idempotent -- safe to call every time before using set_custom_popup_text.
extern "C" int l_install_popup_text_hook(void* L) {
    unsigned long long base = (unsigned long long)GetModuleHandleA(nullptr);
    unsigned long long hookRva = (unsigned long long)p_lua_tointegerx(L, 1, nullptr);
    unsigned long long resumeRva = (unsigned long long)p_lua_tointegerx(L, 2, nullptr);
    unsigned long long callTargetRva = (unsigned long long)p_lua_tointegerx(L, 3, nullptr);
    bool ok = InstallPopupTextHook(base + hookRva, base + resumeRva, base + callTargetRva);
    p_lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

// set_custom_popup_text(byteTable) -> (none)
// byteTable is a Lua array of KHSCII byte values (e.g. from GetKHSCII()).
// Takes effect on the next fnc_enqueue_item_popup-driven popup, real or
// triggered via show_item_popup -- until clear_custom_popup_text() is
// called. Has no effect unless install_popup_text_hook() already succeeded.
extern "C" int l_set_custom_popup_text(void* L) {
    unsigned long long len = p_lua_rawlen(L, 1);
    const unsigned long long maxLen = sizeof(g_customTextBuffer) - 1;
    if (len > maxLen) len = maxLen;
    for (unsigned long long i = 0; i < len; ++i) {
        p_lua_rawgeti(L, 1, (long long)(i + 1));
        g_customTextBuffer[i] = (unsigned char)p_lua_tointegerx(L, -1, nullptr);
        p_lua_settop(L, -2);
    }
    g_customTextBuffer[len] = 0;
    g_customTextActive = 1;
    return 0;
}

// clear_custom_popup_text() -> (none)
// Reverts fnc_draw_item_popup_entry to showing real item names again.
extern "C" int l_clear_custom_popup_text(void* L) {
    g_customTextActive = 0;
    return 0;
}

// poll_debug_action() -> nil | {action=, param1=}
//
// Called every Lua frame by the debug companion script. Also polls F6 to
// show/hide the debug window (same edge-triggered pattern KH1Overlay uses for
// F4), since the window's own thread can't safely call game functions itself.
extern "C" int l_poll_debug_action(void* L) {
    static bool lastF6 = false;
    bool currF6 = (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
    if (currF6 && !lastF6) {
        ToggleFormVisibility();
    }
    lastF6 = currF6;

    bool has;
    char action[32];
    long long param1;
    char paramText[256];
    AcquireSRWLockExclusive(&g_lock);
    has = g_debugActionPending;
    if (has) {
        strncpy_s(action, g_debugAction, _TRUNCATE);
        param1 = g_debugParam1;
        strncpy_s(paramText, g_debugParamText, _TRUNCATE);
        g_debugActionPending = false;
    }
    ReleaseSRWLockExclusive(&g_lock);

    if (!has) return 0;

    p_lua_createtable(L, 0, 3);
    p_lua_pushstring(L, action); p_lua_setfield(L, -2, "action");
    p_lua_pushinteger(L, param1); p_lua_setfield(L, -2, "param1");
    p_lua_pushstring(L, paramText); p_lua_setfield(L, -2, "param_text");
    return 1;
}

// set_debug_result(text) -> (none)
// Called by the debug companion script after dispatching a polled action, so
// the overlay window has something to show for what just happened.
extern "C" int l_set_debug_result(void* L) {
    const char* text = p_lua_tolstring(L, 1, nullptr);
    AcquireSRWLockExclusive(&g_lock);
    strncpy_s(g_debugResult, text ? text : "", _TRUNCATE);
    ReleaseSRWLockExclusive(&g_lock);
    return 0;
}

static const luaL_Reg kh1_native_lib[] = {
    {"call_function", reinterpret_cast<void*>(l_call_function)},
    {"get_module_base", reinterpret_cast<void*>(l_get_module_base)},
    {"write_floats", reinterpret_cast<void*>(l_write_floats)},
    {"install_popup_text_hook", reinterpret_cast<void*>(l_install_popup_text_hook)},
    {"set_custom_popup_text", reinterpret_cast<void*>(l_set_custom_popup_text)},
    {"clear_custom_popup_text", reinterpret_cast<void*>(l_clear_custom_popup_text)},
    {"poll_debug_action", reinterpret_cast<void*>(l_poll_debug_action)},
    {"set_debug_result", reinterpret_cast<void*>(l_set_debug_result)},
    {nullptr, nullptr}
};

// LuaBackend (the OpenKH Lua host) embeds the Lua 5.4 runtime in its own DLL
// rather than loading a separate "lua54.dll", and that host DLL's name varies
// by build/game. So instead of guessing a filename, walk every module loaded
// in this process and use whichever one actually exports the Lua C API.
static HMODULE FindLuaModule() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) return nullptr;

    HMODULE found = nullptr;
    MODULEENTRY32W me = {};
    me.dwSize = sizeof(me);
    if (Module32FirstW(snap, &me)) {
        do {
            if (GetProcAddress(me.hModule, "lua_gettop")) {
                found = me.hModule;
                char msg[MAX_PATH + 32];
                snprintf(msg, sizeof(msg), "Found Lua API in module: %ls", me.szModule);
                LogDebug(msg);
                break;
            }
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return found;
}

extern "C" __declspec(dllexport) int luaopen_kh1_native(void* L) {
    LogDebug("luaopen_kh1_native called");

    HMODULE hLua = FindLuaModule();
    if (hLua && !p_lua_gettop) {
        p_lua_gettop      = (t_lua_gettop)      GetProcAddress(hLua, "lua_gettop");
        p_lua_tointegerx  = (t_lua_tointegerx)  GetProcAddress(hLua, "lua_tointegerx");
        p_lua_tonumberx   = (t_lua_tonumberx)   GetProcAddress(hLua, "lua_tonumberx");
        p_lua_pushinteger = (t_lua_pushinteger) GetProcAddress(hLua, "lua_pushinteger");
        p_lua_pushboolean = (t_lua_pushboolean) GetProcAddress(hLua, "lua_pushboolean");
        p_lua_pushstring  = (t_lua_pushstring)  GetProcAddress(hLua, "lua_pushstring");
        p_luaL_setfuncs   = (t_luaL_setfuncs)   GetProcAddress(hLua, "luaL_setfuncs");
        p_lua_createtable = (t_lua_createtable) GetProcAddress(hLua, "lua_createtable");
        p_lua_setfield    = (t_lua_setfield)    GetProcAddress(hLua, "lua_setfield");
        p_lua_tolstring   = (t_lua_tolstring)   GetProcAddress(hLua, "lua_tolstring");
        p_lua_rawlen      = (t_lua_rawlen)      GetProcAddress(hLua, "lua_rawlen");
        p_lua_rawgeti     = (t_lua_rawgeti)     GetProcAddress(hLua, "lua_rawgeti");
        p_lua_settop      = (t_lua_settop)      GetProcAddress(hLua, "lua_settop");
    }

    if (!p_lua_gettop || !p_lua_tointegerx || !p_lua_tonumberx || !p_lua_pushinteger || !p_lua_pushboolean ||
        !p_lua_pushstring || !p_luaL_setfuncs || !p_lua_createtable || !p_lua_setfield || !p_lua_tolstring ||
        !p_lua_rawlen || !p_lua_rawgeti || !p_lua_settop) {
        // Couldn't find a loaded module exporting the Lua C API -- bail out
        // without touching any of them. Returning 0 (no pushed values) makes
        // require() hand back `true` rather than crashing on a null function
        // pointer call.
        LogDebug("luaopen_kh1_native: failed to resolve Lua API exports, aborting safely");
        return 0;
    }

    p_lua_createtable(L, 0, 2);
    p_luaL_setfuncs(L, kh1_native_lib, 0);
    return 1;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        GetModuleFileNameA(hModule, g_dllDir, MAX_PATH);
        char* last = strrchr(g_dllDir, '\\');
        if (last) *(last + 1) = '\0';

        // Pin ourselves in memory with an extra reference we never release.
        // LuaBackend's script-refresh feature appears to FreeLibrary() native
        // modules it required as part of giving scripts a clean reload -- if
        // the debug form thread is still running at that exact moment, having
        // this DLL's code unmapped out from under it is an instant crash, and
        // waiting for the thread to exit from DLL_PROCESS_DETACH risks a
        // loader-lock deadlock instead (this is the same issue KH1Overlay's
        // dllmain.cpp documents and works around the same way). Holding an
        // extra reference means an external FreeLibrary() call just decrements
        // our refcount instead of actually unloading us, so the thread is
        // never disturbed and DLL_PROCESS_DETACH is never reached mid-session.
        char selfPath[MAX_PATH];
        GetModuleFileNameA(hModule, selfPath, MAX_PATH);
        LoadLibraryA(selfPath);
    } else if (reason == DLL_PROCESS_DETACH) {
        // Only reached on real process shutdown now. When lpReserved is
        // non-null the process is terminating and other threads may already
        // be gone, so per Microsoft's own guidance we must not synchronize
        // with anything here -- just let the OS reclaim everything.
        (void)lpReserved;
        g_shuttingDown = true;
    }
    return TRUE;
}
