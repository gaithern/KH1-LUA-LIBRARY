#include "pch.h"
#include <cstdio>
#include <cstdint>
#include "log.h"
#include "lua_api.h"
#include "call_bridge.h"
#include "process_memory.h"
#include "evdl_syscall.h"

// Allows calling LogDebug from Lua
extern "C" int l_log_debug(void* L) {
    const char* msg = p_lua_tolstring(L, 1, nullptr);
    if (msg) LogDebug(msg);
    return 0;
}

// Allows calling exe function from Lua
// via SafeCall.
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

    unsigned long long result = 0;
    if (!SafeCall(GetGameBase() + rva, args, argCount, result)) {
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

// Allows getting the module base from Lua.
extern "C" int l_get_module_base(void* L) {
    p_lua_pushinteger(L, (long long)GetGameBase());
    return 1;
}

static const int MAX_SCRATCH_FLOATS = 16;
static float g_scratchFloats[MAX_SCRATCH_FLOATS];

// Allows writing Lua numbers into a static float
// scratch buffer in memory and returns its address.
// Used for passing float arrays to game functions.
extern "C" int l_write_floats(void* L) {
    int nargs = p_lua_gettop(L);
    if (nargs > MAX_SCRATCH_FLOATS) nargs = MAX_SCRATCH_FLOATS;
    for (int i = 0; i < nargs; ++i) {
        g_scratchFloats[i] = (float)p_lua_tonumberx(L, i + 1, nullptr);
    }
    p_lua_pushinteger(L, (long long)(unsigned long long)(void*)g_scratchFloats);
    return 1;
}

// Allows allocating memory from Lua.
extern "C" int l_allocate(void* L) {
    unsigned long long size = (unsigned long long)p_lua_tointegerx(L, 1, nullptr);
    bool executable = p_lua_tointegerx(L, 2, nullptr) != 0;
    void* p = AllocateBlock((size_t)size, executable);
    p_lua_pushinteger(L, (long long)(unsigned long long)p);
    return 1;
}

// Allows freeing allocated memory from Lua.
extern "C" int l_free(void* L) {
    unsigned long long addr = (unsigned long long)p_lua_tointegerx(L, 1, nullptr);
    p_lua_pushboolean(L, FreeBlock((void*)(uintptr_t)addr) ? 1 : 0);
    return 1;
}

// Allows allocating memory near a target address
// from Lua.  Useful for when we need to hook
// functions where only relative jumps fit in the
// hook site, meaning we can only reach +/- 2GB.
extern "C" int l_allocate_near(void* L) {
    unsigned long long target = (unsigned long long)p_lua_tointegerx(L, 1, nullptr);
    unsigned long long size = (unsigned long long)p_lua_tointegerx(L, 2, nullptr);
    void* p = (target != 0 && size != 0) ? AllocateNear((void*)(uintptr_t)target, (size_t)size) : nullptr;
    p_lua_pushinteger(L, (long long)(unsigned long long)p);
    return 1;
}

// Allows creating persistent memory that survives
// hot reloads from Lua.
extern "C" int l_persistent_block(void* L) {
    size_t klen = 0;
    const char* key = p_lua_tolstring(L, 1, &klen);
    unsigned long long size = (unsigned long long)p_lua_tointegerx(L, 2, nullptr);
    void* p = PersistentBlock(key, klen, (size_t)size);
    p_lua_pushinteger(L, (long long)(unsigned long long)p);
    return 1;
}

// Allows copying an array of bytes from source to 
// destination address from Lua, similar to 
// ReadArray/WriteArray.
extern "C" int l_copy_memory(void* L) {
    unsigned long long dest = (unsigned long long)p_lua_tointegerx(L, 1, nullptr);
    unsigned long long src = (unsigned long long)p_lua_tointegerx(L, 2, nullptr);
    unsigned long long size = (unsigned long long)p_lua_tointegerx(L, 3, nullptr);
    bool ok = dest != 0 && src != 0 && size > 0
        && GuardedMemcpy((void*)(uintptr_t)dest, (const void*)(uintptr_t)src, (size_t)size);
    p_lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

// Allows writing a Lua string's raw bytes to a 
// specific destination address from Lua.
extern "C" int l_write_bytes(void* L) {
    unsigned long long dest = (unsigned long long)p_lua_tointegerx(L, 1, nullptr);
    size_t len = 0;
    const char* src = p_lua_tolstring(L, 2, &len);
    bool ok = dest != 0 && src && len > 0 && GuardedMemcpy((void*)(uintptr_t)dest, src, len);
    p_lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

// Allows writing an array of bytecode to a
// destination address in a safe way from Lua.
extern "C" int l_patch_code(void* L) {
    unsigned long long dest = (unsigned long long)p_lua_tointegerx(L, 1, nullptr);
    size_t len = 0;
    const char* src = p_lua_tolstring(L, 2, &len);
    bool suspend = p_lua_tointegerx(L, 3, nullptr) != 0;
    bool ok = PatchCode((void*)(uintptr_t)dest, src, len, suspend);
    p_lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

// Allows calling EVDL Syscalls, requiring
// a mocked script context, from Lua.
extern "C" int l_call_evdl_syscall(void* L) {
    unsigned long long rva = (unsigned long long)p_lua_tointegerx(L, 1, nullptr);
    unsigned long long len = p_lua_rawlen(L, 2);
    if (len > MAX_SYSCALL_STACK) len = MAX_SYSCALL_STACK;

    int32_t stack[MAX_SYSCALL_STACK] = {};
    for (unsigned long long i = 0; i < len; ++i) {
        p_lua_rawgeti(L, 2, (long long)(i + 1));
        stack[i] = (int32_t)p_lua_tointegerx(L, -1, nullptr);
        p_lua_settop(L, -2);
    }

    unsigned long long result = 0;
    if (!CallEvdlSyscall(rva, stack, (size_t)len, result)) {
        char msg[128];
        snprintf(msg, sizeof(msg), "call_evdl_syscall crashed: rva=0x%llx stackLen=%llu", rva, len);
        LogDebug(msg);
        p_lua_pushboolean(L, 0);
        p_lua_pushstring(L, "call_evdl_syscall: exception during call (bad address or arguments)");
        return 2;
    }

    p_lua_pushboolean(L, 1);
    p_lua_pushinteger(L, (long long)result);
    return 2;
}

// Name to function registration table.
// The DLL's public API surface - those
// features to be exposed to Lua.
static const luaL_Reg kh1_native_lib[] = {
    {"log_debug", reinterpret_cast<void*>(l_log_debug)},
    {"call_function", reinterpret_cast<void*>(l_call_function)},
    {"get_module_base", reinterpret_cast<void*>(l_get_module_base)},
    {"write_floats", reinterpret_cast<void*>(l_write_floats)},
    {"allocate", reinterpret_cast<void*>(l_allocate)},
    {"allocate_near", reinterpret_cast<void*>(l_allocate_near)},
    {"free", reinterpret_cast<void*>(l_free)},
    {"copy_memory", reinterpret_cast<void*>(l_copy_memory)},
    {"write_bytes", reinterpret_cast<void*>(l_write_bytes)},
    {"patch_code", reinterpret_cast<void*>(l_patch_code)},
    {"persistent_block", reinterpret_cast<void*>(l_persistent_block)},
    {"call_evdl_syscall", reinterpret_cast<void*>(l_call_evdl_syscall)},
    {nullptr, nullptr}
};

// Require "kh1_native" from the lua side searches specifically for
// "luaopen_kh1_native" - comprised from "luaopen_" + module_name.
// Cannot be changed.
extern "C" __declspec(dllexport) int luaopen_kh1_native(void* L) {
    LogDebug("luaopen_kh1_native called");

    if (!ResolveLuaApi()) {
        LogDebug("luaopen_kh1_native: failed to resolve Lua API exports, aborting safely");
        return 0;
    }

    p_lua_createtable(L, 0, 2);
    p_luaL_setfuncs(L, kh1_native_lib, 0);
    return 1;
}
