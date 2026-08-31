#include "pch.h"
#include "log.h"

// Main entry point.
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        InitDllDir(hModule);
        char selfPath[MAX_PATH];
        GetModuleFileNameA(hModule, selfPath, MAX_PATH);
        LoadLibraryA(selfPath);
    } else if (reason == DLL_PROCESS_DETACH) {
        LogDebug(lpReserved ? "DllMain: DLL_PROCESS_DETACH (process terminating)" : "DllMain: DLL_PROCESS_DETACH (FreeLibrary)");
    }
    return TRUE;
}
