#pragma once
#include <windows.h>

void InitDllDir(HMODULE module);
const char* GetDllDir();

void LogDebug(const char* msg);
