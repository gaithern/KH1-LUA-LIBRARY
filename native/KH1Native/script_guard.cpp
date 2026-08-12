#include "pch.h"
#include <cstdio>
#include <cstdint>

#include "kh1_native.h"

// --- CRASH SITE #6 GUARD (behavior-script VM block copy, Steam RVA 0x2CCFED) ---
// The class-0/sub-op-0xA opcode feeds fnc_resolve_resource_handle's raw result straight into memcpy
// as the SOURCE with no validation, unlike its sibling at 0x2CC6E7 which null-checks. Replaces the
// 5-byte CALL to the memcpy thunk with a CALL to an SEH-wrapped wrapper: RCX/RDX/R8 already hold
// dest/src/size, and the stub adds RSI (the VM context) in R9 so faults can be attributed.
static bool g_installed = false;
static volatile uint64_t g_faultCount = 0;
static const uint64_t FAULT_LOG_CAP = 12;   // faults arrive in bursts; cap so the log stays readable

static void SafeBehaviorScriptBlockCopy(void* dest, const void* src, size_t size, void* vmctx) {
    // dest IS the operand-stack slot the handle was read from at 0x2CCFD9, so the handle is still
    // sitting there until this copy overwrites it -- i.e. the exact value that resolved badly.
    uint32_t handle = 0;
    bool handleRead = false;
    __try { handle = *(volatile uint32_t*)dest; handleRead = true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { handleRead = false; }

    bool faulted = false;
    if (src == nullptr) {
        faulted = true;                      // resolve returned 0; copying would fault anyway
    } else {
        __try { memcpy(dest, src, size); }
        __except (EXCEPTION_EXECUTE_HANDLER) { faulted = true; }
    }
    if (!faulted) return;

    uint64_t n = ++g_faultCount;
    if (n > FAULT_LOG_CAP) return;           // destination left unwritten, silently from here on

    uint64_t base = (uint64_t)(uintptr_t)GetModuleHandleA(nullptr);
    uint64_t actor = vmctx ? (uint64_t)(uintptr_t)vmctx - 0x18 : 0;
    // Decode the handle the way fnc_resolve_resource_handle_impl does, so a zero handle, a zero
    // offset (always an unmapped 32MB-aligned bucket base) and a bad bucket are told apart.
    unsigned bucket = (handle & 0x7FFFFFFFu) >> 25;
    unsigned offset = handle & 0x1FFFFFFu;
    const char* verdict =
        !handleRead      ? "dest unreadable" :
        handle == 0      ? "handle is ZERO (field never minted)" :
        offset == 0      ? "offset 0 -> resolves to a bare 32MB bucket base (never mapped)" :
        src == nullptr   ? "handle non-zero but resolved to NULL (unclaimed bucket)" :
                           "resolved non-null but the copy faulted (freed/unmapped)";
    char msg[400];
    snprintf(msg, sizeof(msg),
        "BehaviorScriptCopyGuard fault #%llu: copy SKIPPED, dest left unwritten. "
        "handle=0x%08X (bucket %u, offset 0x%X) src=0x%llX size=0x%llX dest=exe+0x%llX "
        "actor=exe+0x%llX -- %s",
        (unsigned long long)n, handle, bucket, offset,
        (unsigned long long)(uintptr_t)src, (unsigned long long)size,
        (unsigned long long)((uint64_t)(uintptr_t)dest - base),
        (unsigned long long)(actor ? actor - base : 0), verdict);
    LogDebug(msg);
    if (n == FAULT_LOG_CAP) LogDebug("BehaviorScriptCopyGuard: further faults will not be logged");
}

bool InstallBehaviorScriptCopyGuardHook(unsigned long long hookAddr, unsigned long long resumeAddr) {
    if (g_installed) return true;

    unsigned char* hookPtr = (unsigned char*)(uintptr_t)hookAddr;
    static const unsigned char expected[5] = { 0xE8, 0x04, 0x80, 0x0B, 0x00 }; // call <memcpy thunk>
    if (memcmp(hookPtr, expected, 5) != 0) {
        LogDebug("InstallBehaviorScriptCopyGuardHook: unexpected original bytes, aborting");
        return false;
    }

    void* cave = AllocateNear((void*)(uintptr_t)hookAddr, 4096);
    if (!cave) {
        LogDebug("InstallBehaviorScriptCopyGuardHook: failed to allocate a nearby code cave");
        return false;
    }

    unsigned char stub[32] = {};
    size_t off = 0;
    stub[off++] = 0x49; stub[off++] = 0x89; stub[off++] = 0xF1;   // mov r9, rsi  (VM context)
    uint64_t wrapperAddr = (uint64_t)(uintptr_t)&SafeBehaviorScriptBlockCopy;
    stub[off++] = 0x48; stub[off++] = 0xB8;                       // mov rax, wrapper
    memcpy(stub + off, &wrapperAddr, 8); off += 8;
    stub[off++] = 0xFF; stub[off++] = 0xD0;                       // call rax
    stub[off++] = 0xE9;                                           // jmp rel32 -> resume
    size_t jmpOperand = off; off += 4;
    size_t stubLen = off;

    uintptr_t caveBase = (uintptr_t)cave;
    int32_t jmpRel = (int32_t)((int64_t)resumeAddr - (int64_t)(caveBase + jmpOperand + 4));
    memcpy(stub + jmpOperand, &jmpRel, 4);

    unsigned char patch[5];
    patch[0] = 0xE9;
    int32_t hookRel = (int32_t)((int64_t)caveBase - (int64_t)(hookAddr + 5));
    memcpy(patch + 1, &hookRel, 4);

    std::vector<HANDLE> threads = SuspendOtherThreads();
    memcpy(cave, stub, stubLen);
    DWORD oldProtect = 0;
    VirtualProtect((void*)(uintptr_t)hookAddr, 5, PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((void*)(uintptr_t)hookAddr, patch, 5);
    VirtualProtect((void*)(uintptr_t)hookAddr, 5, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), (void*)(uintptr_t)hookAddr, 5);
    FlushInstructionCache(GetCurrentProcess(), cave, stubLen);
    ResumeThreads(threads);

    g_installed = true;
    LogDebug("InstallBehaviorScriptCopyGuardHook: installed successfully");
    return true;
}
