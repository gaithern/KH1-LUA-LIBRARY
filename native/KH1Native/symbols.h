#pragma once
#include <cstdint>

// Addresses from the Steam/EGS globals files, read out of the calling
// script's Lua globals (VersionCheck must have run in that script).
void LoadSymbols(void* L);

// Absolute address (module base + RVA) of a global, 0 if unknown.
uintptr_t Symbol(const char* name);
