#include "pch.h"
#include <dbghelp.h>
#include <psapi.h>
#include <cstdio>
#include <cstdint>

#include "kh1_native.h"

// --- FULL CRASH DUMP (vectored exception handler) ---
// The game installs its own unhandled-exception filter (it writes CrashDump.dmp beside the exe and
// ships DBGHELP.dll), which swallows the exception so Windows Error Reporting never fires -- and its
// own dump omits process memory, which is what made every crash here unanalysable. A VEH runs
// FIRST-CHANCE, before any SEH, so we can write a full-memory dump and then hand the exception on
// untouched. Patches nothing in the game.
static volatile long g_fullDumpWritten = 0;

typedef BOOL(WINAPI* t_MiniDumpWriteDump)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
                                          PMINIDUMP_EXCEPTION_INFORMATION,
                                          PMINIDUMP_USER_STREAM_INFORMATION,
                                          PMINIDUMP_CALLBACK_INFORMATION);

// True if the address lies inside this DLL. Used to ignore our own probe faults.
static bool RipIsInThisModule(uint64_t rip) {
    HMODULE self = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)&g_fullDumpWritten, &self) || !self) return false;
    MODULEINFO mi = {};
    if (!GetModuleInformation(GetCurrentProcess(), self, &mi, sizeof(mi))) return false;
    uint64_t b = (uint64_t)(uintptr_t)self;
    return rip >= b && rip < b + mi.SizeOfImage;
}

static LONG CALLBACK FullDumpVectoredHandler(EXCEPTION_POINTERS* ep) {
    if (!ep || !ep->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;

    // Skip only OUR deliberate probe faults (SafeCall/TryReadU32), which fault at a RIP inside this
    // DLL. Anything else counts -- including CRT code like memcpy called from the game.
    uint64_t rip = (uint64_t)(uintptr_t)ep->ExceptionRecord->ExceptionAddress;
    if (RipIsInThisModule(rip)) return EXCEPTION_CONTINUE_SEARCH;

    HMODULE exe = GetModuleHandleA(nullptr);
    uint64_t base = (uint64_t)(uintptr_t)exe;

    // One dump per process -- these are ~1GB and the first fault is the informative one.
    if (InterlockedCompareExchange(&g_fullDumpWritten, 1, 0) != 0) return EXCEPTION_CONTINUE_SEARCH;

    HMODULE dbg = LoadLibraryA("dbghelp.dll");
    t_MiniDumpWriteDump writeDump =
        dbg ? (t_MiniDumpWriteDump)GetProcAddress(dbg, "MiniDumpWriteDump") : nullptr;
    if (!writeDump) { LogDebug("FullDump: dbghelp/MiniDumpWriteDump unavailable"); return EXCEPTION_CONTINUE_SEARCH; }

    CreateDirectoryA("C:\\KH1CrashDumps", nullptr);
    char path[MAX_PATH];
    SYSTEMTIME st; GetLocalTime(&st);
    snprintf(path, sizeof(path), "C:\\KH1CrashDumps\\kh1_full_%02d%02d%02d_%lu.dmp",
             st.wHour, st.wMinute, st.wSecond, (unsigned long)GetCurrentProcessId());

    HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) { LogDebug("FullDump: could not create dump file"); return EXCEPTION_CONTINUE_SEARCH; }

    MINIDUMP_EXCEPTION_INFORMATION mei = {};
    mei.ThreadId = GetCurrentThreadId();
    mei.ExceptionPointers = ep;
    mei.ClientPointers = FALSE;
    BOOL ok = writeDump(GetCurrentProcess(), GetCurrentProcessId(), f,
                        (MINIDUMP_TYPE)(MiniDumpWithFullMemory | MiniDumpWithHandleData |
                                        MiniDumpWithThreadInfo | MiniDumpWithFullMemoryInfo),
                        &mei, nullptr, nullptr);
    CloseHandle(f);

    // Name the faulting module. A RIP in the CRT (e.g. memcpy) is useless on its own, so also report
    // the return address sitting at RSP -- that is the game-code caller.
    MODULEINFO emi = {};
    bool inExe = GetModuleInformation(GetCurrentProcess(), exe, &emi, sizeof(emi)) &&
                 rip >= base && rip < base + emi.SizeOfImage;
    char where[160];
    if (inExe) {
        snprintf(where, sizeof(where), "exe+0x%llX", (unsigned long long)(rip - base));
    } else {
        HMODULE m = nullptr; char mn[MAX_PATH] = "?";
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)rip, &m);
        if (m) GetModuleBaseNameA(GetCurrentProcess(), m, mn, sizeof(mn));
        uint64_t ret = 0;
        if (ep->ContextRecord && ep->ContextRecord->Rsp) {
            __try { ret = *(uint64_t*)ep->ContextRecord->Rsp; } __except (EXCEPTION_EXECUTE_HANDLER) { ret = 0; }
        }
        snprintf(where, sizeof(where), "%s+0x%llX (caller exe+0x%llX)", mn,
                 (unsigned long long)(rip - (uint64_t)(uintptr_t)m),
                 (unsigned long long)(ret - base));
    }
    char msg[MAX_PATH + 256];
    snprintf(msg, sizeof(msg), "FullDump: access violation at %s reading 0x%llX -- full dump %s: %s",
             where, (unsigned long long)ep->ExceptionRecord->ExceptionInformation[1],
             ok ? "written" : "FAILED", path);
    LogDebug(msg);
    return EXCEPTION_CONTINUE_SEARCH;   // let the game's own handler run exactly as before
}

// Installs the handler once. Called from luaopen_kh1_native.
void InstallCrashDumpHandler() {
    static bool installed = false;
    if (installed) return;
    installed = (AddVectoredExceptionHandler(1, FullDumpVectoredHandler) != nullptr);
    LogDebug(installed ? "FullDump: vectored handler installed"
                       : "FullDump: AddVectoredExceptionHandler FAILED");
}
