#pragma once
#include <cstdint>
#include "kh1_native.h"

// Hook registry behind KH1NativeApi. Hooks are keyed by name and live until
// the process exits, so a Lua hot reload that installs again is a no-op.
bool HookInline(const char* name, uintptr_t target, void* detour, void** original);
bool HookMid(const char* name, uintptr_t address, KH1MidHookFn fn);
bool HookPointer(const char* name, uintptr_t slot, void* expected, void* detour, void** original);

// True when the bytes at address match expected, so a hook only lands on the code it was written for.
bool BytesMatch(uintptr_t address, const uint8_t* expected, size_t len);
