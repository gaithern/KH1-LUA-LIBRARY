#include "pch.h"
#include <cstring>
#include "call_bridge.h"
#include "evdl_syscall.h"

static unsigned char g_scratchScriptCtx[4512] = {};


// Used for running EVDL syscalls.  Creates a mocked
// script context as the first passed argument, as EVDL 
// syscalls require one. Actual function arugments are 
// pulled off the stack.  Currently don't support
// return values though.
bool CallEvdlSyscall(unsigned long long rva, const int32_t* stack, size_t len, unsigned long long& outResult) {
    if (len > MAX_SYSCALL_STACK) len = MAX_SYSCALL_STACK;

    memset(g_scratchScriptCtx, 0, sizeof(g_scratchScriptCtx));
    if (len > 0) memcpy(g_scratchScriptCtx + 408, stack, len * sizeof(int32_t));
    int32_t stackIdx = (int32_t)(len > 0 ? len - 1 : 0);
    memcpy(g_scratchScriptCtx + 404, &stackIdx, sizeof(stackIdx));

    unsigned long long args[1] = { (unsigned long long)(uintptr_t)g_scratchScriptCtx };
    return SafeCall(GetGameBase() + rva, args, 1, outResult);
}
