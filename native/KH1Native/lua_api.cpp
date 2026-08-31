#include "pch.h"
#include <tlhelp32.h>
#include <cstdio>
#include "log.h"
#include "lua_api.h"

// Empty Lua C API function pointers
t_lua_gettop       p_lua_gettop       = nullptr;
t_lua_tointegerx   p_lua_tointegerx   = nullptr;
t_lua_tonumberx    p_lua_tonumberx    = nullptr;
t_lua_tolstring    p_lua_tolstring    = nullptr;
t_lua_pushinteger  p_lua_pushinteger  = nullptr;
t_lua_pushboolean  p_lua_pushboolean  = nullptr;
t_lua_pushstring   p_lua_pushstring   = nullptr;
t_luaL_setfuncs    p_luaL_setfuncs    = nullptr;
t_lua_createtable  p_lua_createtable  = nullptr;
t_lua_rawlen       p_lua_rawlen       = nullptr;
t_lua_rawgeti      p_lua_rawgeti      = nullptr;
t_lua_settop       p_lua_settop       = nullptr;

// List of Lua C API entry points to find
static const char* const kRequiredLuaExports[] = {
    "lua_gettop", "lua_tointegerx", "lua_tonumberx", "lua_tolstring", "lua_pushinteger",
    "lua_pushboolean", "lua_pushstring", "luaL_setfuncs", "lua_createtable",
    "lua_rawlen", "lua_rawgeti", "lua_settop",
};

// Checks to ensure the lua module found
// exports all our required Lua C API
// functions.
static bool ModuleExportsAllRequired(HMODULE mod) {
    if (!mod) return false;
    for (const char* name : kRequiredLuaExports) {
        if (!GetProcAddress(mod, name)) return false;
    }
    return true;
}

// Fall back if bundled lua module is not found,
// or if the bundled lua module doesn't export
// everything required for some reason.
// I don't think this is ever used, can probably
// cut this - but its system specific.
static HMODULE FindLuaModuleByProcessScan() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) return nullptr;

    HMODULE found = nullptr;
    MODULEENTRY32W me = {};
    me.dwSize = sizeof(me);
    if (Module32FirstW(snap, &me)) {
        do {
            if (ModuleExportsAllRequired(me.hModule)) {
                found = me.hModule;
                char msg[MAX_PATH + 32];
                snprintf(msg, sizeof(msg), "FindLuaModuleByProcessScan: found Lua API in module: %ls", me.szModule);
                LogDebug(msg);
                break;
            }
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return found;
}

// Request bundled lua module, or uses the fallback
// code if that fails.
static HMODULE FindLuaModule() {
    HMODULE bundled = GetModuleHandleA("lua54.dll");
    if (ModuleExportsAllRequired(bundled)) {
        LogDebug("FindLuaModule: resolved via bundled dll/lua54.dll");
        return bundled;
    }

    LogDebug("FindLuaModule: bundled lua54.dll not found or incomplete, falling back to process scan");
    return FindLuaModuleByProcessScan();
}

// Populates Lua C API function pointer globals.
// Returns true if all were found.
bool ResolveLuaApi() {
    HMODULE hLua = FindLuaModule();
    if (hLua && !p_lua_gettop) {
        p_lua_gettop      = (t_lua_gettop)      GetProcAddress(hLua, "lua_gettop");
        p_lua_tointegerx  = (t_lua_tointegerx)  GetProcAddress(hLua, "lua_tointegerx");
        p_lua_tonumberx   = (t_lua_tonumberx)   GetProcAddress(hLua, "lua_tonumberx");
        p_lua_tolstring   = (t_lua_tolstring)   GetProcAddress(hLua, "lua_tolstring");
        p_lua_pushinteger = (t_lua_pushinteger) GetProcAddress(hLua, "lua_pushinteger");
        p_lua_pushboolean = (t_lua_pushboolean) GetProcAddress(hLua, "lua_pushboolean");
        p_lua_pushstring  = (t_lua_pushstring)  GetProcAddress(hLua, "lua_pushstring");
        p_luaL_setfuncs   = (t_luaL_setfuncs)   GetProcAddress(hLua, "luaL_setfuncs");
        p_lua_createtable = (t_lua_createtable) GetProcAddress(hLua, "lua_createtable");
        p_lua_rawlen      = (t_lua_rawlen)      GetProcAddress(hLua, "lua_rawlen");
        p_lua_rawgeti     = (t_lua_rawgeti)     GetProcAddress(hLua, "lua_rawgeti");
        p_lua_settop      = (t_lua_settop)      GetProcAddress(hLua, "lua_settop");
    }

    return p_lua_gettop && p_lua_tointegerx && p_lua_tonumberx && p_lua_tolstring
        && p_lua_pushinteger && p_lua_pushboolean && p_lua_pushstring && p_luaL_setfuncs
        && p_lua_createtable && p_lua_rawlen && p_lua_rawgeti && p_lua_settop;
}
