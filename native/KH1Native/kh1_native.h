#pragma once
// Shared internals between dllmain.cpp and the packages/ sources. Defined in dllmain.cpp.

#include <cstddef>

// --- Lua C API pointers, resolved at luaopen time ---
typedef int          (__cdecl* t_lua_gettop)(void* L);
typedef long long    (__cdecl* t_lua_tointegerx)(void* L, int idx, int* isnum);
typedef double       (__cdecl* t_lua_tonumberx)(void* L, int idx, int* isnum);
typedef const char*  (__cdecl* t_lua_tolstring)(void* L, int idx, size_t* len);
typedef void         (__cdecl* t_lua_pushinteger)(void* L, long long n);
typedef void         (__cdecl* t_lua_pushboolean)(void* L, int b);
typedef const char*  (__cdecl* t_lua_pushstring)(void* L, const char* s);
typedef void         (__cdecl* t_luaL_setfuncs)(void* L, const void* l, int nup);
typedef void         (__cdecl* t_lua_createtable)(void* L, int narr, int nrec);
typedef unsigned long long (__cdecl* t_lua_rawlen)(void* L, int idx);
typedef int          (__cdecl* t_lua_rawgeti)(void* L, int idx, long long n);
typedef void         (__cdecl* t_lua_settop)(void* L, int idx);

extern t_lua_gettop       p_lua_gettop;
extern t_lua_tointegerx   p_lua_tointegerx;
extern t_lua_tonumberx    p_lua_tonumberx;
extern t_lua_tolstring    p_lua_tolstring;
extern t_lua_pushinteger  p_lua_pushinteger;
extern t_lua_pushboolean  p_lua_pushboolean;
extern t_lua_pushstring   p_lua_pushstring;
extern t_luaL_setfuncs    p_luaL_setfuncs;
extern t_lua_createtable  p_lua_createtable;
extern t_lua_rawlen       p_lua_rawlen;
extern t_lua_rawgeti      p_lua_rawgeti;
extern t_lua_settop       p_lua_settop;

// Appends a line to kh1_native.log next to the DLL.
void LogDebug(const char* msg);

// Calls base-relative game code SEH-wrapped; false means it faulted.
bool SafeCall(unsigned long long address, const unsigned long long* args, int argCount,
              unsigned long long& outResult);

// Enemy spawn (packages/spawn_enemy.cpp).
extern "C" int l_spawn_enemy(void* L);
// Read-only "would spawn_enemy refuse right now?" -- same gates, no side effects.
extern "C" int l_spawn_enemy_precheck(void* L);
