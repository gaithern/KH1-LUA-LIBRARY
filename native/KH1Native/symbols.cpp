#include "pch.h"
#include <map>
#include <mutex>
#include <string>
#include "call_bridge.h"
#include "lua_api.h"
#include "symbols.h"

static std::mutex g_symbolsMutex;
static std::map<std::string, uintptr_t> g_symbols;

// Copies every integer global into the table. Non-address integers come
// along too; they are harmless because only named lookups read the table.
void LoadSymbols(void* L) {
    std::lock_guard<std::mutex> lock(g_symbolsMutex);
    p_lua_rawgeti(L, LUA_REGISTRYINDEX, LUA_RIDX_GLOBALS);
    p_lua_pushnil(L);
    while (p_lua_next(L, -2)) {
        if (p_lua_type(L, -2) == LUA_TSTRING && p_lua_isinteger(L, -1)) {
            const char* name = p_lua_tolstring(L, -2, nullptr);
            g_symbols[name] = (uintptr_t)p_lua_tointegerx(L, -1, nullptr);
        }
        p_lua_settop(L, -2);
    }
    p_lua_settop(L, -2);
}

uintptr_t Symbol(const char* name) {
    std::lock_guard<std::mutex> lock(g_symbolsMutex);
    auto it = g_symbols.find(name);
    return it == g_symbols.end() ? 0 : (uintptr_t)GetGameBase() + it->second;
}
