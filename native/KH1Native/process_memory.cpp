#include "pch.h"
#include <tlhelp32.h>
#include <cstring>
#include <vector>
#include "process_memory.h"

// Allocates a block of memory of a specified size.
void* AllocateBlock(size_t size, bool executable) {
    if (size == 0) return nullptr;
    DWORD prot = executable ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE;
    return VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, prot);
}

// Frees the previously allocated memory block.
bool FreeBlock(void* addr) {
    return addr != nullptr && VirtualFree(addr, 0, MEM_RELEASE) != 0;
}

// Allocates memory near a target address.
// Useful for when we need to hook functions where 
// only relative jumps fit in the hook site, meaning
// we can only reach +/- 2GB.
void* AllocateNear(void* target, size_t size) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    uintptr_t granularity = si.dwAllocationGranularity ? si.dwAllocationGranularity : 0x10000;
    uintptr_t targetAddr = (uintptr_t)target;
    const uintptr_t maxRange = 0x70000000;

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

struct PersistBlock { char key[64]; void* addr; };
static PersistBlock g_persistBlocks[32];
static int g_persistCount = 0;

// Creates persistent memory that survives
// hot reloads from Lua.  Key/value pairs.
void* PersistentBlock(const char* key, size_t keyLen, size_t size) {
    if (!key || keyLen == 0 || keyLen >= sizeof(g_persistBlocks[0].key)) return nullptr;
    for (int i = 0; i < g_persistCount; ++i) {
        if (memcmp(g_persistBlocks[i].key, key, keyLen) == 0 && g_persistBlocks[i].key[keyLen] == '\0') {
            return g_persistBlocks[i].addr;
        }
    }
    if (g_persistCount >= 32 || size == 0) return nullptr;
    void* p = VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!p) return nullptr;
    memcpy(g_persistBlocks[g_persistCount].key, key, keyLen);
    g_persistBlocks[g_persistCount].key[keyLen] = '\0';
    g_persistBlocks[g_persistCount].addr = p;
    g_persistCount++;
    return p;
}

// Plain memcpy that cannot crash the process.
bool GuardedMemcpy(void* dst, const void* src, size_t len) {
    __try {
        memcpy(dst, src, len);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Suspends every other thread and returns a
// a list of what was paused, so that patch_code
// can overwrite live instructions wihtout another
// thread running a half-written patch.
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

// Undo for function above.
static void ResumeThreads(std::vector<HANDLE>& handles) {
    for (HANDLE h : handles) {
        ResumeThread(h);
        CloseHandle(h);
    }
    handles.clear();
}

// Uses the functions above.  Suspends the
// threads, writes the patch, and resumes.
bool PatchCode(void* dest, const void* src, size_t len, bool suspendThreads) {
    if (!dest || !src || len == 0) return false;
    std::vector<HANDLE> threads;
    if (suspendThreads) threads = SuspendOtherThreads();
    bool ok = false;
    DWORD oldProtect = 0;
    if (VirtualProtect(dest, len, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        ok = GuardedMemcpy(dest, src, len);
        DWORD tmp = 0;
        VirtualProtect(dest, len, oldProtect, &tmp);
        FlushInstructionCache(GetCurrentProcess(), dest, len);
    }
    if (suspendThreads) ResumeThreads(threads);
    return ok;
}
