#include "pch.h"
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include "c_hooks.h"
#include "hooks.h"
#include "log.h"
#include "process_memory.h"
#include "symbols.h"

// Hook .c files compiled at load time by TinyCC (libtcc.dll, loaded on first
// use so kh1_native still works without it). The C side sees kh1_native.h.

typedef struct TCCState TCCState;
typedef TCCState* (*tcc_new_t)(void);
typedef void (*tcc_set_error_func_t)(TCCState*, void*, void (*)(void*, const char*));
typedef void (*tcc_set_options_t)(TCCState*, const char*);
typedef int (*tcc_add_include_path_t)(TCCState*, const char*);
typedef int (*tcc_add_file_t)(TCCState*, const char*);
typedef int (*tcc_set_output_type_t)(TCCState*, int);
typedef int (*tcc_add_symbol_t)(TCCState*, const char*, const void*);
typedef int (*tcc_relocate_t)(TCCState*, void*);
typedef void* (*tcc_get_symbol_t)(TCCState*, const char*);

static const int TCC_OUTPUT_MEMORY = 1;
static void* const TCC_RELOCATE_AUTO = (void*)1;

struct Tcc {
    tcc_new_t new_;
    tcc_set_error_func_t set_error_func;
    tcc_set_options_t set_options;
    tcc_add_include_path_t add_include_path;
    tcc_add_file_t add_file;
    tcc_set_output_type_t set_output_type;
    tcc_add_symbol_t add_symbol;
    tcc_relocate_t relocate;
    tcc_get_symbol_t get_symbol;
};

static std::mutex g_tccMutex;
static std::map<std::string, bool> g_installed;

static const Tcc* LoadTcc() {
    static Tcc tcc = {};
    static bool tried = false;
    if (tried) return tcc.new_ ? &tcc : nullptr;
    tried = true;
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%slibtcc.dll", GetDllDir());
    HMODULE mod = LoadLibraryA(path);
    if (!mod) { LogDebug("install_c: libtcc.dll not found next to kh1_native.dll"); return nullptr; }
    tcc.new_ = (tcc_new_t)GetProcAddress(mod, "tcc_new");
    tcc.set_error_func = (tcc_set_error_func_t)GetProcAddress(mod, "tcc_set_error_func");
    tcc.set_options = (tcc_set_options_t)GetProcAddress(mod, "tcc_set_options");
    tcc.add_include_path = (tcc_add_include_path_t)GetProcAddress(mod, "tcc_add_include_path");
    tcc.add_file = (tcc_add_file_t)GetProcAddress(mod, "tcc_add_file");
    tcc.set_output_type = (tcc_set_output_type_t)GetProcAddress(mod, "tcc_set_output_type");
    tcc.add_symbol = (tcc_add_symbol_t)GetProcAddress(mod, "tcc_add_symbol");
    tcc.relocate = (tcc_relocate_t)GetProcAddress(mod, "tcc_relocate");
    tcc.get_symbol = (tcc_get_symbol_t)GetProcAddress(mod, "tcc_get_symbol");
    if (!tcc.set_error_func || !tcc.set_options || !tcc.add_include_path || !tcc.add_file || !tcc.set_output_type
        || !tcc.add_symbol || !tcc.relocate || !tcc.get_symbol) {
        tcc.new_ = nullptr;
        LogDebug("install_c: libtcc.dll is missing exports");
    }
    return tcc.new_ ? &tcc : nullptr;
}

// The functions kh1_native.h declares for the C side.
static int C_HookInline(const char* name, uintptr_t target, void* detour, void** original) {
    return HookInline(name, target, detour, original);
}
static int C_HookMid(const char* name, uintptr_t address, KH1MidHookFn fn) { return HookMid(name, address, fn); }
static int C_HookPointer(const char* name, uintptr_t slot, void* expected, void* detour, void** original) {
    return HookPointer(name, slot, expected, detour, original);
}
static void* C_PersistentBlock(const char* key, size_t size) { return PersistentBlock(key, strlen(key), size); }
static void C_Log(const char* msg) { LogDebug(msg); }

static void OnTccError(void* path, const char* msg) {
    char line[512];
    snprintf(line, sizeof(line), "install_c %s: %s", (const char*)path, msg);
    LogDebug(line);
}

// Kept free of C++ objects so a crashing install() can be caught.
static int GuardedInstall(int (*install)(void)) {
    __try {
        return install();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

bool InstallC(const char* path) {
    std::lock_guard<std::mutex> lock(g_tccMutex);
    auto it = g_installed.find(path);
    if (it != g_installed.end() && it->second) return true;

    const Tcc* tcc = LoadTcc();
    if (!tcc) return false;

    char fullPath[MAX_PATH];
    snprintf(fullPath, sizeof(fullPath), "%s%s", GetDllDir(), path);

    // Never deleted: the compiled code backs installed hooks for the rest of the process.
    TCCState* s = tcc->new_();
    tcc->set_error_func(s, (void*)path, OnTccError);
    tcc->set_options(s, "-nostdlib");
    tcc->set_output_type(s, TCC_OUTPUT_MEMORY);
    tcc->add_include_path(s, GetDllDir());
    tcc->add_symbol(s, "kh1_symbol", (const void*)&Symbol);
    tcc->add_symbol(s, "kh1_hook_inline", (const void*)&C_HookInline);
    tcc->add_symbol(s, "kh1_hook_mid", (const void*)&C_HookMid);
    tcc->add_symbol(s, "kh1_hook_pointer", (const void*)&C_HookPointer);
    tcc->add_symbol(s, "kh1_persistent_block", (const void*)&C_PersistentBlock);
    tcc->add_symbol(s, "kh1_log", (const void*)&C_Log);
    tcc->add_symbol(s, "memcmp", (const void*)&memcmp);
    tcc->add_symbol(s, "memcpy", (const void*)&memcpy);
    tcc->add_symbol(s, "memset", (const void*)&memset);

    bool ok = false;
    const char* result = "compile failed";
    if (tcc->add_file(s, fullPath) == 0 && tcc->relocate(s, TCC_RELOCATE_AUTO) >= 0) {
        auto install = (int (*)(void))tcc->get_symbol(s, "install");
        result = !install ? "no install() defined" : (ok = GuardedInstall(install) != 0) ? "ok" : "install() failed";
    }
    char msg[MAX_PATH + 64];
    snprintf(msg, sizeof(msg), "install_c %s: %s", path, result);
    LogDebug(msg);
    g_installed[path] = ok;
    return ok;
}
