#include "pch.h"
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <safetyhook.hpp>
#include "hooks.h"
#include "log.h"
#include "process_memory.h"

static_assert(sizeof(KH1Context) == sizeof(safetyhook::Context), "KH1Context must mirror safetyhook::Context");
static_assert(offsetof(KH1Context, xmm) == offsetof(safetyhook::Context, xmm0), "KH1Context::xmm");
static_assert(offsetof(KH1Context, rflags) == offsetof(safetyhook::Context, rflags), "KH1Context::rflags");
static_assert(offsetof(KH1Context, rax) == offsetof(safetyhook::Context, rax), "KH1Context::rax");
static_assert(offsetof(KH1Context, rip) == offsetof(safetyhook::Context, rip), "KH1Context::rip");

struct InstalledHook {
    safetyhook::InlineHook inl;
    safetyhook::MidHook mid;
    void* original = nullptr;
};

static std::mutex g_hooksMutex;
static std::map<std::string, InstalledHook> g_hooks;

static void LogHook(const char* name, uintptr_t address, const char* result) {
    char msg[192];
    snprintf(msg, sizeof(msg), "hook %s at RVA 0x%llX: %s", name,
        (unsigned long long)(address - (uintptr_t)GetModuleHandleA(nullptr)), result);
    LogDebug(msg);
}

// Returns true and fills *original when name is already installed.
static bool AlreadyInstalled(const char* name, void** original) {
    auto it = g_hooks.find(name);
    if (it == g_hooks.end()) return false;
    if (original) *original = it->second.original;
    return true;
}

bool HookInline(const char* name, uintptr_t target, void* detour, void** original) {
    std::lock_guard<std::mutex> lock(g_hooksMutex);
    if (AlreadyInstalled(name, original)) return true;
    if (!target || !detour) { LogHook(name, target, "missing address"); return false; }
    auto hook = safetyhook::InlineHook::create(reinterpret_cast<void*>(target), detour);
    if (!hook) {
        char why[48];
        snprintf(why, sizeof(why), "inline hook failed (error %d)", (int)hook.error().type);
        LogHook(name, target, why);
        return false;
    }
    InstalledHook& h = g_hooks[name];
    h.inl = std::move(*hook);
    h.original = h.inl.original<void*>();
    if (original) *original = h.original;
    LogHook(name, target, "installed");
    return true;
}

bool HookMid(const char* name, uintptr_t address, KH1MidHookFn fn) {
    std::lock_guard<std::mutex> lock(g_hooksMutex);
    if (AlreadyInstalled(name, nullptr)) return true;
    if (!address || !fn) { LogHook(name, address, "missing address"); return false; }
    auto hook = safetyhook::MidHook::create(reinterpret_cast<void*>(address), reinterpret_cast<safetyhook::MidHookFn>(fn));
    if (!hook) {
        char why[48];
        snprintf(why, sizeof(why), "mid hook failed (error %d)", (int)hook.error().type);
        LogHook(name, address, why);
        return false;
    }
    g_hooks[name].mid = std::move(*hook);
    LogHook(name, address, "installed");
    return true;
}

bool HookPointer(const char* name, uintptr_t slot, void* expected, void* detour, void** original) {
    std::lock_guard<std::mutex> lock(g_hooksMutex);
    if (AlreadyInstalled(name, original)) return true;
    void** p = reinterpret_cast<void**>(slot);
    if (!slot || !detour || *p != expected) { LogHook(name, slot, "slot does not hold the expected pointer"); return false; }
    if (!PatchCode(p, &detour, sizeof(detour), false)) { LogHook(name, slot, "patch failed"); return false; }
    InstalledHook& h = g_hooks[name];
    h.original = expected;
    if (original) *original = expected;
    LogHook(name, slot, "installed");
    return true;
}

bool BytesMatch(uintptr_t address, const uint8_t* expected, size_t len) {
    uint8_t actual[32];
    if (!address || len > sizeof(actual)) return false;
    return GuardedMemcpy(actual, reinterpret_cast<const void*>(address), len) && memcmp(actual, expected, len) == 0;
}
