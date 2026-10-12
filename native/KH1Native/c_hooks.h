#pragma once

// Compiles a hook .c file (path relative to kh1_native.dll) with TinyCC and runs its install().
bool InstallC(const char* path);
