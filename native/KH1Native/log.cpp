#include "pch.h"
#include <cstdio>
#include <cstring>
#include "log.h"

// Holds the DLL directory.
static char g_dllDir[MAX_PATH] = "";

// Writes to log file for debugging.
void LogDebug(const char* msg) {
    if (!g_dllDir[0]) return;
    char path[MAX_PATH];
    snprintf(path, MAX_PATH, "%skh1_native.log", g_dllDir);
    FILE* f = nullptr;
    if (fopen_s(&f, path, "a") == 0 && f) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(f, "[%02d:%02d:%02d] %s\n", st.wHour, st.wMinute, st.wSecond, msg);
        fclose(f);
    }
}

// Finds the DLL directory.
void InitDllDir(HMODULE module) {
    GetModuleFileNameA(module, g_dllDir, MAX_PATH);
    char* last = strrchr(g_dllDir, '\\');
    if (last) *(last + 1) = '\0';
}

// Returns the DLL directory.
const char* GetDllDir() {
    return g_dllDir;
}
