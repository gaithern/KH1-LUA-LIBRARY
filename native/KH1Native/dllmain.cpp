#include "pch.h"
#include <tlhelp32.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>

// --- DEBUG LOGGING ---
static char g_dllDir[MAX_PATH] = "";

static void LogDebug(const char* msg) {
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

// --- LUA FUNCTION POINTERS ---
// Resolved against whichever already-loaded module in the host process exports
// the Lua C API (no Lua headers needed) -- see FindLuaModule(). Same technique
// as KH1Overlay's dllmain.cpp in the randomizer repo; duplicated here rather
// than shared because this module is intentionally independent of it.
typedef int          (__cdecl* t_lua_gettop)(void* L);
typedef long long    (__cdecl* t_lua_tointegerx)(void* L, int idx, int* isnum);
typedef double       (__cdecl* t_lua_tonumberx)(void* L, int idx, int* isnum);
typedef void         (__cdecl* t_lua_pushinteger)(void* L, long long n);
typedef void         (__cdecl* t_lua_pushboolean)(void* L, int b);
typedef const char*  (__cdecl* t_lua_pushstring)(void* L, const char* s);
typedef void         (__cdecl* t_luaL_setfuncs)(void* L, const void* l, int nup);
typedef void         (__cdecl* t_lua_createtable)(void* L, int narr, int nrec);
typedef unsigned long long (__cdecl* t_lua_rawlen)(void* L, int idx);
typedef int          (__cdecl* t_lua_rawgeti)(void* L, int idx, long long n);
typedef void         (__cdecl* t_lua_settop)(void* L, int idx);

static t_lua_gettop       p_lua_gettop       = nullptr;
static t_lua_tointegerx   p_lua_tointegerx   = nullptr;
static t_lua_tonumberx    p_lua_tonumberx    = nullptr;
static t_lua_pushinteger  p_lua_pushinteger  = nullptr;
static t_lua_pushboolean  p_lua_pushboolean  = nullptr;
static t_lua_pushstring   p_lua_pushstring   = nullptr;
static t_luaL_setfuncs    p_luaL_setfuncs    = nullptr;
static t_lua_createtable  p_lua_createtable  = nullptr;
static t_lua_rawlen       p_lua_rawlen       = nullptr;
static t_lua_rawgeti      p_lua_rawgeti      = nullptr;
static t_lua_settop       p_lua_settop       = nullptr;

struct luaL_Reg { const char* name; void* func; };

// --- CALL BRIDGE ---
// Windows x64 has a single calling convention (first four integer/pointer args
// in RCX,RDX,R8,R9, remainder on the stack, 32-byte shadow space). Casting a
// raw address to a fixed-arity function pointer type of the right shape and
// calling it normally produces exactly that ABI -- no inline asm needed (x64
// MSVC doesn't support __asm anyway). Only integer/pointer args are supported;
// float/double args go in XMM registers under this ABI and are not marshaled
// here.
typedef unsigned long long(__fastcall* Func0)();
typedef unsigned long long(__fastcall* Func1)(unsigned long long);
typedef unsigned long long(__fastcall* Func2)(unsigned long long, unsigned long long);
typedef unsigned long long(__fastcall* Func3)(unsigned long long, unsigned long long, unsigned long long);
typedef unsigned long long(__fastcall* Func4)(unsigned long long, unsigned long long, unsigned long long, unsigned long long);
typedef unsigned long long(__fastcall* Func5)(unsigned long long, unsigned long long, unsigned long long, unsigned long long, unsigned long long);
typedef unsigned long long(__fastcall* Func6)(unsigned long long, unsigned long long, unsigned long long, unsigned long long, unsigned long long, unsigned long long);

static const int MAX_CALL_ARGS = 6;

// Kept separate from l_call_function so the __try block doesn't have to share
// a stack frame with any C++ object that has a destructor (MSVC forbids that).
//
// __except(EXCEPTION_EXECUTE_HANDLER) turns a hardware fault (e.g. an access
// violation from a bad address or a wrong argument) into a normal false
// return instead of taking down the whole game process -- this is the only
// safety net a generic "call anything" bridge can offer. It does not protect
// against a call that "succeeds" but corrupts game state because the address,
// argument count, or argument types were wrong for that function.
static bool SafeCall(unsigned long long address, const unsigned long long* args, int argCount, unsigned long long& outResult) {
    __try {
        switch (argCount) {
        case 0: outResult = ((Func0)address)(); break;
        case 1: outResult = ((Func1)address)(args[0]); break;
        case 2: outResult = ((Func2)address)(args[0], args[1]); break;
        case 3: outResult = ((Func3)address)(args[0], args[1], args[2]); break;
        case 4: outResult = ((Func4)address)(args[0], args[1], args[2], args[3]); break;
        case 5: outResult = ((Func5)address)(args[0], args[1], args[2], args[3], args[4]); break;
        default: outResult = ((Func6)address)(args[0], args[1], args[2], args[3], args[4], args[5]); break;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// --- LUA-CALLABLE FUNCTIONS ---

// call_function(rva, arg1, arg2, ...) -> ok(boolean), result(integer) | errorMessage(string)
//
// `rva` is an offset from the main module's current runtime base (the same
// convention this library's existing ReadByte/WriteByte-style addresses use),
// not an absolute address -- this keeps call sites stable across an
// ASLR-relocated base. Up to 6 integer/pointer arguments are supported and
// are passed through to the target function unchanged (they are NOT treated
// as RVAs -- e.g. an already-dereferenced pointer from ReadLong() should be
// passed as-is).
//
// This executes arbitrary code in the game process. SafeCall guards against a
// hard crash, but a wrong address/argument can still corrupt game state even
// when the call itself doesn't fault. Prefer wrapping a specific, tested call
// sequence in a named Lua function (see kh1_lua_library.lua) rather than
// calling this directly from gameplay-facing code.
extern "C" int l_call_function(void* L) {
    int nargs = p_lua_gettop(L);
    if (nargs < 1) {
        p_lua_pushboolean(L, 0);
        p_lua_pushstring(L, "call_function requires at least an address");
        return 2;
    }

    unsigned long long rva = (unsigned long long)p_lua_tointegerx(L, 1, nullptr);
    int argCount = nargs - 1;
    if (argCount > MAX_CALL_ARGS) argCount = MAX_CALL_ARGS;

    unsigned long long args[MAX_CALL_ARGS] = {};
    for (int i = 0; i < argCount; ++i) {
        args[i] = (unsigned long long)p_lua_tointegerx(L, i + 2, nullptr);
    }

    unsigned long long base = (unsigned long long)GetModuleHandleA(nullptr);
    unsigned long long address = base + rva;

    unsigned long long result = 0;
    bool ok = SafeCall(address, args, argCount, result);

    if (!ok) {
        char msg[128];
        snprintf(msg, sizeof(msg), "call_function crashed: rva=0x%llx argCount=%d", rva, argCount);
        LogDebug(msg);
        p_lua_pushboolean(L, 0);
        p_lua_pushstring(L, "call_function: exception during call (bad address or arguments)");
        return 2;
    }

    p_lua_pushboolean(L, 1);
    p_lua_pushinteger(L, (long long)result);
    return 2;
}

// get_module_base() -> integer
// The main module's current runtime base address, for scripts that need to
// reason about/log absolute addresses rather than just supplying an RVA to
// call_function.
extern "C" int l_get_module_base(void* L) {
    p_lua_pushinteger(L, (long long)(unsigned long long)GetModuleHandleA(nullptr));
    return 1;
}

// write_floats(f1, f2, ...) -> address
//
// Lua has no way to take the address of a local value, but some game
// functions expect a pointer to a small packed struct/vector (e.g. an {x,y,z}
// position) rather than a plain integer/pointer argument. This writes up to
// 16 Lua numbers as packed 32-bit floats into a static scratch buffer and
// returns its address, for use as an argument to call_function.
//
// The buffer is a single static instance, overwritten by every call and only
// valid until the next write_floats call -- pass the returned address into
// call_function immediately, don't hold onto it.
static const int MAX_SCRATCH_FLOATS = 16;
static float g_scratchFloats[MAX_SCRATCH_FLOATS];

extern "C" int l_write_floats(void* L) {
    int nargs = p_lua_gettop(L);
    if (nargs > MAX_SCRATCH_FLOATS) nargs = MAX_SCRATCH_FLOATS;
    for (int i = 0; i < nargs; ++i) {
        g_scratchFloats[i] = (float)p_lua_tonumberx(L, i + 1, nullptr);
    }
    p_lua_pushinteger(L, (long long)(unsigned long long)(void*)g_scratchFloats);
    return 1;
}

// --- ENEMY SPAWN (PLACEMENT-TABLE SPLICE) ---
// spawn_enemy(spawnFnRva, tablePtrRva, tableCountRva, x, y, z, species) ->
//   ok(boolean), entityPtr(integer) | errorMessage(string)
//
// Constructs a Heartless (or any other placement-table-driven entity) at an
// arbitrary world position by splicing a synthetic record into the current
// room's live placement table, then calling fnc_spawn_world_gimmick_entity
// (spawnFnRva) with that record's id -- the exact same low-level constructor
// the room's own loader uses for doors/chests/party/Heartless. See
// KH1-EVDL-TOOLS/docs/enemy_ai/heartless_field_spawn_investigation.md for the
// full record format and how this was reverse-engineered.
//
// WHY THIS HAS TO BE A NATIVE FUNCTION, NOT A CHEAT ENGINE SCRIPT: an entire
// session was spent proving that calling fnc_spawn_world_gimmick_entity via
// Cheat Engine's execute_code (which runs the call from a CE-injected
// foreign thread) reliably freezes then crashes the game -- confirmed with
// byte-exact real game data, ruling out bad data as the cause. Trivial and
// medium-complexity calls via the same CE mechanism were stable; only this
// function's full construction path, reached via a foreign thread, died.
// SafeCall below runs the same call in-process, from whatever thread the
// Lua script is already executing on (the game's own thread via LuaBackend),
// which is the untested-but-well-evidenced fix for that failure mode.
//
// HOW IT WORKS:
// 1. Reads the live placement-table pointer/count from tablePtrRva/
//    tableCountRva (DAT_14296b630/DAT_14296b628 on Steam).
// 2. Scans the CURRENT table for an existing record whose species byte
//    (record+0x55) matches the requested `species` -- this becomes a
//    template, since most of a placement record's ~30 fields (resolved
//    resource handles, scale, def-kind, orientation-branch selector, etc.)
//    aren't independently understood well enough to synthesize from
//    scratch. IMPORTANT LIMITATION: this means spawn_enemy can only spawn a
//    species that ALREADY has at least one native placement record in the
//    current room (e.g. Shadows, species index 30, in Traverse Town 2nd
//    District) -- it cannot yet conjure a species with zero presence in the
//    room's own placement data. Making this fully general would need either
//    a hand-built record for every species (a lot of unmapped fields per
//    species) or captured template records shipped as static data.
// 3. Allocates a fresh (count+1)-record buffer, copies the old table in,
//    appends a clone of the template record with a new id built from a real
//    category (3, character/actor) and this slot's own real index in the
//    resized table -- not an out-of-band category, since the constructed
//    entity's "kind" byte is derived directly from this same field, and
//    every kind-specific setup branch in fnc_spawn_world_gimmick_entity
//    (party-linkage for kind==3, the AI/motion-activation call for kind==2)
//    is keyed off it. An out-of-band category matches none of those
//    branches, so the entity renders but is completely non-interactable.
//    The requested x/y/z is written into the position fields
//    (record+0x1C/+0x20/+0x24 -- the RUNTIME record layout, which differs
//    from the .ard FILE layout, see the doc).
// 4. Repoints the two live globals at the new buffer/count and calls the
//    constructor with the new record's id.
//
// KNOWN LIMITATION: the old and every intermediate table buffer are
// intentionally leaked (not VirtualFree'd) -- the very first table belongs
// to the game's own allocator, not ours, so freeing it would be unsafe, and
// distinguishing "ours" from "the game's" reliably wasn't worth the
// complexity for what's meant to be an occasional debug/testing tool, not a
// hot-loop spawner. Each call leaks one VirtualAlloc allocation (minimum
// 64KB granularity even though the actual table is much smaller).
static const size_t PLACEMENT_RECORD_SIZE = 0x78;
static const int PLACEMENT_SPECIES_OFFSET = 0x55;
static const int PLACEMENT_POS_X_OFFSET = 0x1C;
static const int PLACEMENT_POS_Y_OFFSET = 0x20;
static const int PLACEMENT_POS_Z_OFFSET = 0x24;
static const int PLACEMENT_MODEL_HANDLE_OFFSET = 0x60;
static const int PLACEMENT_MOTION_HANDLE_OFFSET = 0x64;

// The per-species asset-load state struct (DAT_142869dd0 in Ghidra, 0x50
// bytes per species index) that loadedSpeciesPtrTable (passed in as
// loadedPtrTableRva, pointing at this struct's +0x48 resolved-pointer field)
// lives inside. Confirmed via decompiling FUN_140285ee0/FUN_140286420:
//  - a state byte at +3 starts at 0 and is explicitly zeroed by
//    fnc_load_gimmick_assets before it kicks off a fresh load for a slot,
//    then climbs (1/2/4/6...) as the async load progresses -- so state != 0
//    is a direct, general signal that "something has already touched this
//    local species-slot number this session," independent of and more
//    reliable than scanning the room's static placement records (a purely
//    scripted/runtime spawn can claim a slot without ever appearing there).
//  - a cached model-filename string at +4 (32 bytes) that FUN_140286420
//    string-compares against each new load request for the same slot --
//    matching means "reuse", mismatched means "evict and reload in place".
// Together these let l_spawn_enemy's slot-collision guard (below, where the
// load is actually triggered) tell "this slot already holds OUR species,
// safe to reuse" apart from "this slot holds a DIFFERENT creature, refuse
// rather than silently corrupt it".
static const int LOADED_SPECIES_STRIDE = 0x50;
static const int LOADED_SPECIES_STATE_OFFSET_FROM_PTR = -0x45;
static const int LOADED_SPECIES_MODEL_NAME_OFFSET_FROM_PTR = -0x44;
static const int LOADED_SPECIES_MODEL_NAME_SIZE = 0x20;

// Fallback templates for species with zero placement record in the current room.
// Captured live via Cheat Engine from a room where the species actually spawns
// (see KH1-EVDL-TOOLS/docs/enemy_ai/heartless_field_spawn_investigation.md,
// session 5), rather than hand-built -- most of a record's ~30 fields still
// aren't independently understood, so cloning a real one is the only reliable
// source. id (+0x0), position (+0x1C/+0x20/+0x24), species (+0x55), char-id
// (+0x4c), and weight (+0x59) all get overwritten/forced by l_spawn_enemy
// regardless of what this template carries -- every field NOT in that list
// is trusted as-is from the capture, so a still-unverified field could in
// principle cause a similar problem to the ones already found and fixed
// (record+8's stale resolved-handle crash, the species-byte transcription
// bug, the record+0x4c g_SoraObjPtr-hijack bug -- see l_spawn_enemy and the
// investigation doc, session 5, for all three).
static const uint8_t kFallbackTemplate_Species34_Soldier[PLACEMENT_RECORD_SIZE] = {
    0x03, 0x00, 0x03, 0x00, 0x22, 0x1C, 0x00, 0x00, 0xA0, 0xAD, 0x3F, 0x81, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xC8, 0x42, 0x00, 0x00,
    0x3F, 0xC3, 0x00, 0x00, 0xC8, 0xC3, 0x00, 0x80, 0xB5, 0x43, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3F,
    0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2C, 0x01, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x05, 0x22, 0x05, 0x00, 0x06, 0x04, 0x01, 0x27, 0x00, 0x00, 0x00, 0x00, 0x00, 0xB0,
    0x8E, 0x80, 0x20, 0xB0, 0x8E, 0x80, 0x4B, 0x41, 0x47, 0x45, 0x5F, 0x36, 0x5F, 0x31, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static const uint8_t* FindFallbackTemplate(uint8_t species) {
    if (species == 34) return kFallbackTemplate_Species34_Soldier;
    return nullptr;
}

// record+0x4c is a "character id" field fnc_spawn_world_gimmick_entity reads
// for any kind==3 entity: if it resolves (via FUN_140285030) to < 3, the
// entity is treated as an actual PARTY MEMBER and g_SoraObjPtr[that index]
// gets overwritten with the new entity's pointer. kFallbackTemplate_Species34_Soldier
// has 0 here -- confirmed live 2026-07-21 that this hijacked g_SoraObjPtr[0],
// replacing the game's own reference to the real Sora with the spawned
// Soldier (HUD face vanished, entity uninteractable, game crashed shortly
// after). Confirmed live the same session that REAL, natively-placed
// species-34 records read 0x12C (300) here instead, uniformly across every
// record checked -- a fixed, species-constant value (like the model/motion
// filename strings), not session-local data, so it's safe to force the same
// way species/position are forced. Only applied when the fallback template
// was used (usedFallback), never for an in-room clone, since a real record
// already carries its own correct value and this table only has verified
// data for species 34.
struct SpeciesCharId {
    uint8_t species;
    uint16_t charId;
    uint8_t weight; // record+0x59 -- also confirmed wrong in the template (39 vs real 4), not safety-critical but easy to fix alongside
};
static const SpeciesCharId kSpeciesCharId[] = {
    { 34, 300, 4 }, // Soldier
};
static bool FindCharId(uint8_t species, uint16_t* outCharId, uint8_t* outWeight) {
    for (const auto& entry : kSpeciesCharId) {
        if (entry.species == species) {
            *outCharId = entry.charId;
            *outWeight = entry.weight;
            return true;
        }
    }
    return false;
}

// record+0x60/+0x64 hold handles (same bucket-table encoding as record+8, see
// FUN_14038adc0) to the model/motion filename strings the async load callback
// (FUN_140286420) dereferences. Confirmed live 2026-07-21 across all 13
// Soldier records in a real room that these strings are genuinely
// species-constant, not per-record -- but the raw handle NUMBERS are
// session-relative (they encode an index into a table of 32MB-aligned heap
// regions allocated fresh each session), which is why reusing a captured
// template's raw handle crashed in a different session. Only species with a
// verified entry here get the asset-load trigger attempted at all; see
// l_spawn_enemy for the safe fallback when a species isn't listed.
struct SpeciesResourceStrings {
    uint8_t species;
    const char* modelPath;
    const char* motionPath;
};
static const SpeciesResourceStrings kSpeciesResourceStrings[] = {
    { 34, "xa_ex_2010.mdls", "xa_ex_2010.mset" }, // Soldier
};
static bool FindResourceStrings(uint8_t species, const char** outModel, const char** outMotion) {
    for (const auto& entry : kSpeciesResourceStrings) {
        if (entry.species == species) {
            *outModel = entry.modelPath;
            *outMotion = entry.motionPath;
            return true;
        }
    }
    return false;
}

extern "C" int l_spawn_enemy(void* L) {
    unsigned long long spawnFnRva = (unsigned long long)p_lua_tointegerx(L, 1, nullptr);
    unsigned long long tablePtrRva = (unsigned long long)p_lua_tointegerx(L, 2, nullptr);
    unsigned long long tableCountRva = (unsigned long long)p_lua_tointegerx(L, 3, nullptr);
    unsigned long long loadAssetsFnRva = (unsigned long long)p_lua_tointegerx(L, 4, nullptr);
    unsigned long long loadedPtrTableRva = (unsigned long long)p_lua_tointegerx(L, 5, nullptr);
    unsigned long long mintHandleFnRva = (unsigned long long)p_lua_tointegerx(L, 6, nullptr);
    unsigned long long evSystemFlagsRva = (unsigned long long)p_lua_tointegerx(L, 7, nullptr);
    float x = (float)p_lua_tonumberx(L, 8, nullptr);
    float y = (float)p_lua_tonumberx(L, 9, nullptr);
    float z = (float)p_lua_tonumberx(L, 10, nullptr);
    uint8_t species = (uint8_t)p_lua_tointegerx(L, 11, nullptr);

    unsigned long long base = (unsigned long long)GetModuleHandleA(nullptr);
    uint8_t** tablePtrAddr = (uint8_t**)(uintptr_t)(base + tablePtrRva);
    int32_t* tableCountAddr = (int32_t*)(uintptr_t)(base + tableCountRva);

    uint8_t* oldTable = *tablePtrAddr;
    int32_t oldCount = *tableCountAddr;
    if (!oldTable || oldCount <= 0 || oldCount > 4096) {
        p_lua_pushboolean(L, 0);
        p_lua_pushstring(L, "spawn_enemy: placement table not valid right now (wrong room state?)");
        return 2;
    }

    const uint8_t* templateRec = nullptr;
    for (int32_t i = 0; i < oldCount; ++i) {
        uint8_t* rec = oldTable + (size_t)i * PLACEMENT_RECORD_SIZE;
        if (rec[PLACEMENT_SPECIES_OFFSET] == species) {
            templateRec = rec;
            break;
        }
    }
    bool usedFallback = false;
    if (!templateRec) {
        templateRec = FindFallbackTemplate(species);
        usedFallback = (templateRec != nullptr);
    }
    if (!templateRec) {
        p_lua_pushboolean(L, 0);
        p_lua_pushstring(L, "spawn_enemy: no existing record of that species in this room, and no captured fallback template for it");
        return 2;
    }
    if (usedFallback) {
        LogDebug("spawn_enemy: using captured fallback template (species not native to this room) -- known-crash fields (handle at +8, species, char-id, weight) are forced/fixed, but not every field in this template is independently verified yet");
    }

    // Resolved once, up front, rather than down where it's actually used (by
    // the asset-load-trigger block below) -- the slot-collision guard right
    // after this needs modelPath too, and it MUST run before the placement
    // table is resized/swapped in below. (Confirmed live 2026-07-21: an
    // earlier version of this guard ran too late, after *tablePtrAddr/
    // *tableCountAddr were already updated -- a refused spawn still left a
    // permanent half-built phantom record in the table, growing
    // placementTableCount on every refusal even though no entity was ever
    // constructed. Doing the check here, before any of that, means a refusal
    // is a true no-op.)
    const char* modelPath = nullptr;
    const char* motionPath = nullptr;
    bool haveResourceStrings = FindResourceStrings(species, &modelPath, &motionPath);

    if (usedFallback && haveResourceStrings) {
        // Slot-collision guard. If this species slot's state byte is already
        // non-zero, something has loaded (or is loading) data into it this
        // session -- check whether the cached model filename matches what
        // we're about to request. A match means it's already US (or an
        // identical prior fallback spawn of the same species) and reuse is
        // exactly the intended, already-live-confirmed behavior (session 5:
        // "second spawn attempt worked perfectly"). A mismatch means a
        // DIFFERENT creature already owns this local slot number this
        // session -- calling fnc_load_gimmick_assets would evict its cached
        // data out from under it (confirmed via decompiling
        // FUN_140286420: a cached-filename mismatch for an already-touched
        // slot triggers an immediate in-place reload with the new request).
        // Refuse rather than risk that. Closes the gap flagged at the end of
        // session 5 -- the species=34 fallback previously only avoided this
        // by room-by-room coincidence, with nothing actually checking for
        // it. Live-confirmed 2026-07-21 (forged a fake different-species
        // cached filename via KH1-LUA-LIBRARY-DEBUG's Forge Species Slot
        // panel): refused cleanly, no crash. See
        // KH1-EVDL-TOOLS/docs/enemy_ai/heartless_field_spawn_investigation.md.
        volatile uint8_t* stateAddr = (volatile uint8_t*)(uintptr_t)(base + loadedPtrTableRva + LOADED_SPECIES_STATE_OFFSET_FROM_PTR + (size_t)species * LOADED_SPECIES_STRIDE);
        if (*stateAddr != 0) {
            const char* cachedName = (const char*)(uintptr_t)(base + loadedPtrTableRva + LOADED_SPECIES_MODEL_NAME_OFFSET_FROM_PTR + (size_t)species * LOADED_SPECIES_STRIDE);
            if (strncmp(cachedName, modelPath, LOADED_SPECIES_MODEL_NAME_SIZE) != 0) {
                char msg[192];
                snprintf(msg, sizeof(msg), "spawn_enemy: species=%d slot already holds a different creature's data this session (cached model file doesn't match %s) -- refusing to avoid corrupting it", species, modelPath);
                LogDebug(msg);
                p_lua_pushboolean(L, 0);
                p_lua_pushstring(L, "spawn_enemy: fallback species slot collides with another creature already active in this room -- refusing");
                return 2;
            }
        }
    }

    size_t newSize = (size_t)(oldCount + 1) * PLACEMENT_RECORD_SIZE;
    uint8_t* newTable = (uint8_t*)VirtualAlloc(nullptr, newSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!newTable) {
        p_lua_pushboolean(L, 0);
        p_lua_pushstring(L, "spawn_enemy: VirtualAlloc failed");
        return 2;
    }

    memcpy(newTable, oldTable, (size_t)oldCount * PLACEMENT_RECORD_SIZE);
    uint8_t* newRec = newTable + (size_t)oldCount * PLACEMENT_RECORD_SIZE;
    memcpy(newRec, templateRec, PLACEMENT_RECORD_SIZE);

    // record+0x08 is a cached resource-handle field. fnc_spawn_world_gimmick_entity
    // already has a self-heal for it: it resolves this handle, and if that resolves
    // to exactly 0 (unset), it re-resolves a fresh one from the global per-species
    // resource table (keyed purely by the species byte at +0x55) and writes it back.
    // A cloned template carries over whatever value was cached here in ITS room/
    // session, though -- confirmed live 2026-07-21 that reusing a captured
    // out-of-room template (Soldier, species 34) crashed the constructor
    // ("exception during constructor call", caught safely by SafeCall). The stale
    // handle most likely resolved to something non-zero-but-wrong in the new room
    // rather than cleanly 0, so the self-heal never triggered. Zeroing it here
    // forces the constructor's own existing fallback to always resolve it fresh
    // for whatever room this actually runs in, whether templateRec came from the
    // current room (harmless -- it'll just re-resolve the same thing) or a
    // captured fallback template (the actual fix). Untested against other
    // possibly-similar fields elsewhere in the record as of this change.
    memset(newRec + 8, 0, 4);

    // Category byte drives more than id lookup: fnc_spawn_world_gimmick_entity derives
    // the constructed entity's "kind" byte directly from this same high byte, and every
    // kind-specific setup branch in that constructor (party-linkage for kind==3, the
    // FUN_1402a10b0 AI/motion-activation call for kind==2) is keyed off it. The previous
    // out-of-band category 0x99 matched none of those branches, so a spawned entity
    // rendered but skipped all per-kind registration -- confirmed live 2026-07-21: it was
    // completely non-interactable (no lock-on, no hit detection) even while alive and
    // long before any room-reload slot reuse. Use the real category (3, character/actor,
    // matching the cloned template) with a real slot index (this record's own position
    // in the resized table) so the entity gets the same kind byte a legitimately
    // constructed record would.
    uint32_t newId = ((uint32_t)3 << 16) | ((uint32_t)oldCount & 0xFFFFu);
    memcpy(newRec + 0, &newId, 4);
    memcpy(newRec + PLACEMENT_POS_X_OFFSET, &x, 4);
    memcpy(newRec + PLACEMENT_POS_Y_OFFSET, &y, 4);
    memcpy(newRec + PLACEMENT_POS_Z_OFFSET, &z, 4);

    // Force the species byte to what was actually requested rather than trusting
    // whatever's baked into templateRec at this offset. For an in-room clone this
    // is a no-op (it's already equal, that's how templateRec was found). For a
    // captured fallback template it's a real safety net: confirmed live 2026-07-21
    // that a hand-transcribed static template (kFallbackTemplate_Species34_Soldier)
    // had a transcription error that put the WRONG byte at this exact offset,
    // spawning as species 0 (Sora) instead of 34 (Soldier) -- rendered as another
    // Sora and fell endlessly, presumably because the neighboring char-id field
    // this constructor checks (species_def+0x4c, see fnc_spawn_world_gimmick_entity)
    // was corrupted the same way and got misread as a party-member slot. This line
    // guarantees the species byte specifically is always correct regardless of
    // template provenance; it does NOT fix other fields a mistranscribed template
    // might still get wrong (that needs a clean re-capture, ideally generated
    // mechanically rather than hand-typed, to avoid repeating this mistake).
    newRec[PLACEMENT_SPECIES_OFFSET] = species;

    // Only for a captured fallback template (never an in-room clone, which
    // already carries a correct real value): force record+0x4c ("character
    // id") and record+0x59 (weight) to live-verified, species-constant
    // values instead of whatever the template happened to carry. This is
    // the critical safety fix for the g_SoraObjPtr hijack documented above
    // -- confirmed live 2026-07-21 that kFallbackTemplate_Species34_Soldier's
    // own char-id (0) hijacked Sora's own party slot; real records read 300.
    if (usedFallback) {
        uint16_t charId = 0;
        uint8_t weight = 0;
        if (FindCharId(species, &charId, &weight)) {
            memcpy(newRec + 0x4c, &charId, 2);
            newRec[0x59] = weight;
        } else {
            // No verified char-id data for this species -- refuse rather than
            // risk another silent g_SoraObjPtr hijack with an unknown value.
            p_lua_pushboolean(L, 0);
            p_lua_pushstring(L, "spawn_enemy: fallback template exists for this species but no verified char-id data -- refusing to risk a g_SoraObjPtr hijack");
            return 2;
        }
    }

    *tablePtrAddr = newTable;
    *tableCountAddr = oldCount + 1;

    // RE-ENABLED 2026-07-21 (third attempt), fundamentally different from the
    // first two rather than a retry of the same thing. Both earlier crashes
    // are now believed explained: record+0x60/+0x64 hold handles to the
    // model/motion filename strings FUN_140286420 dereferences during the
    // async load, encoded the same session-relative way as record+8 (see
    // FUN_14038adc0/FUN_14038aee0) -- a cloned template's raw handle NUMBER
    // for these fields is presumptively invalid in a different session
    // (it indexes a bucket table of 32MB-aligned heap regions allocated
    // fresh each run), which is almost certainly what crashed attempt 1.
    // Attempt 2's crash is separately explained by an unguarded write this
    // version no longer makes (see below).
    //
    // Fix: never copy the captured handle number. Mint a FRESH, this-session
    // handle for OUR OWN static string data via FUN_14038ad90 (mintHandleFnRva),
    // confirmed via decompiling its callees (FUN_14038ae10/FUN_14038aee0) to be
    // a generic, self-registering pointer-to-handle encoder -- it has no
    // requirement the pointer belong to any pre-existing game allocation,
    // it just dynamically registers a new bucket for whatever 32MB-aligned
    // region a never-seen pointer falls in. Only attempted for species with a
    // verified live-captured resource-string pair in kSpeciesResourceStrings
    // (species 34/Soldier only so far) -- for any other species this whole
    // block is skipped and spawn_enemy behaves exactly as before (no crash
    // risk, just no load trigger).
    //
    // Deliberately NOT setting g_EVSystemFlags this time (attempt 2 did,
    // matching the real caller fnc_0B5_load_model) -- that was a raw,
    // unguarded pointer write with no SafeCall/__try protection, unlike
    // everything else in this file, and is the leading theory for attempt 2's
    // crash (zero breakpoint hits were recorded on the ENTIRE call chain,
    // including fnc_load_gimmick_assets's own entry, meaning execution never
    // even reached it -- the flags write was the only new code before that
    // point). No real evidence it's load-bearing for safety here.
    // modelPath/motionPath/haveResourceStrings resolved earlier, alongside
    // the slot-collision guard above (which already ran, before the
    // placement table was touched).
    if (loadAssetsFnRva != 0 && mintHandleFnRva != 0 && haveResourceStrings) {
        unsigned long long mintFnAddr = base + mintHandleFnRva;

        unsigned long long modelArgs[1] = { (unsigned long long)(uintptr_t)modelPath };
        unsigned long long modelHandle = 0;
        bool modelOk = SafeCall(mintFnAddr, modelArgs, 1, modelHandle);

        unsigned long long motionArgs[1] = { (unsigned long long)(uintptr_t)motionPath };
        unsigned long long motionHandle = 0;
        bool motionOk = SafeCall(mintFnAddr, motionArgs, 1, motionHandle);

        if (modelOk && motionOk) {
            uint32_t modelHandle32 = (uint32_t)modelHandle;
            uint32_t motionHandle32 = (uint32_t)motionHandle;
            memcpy(newRec + PLACEMENT_MODEL_HANDLE_OFFSET, &modelHandle32, 4);
            memcpy(newRec + PLACEMENT_MOTION_HANDLE_OFFSET, &motionHandle32, 4);

            unsigned long long loadArgs[2] = { (unsigned long long)newId, 0 };
            unsigned long long loadResult = 0;
            bool loadOk = SafeCall(base + loadAssetsFnRva, loadArgs, 2, loadResult);
            if (!loadOk) {
                char msg[160];
                snprintf(msg, sizeof(msg), "spawn_enemy: asset-load trigger crashed: loadAssetsFnRva=0x%llx id=0x%x species=%d", loadAssetsFnRva, newId, species);
                LogDebug(msg);
                p_lua_pushboolean(L, 0);
                p_lua_pushstring(L, "spawn_enemy: exception during asset-load trigger call");
                return 2;
            }

            // Confirmed live 2026-07-21: a truly cold (first-ever-this-session)
            // load can take longer than the original 2s timeout -- constructing
            // anyway after a timeout produced an entity with position stuck at
            // (0,0,0), invisible and uninteractable, even though every record
            // field was correct. A second spawn attempt (species already cached
            // from the first load finishing in the background) worked
            // perfectly every time. Fix: refuse to construct rather than build
            // against not-yet-ready data -- the caller can just retry.
            volatile uint64_t* loadedPtrAddr = (volatile uint64_t*)(uintptr_t)(base + loadedPtrTableRva + (size_t)species * LOADED_SPECIES_STRIDE);
            const int kPollIntervalMs = 20;
            const int kMaxPolls = 500; // ~10 seconds -- generous for a cold first-time load
            bool loaded = false;
            for (int i = 0; i < kMaxPolls; ++i) {
                if (*loadedPtrAddr != 0) { loaded = true; break; }
                Sleep(kPollIntervalMs);
            }
            if (!loaded) {
                char msg[160];
                snprintf(msg, sizeof(msg), "spawn_enemy: asset load for species=%d did not complete within %dms, refusing to construct -- try again", species, kMaxPolls * kPollIntervalMs);
                LogDebug(msg);
                p_lua_pushboolean(L, 0);
                p_lua_pushstring(L, "spawn_enemy: asset load did not complete in time -- try again (this is normal for the first spawn of a species in a session)");
                return 2;
            }
        } else {
            LogDebug("spawn_enemy: minting a fresh resource-string handle crashed -- skipping asset-load trigger, constructing without it");
        }
    }
    (void)evSystemFlagsRva;

    unsigned long long spawnFnAddr = base + spawnFnRva;
    unsigned long long args[1] = { (unsigned long long)newId };
    unsigned long long result = 0;
    bool ok = SafeCall(spawnFnAddr, args, 1, result);

    if (!ok) {
        char msg[160];
        snprintf(msg, sizeof(msg), "spawn_enemy crashed: spawnFnRva=0x%llx id=0x%x species=%d", spawnFnRva, newId, species);
        LogDebug(msg);
        p_lua_pushboolean(L, 0);
        p_lua_pushstring(L, "spawn_enemy: exception during constructor call");
        return 2;
    }

    p_lua_pushboolean(L, 1);
    p_lua_pushinteger(L, (long long)result);
    return 2;
}

// --- EVDL SYSCALL BRIDGE ---
// call_evdl_syscall(rva, {arg1, arg2, ...}) -> ok(boolean), result(integer) | errorMessage(string)
//
// Some EVDL opcode handlers (fnc_000_open_window, fnc_001_display_message,
// fnc_002_close_window) don't use the normal x64 calling convention -- they
// are script-VM opcode handlers that read their arguments off a scriptCtx's
// m_scriptData.m_stack array (scriptCtx: m_stackIdx at offset 404,
// m_scriptData/m_stack starting at offset 408) rather than registers, the
// same way the real EVDL bytecode interpreter feeds them when a script
// executes a SYSCALL instruction. This builds a throwaway zeroed scriptCtx,
// writes the given integers into m_stack in push order (arg1 at m_stack[0],
// last arg at the top / m_stackIdx), and calls the handler with a pointer to
// it in RCX -- exactly the shape a real script's SYSCALL would set up. Only
// the scriptCtx fields these three handlers actually read (m_stackIdx,
// m_stack[0..N]) are touched; other fields (m_pTgtEntity, m_nextCmd) are left
// zeroed, which is a safe default outside a couple of documented
// room-specific position nudges in fnc_000_open_window (Halloween Town's
// tea-cup ride) that this doesn't reproduce. Verified live via Cheat Engine
// (chest-open text prompt) + static disassembly, 2026-07-12 -- see
// SteamGlobal_1_0_0_2.lua's fnc_000/001/002 comments.
static const unsigned long long MAX_SYSCALL_STACK = 32;
static unsigned char g_scratchScriptCtx[4512] = {};

extern "C" int l_call_evdl_syscall(void* L) {
    unsigned long long rva = (unsigned long long)p_lua_tointegerx(L, 1, nullptr);
    unsigned long long len = p_lua_rawlen(L, 2);
    if (len > MAX_SYSCALL_STACK) len = MAX_SYSCALL_STACK;

    memset(g_scratchScriptCtx, 0, sizeof(g_scratchScriptCtx));
    for (unsigned long long i = 0; i < len; ++i) {
        p_lua_rawgeti(L, 2, (long long)(i + 1));
        int32_t v = (int32_t)p_lua_tointegerx(L, -1, nullptr);
        p_lua_settop(L, -2);
        memcpy(g_scratchScriptCtx + 408 + i * 4, &v, 4);
    }
    int32_t stackIdx = (int32_t)(len > 0 ? len - 1 : 0);
    memcpy(g_scratchScriptCtx + 404, &stackIdx, 4);

    unsigned long long base = (unsigned long long)GetModuleHandleA(nullptr);
    unsigned long long address = base + rva;
    unsigned long long args[1] = { (unsigned long long)(uintptr_t)g_scratchScriptCtx };

    unsigned long long result = 0;
    bool ok = SafeCall(address, args, 1, result);
    if (!ok) {
        char msg[128];
        snprintf(msg, sizeof(msg), "call_evdl_syscall crashed: rva=0x%llx stackLen=%llu", rva, len);
        LogDebug(msg);
        p_lua_pushboolean(L, 0);
        p_lua_pushstring(L, "call_evdl_syscall: exception during call (bad address or arguments)");
        return 2;
    }

    p_lua_pushboolean(L, 1);
    p_lua_pushinteger(L, (long long)result);
    return 2;
}

// --- POPUP TEXT HOOK ---
// Persistent, in-process equivalent of the Cheat Engine prototype that
// proved this works (see kh1-widget-prize-system-findings memory / project
// history): patches fnc_draw_item_popup_entry right after it resolves the
// real item name into RDI, redirecting RDI to g_customTextBuffer instead
// whenever g_customTextActive is set. No Cheat Engine involved -- this DLL
// does its own nearby-memory allocation and (unlike CE's external
// pause_process) suspends every other thread in this process itself while
// patching the live instruction stream, then resumes them.
static unsigned char g_customTextBuffer[512] = {};
static volatile unsigned char g_customTextActive = 0;
static bool g_popupHookInstalled = false;

// Windows only relocates a VirtualAlloc'd region to the exact address you
// ask for (or fails) -- it never silently moves it elsewhere -- so probing
// outward from the target in fixed steps is a standard, reliable way to land
// a code cave within jmp/call rel32 range (matches the technique CE's own
// "preferred address" allocation uses under the hood).
static void* AllocateNear(void* target, size_t size) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    uintptr_t granularity = si.dwAllocationGranularity ? si.dwAllocationGranularity : 0x10000;
    uintptr_t targetAddr = (uintptr_t)target;
    const uintptr_t maxRange = 0x70000000; // stay well inside +/-2GB for rel32 safety margin

    for (uintptr_t offset = 0; offset < maxRange; offset += granularity) {
        uintptr_t candidates[2] = { targetAddr + offset, (targetAddr > offset) ? (targetAddr - offset) : 0 };
        for (int i = 0; i < 2; ++i) {
            uintptr_t addr = candidates[i];
            if (addr == 0) continue;
            addr -= addr % granularity;
            void* p = VirtualAlloc((void*)addr, size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
            if (p) return p;
        }
    }
    return nullptr;
}

// Suspends every thread in this process except the caller, so patching a
// live instruction stream can't race a torn fetch on another thread -- the
// same principle as always pause_process()-ing before a Cheat Engine patch,
// just done from inside the process instead of from an external debugger.
static std::vector<HANDLE> SuspendOtherThreads() {
    std::vector<HANDLE> handles;
    DWORD selfTid = GetCurrentThreadId();
    DWORD pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return handles;

    THREADENTRY32 te = {};
    te.dwSize = sizeof(te);
    if (Thread32First(snap, &te)) {
        do {
            if (te.dwSize >= (FIELD_OFFSET(THREADENTRY32, th32OwnerProcessID) + sizeof(te.th32OwnerProcessID))) {
                if (te.th32OwnerProcessID == pid && te.th32ThreadID != selfTid) {
                    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
                    if (h) {
                        SuspendThread(h);
                        handles.push_back(h);
                    }
                }
            }
            te.dwSize = sizeof(te);
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    return handles;
}

static void ResumeThreads(std::vector<HANDLE>& handles) {
    for (HANDLE h : handles) {
        ResumeThread(h);
        CloseHandle(h);
    }
    handles.clear();
}

// --- TEXT BOX HOOK ---
// Redirects a message-string pointer resolved from g_pEVStringDataPtr (a
// table scoped to whichever EVDL script is currently loaded -- not globally
// addressable arbitrary text) to an arbitrary Lua-supplied KHSCII buffer.
//
// Two independent hook sites feed the same window, and BOTH are needed:
//  1. Inside fnc_001_display_message itself -- but this only actually
//     displays anything when the window is already state==4 (idle/ready),
//     which a window freshly opened in the same call never is; otherwise it
//     just queues the message and returns early.
//  2. Inside fnc_display_message_on_window_opened -- the one-shot callback
//     that fires on whatever LATER frame the window's open animation
//     actually finishes, which is what displays the queued message in the
//     normal (fresh-window) case. Confirmed live via Cheat Engine breakpoint
//     tracing after the first hook alone was found to leave real text
//     showing every time -- fnc_001_display_message was taking its
//     early-return path on every call, and this second callback (with its
//     own independent g_pEVStringDataPtr resolution) was what actually ran.
//
// Both hook stubs self-clear g_textBoxActive the moment they actually
// consume it (rather than requiring a separate completion hook or a
// same-call clear_textbox_text(), which would race the deferred site #2 --
// the display can happen several frames after open_text_box's Lua call
// already returned). Whichever site fires first "wins"; the other then sees
// the flag already cleared and behaves normally.
static unsigned char g_textBoxBuffer[512] = {};
static volatile unsigned char g_textBoxActive = 0;
static bool g_textBoxHookInstalled = false;
static bool g_textBoxAnimHookInstalled = false;

// Site #1. Original bytes at hookAddr are expected to be exactly an 8-byte
// "mov rdx, qword ptr [r11+r10*8+disp32]" (REX.WRB 8B /r SIB disp32) -- the
// instruction that resolves g_pEVStringDataPtr[message_id] into RDX right
// before it's handed to the low-level "set window message" call. Verified
// via live Cheat Engine (chest-open text prompt) + static disassembly on
// both Steam and EGS builds -- see the plate comment on fnc_001_display_message.
//
// Stub layout (all in the allocated cave):
//   <original 8 bytes, replayed verbatim -- RDX gets the real pointer>
//   push rax
//   mov rax, &g_textBoxActive
//   cmp byte ptr [rax],1
//   jne skipOverride
//   mov byte ptr [rax],0   ; self-clear (one-shot)
//   mov rdx, &g_textBoxBuffer
//   skipOverride:
//   pop rax
//   jmp resumeAddr
static bool InstallTextBoxHook(unsigned long long hookAddr, unsigned long long resumeAddr) {
    if (g_textBoxHookInstalled) return true;

    unsigned char* hookPtr = (unsigned char*)(uintptr_t)hookAddr;
    bool movRdx = (hookPtr[0] == 0x4B) && (hookPtr[1] == 0x8B) && (hookPtr[2] == 0x94);
    if (!movRdx) {
        LogDebug("InstallTextBoxHook: unexpected original bytes at hook address, aborting");
        return false;
    }

    void* cave = AllocateNear((void*)(uintptr_t)hookAddr, 4096);
    if (!cave) {
        LogDebug("InstallTextBoxHook: failed to allocate a nearby code cave");
        return false;
    }

    unsigned char stub[64] = {};
    size_t off = 0;
    uint64_t flagAddr = (uint64_t)(uintptr_t)&g_textBoxActive;
    uint64_t bufAddr = (uint64_t)(uintptr_t)&g_textBoxBuffer;

    memcpy(stub + off, hookPtr, 8); off += 8; // replay original mov rdx,[r11+r10*8+disp32]

    stub[off++] = 0x50; // push rax

    stub[off++] = 0x48; stub[off++] = 0xB8; // mov rax, flagAddr
    memcpy(stub + off, &flagAddr, 8); off += 8;

    stub[off++] = 0x80; stub[off++] = 0x38; stub[off++] = 0x01; // cmp byte ptr [rax],1

    stub[off++] = 0x0F; stub[off++] = 0x85; // jne rel32 (patched below)
    size_t jneOperand = off; off += 4;

    stub[off++] = 0xC6; stub[off++] = 0x00; stub[off++] = 0x00; // mov byte ptr [rax],0

    stub[off++] = 0x48; stub[off++] = 0xBA; // mov rdx, bufAddr
    memcpy(stub + off, &bufAddr, 8); off += 8;

    size_t skipOverridePos = off;
    stub[off++] = 0x58; // pop rax

    stub[off++] = 0xE9; // jmp rel32 (patched below) -> resumeAddr
    size_t jmpBackOperand = off; off += 4;

    size_t stubLen = off;
    uintptr_t caveBase = (uintptr_t)cave;

    int32_t jneRel = (int32_t)((int64_t)skipOverridePos - (int64_t)(jneOperand + 4));
    memcpy(stub + jneOperand, &jneRel, 4);

    int32_t jmpBackRel = (int32_t)((int64_t)resumeAddr - (int64_t)(caveBase + jmpBackOperand + 4));
    memcpy(stub + jmpBackOperand, &jmpBackRel, 4);

    unsigned char patch[8];
    patch[0] = 0xE9;
    int32_t hookRel = (int32_t)((int64_t)caveBase - (int64_t)(hookAddr + 5));
    memcpy(patch + 1, &hookRel, 4);
    patch[5] = 0x90; patch[6] = 0x90; patch[7] = 0x90; // NOP-pad the leftover 3 bytes of the 8-byte window

    std::vector<HANDLE> threads = SuspendOtherThreads();

    memcpy(cave, stub, stubLen);

    DWORD oldProtect = 0;
    VirtualProtect((void*)(uintptr_t)hookAddr, 8, PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((void*)(uintptr_t)hookAddr, patch, 8);
    VirtualProtect((void*)(uintptr_t)hookAddr, 8, oldProtect, &oldProtect);

    FlushInstructionCache(GetCurrentProcess(), (void*)(uintptr_t)hookAddr, 8);
    FlushInstructionCache(GetCurrentProcess(), cave, stubLen);

    ResumeThreads(threads);

    g_textBoxHookInstalled = true;
    LogDebug("InstallTextBoxHook: installed successfully");
    return true;
}

// install_textbox_hook(hookRva, resumeRva) -> ok(boolean)
// Idempotent -- safe to call every time before using set_textbox_text.
extern "C" int l_install_textbox_hook(void* L) {
    unsigned long long base = (unsigned long long)GetModuleHandleA(nullptr);
    unsigned long long hookRva = (unsigned long long)p_lua_tointegerx(L, 1, nullptr);
    unsigned long long resumeRva = (unsigned long long)p_lua_tointegerx(L, 2, nullptr);
    bool ok = InstallTextBoxHook(base + hookRva, base + resumeRva);
    p_lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

// Site #2, inside fnc_display_message_on_window_opened. Original bytes at
// hookAddr are expected to be exactly a 9-byte
// "mov rdx, qword ptr [r12+rdx*8]; call fnc_leaf_display_message" window
// (4-byte MOV + 5-byte CALL rel32) -- steals part of the following
// instruction the same way InstallPopupTextHook does, since the MOV alone is
// too short to host a 5-byte jmp patch. Verified via live Cheat Engine +
// static disassembly on both Steam and EGS builds -- see the plate comment
// on fnc_display_message_on_window_opened.
//
// Stub layout (all in the allocated cave):
//   push rax
//   mov rax, &g_textBoxActive
//   cmp byte ptr [rax],1
//   jne useOriginal
//   mov byte ptr [rax],0   ; self-clear (one-shot)
//   mov rdx, &g_textBoxBuffer
//   pop rax
//   jmp continueCall
//   useOriginal:
//   pop rax
//   <original 4 bytes, replayed verbatim -- mov rdx,[r12+rdx*8]>
//   continueCall:
//   call callTargetAddr
//   jmp resumeAddr
static bool InstallTextBoxAnimHook(unsigned long long hookAddr, unsigned long long resumeAddr, unsigned long long callTargetAddr) {
    if (g_textBoxAnimHookInstalled) return true;

    unsigned char* hookPtr = (unsigned char*)(uintptr_t)hookAddr;
    bool movRdx = (hookPtr[0] == 0x49) && (hookPtr[1] == 0x8B) && (hookPtr[2] == 0x14) && (hookPtr[3] == 0xD4);
    bool callRel32 = (hookPtr[4] == 0xE8);
    if (!movRdx || !callRel32) {
        LogDebug("InstallTextBoxAnimHook: unexpected original bytes at hook address, aborting");
        return false;
    }

    void* cave = AllocateNear((void*)(uintptr_t)hookAddr, 4096);
    if (!cave) {
        LogDebug("InstallTextBoxAnimHook: failed to allocate a nearby code cave");
        return false;
    }

    unsigned char stub[64] = {};
    size_t off = 0;
    uint64_t flagAddr = (uint64_t)(uintptr_t)&g_textBoxActive;
    uint64_t bufAddr = (uint64_t)(uintptr_t)&g_textBoxBuffer;

    stub[off++] = 0x50; // push rax

    stub[off++] = 0x48; stub[off++] = 0xB8; // mov rax, flagAddr
    memcpy(stub + off, &flagAddr, 8); off += 8;

    stub[off++] = 0x80; stub[off++] = 0x38; stub[off++] = 0x01; // cmp byte ptr [rax],1

    stub[off++] = 0x0F; stub[off++] = 0x85; // jne rel32 (patched below)
    size_t jneOperand = off; off += 4;

    stub[off++] = 0xC6; stub[off++] = 0x00; stub[off++] = 0x00; // mov byte ptr [rax],0

    stub[off++] = 0x48; stub[off++] = 0xBA; // mov rdx, bufAddr
    memcpy(stub + off, &bufAddr, 8); off += 8;

    stub[off++] = 0x58; // pop rax

    stub[off++] = 0xE9; // jmp rel32 (patched below) -> continueCall
    size_t jmpOperand = off; off += 4;

    size_t useOriginalPos = off;
    stub[off++] = 0x58; // pop rax
    memcpy(stub + off, hookPtr, 4); off += 4; // replay original mov rdx,[r12+rdx*8]

    size_t continueCallPos = off;
    stub[off++] = 0xE8; // call rel32 (patched below)
    size_t callOperand = off; off += 4;

    stub[off++] = 0xE9; // jmp rel32 (patched below) -> resumeAddr
    size_t jmpBackOperand = off; off += 4;

    size_t stubLen = off;
    uintptr_t caveBase = (uintptr_t)cave;

    int32_t jneRel = (int32_t)((int64_t)useOriginalPos - (int64_t)(jneOperand + 4));
    memcpy(stub + jneOperand, &jneRel, 4);

    int32_t jmpRel = (int32_t)((int64_t)continueCallPos - (int64_t)(jmpOperand + 4));
    memcpy(stub + jmpOperand, &jmpRel, 4);

    int32_t callRel = (int32_t)((int64_t)callTargetAddr - (int64_t)(caveBase + callOperand + 4));
    memcpy(stub + callOperand, &callRel, 4);

    int32_t jmpBackRel = (int32_t)((int64_t)resumeAddr - (int64_t)(caveBase + jmpBackOperand + 4));
    memcpy(stub + jmpBackOperand, &jmpBackRel, 4);

    unsigned char patch[9];
    patch[0] = 0xE9;
    int32_t hookRel = (int32_t)((int64_t)caveBase - (int64_t)(hookAddr + 5));
    memcpy(patch + 1, &hookRel, 4);
    patch[5] = 0x90; patch[6] = 0x90; patch[7] = 0x90; patch[8] = 0x90; // NOP-pad the leftover 4 bytes of the 9-byte window

    std::vector<HANDLE> threads = SuspendOtherThreads();

    memcpy(cave, stub, stubLen);

    DWORD oldProtect = 0;
    VirtualProtect((void*)(uintptr_t)hookAddr, 9, PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((void*)(uintptr_t)hookAddr, patch, 9);
    VirtualProtect((void*)(uintptr_t)hookAddr, 9, oldProtect, &oldProtect);

    FlushInstructionCache(GetCurrentProcess(), (void*)(uintptr_t)hookAddr, 9);
    FlushInstructionCache(GetCurrentProcess(), cave, stubLen);

    ResumeThreads(threads);

    g_textBoxAnimHookInstalled = true;
    LogDebug("InstallTextBoxAnimHook: installed successfully");
    return true;
}

// install_textbox_anim_hook(hookRva, resumeRva, callTargetRva) -> ok(boolean)
// Idempotent -- safe to call every time before using set_textbox_text.
extern "C" int l_install_textbox_anim_hook(void* L) {
    unsigned long long base = (unsigned long long)GetModuleHandleA(nullptr);
    unsigned long long hookRva = (unsigned long long)p_lua_tointegerx(L, 1, nullptr);
    unsigned long long resumeRva = (unsigned long long)p_lua_tointegerx(L, 2, nullptr);
    unsigned long long callTargetRva = (unsigned long long)p_lua_tointegerx(L, 3, nullptr);
    bool ok = InstallTextBoxAnimHook(base + hookRva, base + resumeRva, base + callTargetRva);
    p_lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

// set_textbox_text(byteTable) -> (none)
// byteTable is a Lua array of KHSCII byte values (e.g. from GetKHSCII()).
// Takes effect on whichever of the two hook sites above actually displays
// the next queued message first (self-clearing, one-shot) -- do NOT call
// clear_textbox_text() right after firing the syscalls, since the real
// display is usually deferred to a later frame (see the comment above
// InstallTextBoxHook). Has no effect unless both install_textbox_hook() and
// install_textbox_anim_hook() already succeeded.
extern "C" int l_set_textbox_text(void* L) {
    unsigned long long len = p_lua_rawlen(L, 1);
    const unsigned long long maxLen = sizeof(g_textBoxBuffer) - 1;
    if (len > maxLen) len = maxLen;
    for (unsigned long long i = 0; i < len; ++i) {
        p_lua_rawgeti(L, 1, (long long)(i + 1));
        g_textBoxBuffer[i] = (unsigned char)p_lua_tointegerx(L, -1, nullptr);
        p_lua_settop(L, -2);
    }
    g_textBoxBuffer[len] = 0;
    g_textBoxActive = 1;
    return 0;
}

// clear_textbox_text() -> (none)
// Fallback/safety-net only (e.g. if the window never actually opened) --
// normal use relies on the hooks' own self-clearing.
extern "C" int l_clear_textbox_text(void* L) {
    g_textBoxActive = 0;
    return 0;
}

// --- PENDING TEXT BOX TRACKING ---
// A script reload (F1 in the OpenKH Lua host) tears down and re-requires
// every Lua module -- including kh1_lua_library -- but this DLL pins itself
// in memory across that (see DllMain's LoadLibraryA self-reference below),
// so a plain static survives the reload even though any Lua-side state
// (e.g. a module-local table) does not. open_text_box records the window_id
// AND the fnc_002_close_window RVA it used here; kh1_lua_library calls
// close_pending_text_box() at module-load time (so it runs on every
// require, including post-reload) to close it if still set.
//
// The close RVA is stored (not just the window_id) specifically so this
// doesn't depend on any Lua global like fnc_002_close_window already being
// populated -- confirmed live that it isn't: on a fast F1 reload,
// kh1_lua_library's module-load code can run before the consuming script's
// require("VersionCheck") has re-populated that global this cycle, and an
// earlier version of this that looked fnc_002_close_window up fresh at
// cleanup time crashed the syscall with rva=0x0. Storing the already-known
// RVA from when the box was opened sidesteps that ordering entirely.
// -1 means "none pending".
static volatile long g_pendingTextBoxWindowId = -1;
static volatile unsigned long long g_pendingTextBoxCloseRva = 0;

// set_pending_text_box(windowId, closeRva) -> (none)
extern "C" int l_set_pending_text_box(void* L) {
    g_pendingTextBoxWindowId = (long)p_lua_tointegerx(L, 1, nullptr);
    g_pendingTextBoxCloseRva = (unsigned long long)p_lua_tointegerx(L, 2, nullptr);
    return 0;
}

// clear_pending_text_box() -> (none)
extern "C" int l_clear_pending_text_box(void* L) {
    g_pendingTextBoxWindowId = -1;
    g_pendingTextBoxCloseRva = 0;
    return 0;
}

// close_pending_text_box() -> ok(boolean)
// Closes whatever text box open_text_box last opened (if any), using the
// window_id and close-syscall RVA stored at open time -- see the comment
// above for why it doesn't just look fnc_002_close_window up fresh. Builds
// its own throwaway scriptCtx the same way call_evdl_syscall does (a single
// stack arg: window_id). Returns false (no-op) if nothing is pending.
extern "C" int l_close_pending_text_box(void* L) {
    if (g_pendingTextBoxWindowId < 0 || g_pendingTextBoxCloseRva == 0) {
        p_lua_pushboolean(L, 0);
        return 1;
    }

    memset(g_scratchScriptCtx, 0, sizeof(g_scratchScriptCtx));
    int32_t windowId = (int32_t)g_pendingTextBoxWindowId;
    memcpy(g_scratchScriptCtx + 408, &windowId, 4);
    int32_t stackIdx = 0;
    memcpy(g_scratchScriptCtx + 404, &stackIdx, 4);

    unsigned long long base = (unsigned long long)GetModuleHandleA(nullptr);
    unsigned long long address = base + g_pendingTextBoxCloseRva;
    unsigned long long args[1] = { (unsigned long long)(uintptr_t)g_scratchScriptCtx };
    unsigned long long result = 0;
    bool ok = SafeCall(address, args, 1, result);

    if (!ok) {
        char msg[128];
        snprintf(msg, sizeof(msg), "close_pending_text_box crashed: windowId=%ld rva=0x%llx",
            g_pendingTextBoxWindowId, g_pendingTextBoxCloseRva);
        LogDebug(msg);
    }

    g_pendingTextBoxWindowId = -1;
    g_pendingTextBoxCloseRva = 0;

    p_lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

// Builds and installs the hook stub. hookAddr/resumeAddr/callTargetAddr are
// absolute addresses (base + RVA), resolved by the caller from the
// version-correct SteamGlobal/EGSGlobal address tables -- this function
// itself has no version-specific knowledge at all.
//
// Original bytes at hookAddr are expected to be exactly:
//   mov rdi,rax   (3 bytes: 48 8B F8 or 48 89 C7)
//   call rel32    (5 bytes: E8 xx xx xx xx)
// i.e. fnc_draw_item_popup_entry stashing the resolved item-name pointer
// into RDI right before calling fnc_queue_item_popup_text. Verified via
// live Cheat Engine testing on both Steam and EGS builds before this was
// written -- see native/KH1Native and the project memory for the writeup.
//
// Stub layout (all in the allocated cave):
//   push rax
//   mov rax, &g_customTextActive
//   cmp byte ptr [rax],1
//   jne useOriginal
//   mov rdi, &g_customTextBuffer
//   pop rax
//   jmp continueCall
//   useOriginal:
//   pop rax
//   mov rdi,rax
//   continueCall:
//   call callTargetAddr
//   jmp resumeAddr
//
// Note this hook fires every frame the popup box is drawn (many frames per
// display, ~30-70 @60fps across its hold+fade cycle) -- it must NOT clear
// g_customTextActive itself (an earlier version did, on first fire, which
// made the custom text revert to the real item name after a single frame).
// The flag is cleared by InstallPopupCompletionHook below instead, which
// detects the popup's actual finish.
static bool InstallPopupTextHook(unsigned long long hookAddr, unsigned long long resumeAddr, unsigned long long callTargetAddr) {
    if (g_popupHookInstalled) return true;

    unsigned char* hookPtr = (unsigned char*)(uintptr_t)hookAddr;
    bool movRdiRax = (hookPtr[0] == 0x48) &&
        ((hookPtr[1] == 0x8B && hookPtr[2] == 0xF8) || (hookPtr[1] == 0x89 && hookPtr[2] == 0xC7));
    bool callRel32 = (hookPtr[3] == 0xE8);
    if (!movRdiRax || !callRel32) {
        LogDebug("InstallPopupTextHook: unexpected original bytes at hook address, aborting");
        return false;
    }

    void* cave = AllocateNear((void*)(uintptr_t)hookAddr, 4096);
    if (!cave) {
        LogDebug("InstallPopupTextHook: failed to allocate a nearby code cave");
        return false;
    }

    unsigned char stub[64] = {};
    size_t off = 0;
    uint64_t flagAddr = (uint64_t)(uintptr_t)&g_customTextActive;
    uint64_t bufAddr = (uint64_t)(uintptr_t)&g_customTextBuffer;

    stub[off++] = 0x50; // push rax

    stub[off++] = 0x48; stub[off++] = 0xB8; // mov rax, flagAddr
    memcpy(stub + off, &flagAddr, 8); off += 8;

    stub[off++] = 0x80; stub[off++] = 0x38; stub[off++] = 0x01; // cmp byte ptr [rax],1

    stub[off++] = 0x0F; stub[off++] = 0x85; // jne rel32 (patched below)
    size_t jneOperand = off; off += 4;

    stub[off++] = 0x48; stub[off++] = 0xBF; // mov rdi, bufAddr
    memcpy(stub + off, &bufAddr, 8); off += 8;

    stub[off++] = 0x58; // pop rax

    stub[off++] = 0xE9; // jmp rel32 (patched below) -> continueCall
    size_t jmpOperand = off; off += 4;

    size_t useOriginalPos = off;
    stub[off++] = 0x58; // pop rax
    stub[off++] = 0x48; stub[off++] = 0x8B; stub[off++] = 0xF8; // mov rdi,rax

    size_t continueCallPos = off;
    stub[off++] = 0xE8; // call rel32 (patched below)
    size_t callOperand = off; off += 4;

    stub[off++] = 0xE9; // jmp rel32 (patched below) -> resumeAddr
    size_t jmpBackOperand = off; off += 4;

    size_t stubLen = off;
    uintptr_t caveBase = (uintptr_t)cave;

    int32_t jneRel = (int32_t)((int64_t)useOriginalPos - (int64_t)(jneOperand + 4));
    memcpy(stub + jneOperand, &jneRel, 4);

    int32_t jmpRel = (int32_t)((int64_t)continueCallPos - (int64_t)(jmpOperand + 4));
    memcpy(stub + jmpOperand, &jmpRel, 4);

    int32_t callRel = (int32_t)((int64_t)callTargetAddr - (int64_t)(caveBase + callOperand + 4));
    memcpy(stub + callOperand, &callRel, 4);

    int32_t jmpBackRel = (int32_t)((int64_t)resumeAddr - (int64_t)(caveBase + jmpBackOperand + 4));
    memcpy(stub + jmpBackOperand, &jmpBackRel, 4);

    unsigned char patch[8];
    patch[0] = 0xE9;
    int32_t hookRel = (int32_t)((int64_t)caveBase - (int64_t)(hookAddr + 5));
    memcpy(patch + 1, &hookRel, 4);
    patch[5] = 0x90; patch[6] = 0x90; patch[7] = 0x90; // NOP-pad the leftover 3 bytes of the 8-byte window

    std::vector<HANDLE> threads = SuspendOtherThreads();

    memcpy(cave, stub, stubLen);

    DWORD oldProtect = 0;
    VirtualProtect((void*)(uintptr_t)hookAddr, 8, PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((void*)(uintptr_t)hookAddr, patch, 8);
    VirtualProtect((void*)(uintptr_t)hookAddr, 8, oldProtect, &oldProtect);

    FlushInstructionCache(GetCurrentProcess(), (void*)(uintptr_t)hookAddr, 8);
    FlushInstructionCache(GetCurrentProcess(), cave, stubLen);

    ResumeThreads(threads);

    g_popupHookInstalled = true;
    LogDebug("InstallPopupTextHook: installed successfully");
    return true;
}

// install_popup_text_hook(hookRva, resumeRva, callTargetRva) -> ok(boolean)
// Idempotent -- safe to call every time before using set_custom_popup_text.
extern "C" int l_install_popup_text_hook(void* L) {
    unsigned long long base = (unsigned long long)GetModuleHandleA(nullptr);
    unsigned long long hookRva = (unsigned long long)p_lua_tointegerx(L, 1, nullptr);
    unsigned long long resumeRva = (unsigned long long)p_lua_tointegerx(L, 2, nullptr);
    unsigned long long callTargetRva = (unsigned long long)p_lua_tointegerx(L, 3, nullptr);
    bool ok = InstallPopupTextHook(base + hookRva, base + resumeRva, base + callTargetRva);
    p_lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

// --- POPUP COMPLETION HOOK ---
// Detects when the item-popup box has actually finished displaying, so
// g_customTextActive can be cleared at the right moment instead of guessing
// a frame count or clearing too early. fnc_item_popup_tick is an
// always-ticking per-frame consumer (runs every frame regardless of whether
// a popup is visible) that drives a small lifecycle state machine at
// g_item_popup_state: 0=idle, 1=just-dequeued, 2=holding, 3=start-fade,
// 4=fading-out, 5=force-cancel. It transitions back to 0 exactly once, on
// the frame right after the last visible frame -- fnc_draw_item_popup_entry
// itself is not called at all once state hits 0, so this can't be detected
// from inside that hook; it has to be sampled from somewhere that always
// runs, which is why this is a second, independent, passive hook.
static uint32_t g_prevPopupState = 0;
static bool g_popupCompletionHookInstalled = false;

// Original bytes at fnc_item_popup_tick's entry, identical on both builds:
//   sub rsp,0x28   (48 83 EC 28)
//   xor edx,edx    (33 D2)
// verified via live read_memory on both Steam and EGS before this was
// written -- see project memory / Ghidra plate comments on fnc_item_popup_tick.
//
// Stub layout (all in the allocated cave):
//   push rax / push r10 / push r11
//   mov r10, &g_item_popup_state ; mov eax, dword ptr [r10]   (current)
//   mov r11, &g_prevPopupState   ; mov r10d, dword ptr [r11]  (previous)
//   cmp r10d,0
//   je skipClear                 ; previous already idle -> no transition
//   cmp eax,0
//   jne skipClear                ; not finished yet
//   mov r10, &g_customTextActive
//   mov byte ptr [r10],0         ; transition detected: clear one-shot flag
//   skipClear:
//   mov r10, &g_prevPopupState
//   mov dword ptr [r10],eax
//   pop r11 / pop r10 / pop rax
//   sub rsp,0x28                 ; replay original bytes
//   xor edx,edx
//   jmp resumeAddr
static bool InstallPopupCompletionHook(unsigned long long tickAddr, unsigned long long resumeAddr, unsigned long long stateAddr) {
    if (g_popupCompletionHookInstalled) return true;

    unsigned char* tickPtr = (unsigned char*)(uintptr_t)tickAddr;
    static const unsigned char expected[6] = { 0x48, 0x83, 0xEC, 0x28, 0x33, 0xD2 };
    if (memcmp(tickPtr, expected, 6) != 0) {
        LogDebug("InstallPopupCompletionHook: unexpected original bytes at tick address, aborting");
        return false;
    }

    void* cave = AllocateNear((void*)(uintptr_t)tickAddr, 4096);
    if (!cave) {
        LogDebug("InstallPopupCompletionHook: failed to allocate a nearby code cave");
        return false;
    }

    unsigned char stub[96] = {};
    size_t off = 0;
    uint64_t stateAddrImm = stateAddr;
    uint64_t prevAddrImm = (uint64_t)(uintptr_t)&g_prevPopupState;
    uint64_t flagAddrImm = (uint64_t)(uintptr_t)&g_customTextActive;

    stub[off++] = 0x50;             // push rax
    stub[off++] = 0x41; stub[off++] = 0x52; // push r10
    stub[off++] = 0x41; stub[off++] = 0x53; // push r11

    stub[off++] = 0x49; stub[off++] = 0xBA; // mov r10, stateAddr
    memcpy(stub + off, &stateAddrImm, 8); off += 8;
    stub[off++] = 0x41; stub[off++] = 0x8B; stub[off++] = 0x02; // mov eax, dword ptr [r10]

    stub[off++] = 0x49; stub[off++] = 0xBB; // mov r11, prevAddr
    memcpy(stub + off, &prevAddrImm, 8); off += 8;
    stub[off++] = 0x45; stub[off++] = 0x8B; stub[off++] = 0x13; // mov r10d, dword ptr [r11]

    stub[off++] = 0x41; stub[off++] = 0x83; stub[off++] = 0xFA; stub[off++] = 0x00; // cmp r10d,0
    stub[off++] = 0x0F; stub[off++] = 0x84; // je rel32 (patched below)
    size_t jeOperand = off; off += 4;

    stub[off++] = 0x83; stub[off++] = 0xF8; stub[off++] = 0x00; // cmp eax,0
    stub[off++] = 0x0F; stub[off++] = 0x85; // jne rel32 (patched below)
    size_t jneOperand = off; off += 4;

    stub[off++] = 0x49; stub[off++] = 0xBA; // mov r10, flagAddr
    memcpy(stub + off, &flagAddrImm, 8); off += 8;
    stub[off++] = 0x41; stub[off++] = 0xC6; stub[off++] = 0x02; stub[off++] = 0x00; // mov byte ptr [r10],0

    size_t skipClearPos = off;
    stub[off++] = 0x49; stub[off++] = 0xBA; // mov r10, prevAddr
    memcpy(stub + off, &prevAddrImm, 8); off += 8;
    stub[off++] = 0x41; stub[off++] = 0x89; stub[off++] = 0x02; // mov dword ptr [r10],eax

    stub[off++] = 0x41; stub[off++] = 0x5B; // pop r11
    stub[off++] = 0x41; stub[off++] = 0x5A; // pop r10
    stub[off++] = 0x58;                     // pop rax

    stub[off++] = 0x48; stub[off++] = 0x83; stub[off++] = 0xEC; stub[off++] = 0x28; // sub rsp,0x28
    stub[off++] = 0x33; stub[off++] = 0xD2; // xor edx,edx

    stub[off++] = 0xE9; // jmp rel32 (patched below) -> resumeAddr
    size_t jmpBackOperand = off; off += 4;

    size_t stubLen = off;
    uintptr_t caveBase = (uintptr_t)cave;

    int32_t jeRel = (int32_t)((int64_t)skipClearPos - (int64_t)(jeOperand + 4));
    memcpy(stub + jeOperand, &jeRel, 4);

    int32_t jneRel = (int32_t)((int64_t)skipClearPos - (int64_t)(jneOperand + 4));
    memcpy(stub + jneOperand, &jneRel, 4);

    int32_t jmpBackRel = (int32_t)((int64_t)resumeAddr - (int64_t)(caveBase + jmpBackOperand + 4));
    memcpy(stub + jmpBackOperand, &jmpBackRel, 4);

    unsigned char patch[6];
    patch[0] = 0xE9;
    int32_t hookRel = (int32_t)((int64_t)caveBase - (int64_t)(tickAddr + 5));
    memcpy(patch + 1, &hookRel, 4);
    patch[5] = 0x90; // NOP-pad the 1 leftover byte of the 6-byte window

    std::vector<HANDLE> threads = SuspendOtherThreads();

    memcpy(cave, stub, stubLen);

    DWORD oldProtect = 0;
    VirtualProtect((void*)(uintptr_t)tickAddr, 6, PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((void*)(uintptr_t)tickAddr, patch, 6);
    VirtualProtect((void*)(uintptr_t)tickAddr, 6, oldProtect, &oldProtect);

    FlushInstructionCache(GetCurrentProcess(), (void*)(uintptr_t)tickAddr, 6);
    FlushInstructionCache(GetCurrentProcess(), cave, stubLen);

    ResumeThreads(threads);

    g_popupCompletionHookInstalled = true;
    LogDebug("InstallPopupCompletionHook: installed successfully");
    return true;
}

// install_popup_completion_hook(tickRva, resumeRva, stateRva) -> ok(boolean)
// Idempotent -- safe to call every time before using set_custom_popup_text.
extern "C" int l_install_popup_completion_hook(void* L) {
    unsigned long long base = (unsigned long long)GetModuleHandleA(nullptr);
    unsigned long long tickRva = (unsigned long long)p_lua_tointegerx(L, 1, nullptr);
    unsigned long long resumeRva = (unsigned long long)p_lua_tointegerx(L, 2, nullptr);
    unsigned long long stateRva = (unsigned long long)p_lua_tointegerx(L, 3, nullptr);
    bool ok = InstallPopupCompletionHook(base + tickRva, base + resumeRva, base + stateRva);
    p_lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

// set_custom_popup_text(byteTable) -> (none)
// byteTable is a Lua array of KHSCII byte values (e.g. from GetKHSCII()).
// Takes effect on the next fnc_enqueue_item_popup-driven popup, real or
// triggered via show_item_popup -- until clear_custom_popup_text() is
// called. Has no effect unless install_popup_text_hook() already succeeded.
extern "C" int l_set_custom_popup_text(void* L) {
    unsigned long long len = p_lua_rawlen(L, 1);
    const unsigned long long maxLen = sizeof(g_customTextBuffer) - 1;
    if (len > maxLen) len = maxLen;
    for (unsigned long long i = 0; i < len; ++i) {
        p_lua_rawgeti(L, 1, (long long)(i + 1));
        g_customTextBuffer[i] = (unsigned char)p_lua_tointegerx(L, -1, nullptr);
        p_lua_settop(L, -2);
    }
    g_customTextBuffer[len] = 0;
    g_customTextActive = 1;
    return 0;
}

// clear_custom_popup_text() -> (none)
// Reverts fnc_draw_item_popup_entry to showing real item names again.
extern "C" int l_clear_custom_popup_text(void* L) {
    g_customTextActive = 0;
    return 0;
}

static const luaL_Reg kh1_native_lib[] = {
    {"call_function", reinterpret_cast<void*>(l_call_function)},
    {"get_module_base", reinterpret_cast<void*>(l_get_module_base)},
    {"write_floats", reinterpret_cast<void*>(l_write_floats)},
    {"spawn_enemy", reinterpret_cast<void*>(l_spawn_enemy)},
    {"install_popup_text_hook", reinterpret_cast<void*>(l_install_popup_text_hook)},
    {"install_popup_completion_hook", reinterpret_cast<void*>(l_install_popup_completion_hook)},
    {"set_custom_popup_text", reinterpret_cast<void*>(l_set_custom_popup_text)},
    {"clear_custom_popup_text", reinterpret_cast<void*>(l_clear_custom_popup_text)},
    {"call_evdl_syscall", reinterpret_cast<void*>(l_call_evdl_syscall)},
    {"install_textbox_hook", reinterpret_cast<void*>(l_install_textbox_hook)},
    {"install_textbox_anim_hook", reinterpret_cast<void*>(l_install_textbox_anim_hook)},
    {"set_textbox_text", reinterpret_cast<void*>(l_set_textbox_text)},
    {"clear_textbox_text", reinterpret_cast<void*>(l_clear_textbox_text)},
    {"set_pending_text_box", reinterpret_cast<void*>(l_set_pending_text_box)},
    {"clear_pending_text_box", reinterpret_cast<void*>(l_clear_pending_text_box)},
    {"close_pending_text_box", reinterpret_cast<void*>(l_close_pending_text_box)},
    {nullptr, nullptr}
};

// Every Lua C API export this module needs to bridge into the host's Lua
// state. A candidate module only counts if ALL of these resolve from it --
// see ModuleExportsAllRequired().
static const char* const kRequiredLuaExports[] = {
    "lua_gettop", "lua_tointegerx", "lua_tonumberx", "lua_pushinteger",
    "lua_pushboolean", "lua_pushstring", "luaL_setfuncs", "lua_createtable",
    "lua_rawlen", "lua_rawgeti", "lua_settop",
};

static bool ModuleExportsAllRequired(HMODULE mod) {
    if (!mod) return false;
    for (const char* name : kRequiredLuaExports) {
        if (!GetProcAddress(mod, name)) return false;
    }
    return true;
}

// Last-resort fallback in case the bundled lua54.dll (see FindLuaModule)
// somehow isn't loaded: scan every module in the process, requiring ALL
// required symbols to resolve from the SAME module before accepting it (a
// module that only partially matches can't win just by enumerating first).
// This can still land on lua-apclientpp.dll's embedded Lua (the Archipelago
// Lua binding, shipped by KH1-RANDOMIZER, which statically embeds its own
// separate copy of Lua 5.4 and -- as a side effect of its MinGW build not
// hiding symbol visibility -- exports the exact same names) if that's the
// only thing loaded exporting them. That's an acceptable last resort, not
// the intended path.
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

// The current LuaBackend build statically embeds Lua 5.4 as private,
// unexported code -- confirmed live (its export table has exactly 2 entries,
// neither of them any Lua function) -- so there is no external door into it
// by any technique, static or dynamic. Rather than depend on whatever LuaBackend
// build a given player happens to have, this mod bundles its own known-good
// lua54.dll directly (see dll/lua54.dll, loaded automatically by Panacea
// before any script runs -- see mod.yml), and looks for that specific,
// guaranteed-present module by name.
static HMODULE FindLuaModule() {
    HMODULE bundled = GetModuleHandleA("lua54.dll");
    if (ModuleExportsAllRequired(bundled)) {
        LogDebug("FindLuaModule: resolved via bundled dll/lua54.dll");
        return bundled;
    }

    LogDebug("FindLuaModule: bundled lua54.dll not found or incomplete, falling back to process scan");
    return FindLuaModuleByProcessScan();
}

extern "C" __declspec(dllexport) int luaopen_kh1_native(void* L) {
    LogDebug("luaopen_kh1_native called");

    HMODULE hLua = FindLuaModule();
    if (hLua && !p_lua_gettop) {
        p_lua_gettop      = (t_lua_gettop)      GetProcAddress(hLua, "lua_gettop");
        p_lua_tointegerx  = (t_lua_tointegerx)  GetProcAddress(hLua, "lua_tointegerx");
        p_lua_tonumberx   = (t_lua_tonumberx)   GetProcAddress(hLua, "lua_tonumberx");
        p_lua_pushinteger = (t_lua_pushinteger) GetProcAddress(hLua, "lua_pushinteger");
        p_lua_pushboolean = (t_lua_pushboolean) GetProcAddress(hLua, "lua_pushboolean");
        p_lua_pushstring  = (t_lua_pushstring)  GetProcAddress(hLua, "lua_pushstring");
        p_luaL_setfuncs   = (t_luaL_setfuncs)   GetProcAddress(hLua, "luaL_setfuncs");
        p_lua_createtable = (t_lua_createtable) GetProcAddress(hLua, "lua_createtable");
        p_lua_rawlen      = (t_lua_rawlen)      GetProcAddress(hLua, "lua_rawlen");
        p_lua_rawgeti     = (t_lua_rawgeti)     GetProcAddress(hLua, "lua_rawgeti");
        p_lua_settop      = (t_lua_settop)      GetProcAddress(hLua, "lua_settop");
    }

    if (!p_lua_gettop || !p_lua_tointegerx || !p_lua_tonumberx || !p_lua_pushinteger || !p_lua_pushboolean ||
        !p_lua_pushstring || !p_luaL_setfuncs || !p_lua_createtable ||
        !p_lua_rawlen || !p_lua_rawgeti || !p_lua_settop) {
        // Couldn't find a loaded module exporting the Lua C API -- bail out
        // without touching any of them. Returning 0 (no pushed values) makes
        // require() hand back `true` rather than crashing on a null function
        // pointer call.
        LogDebug("luaopen_kh1_native: failed to resolve Lua API exports, aborting safely");
        return 0;
    }

    p_lua_createtable(L, 0, 2);
    p_luaL_setfuncs(L, kh1_native_lib, 0);
    return 1;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        GetModuleFileNameA(hModule, g_dllDir, MAX_PATH);
        char* last = strrchr(g_dllDir, '\\');
        if (last) *(last + 1) = '\0';

        // Pin ourselves in memory with an extra reference we never release.
        // LuaBackend's script-refresh feature appears to FreeLibrary() native
        // modules it required as part of giving scripts a clean reload -- the
        // popup hooks above install trampolines (VirtualAlloc'd code caves,
        // outside this DLL) that read/write this DLL's own static globals
        // (g_customTextActive, g_customTextBuffer, g_prevPopupState) every
        // time the game calls into the hooked functions. If this DLL got
        // unloaded while a hook is still installed, that memory becomes
        // unmapped out from under a trampoline the game keeps calling into --
        // an instant crash. Holding an extra reference means an external
        // FreeLibrary() call just decrements our refcount instead of actually
        // unloading us, so installed hooks stay valid for the rest of the
        // game session (matches KH1Overlay's dllmain.cpp, which documents the
        // same FreeLibrary risk for its own persistent thread).
        char selfPath[MAX_PATH];
        GetModuleFileNameA(hModule, selfPath, MAX_PATH);
        LoadLibraryA(selfPath);
    }
    return TRUE;
}
