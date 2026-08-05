#include "pch.h"
#include <tlhelp32.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>

// --- DEBUG LOGGING ---

// Buffer to hold the directory of the KH1Native DLL.
static char g_dllDir[MAX_PATH] = "";

// Calculates the log file path, opens it, and writes to it.
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

// Predefines arguments and return types of lua module
// functions before their addresses are found/registered
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

// Define each definition's function pointer as null for now
static t_lua_gettop       p_lua_gettop       = nullptr;
static t_lua_tointegerx   p_lua_tointegerx   = nullptr;
static t_lua_tonumberx    p_lua_tonumberx    = nullptr;
static t_lua_tolstring    p_lua_tolstring    = nullptr;
static t_lua_pushinteger  p_lua_pushinteger  = nullptr;
static t_lua_pushboolean  p_lua_pushboolean  = nullptr;
static t_lua_pushstring   p_lua_pushstring   = nullptr;
static t_luaL_setfuncs    p_luaL_setfuncs    = nullptr;
static t_lua_createtable  p_lua_createtable  = nullptr;
static t_lua_rawlen       p_lua_rawlen       = nullptr;
static t_lua_rawgeti      p_lua_rawgeti      = nullptr;
static t_lua_settop       p_lua_settop       = nullptr;

// Structure to hold functions we want to expose
// to lua to use.
struct luaL_Reg { const char* name; void* func; };

// --- CALL BRIDGE ---

// Need to define the functions as call bridges
// to handle different argument counts that can come
// from lua.  6 args is arbitrary, can be increased if
// needed.
typedef unsigned long long(__fastcall* Func0)();
typedef unsigned long long(__fastcall* Func1)(unsigned long long);
typedef unsigned long long(__fastcall* Func2)(unsigned long long, unsigned long long);
typedef unsigned long long(__fastcall* Func3)(unsigned long long, unsigned long long, unsigned long long);
typedef unsigned long long(__fastcall* Func4)(unsigned long long, unsigned long long, unsigned long long, unsigned long long);
typedef unsigned long long(__fastcall* Func5)(unsigned long long, unsigned long long, unsigned long long, unsigned long long, unsigned long long);
typedef unsigned long long(__fastcall* Func6)(unsigned long long, unsigned long long, unsigned long long, unsigned long long, unsigned long long, unsigned long long);

static const int MAX_CALL_ARGS = 6;

// Handles safely calling the exe function,
// wrapped in try/except for hardware fault
// capturing.
//
// On a caught exception, logs the real fault address/code (diagnostic only
// -- doesn't change the false-on-exception contract). Root-causing a crash
// inside a large, undocumented function (e.g. fnc_spawn_world_gimmick_
// entity's ~150-field constructor) is far more tractable with the actual
// faulting RVA than with just "it threw somewhere" -- added 2026-08-04
// while investigating a room-specific spawn_enemy crash (see KH1-LUA-
// LIBRARY memory: project_spawn_enemy_cold_spawn_crash_containment.md).
static bool SafeCall(unsigned long long address, const unsigned long long* args, int argCount, unsigned long long& outResult) {
    DWORD exceptionCode = 0;
    ULONG_PTR exceptionAddr = 0;
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
    } __except (
        exceptionCode = GetExceptionCode(),
        exceptionAddr = (ULONG_PTR)((EXCEPTION_POINTERS*)GetExceptionInformation())->ExceptionRecord->ExceptionAddress,
        EXCEPTION_EXECUTE_HANDLER) {
        unsigned long long base = (unsigned long long)GetModuleHandleA(nullptr);
        char msg[192];
        snprintf(msg, sizeof(msg),
            "SafeCall: exception 0x%08X at KH1FM.exe RVA=0x%llX (calling target RVA=0x%llX)",
            (unsigned)exceptionCode, (unsigned long long)exceptionAddr - base, address - base);
        LogDebug(msg);
        return false;
    }
}

// --- LUA-CALLABLE FUNCTIONS ---

// First function exposed to lua.
// This simply takes an RVA and a list of
// arguments (if applicable) and calls the
// function at base + RVA with those arguments.
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

// Helper function to expose the base
// to lua if needed.
extern "C" int l_get_module_base(void* L) {
    p_lua_pushinteger(L, (long long)(unsigned long long)GetModuleHandleA(nullptr));
    return 1;
}

// Certain functions need to pass floats
// (usually position data).  This function
// converts the passed floats into a type
// the game expects.
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
// spawn_enemy(spawnFnRva, tablePtrRva, tableCountRva, loadAssetsFnRva,
//   loadedPtrTableRva, mintHandleFnRva, resolveHandleFnRva, modelPath,
//   motionPath, x, y, z) -> ok(boolean), entityPtr(integer) | errorMessage(string)
//
// Spawns a creature at a world position by splicing a synthetic record into
// the room's live placement table, then calling fnc_spawn_world_gimmick_entity
// with its id -- the same constructor the room's own loader uses. Identifies
// the creature by its model/motion filename pair rather than species number
// (which is per-room-local); reuses an already-loaded slot if one exists,
// otherwise claims a free slot and triggers an asset load. Runs in-process
// via SafeCall -- Cheat Engine's execute_code crashes this call chain.
// Leaks the old table buffer on every call (can't be safely freed).
static const size_t PLACEMENT_RECORD_SIZE = 0x78;
static const int PLACEMENT_SPECIES_OFFSET = 0x55;
static const int PLACEMENT_POS_X_OFFSET = 0x1C;
static const int PLACEMENT_POS_Y_OFFSET = 0x20;
static const int PLACEMENT_POS_Z_OFFSET = 0x24;
static const int PLACEMENT_MODEL_HANDLE_OFFSET = 0x60;
static const int PLACEMENT_MOTION_HANDLE_OFFSET = 0x64;

// Shared 96-slot global entity pool (stride 0x4B0/1200 bytes) -- the SAME
// pool Sora, party members, doors/chests/markers, and every
// fnc_spawn_world_gimmick_entity-constructed creature live in (confirmed
// live 2026-08-04: Sora's own struct pointer, from soraPointer, landed
// exactly on a pool slot boundary at index 4). Position is 3 floats at
// +0x10/+0x14/+0x18 -- also confirmed live against Sora's own known
// position and cross-checked against a CC-spawned Large Body's slot.
// Occupied-flag at +0x374 bit 0 was already known from an earlier session
// (see fnc_spawn_world_gimmick_entity's own plate comment).
static const unsigned long long ENTITY_POOL_STRIDE = 0x4B0;
static const int ENTITY_POOL_COUNT = 96;
static const int ENTITY_OCCUPIED_FLAG_OFFSET = 0x374;
static const int ENTITY_POS_X_OFFSET = 0x10;
static const int ENTITY_POS_Y_OFFSET = 0x14;
static const int ENTITY_POS_Z_OFFSET = 0x18;
static const int ENTITY_KIND_OFFSET = 0x6;
// Sora, party members, AND every fnc_spawn_world_gimmick_entity-constructed
// creature (including a CC-spawned Large Body) all carry kind==3 -- doors/
// chests/trigger markers/other room furniture use other kind values
// (observed 2,4,5,9,10). Confirmed live 2026-08-04.
static const uint8_t ENTITY_KIND_ACTOR = 3;
static const float MIN_SPAWN_DISTANCE_FROM_SORA = 100.0f; // live-tuned 2026-08-04 (was 500, then 100)

// A fresh spawn lands at (or very near) Sora's own position -- if another
// ENEMY is already sitting close to Sora when that happens, the new
// creature can end up overlapping it directly. Live-confirmed 2026-08-04 as
// a likely trigger for the spawn_enemy crash investigation (see
// project_spawn_enemy_cold_spawn_crash_containment.md) -- the crash risk is
// specifically enemy-vs-enemy overlap, NOT enemy-vs-Sora (spawning
// literally on top of Sora is fine -- the engine pushes him out of the
// way, confirmed by repeated live testing all session).
//
// Refined 2026-08-04 from an earlier version that counted every occupied
// pool slot -- that refused almost every spawn attempt, since doors/
// chests/markers near Sora aren't a crash risk at all. Now: only counts
// kind==3 entities (see ENTITY_KIND_ACTOR), and explicitly excludes Sora
// and the active party (soraObjPtrRva/partyMember1PtrRva/
// partyMember2PtrRva) since they're also kind==3 and would otherwise
// always be "too close" just by following Sora around. All RVA params are
// optional, same convention as the other guard RVAs -- 0/unset just skips
// that exclusion (or the whole check, for entityPoolBaseRva/soraPointerRva).
static bool AnyEnemyTooCloseToSora(unsigned long long base, unsigned long long entityPoolBaseRva,
                                     unsigned long long soraPointerRva, unsigned long long soraObjPtrRva,
                                     unsigned long long partyMember1PtrRva, unsigned long long partyMember2PtrRva,
                                     float minDistance) {
    if (entityPoolBaseRva == 0 || soraPointerRva == 0) return false;
    unsigned long long soraEntityPtr = *(unsigned long long*)(uintptr_t)(base + soraPointerRva);
    if (soraEntityPtr == 0) return false;
    float soraX = *(float*)(uintptr_t)(soraEntityPtr + ENTITY_POS_X_OFFSET);
    float soraY = *(float*)(uintptr_t)(soraEntityPtr + ENTITY_POS_Y_OFFSET);
    float soraZ = *(float*)(uintptr_t)(soraEntityPtr + ENTITY_POS_Z_OFFSET);

    unsigned long long excluded[3] = { soraEntityPtr, 0, 0 };
    if (soraObjPtrRva != 0) excluded[0] = *(unsigned long long*)(uintptr_t)(base + soraObjPtrRva);
    if (partyMember1PtrRva != 0) excluded[1] = *(unsigned long long*)(uintptr_t)(base + partyMember1PtrRva);
    if (partyMember2PtrRva != 0) excluded[2] = *(unsigned long long*)(uintptr_t)(base + partyMember2PtrRva);

    unsigned long long poolBase = base + entityPoolBaseRva;
    float minDistSq = minDistance * minDistance;
    for (int i = 0; i < ENTITY_POOL_COUNT; ++i) {
        unsigned long long slot = poolBase + (unsigned long long)i * ENTITY_POOL_STRIDE;
        if (slot == soraEntityPtr || slot == excluded[0] || slot == excluded[1] || slot == excluded[2]) continue;
        uint32_t flags = *(volatile uint32_t*)(uintptr_t)(slot + ENTITY_OCCUPIED_FLAG_OFFSET);
        if ((flags & 1) == 0) continue;
        uint8_t kind = *(volatile uint8_t*)(uintptr_t)(slot + ENTITY_KIND_OFFSET);
        if (kind != ENTITY_KIND_ACTOR) continue;
        float x = *(float*)(uintptr_t)(slot + ENTITY_POS_X_OFFSET);
        float y = *(float*)(uintptr_t)(slot + ENTITY_POS_Y_OFFSET);
        float z = *(float*)(uintptr_t)(slot + ENTITY_POS_Z_OFFSET);
        float dx = x - soraX, dy = y - soraY, dz = z - soraZ;
        if ((dx * dx + dy * dy + dz * dz) < minDistSq) {
            return true;
        }
    }
    return false;
}

// Per-species asset-load state struct loadedSpeciesPtrTable points into.
// State byte (+3) is 0 until a load starts, then climbs as it progresses.
// Cached filename string (+4) tells reuse from collision for that slot.
static const int LOADED_SPECIES_STRIDE = 0x50;
static const int LOADED_SPECIES_STATE_OFFSET_FROM_PTR = -0x45;
static const int LOADED_SPECIES_MODEL_NAME_OFFSET_FROM_PTR = -0x44;
static const int LOADED_SPECIES_MODEL_NAME_SIZE = 0x20;
static const int SPECIES_SLOT_COUNT = 256; // species/slot index is a uint8_t (record+0x55) -- the full addressable range

// species (record+0x55) is a per-room-local slot index, not a stable
// creature ID -- creatures are identified by model/motion filename instead.
// Char-id/weight/template data comes from kh1_creature_data.lua, looked up
// Lua-side and passed in as luaCharId/luaWeight/luaTemplateRecord.
//
// Per-species resource-blob table (0x40000/256KB stride per slot).
// RESOURCE_BLOB_MAX_SPECIES matches the game's own bounds check (65 valid
// entries, species 0..64).
static const size_t RESOURCE_BLOB_SIZE = 0x40000;
static const int RESOURCE_BLOB_MAX_SPECIES = 0x40;

// Debugging aid: entity id of the most recent spawn_enemy call to reach the
// constructor, for an external CE hook to target. Not read in this DLL.
static volatile uint32_t g_lastSpawnAttemptId = 0;

// Placement-record pointer tagged right before it's handed to
// fnc_load_gimmick_assets, so InstallJobRecordGuardHook can tell our own
// queued job apart from unrelated callers.
static volatile uint64_t g_lastQueuedGimmickRecordPtr = 0;

// Room identity, snapshotted with the record pointer above -- the guard's
// real staleness signal is "did the room change", not the placement table
// pointer, which gets mutated for unrelated reasons.
static volatile int32_t g_lastQueuedGimmickWorld = 0;
static volatile int32_t g_lastQueuedGimmickArea = 0;
static volatile int32_t g_lastQueuedGimmickSet = 0;
static volatile bool g_lastQueuedGimmickRoomIdentityValid = false;

// Tracks model_paths already triggered for a load this session, and which
// slot -- catches a retry frame re-entering while a load is still in
// flight (the engine's own state byte can't distinguish "loading" from
// "never started"). Checked before FindLoadedSlotByFilename.
struct TriggeredLoadEntry {
    char modelPath[64];
    uint8_t species;
};
static const int MAX_TRIGGERED_LOADS = 64;
static TriggeredLoadEntry g_triggeredLoads[MAX_TRIGGERED_LOADS];
static int g_triggeredLoadCount = 0;

static bool FindTriggeredLoad(const char* modelPath, uint8_t* outSpecies) {
    for (int i = 0; i < g_triggeredLoadCount; ++i) {
        if (strcmp(g_triggeredLoads[i].modelPath, modelPath) == 0) {
            *outSpecies = g_triggeredLoads[i].species;
            return true;
        }
    }
    return false;
}

// Entries stay valid for the rest of the session, even once the load
// completes.
static void RecordTriggeredLoad(const char* modelPath, uint8_t species) {
    if (g_triggeredLoadCount >= MAX_TRIGGERED_LOADS) return;
    TriggeredLoadEntry& entry = g_triggeredLoads[g_triggeredLoadCount];
    strncpy_s(entry.modelPath, modelPath, _TRUNCATE);
    entry.species = species;
    g_triggeredLoadCount++;
}

// A species recorded here was already assigned to a specific creature by a
// load WE triggered this session -- never hand it to a DIFFERENT creature,
// even if the live engine's own state byte looks free or
// MarkSpeciesInUseByLiveEntities couldn't resolve the occupying entity's
// handle. That resolve failing is exactly the same failure mode as the
// crashes this DLL guards against elsewhere (unminted resource handle), so
// a live entity we ourselves spawned can go undetected by that live-scan
// and silently free up its own slot for reuse -- confirmed 2026-08-04 as
// the likely cause of "spawn while another CC-spawned creature is already
// on screen" crashes. This is our own bookkeeping, so it doesn't depend on
// the handle resolving correctly at all.
static bool IsSpeciesTriggeredThisSession(uint8_t species) {
    for (int i = 0; i < g_triggeredLoadCount; ++i) {
        if (g_triggeredLoads[i].species == species) return true;
    }
    return false;
}

// model_path/motion_path from Lua are only valid for this call, but the
// async load pipeline dereferences them later -- copies into DLL-owned
// storage so minted handles don't dangle.
struct InternedPathEntry {
    char path[64];
};
static const int MAX_INTERNED_PATHS = 128;
static InternedPathEntry g_internedPaths[MAX_INTERNED_PATHS];
static int g_internedPathCount = 0;

static const char* InternPath(const char* s) {
    for (int i = 0; i < g_internedPathCount; ++i) {
        if (strcmp(g_internedPaths[i].path, s) == 0) return g_internedPaths[i].path;
    }
    if (g_internedPathCount >= MAX_INTERNED_PATHS) return s; // pool exhausted -- fall back to the raw (unsafe) pointer rather than crash here
    InternedPathEntry& entry = g_internedPaths[g_internedPathCount];
    strncpy_s(entry.path, s, _TRUNCATE);
    g_internedPathCount++;
    return entry.path;
}

// Fallback path: scan every slot's asset-load state for one already
// holding this exact creature (state != 0, cached filename matches --
// reuse it) or, failing that, the first untouched slot (state == 0 --
// safe to claim).
static bool FindLoadedSlotByFilename(unsigned long long base, unsigned long long loadedPtrTableRva, const char* modelPath, uint8_t* outSpecies) {
    for (int s = 0; s < SPECIES_SLOT_COUNT; ++s) {
        volatile uint8_t* stateAddr = (volatile uint8_t*)(uintptr_t)(base + loadedPtrTableRva + LOADED_SPECIES_STATE_OFFSET_FROM_PTR + (size_t)s * LOADED_SPECIES_STRIDE);
        if (*stateAddr == 0) continue;
        const char* cachedName = (const char*)(uintptr_t)(base + loadedPtrTableRva + LOADED_SPECIES_MODEL_NAME_OFFSET_FROM_PTR + (size_t)s * LOADED_SPECIES_STRIDE);
        if (strncmp(cachedName, modelPath, LOADED_SPECIES_MODEL_NAME_SIZE) == 0) {
            *outSpecies = (uint8_t)s;
            return true;
        }
    }
    return false;
}

// The slot-scan helpers don't know if the current room already uses a slot
// number for something else -- resolves record+0x60's handle to a real
// string pointer for comparison.
static bool ResolvedModelMatches(unsigned long long resolveFnAddr, uint32_t modelHandle, const char* wantModel) {
    if (modelHandle == 0) return false;
    unsigned long long args[1] = { (unsigned long long)modelHandle };
    unsigned long long resolved = 0;
    if (!SafeCall(resolveFnAddr, args, 1, resolved) || resolved == 0) return false;
    return strncmp((const char*)(uintptr_t)resolved, wantModel, LOADED_SPECIES_MODEL_NAME_SIZE) == 0;
}

// A room's own native record of the SAME creature being spawned is fine --
// only a same-species record belonging to a genuinely different creature
// (resolved model doesn't match) counts as a real collision.
static bool RoomHasNativeSpecies(const uint8_t* table, int32_t count, uint8_t species, unsigned long long resolveFnAddr, const char* modelPath) {
    for (int32_t i = 0; i < count; ++i) {
        const uint8_t* rec = table + (size_t)i * PLACEMENT_RECORD_SIZE;
        if (rec[PLACEMENT_SPECIES_OFFSET] != species) continue;
        uint32_t modelHandle;
        memcpy(&modelHandle, rec + PLACEMENT_MODEL_HANDLE_OFFSET, 4);
        if (!ResolvedModelMatches(resolveFnAddr, modelHandle, modelPath)) {
            return true;
        }
    }
    return false;
}

// Walks the live entity pool (pass 0 to start, pass the previous result to
// advance, 0 means done) and marks each entity's resource-handle as
// "species in use" -- catches an unrelated live entity using a slot with
// no placement-table record.
static void MarkSpeciesInUseByLiveEntities(unsigned long long base, unsigned long long entityIterFnRva,
                                             unsigned long long resolveHandleFnRva, unsigned long long speciesResourceTableRva,
                                             bool outInUse[RESOURCE_BLOB_MAX_SPECIES + 1]) {
    if (entityIterFnRva == 0 || speciesResourceTableRva == 0) return;
    unsigned long long tableBase = base + speciesResourceTableRva;
    unsigned long long entity = 0;
    unsigned long long iterArgs[1] = { 0 };
    if (!SafeCall(base + entityIterFnRva, iterArgs, 1, entity)) return;
    int guard = 0; // hard cap matching the pool's own real size -- never trust an external loop to self-terminate
    while (entity != 0 && guard < 128) {
        ++guard;
        uint32_t handle = *(volatile uint32_t*)(uintptr_t)(entity + 0x134);
        if (handle != 0) {
            unsigned long long resolveArgs[1] = { (unsigned long long)handle };
            unsigned long long resolved = 0;
            if (SafeCall(base + resolveHandleFnRva, resolveArgs, 1, resolved) && resolved != 0 &&
                resolved >= tableBase && resolved < tableBase + (unsigned long long)(RESOURCE_BLOB_MAX_SPECIES + 1) * RESOURCE_BLOB_SIZE) {
                int species = (int)((resolved - tableBase) >> 18);
                if (species >= 0 && species <= RESOURCE_BLOB_MAX_SPECIES) outInUse[species] = true;
            }
        }
        iterArgs[0] = entity;
        if (!SafeCall(base + entityIterFnRva, iterArgs, 1, entity)) return;
    }
}

// Bounded by RESOURCE_BLOB_MAX_SPECIES (64), NOT SPECIES_SLOT_COUNT (256) --
// fnc_load_gimmick_assets does no bounds check before minting a handle into
// the resource-blob table, unlike the constructor's own self-heal path, so
// handing out a species above 64 here would mint a handle pointing outside
// the real table -- unguarded heap corruption once the async load callback
// writes through it. SPECIES_SLOT_COUNT remains correct for
// FindLoadedSlotByFilename above, which only reads existing state.
//
// Scans starting from 20 rather than 0: low slot numbers collide with
// unrelated live systems that share the same resource-blob range but
// aren't tracked in any room's placement table (an unrelated entity's own
// cached type-def handle, or even an unrelated per-party ability-data
// struct) -- there is no fixed "safe" range, so
// MarkSpeciesInUseByLiveEntities above is also consulted, and slots ~48-64
// are avoided since they're hardcoded-reserved by unrelated engine
// subsystems (menu/JP-sysfont, voice, event motion/effect).
//
// IsSpeciesTriggeredThisSession is also consulted (2026-08-04) --
// MarkSpeciesInUseByLiveEntities can't be trusted alone, since it depends
// on resolving each live entity's own resource handle, which is exactly
// the kind of call that fails for the creatures this whole file is trying
// to guard. Our own triggered-load bookkeeping doesn't have that problem.
static bool FindFreeLoadedSlot(unsigned long long base, unsigned long long loadedPtrTableRva, const bool* speciesInUseByLiveEntity, uint8_t* outSpecies) {
    for (int s = 20; s <= RESOURCE_BLOB_MAX_SPECIES; ++s) {
        if (speciesInUseByLiveEntity && speciesInUseByLiveEntity[s]) continue;
        if (IsSpeciesTriggeredThisSession((uint8_t)s)) continue;
        volatile uint8_t* stateAddr = (volatile uint8_t*)(uintptr_t)(base + loadedPtrTableRva + LOADED_SPECIES_STATE_OFFSET_FROM_PTR + (size_t)s * LOADED_SPECIES_STRIDE);
        if (*stateAddr == 0) {
            *outSpecies = (uint8_t)s;
            return true;
        }
    }
    return false;
}

// Forward declarations -- defined later in this file, installed from here.
static bool InstallJobRecordGuardHook(unsigned long long hookAddr, unsigned long long resumeAddr,
                                        unsigned long long tablePtrRva, unsigned long long tableCountRva,
                                        unsigned long long worldNumRva, unsigned long long areaNumRva,
                                        unsigned long long setNumRva);
static bool InstallVelocityBlendGuardHook(unsigned long long hookAddr, unsigned long long resumeAddr);
static bool InstallPartyAbilityIndexGuardHook(unsigned long long hookAddr, unsigned long long resumeAddr,
                                                unsigned long long resolveHandleFnAddr);
static bool InstallSkeletonHandleGuardHook(unsigned long long hookAddr, unsigned long long resumeAddr,
                                             unsigned long long resolveHandleFnAddr);
static bool InstallSkeletonBlendCallGuardHook(unsigned long long hookAddr);
static bool InstallKeyframeListEntryGuardHook(unsigned long long hookAddr, unsigned long long resumeAddr,
                                                unsigned long long notFoundAddr,
                                                unsigned long long resolveHandleFnAddr);

extern "C" int l_spawn_enemy(void* L) {
    unsigned long long spawnFnRva = (unsigned long long)p_lua_tointegerx(L, 1, nullptr);
    unsigned long long tablePtrRva = (unsigned long long)p_lua_tointegerx(L, 2, nullptr);
    unsigned long long tableCountRva = (unsigned long long)p_lua_tointegerx(L, 3, nullptr);
    unsigned long long loadAssetsFnRva = (unsigned long long)p_lua_tointegerx(L, 4, nullptr);
    unsigned long long loadedPtrTableRva = (unsigned long long)p_lua_tointegerx(L, 5, nullptr);
    unsigned long long mintHandleFnRva = (unsigned long long)p_lua_tointegerx(L, 6, nullptr);
    unsigned long long resolveHandleFnRva = (unsigned long long)p_lua_tointegerx(L, 7, nullptr);
    unsigned long long speciesResourceTableRva = (unsigned long long)p_lua_tointegerx(L, 8, nullptr);
    const char* modelPath = p_lua_tolstring(L, 9, nullptr);
    const char* motionPath = p_lua_tolstring(L, 10, nullptr);
    float x = (float)p_lua_tonumberx(L, 11, nullptr);
    float y = (float)p_lua_tonumberx(L, 12, nullptr);
    float z = (float)p_lua_tonumberx(L, 13, nullptr);
    // Room identity for InstallJobRecordGuardHook. Optional -- 0/unset
    // falls back to the guard hook's table-bounds heuristic.
    unsigned long long worldNumRva = (unsigned long long)p_lua_tointegerx(L, 14, nullptr);
    unsigned long long areaNumRva = (unsigned long long)p_lua_tointegerx(L, 15, nullptr);
    unsigned long long setNumRva = (unsigned long long)p_lua_tointegerx(L, 16, nullptr);
    // Entry points for the guard hooks installed below. Both fix real
    // crashes on this path; a fallback spawn refuses cleanly if either RVA
    // is 0 or either install fails.
    unsigned long long jobCallbackFnRva = (unsigned long long)p_lua_tointegerx(L, 17, nullptr);
    unsigned long long velocityBlendFnRva = (unsigned long long)p_lua_tointegerx(L, 18, nullptr);
    // Entry point for the 96-slot live-entity pool iterator. Optional --
    // FindFreeLoadedSlot degrades to its start-at-20 heuristic without it.
    unsigned long long entityIterFnRva = (unsigned long long)p_lua_tointegerx(L, 19, nullptr);
    // Entry point of the CALL-to-fnc_resolve_resource_handle instruction
    // inside the shared party-ability-index helper. Optional -- refuses
    // cleanly, same as the other two guard hooks, if unconfigured.
    unsigned long long partyAbilityIndexFnRva = (unsigned long long)p_lua_tointegerx(L, 20, nullptr);
    // Fallback char-id/weight/template data, looked up Lua-side
    // (kh1_creature_data.lua). charId==0 doubles as "no offline data for
    // this model" (record+0x4c==0 is Sora's own party slot, forced below).
    // luaTemplateRecord's real length must come from lua_tolstring's
    // out-param, not strlen -- it's a raw byte string.
    long long luaCharId = (long long)p_lua_tointegerx(L, 21, nullptr);
    long long luaWeight = (long long)p_lua_tointegerx(L, 22, nullptr);
    size_t luaTemplateLen = 0;
    const char* luaTemplateRecord = p_lua_tolstring(L, 23, &luaTemplateLen);
    // Entry point of the CALL-to-fnc_resolve_resource_handle instruction
    // inside the engine's skeleton/bone animation-blend code (see
    // InstallSkeletonHandleGuardHook's own comment). Optional and NOT
    // required like the other three guard hooks above -- unlike those,
    // this one guards a crash in code this DLL doesn't call at all, so
    // there's nothing to "refuse" if it's unconfigured for this game
    // build (e.g. not yet found for EGS) -- just proceed without it,
    // same crash risk as before this hook existed.
    unsigned long long skeletonHandleHookFnRva = (unsigned long long)p_lua_tointegerx(L, 24, nullptr);
    // Entry point of the whole per-entity animation/skeleton-blend update
    // function (see InstallSkeletonBlendCallGuardHook's own comment).
    // Optional, same reasoning as skeletonHandleHookFnRva above.
    unsigned long long skeletonBlendCallHookFnRva = (unsigned long long)p_lua_tointegerx(L, 25, nullptr);
    // CALL-to-fnc_resolve_resource_handle inside the keyframe/blend-list
    // lookup helper (see InstallKeyframeListEntryGuardHook's own comment).
    // Optional, same reasoning as the two hook RVAs above.
    unsigned long long keyframeListEntryHookFnRva = (unsigned long long)p_lua_tointegerx(L, 26, nullptr);
    // Entity pool base + soraPointer + active-party pointers, for
    // AnyEnemyTooCloseToSora (see its own comment). All optional -- 0/unset
    // just skips the check (or that one exclusion), same convention as the
    // hook RVAs above.
    unsigned long long entityPoolBaseRva = (unsigned long long)p_lua_tointegerx(L, 27, nullptr);
    unsigned long long soraPointerRva = (unsigned long long)p_lua_tointegerx(L, 28, nullptr);
    unsigned long long soraObjPtrRva = (unsigned long long)p_lua_tointegerx(L, 29, nullptr);
    unsigned long long partyMember1PtrRva = (unsigned long long)p_lua_tointegerx(L, 30, nullptr);
    unsigned long long partyMember2PtrRva = (unsigned long long)p_lua_tointegerx(L, 31, nullptr);

    if (!modelPath || !motionPath) {
        p_lua_pushboolean(L, 0);
        p_lua_pushstring(L, "spawn_enemy: model_path/motion_path are required");
        return 2;
    }
    // Copy off Lua's own string storage immediately -- see InternPath.
    modelPath = InternPath(modelPath);
    motionPath = InternPath(motionPath);
    if (resolveHandleFnRva == 0) {
        p_lua_pushboolean(L, 0);
        p_lua_pushstring(L, "spawn_enemy: fnc_resolve_resource_handle address not configured for this game build");
        return 2;
    }

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

    // See AnyEnemyTooCloseToSora's own comment -- a fresh spawn lands at
    // Sora's own position, so if another ENEMY is already sitting there,
    // refuse rather than risk two enemies overlapping directly (spawning
    // on Sora himself is fine). Checked before any species resolution/load
    // work, since nothing below matters if this refuses.
    if (AnyEnemyTooCloseToSora(base, entityPoolBaseRva, soraPointerRva, soraObjPtrRva,
                                 partyMember1PtrRva, partyMember2PtrRva, MIN_SPAWN_DISTANCE_FROM_SORA)) {
        LogDebug("spawn_enemy: refusing -- another enemy is already too close to Sora's position");
        p_lua_pushboolean(L, 0);
        p_lua_pushstring(L, "spawn_enemy: another enemy is already too close to Sora -- refusing to avoid spawning on top of it");
        return 2;
    }

    // species (the local slot number) is derived below: reuse an
    // already-loaded slot, or claim a free one and load fresh.
    uint8_t species = 0;
    bool needsLoad = false;

    // kh1_creature_data.lua is the only source of char-id/weight/template
    // data; refuse cleanly if this model isn't in it. luaTemplateRecord is
    // consumed synchronously below, unlike modelPath/motionPath, which must
    // be interned (see InternPath).
    if (luaCharId == 0 || !luaTemplateRecord || luaTemplateLen != PLACEMENT_RECORD_SIZE) {
        p_lua_pushboolean(L, 0);
        p_lua_pushstring(L, "spawn_enemy: no offline fallback data for this creature (missing from kh1_creature_data.lua)");
        return 2;
    }
    const uint8_t* templateRec = (const uint8_t*)luaTemplateRecord;
    uint16_t charId = (uint16_t)luaCharId;
    uint8_t weight = (uint8_t)luaWeight;

    if (loadedPtrTableRva == 0) {
        p_lua_pushboolean(L, 0);
        p_lua_pushstring(L, "spawn_enemy: loadedSpeciesPtrTable address not configured for this game build");
        return 2;
    }
    // Install all three crash-fixing hooks (idempotent) before touching
    // anything else.
    if (jobCallbackFnRva == 0 || velocityBlendFnRva == 0 || partyAbilityIndexFnRva == 0) {
        p_lua_pushboolean(L, 0);
        p_lua_pushstring(L, "spawn_enemy: job-record-guard/velocity-blend-guard/party-ability-index-guard hook addresses not configured for this game build");
        return 2;
    }
    if (!InstallJobRecordGuardHook(base + jobCallbackFnRva, base + jobCallbackFnRva + 5,
                                     tablePtrRva, tableCountRva, worldNumRva, areaNumRva, setNumRva) ||
        !InstallVelocityBlendGuardHook(base + velocityBlendFnRva, base + velocityBlendFnRva + 5) ||
        // +9, not +5: the hook replaces both the call and the risky read
        // right after it -- resumeAddr must skip past both.
        !InstallPartyAbilityIndexGuardHook(base + partyAbilityIndexFnRva, base + partyAbilityIndexFnRva + 9, base + resolveHandleFnRva)) {
        p_lua_pushboolean(L, 0);
        p_lua_pushstring(L, "spawn_enemy: failed to install the async-load/velocity-blend/party-ability-index guard hooks -- refusing rather than risk the crash they fix");
        return 2;
    }
    // Optional fourth guard hook -- see its own comment for why this one
    // doesn't refuse the spawn if unconfigured/failed to install, unlike
    // the three required ones above.
    if (skeletonHandleHookFnRva != 0 && resolveHandleFnRva != 0) {
        if (!InstallSkeletonHandleGuardHook(base + skeletonHandleHookFnRva, base + skeletonHandleHookFnRva + 8, base + resolveHandleFnRva)) {
            LogDebug("spawn_enemy: InstallSkeletonHandleGuardHook failed to install -- proceeding without it");
        }
    }
    if (skeletonBlendCallHookFnRva != 0) {
        if (!InstallSkeletonBlendCallGuardHook(base + skeletonBlendCallHookFnRva)) {
            LogDebug("spawn_enemy: InstallSkeletonBlendCallGuardHook failed to install -- proceeding without it");
        }
    }
    if (keyframeListEntryHookFnRva != 0 && resolveHandleFnRva != 0) {
        if (!InstallKeyframeListEntryGuardHook(base + keyframeListEntryHookFnRva, base + keyframeListEntryHookFnRva + 13,
                                                 base + keyframeListEntryHookFnRva + 0x2b, base + resolveHandleFnRva)) {
            LogDebug("spawn_enemy: InstallKeyframeListEntryGuardHook failed to install -- proceeding without it");
        }
    }
    if (mintHandleFnRva == 0) {
        p_lua_pushboolean(L, 0);
        p_lua_pushstring(L, "spawn_enemy: fnc_mint_resource_handle not configured for this game build -- can't safely reuse a captured template's model/motion handles");
        return 2;
    }
    bool speciesInUseByLiveEntity[RESOURCE_BLOB_MAX_SPECIES + 1] = {};
    MarkSpeciesInUseByLiveEntities(base, entityIterFnRva, resolveHandleFnRva, speciesResourceTableRva, speciesInUseByLiveEntity);
    if (FindTriggeredLoad(modelPath, &species)) {
        // Already triggered a load for this creature this session -- reuse
        // the species and don't trigger another load.
        if (RoomHasNativeSpecies(oldTable, oldCount, species, base + resolveHandleFnRva, modelPath)) {
            char msg[160];
            snprintf(msg, sizeof(msg), "spawn_enemy: slot %d (already triggered by us this session as %s) is used by a different native record in this room -- refusing", species, modelPath);
            LogDebug(msg);
            p_lua_pushboolean(L, 0);
            p_lua_pushstring(L, "spawn_enemy: chosen slot collides with a different creature already native to this room -- refusing");
            return 2;
        }
    } else if (FindLoadedSlotByFilename(base, loadedPtrTableRva, modelPath, &species)) {
        // Already loaded this session -- reuse it, but check the room's own
        // placement table doesn't already use this slot for something else.
        if (RoomHasNativeSpecies(oldTable, oldCount, species, base + resolveHandleFnRva, modelPath)) {
            char msg[160];
            snprintf(msg, sizeof(msg), "spawn_enemy: slot %d (already loaded elsewhere this session as %s) is used by a different native record in this room -- refusing", species, modelPath);
            LogDebug(msg);
            p_lua_pushboolean(L, 0);
            p_lua_pushstring(L, "spawn_enemy: chosen slot collides with a different creature already native to this room -- refusing");
            return 2;
        }
    } else if (FindFreeLoadedSlot(base, loadedPtrTableRva, speciesInUseByLiveEntity, &species)) {
        // mintHandleFnRva/loadAssetsFnRva are required for this specific
        // case (a genuinely fresh slot, nothing loaded into it yet) --
        // checked here, before the placement table is touched at all, to
        // keep this particular check a true no-op.
        if (loadAssetsFnRva == 0) {
            p_lua_pushboolean(L, 0);
            p_lua_pushstring(L, "spawn_enemy: fnc_load_gimmick_assets not configured for this game build -- can't load a creature with zero presence in this room");
            return 2;
        }
        // Same collision risk as the reuse branch above.
        if (RoomHasNativeSpecies(oldTable, oldCount, species, base + resolveHandleFnRva, modelPath)) {
            char msg[160];
            snprintf(msg, sizeof(msg), "spawn_enemy: slot %d is used by a different native record in this room -- refusing", species);
            LogDebug(msg);
            p_lua_pushboolean(L, 0);
            p_lua_pushstring(L, "spawn_enemy: chosen slot collides with a different creature already native to this room -- refusing");
            return 2;
        }
        needsLoad = true;
    } else {
        p_lua_pushboolean(L, 0);
        p_lua_pushstring(L, "spawn_enemy: no free local slot available in this room this session (all 256 in use) -- refusing");
        return 2;
    }
    // Room-exclusion guard for World=3/Area=2/Set=2 intentionally REMOVED
    // 2026-08-04 -- under active investigation now instead of being refused
    // outright. See project_spawn_enemy_cold_spawn_crash_containment.md for
    // the crash this room causes and what's already known about it; git
    // history has the removed check if it needs to come back.
    //
    // Defense in depth: re-check the slot right before touching anything,
    // in case its state changed since the scan above.
    if (needsLoad) {
        volatile uint8_t* stateAddr = (volatile uint8_t*)(uintptr_t)(base + loadedPtrTableRva + LOADED_SPECIES_STATE_OFFSET_FROM_PTR + (size_t)species * LOADED_SPECIES_STRIDE);
        if (*stateAddr != 0) {
            const char* cachedName = (const char*)(uintptr_t)(base + loadedPtrTableRva + LOADED_SPECIES_MODEL_NAME_OFFSET_FROM_PTR + (size_t)species * LOADED_SPECIES_STRIDE);
            if (strncmp(cachedName, modelPath, LOADED_SPECIES_MODEL_NAME_SIZE) != 0) {
                char msg[192];
                snprintf(msg, sizeof(msg), "spawn_enemy: slot %d already holds a different creature's data this session (cached model file doesn't match %s) -- refusing to avoid corrupting it", species, modelPath);
                LogDebug(msg);
                p_lua_pushboolean(L, 0);
                p_lua_pushstring(L, "spawn_enemy: chosen slot collides with another creature already active in this room -- refusing");
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

    // record+0x08 is a cached resource-handle field with a self-heal in the
    // constructor for the value 0 (unset) -- a cloned template's stale
    // value can resolve non-zero-but-wrong in a new room, so zero it to
    // force a fresh resolve.
    memset(newRec + 8, 0, 4);

    // The category/kind byte drives every kind-specific setup branch in the
    // constructor -- an out-of-band value renders but leaves the entity
    // non-interactable. Use the real category (3, character/actor) with
    // this record's own slot index.
    uint32_t newId = ((uint32_t)3 << 16) | ((uint32_t)oldCount & 0xFFFFu);
    memcpy(newRec + 0, &newId, 4);
    memcpy(newRec + PLACEMENT_POS_X_OFFSET, &x, 4);
    memcpy(newRec + PLACEMENT_POS_Y_OFFSET, &y, 4);
    memcpy(newRec + PLACEMENT_POS_Z_OFFSET, &z, 4);

    // Force the species byte to the chosen slot rather than trusting
    // templateRec's own value, which can be stale from a different room.
    newRec[PLACEMENT_SPECIES_OFFSET] = species;

    // char-id 0 is treated by the game as a party-member index and would
    // overwrite Sora's own object pointer if left unset -- force the
    // offline-derived char-id/weight instead of the template's own values.
    memcpy(newRec + 0x4c, &charId, 2);
    newRec[0x59] = weight;

    // Publish before the load-trigger block below: fnc_load_gimmick_assets
    // looks our record up via `newId` in this same live table. Every
    // refusal path below explicitly rolls this back.
    *tablePtrAddr = newTable;
    *tableCountAddr = oldCount + 1;

    // record+0x60/+0x64 hold handles to the model/motion filename strings,
    // encoded relative to this session's bucket table -- a cloned
    // template's raw handle is invalid here. Mint fresh handles for our own
    // strings instead of ever copying the captured ones. Runs for every
    // spawn, not just needsLoad.
    bool handlesMinted = false;
    {
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
            handlesMinted = true;
        } else {
            LogDebug("spawn_enemy: minting a fresh resource-string handle crashed -- constructing with the template's original (possibly stale) handles");
        }
    }

    if (needsLoad && handlesMinted) {
        // Tag this record as "ours" before queuing the job -- the guard
        // hook needs this set before fnc_load_gimmick_assets captures it.
        g_lastQueuedGimmickRecordPtr = (uint64_t)(uintptr_t)newRec;
        if (worldNumRva != 0 && areaNumRva != 0 && setNumRva != 0) {
            g_lastQueuedGimmickWorld = *(int32_t*)(uintptr_t)(base + worldNumRva);
            g_lastQueuedGimmickArea = *(int32_t*)(uintptr_t)(base + areaNumRva);
            g_lastQueuedGimmickSet = *(int32_t*)(uintptr_t)(base + setNumRva);
            g_lastQueuedGimmickRoomIdentityValid = true;
        } else {
            g_lastQueuedGimmickRoomIdentityValid = false;
        }
        unsigned long long loadArgs[2] = { (unsigned long long)newId, 0 };
        unsigned long long loadResult = 0;
        bool loadOk = SafeCall(base + loadAssetsFnRva, loadArgs, 2, loadResult);
        if (!loadOk) {
            char msg[160];
            snprintf(msg, sizeof(msg), "spawn_enemy: asset-load trigger crashed: loadAssetsFnRva=0x%llx id=0x%x species=%d", loadAssetsFnRva, newId, species);
            LogDebug(msg);
            *tablePtrAddr = oldTable;
            *tableCountAddr = oldCount;
            p_lua_pushboolean(L, 0);
            p_lua_pushstring(L, "spawn_enemy: exception during asset-load trigger call");
            return 2;
        }
        // Record immediately, even though the load hasn't completed, to
        // stop a retry frame from triggering a second concurrent load.
        RecordTriggeredLoad(modelPath, species);

        // Doesn't block: the async job is drained by the game's own
        // per-frame loop on this same thread, so blocking here would
        // prevent it from ever running. Checks once and, if not ready,
        // rolls back and returns a distinct third value (stillLoading=true)
        // so the caller retries next frame (see spawn_enemy_async).
        // Requires state==6, not just loadedPtrAddr!=0 -- a fresh species's
        // first callback writes the completion pointer but leaves state 0.
        volatile uint64_t* loadedPtrAddr = (volatile uint64_t*)(uintptr_t)(base + loadedPtrTableRva + (size_t)species * LOADED_SPECIES_STRIDE);
        volatile uint8_t* stateAddr = (volatile uint8_t*)(uintptr_t)(base + loadedPtrTableRva + LOADED_SPECIES_STATE_OFFSET_FROM_PTR + (size_t)species * LOADED_SPECIES_STRIDE);
        if (*loadedPtrAddr == 0 || *stateAddr != 6) {
            char msg[160];
            snprintf(msg, sizeof(msg), "spawn_enemy: asset load for species=%d not ready yet (state=%u), refusing without blocking -- caller should retry next frame", species, (unsigned)*stateAddr);
            LogDebug(msg);
            *tablePtrAddr = oldTable;
            *tableCountAddr = oldCount;
            p_lua_pushboolean(L, 0);
            p_lua_pushstring(L, "spawn_enemy: asset load still in progress -- call again next frame (this is normal for a creature's first load this session)");
            p_lua_pushboolean(L, 1);
            return 3;
        }
    } else if (needsLoad) {
        // Minting crashed -- the readiness check below catches this too
        // (loadedPtrAddr reads 0 for a species never triggered).
        LogDebug("spawn_enemy: minting a fresh resource-string handle crashed -- refusing rather than constructing with possibly-stale handles");
    }

    // Same readiness check for the FindLoadedSlotByFilename reuse branch,
    // which never triggers a load itself.
    if (!needsLoad) {
        volatile uint64_t* loadedPtrAddr = (volatile uint64_t*)(uintptr_t)(base + loadedPtrTableRva + (size_t)species * LOADED_SPECIES_STRIDE);
        volatile uint8_t* stateAddr = (volatile uint8_t*)(uintptr_t)(base + loadedPtrTableRva + LOADED_SPECIES_STATE_OFFSET_FROM_PTR + (size_t)species * LOADED_SPECIES_STRIDE);
        if (*loadedPtrAddr == 0 || *stateAddr != 6) {
            char msg[160];
            snprintf(msg, sizeof(msg), "spawn_enemy: slot %d marked loaded but not fully ready yet (state=%u) -- refusing without blocking", species, (unsigned)*stateAddr);
            LogDebug(msg);
            *tablePtrAddr = oldTable;
            *tableCountAddr = oldCount;
            p_lua_pushboolean(L, 0);
            p_lua_pushstring(L, "spawn_enemy: asset load still in progress -- call again next frame (this is normal for a creature's first load this session)");
            p_lua_pushboolean(L, 1);
            return 3;
        }
    }

    unsigned long long spawnFnAddr = base + spawnFnRva;
    unsigned long long args[1] = { (unsigned long long)newId };
    unsigned long long result = 0;

    // Diagnostic: log the record's handle-bearing fields before the
    // constructor call, since a crash rolls the table publish back with no
    // other way to inspect what the constructor saw.
    {
        uint32_t h8, h60, h64;
        memcpy(&h8, newRec + 8, 4);
        memcpy(&h60, newRec + PLACEMENT_MODEL_HANDLE_OFFSET, 4);
        memcpy(&h64, newRec + PLACEMENT_MOTION_HANDLE_OFFSET, 4);
        char diagMsg[192];
        snprintf(diagMsg, sizeof(diagMsg), "spawn_enemy: pre-construct id=0x%x species=%d needsLoad=%d handlesMinted=%d rec+8=0x%08x rec+0x60=0x%08x rec+0x64=0x%08x charId=%u weight=%u",
            newId, species, (int)needsLoad, (int)handlesMinted, h8, h60, h64,
            (unsigned)(*(uint16_t*)(newRec + 0x4c)), (unsigned)newRec[0x59]);
        LogDebug(diagMsg);
    }

    g_lastSpawnAttemptId = newId;
    bool ok = SafeCall(spawnFnAddr, args, 1, result);

    if (!ok) {
        char msg[160];
        snprintf(msg, sizeof(msg), "spawn_enemy crashed: spawnFnRva=0x%llx id=0x%x species=%d", spawnFnRva, newId, species);
        LogDebug(msg);
        // Roll back the table publish -- no entity was constructed.
        *tablePtrAddr = oldTable;
        *tableCountAddr = oldCount;
        p_lua_pushboolean(L, 0);
        p_lua_pushstring(L, "spawn_enemy: exception during constructor call");
        return 2;
    }

    // A null return means the constructor refused (room's concurrent-entity
    // budget full), not a crash -- the table is already spliced.
    if (result == 0) {
        char msg[192];
        snprintf(msg, sizeof(msg), "spawn_enemy: constructor call succeeded but returned a null entity -- likely the room's concurrent-entity budget is full (id=0x%x)", newId);
        LogDebug(msg);
        *tablePtrAddr = oldTable;
        *tableCountAddr = oldCount;
        p_lua_pushboolean(L, 0);
        p_lua_pushstring(L, "spawn_enemy: constructor refused (room's concurrent-entity budget is likely full right now) -- try again after some are cleared");
        return 2;
    }

    p_lua_pushboolean(L, 1);
    p_lua_pushinteger(L, (long long)result);
    return 2;
}

// --- EVDL SYSCALL BRIDGE ---

// Certain functions aren't just calls to
// standalone functions.  Some are specific
// to an EVDL context, and require a scriptCtx.
// That includes mainly a stack.  This function
// mocks up a stack to call such functions rather
// than messing with injecting directly into
// the actually running scriptCtx (dangerous)

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

// Below is the custom work we have to do
// to handle injecting custom text into 
// the prize text pop up.

// Buffer to hold custom prize text
static unsigned char g_customTextBuffer[512] = {};

// Need to set this as volatile to ensure
// every read of that flag actually goes
// to real memory rather than a cached/stale
// value. The injected game code itself can
// touch this flag, something this codes ctx
// would have no visibility into.
static volatile unsigned char g_customTextActive = 0;

// Flag to handle whether the custom byte
// code has been injected or not.
static bool g_popupHookInstalled = false;

// Function that tries to find executable
// near the target, since jmp/call instructions
// can only reference other addresses within
// a certain memory range
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

// Pauses every thread in the process except
// the calling one so we can hook bytes
// without another thread executing the code
// mid patch.
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

// Opposite of above, resumes those threads
// after the hook is installed.
static void ResumeThreads(std::vector<HANDLE>& handles) {
    for (HANDLE h : handles) {
        ResumeThread(h);
        CloseHandle(h);
    }
    handles.clear();
}

// --- TEXT BOX HOOK ---

// Buffer to hold our custom text
static unsigned char g_textBoxBuffer[512] = {};

// Need to set this as volatile to ensure
// every read of that flag actually goes
// to real memory rather than a cached/stale
// value. The injected game code itself can
// touch this flag, something this codes ctx
// would have no visibility into.
static volatile unsigned char g_textBoxActive = 0;

// Flags
static bool g_textBoxHookInstalled = false;
static bool g_textBoxAnimHookInstalled = false;

// Handles the instruction hook
// to point to our custom text
// for the duration of the box's
// life.
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

// Lua-callable wrapper for the hook above,
// takes the addresses as RVAs off the base.
extern "C" int l_install_textbox_hook(void* L) {
    unsigned long long base = (unsigned long long)GetModuleHandleA(nullptr);
    unsigned long long hookRva = (unsigned long long)p_lua_tointegerx(L, 1, nullptr);
    unsigned long long resumeRva = (unsigned long long)p_lua_tointegerx(L, 2, nullptr);
    bool ok = InstallTextBoxHook(base + hookRva, base + resumeRva);
    p_lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

// Same idea as InstallTextBoxHook, but for the
// call site that plays the per-character text
// box animation.
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

// Lua-callable wrapper for the hook above.
extern "C" int l_install_textbox_anim_hook(void* L) {
    unsigned long long base = (unsigned long long)GetModuleHandleA(nullptr);
    unsigned long long hookRva = (unsigned long long)p_lua_tointegerx(L, 1, nullptr);
    unsigned long long resumeRva = (unsigned long long)p_lua_tointegerx(L, 2, nullptr);
    unsigned long long callTargetRva = (unsigned long long)p_lua_tointegerx(L, 3, nullptr);
    bool ok = InstallTextBoxAnimHook(base + hookRva, base + resumeRva, base + callTargetRva);
    p_lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

// Copies the lua string (as a byte array) into
// our buffer and flags the text box as active.
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

// Turns off the text box override flag.
extern "C" int l_clear_textbox_text(void* L) {
    g_textBoxActive = 0;
    return 0;
}

// --- PENDING TEXT BOX TRACKING ---

// Keeps track of the most recently opened text
// box window so we can close it on demand from
// lua without the caller needing to track ids.
static volatile long g_pendingTextBoxWindowId = -1;
static volatile unsigned long long g_pendingTextBoxCloseRva = 0;

// Records the window id and close function RVA
// for the currently open text box.
extern "C" int l_set_pending_text_box(void* L) {
    g_pendingTextBoxWindowId = (long)p_lua_tointegerx(L, 1, nullptr);
    g_pendingTextBoxCloseRva = (unsigned long long)p_lua_tointegerx(L, 2, nullptr);
    return 0;
}

// Clears the tracked window without closing it.
extern "C" int l_clear_pending_text_box(void* L) {
    g_pendingTextBoxWindowId = -1;
    g_pendingTextBoxCloseRva = 0;
    return 0;
}

// Calls the game's close function for the
// tracked text box, mocking up a scriptCtx
// the same way call_evdl_syscall does.
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

// Hooks the call that sets up the prize popup's
// text pointer, swapping in our custom buffer
// whenever the active flag is set.
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

// Lua-callable wrapper for the hook above.
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

// Below is the work needed to know when the
// prize popup has actually finished displaying,
// so we can clear our custom text flag once
// its lifecycle is done.

// Remembers the popup's state from the last
// tick so we can detect the transition back
// to 0 (closed).
static uint32_t g_prevPopupState = 0;
static bool g_popupCompletionHookInstalled = false;

// Hooks the popup's tick function so we can
// watch its state and clear the custom text
// flag once the popup closes.
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

// Lua-callable wrapper for the hook above.
extern "C" int l_install_popup_completion_hook(void* L) {
    unsigned long long base = (unsigned long long)GetModuleHandleA(nullptr);
    unsigned long long tickRva = (unsigned long long)p_lua_tointegerx(L, 1, nullptr);
    unsigned long long resumeRva = (unsigned long long)p_lua_tointegerx(L, 2, nullptr);
    unsigned long long stateRva = (unsigned long long)p_lua_tointegerx(L, 3, nullptr);
    bool ok = InstallPopupCompletionHook(base + tickRva, base + resumeRva, base + stateRva);
    p_lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

// Copies the lua string (as a byte array) into
// our buffer and flags the popup text as active.
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

// Turns off the custom popup text flag.
extern "C" int l_clear_custom_popup_text(void* L) {
    g_customTextActive = 0;
    return 0;
}

// --- VELOCITY BLEND GUARD HOOK ---
// Fix for a crash in a shared per-frame motion/velocity-blend utility that
// unconditionally dereferences its second parameter (a motion-descriptor
// pointer) with no validity check. Hooks the function's entry and probes
// whether the descriptor is readable before the original body runs -- a
// readable descriptor takes the exact original path, zero behavior change.
static bool g_velocityBlendGuardHookInstalled = false;
static volatile uint64_t g_velocityBlendGuardSkipCount = 0;

// Fix for a TOCTOU race: probing readability then letting the original
// code independently re-read the same live pointer leaves a window for
// another thread to invalidate that memory. Copies the probed bytes into a
// thread-local snapshot and redirects the original code to that stable
// copy instead.
static thread_local uint8_t g_veloBlendSnapshot[0x14];

// __except turns a hardware fault into a zero return. Returns 0 if
// unreadable (and zero-fills param3's output, the caller's uninitialized
// stack buffer), or the snapshot's address on success.
static uint64_t CheckVelocityBlendParamSafe(uint64_t param2, uint64_t param3) {
    __try {
        memcpy(g_veloBlendSnapshot, (const void*)(uintptr_t)param2, sizeof(g_veloBlendSnapshot));
        return (uint64_t)(uintptr_t)g_veloBlendSnapshot;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        __try {
            memset((void*)(uintptr_t)param3, 0, 0x20);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            LogDebug("VelocityBlendGuard: param3 output buffer itself unwritable, leaving it as-is");
        }
        char msg[160];
        snprintf(msg, sizeof(msg),
            "VelocityBlendGuard: param2=0x%llX unreadable, skipping+zeroing this call (lastSpawnAttemptId=%u, skipsSoFar=%llu)",
            (unsigned long long)param2, g_lastSpawnAttemptId,
            (unsigned long long)g_velocityBlendGuardSkipCount + 1);
        LogDebug(msg);
        ++g_velocityBlendGuardSkipCount;
        return 0;
    }
}

// Hooks the function's entry (expected: 5-byte "mov qword ptr [rsp+8],
// rbx"), redirecting through CheckVelocityBlendParamSafe.
static bool InstallVelocityBlendGuardHook(unsigned long long hookAddr, unsigned long long resumeAddr) {
    if (g_velocityBlendGuardHookInstalled) return true;

    unsigned char* hookPtr = (unsigned char*)(uintptr_t)hookAddr;
    static const unsigned char expected[5] = { 0x48, 0x89, 0x5C, 0x24, 0x08 };
    if (memcmp(hookPtr, expected, 5) != 0) {
        LogDebug("InstallVelocityBlendGuardHook: unexpected original bytes at hook address, aborting");
        return false;
    }

    void* cave = AllocateNear((void*)(uintptr_t)hookAddr, 4096);
    if (!cave) {
        LogDebug("InstallVelocityBlendGuardHook: failed to allocate a nearby code cave");
        return false;
    }

    unsigned char stub[96] = {};
    size_t off = 0;
    uint64_t checkFnAddr = (uint64_t)(uintptr_t)&CheckVelocityBlendParamSafe;

    stub[off++] = 0x51; // push rcx
    stub[off++] = 0x52; // push rdx
    stub[off++] = 0x41; stub[off++] = 0x50; // push r8

    stub[off++] = 0x48; stub[off++] = 0x89; stub[off++] = 0xD0; // mov rax, rdx  (rax = param2)
    stub[off++] = 0x4C; stub[off++] = 0x89; stub[off++] = 0xC2; // mov rdx, r8   (rdx/arg2 = param3)
    stub[off++] = 0x48; stub[off++] = 0x89; stub[off++] = 0xC1; // mov rcx, rax  (rcx/arg1 = param2)

    stub[off++] = 0x48; stub[off++] = 0x83; stub[off++] = 0xEC; stub[off++] = 0x20; // sub rsp, 0x20

    stub[off++] = 0x48; stub[off++] = 0xB8; // mov rax, checkFnAddr
    memcpy(stub + off, &checkFnAddr, 8); off += 8;

    stub[off++] = 0xFF; stub[off++] = 0xD0; // call rax

    stub[off++] = 0x48; stub[off++] = 0x83; stub[off++] = 0xC4; stub[off++] = 0x20; // add rsp, 0x20

    stub[off++] = 0x48; stub[off++] = 0x85; stub[off++] = 0xC0; // test rax, rax (64-bit -- rax is a pointer)

    stub[off++] = 0x41; stub[off++] = 0x58; // pop r8
    stub[off++] = 0x5A;                     // pop rdx
    stub[off++] = 0x59;                     // pop rcx

    stub[off++] = 0x0F; stub[off++] = 0x84; // jz rel32 (patched below) -> skipCall
    size_t jzOperand = off; off += 4;

    stub[off++] = 0x48; stub[off++] = 0x89; stub[off++] = 0xC2; // mov rdx, rax (redirect to the safe snapshot)

    memcpy(stub + off, hookPtr, 5); off += 5; // replay original mov [rsp+8],rbx

    stub[off++] = 0xE9; // jmp rel32 (patched below) -> resumeAddr
    size_t jmpBackOperand = off; off += 4;

    size_t skipCallPos = off;
    stub[off++] = 0xC3; // ret

    size_t stubLen = off;
    uintptr_t caveBase = (uintptr_t)cave;

    int32_t jzRel = (int32_t)((int64_t)skipCallPos - (int64_t)(jzOperand + 4));
    memcpy(stub + jzOperand, &jzRel, 4);

    int32_t jmpBackRel = (int32_t)((int64_t)resumeAddr - (int64_t)(caveBase + jmpBackOperand + 4));
    memcpy(stub + jmpBackOperand, &jmpBackRel, 4);

    unsigned char patch[5];
    patch[0] = 0xE9;
    int32_t hookRel = (int32_t)((int64_t)caveBase - (int64_t)(hookAddr + 5));
    memcpy(patch + 1, &hookRel, 4);

    std::vector<HANDLE> threads = SuspendOtherThreads();

    memcpy(cave, stub, stubLen);

    DWORD oldProtect = 0;
    VirtualProtect((void*)(uintptr_t)hookAddr, 5, PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((void*)(uintptr_t)hookAddr, patch, 5);
    VirtualProtect((void*)(uintptr_t)hookAddr, 5, oldProtect, &oldProtect);

    FlushInstructionCache(GetCurrentProcess(), (void*)(uintptr_t)hookAddr, 5);
    FlushInstructionCache(GetCurrentProcess(), cave, stubLen);

    ResumeThreads(threads);

    g_velocityBlendGuardHookInstalled = true;
    LogDebug("InstallVelocityBlendGuardHook: installed successfully");
    return true;
}

// install_velocity_blend_guard_hook(hookRva, resumeRva) -> ok(boolean)
// Idempotent. hookRva should be the function's entry, resumeRva its entry+5.
extern "C" int l_install_velocity_blend_guard_hook(void* L) {
    unsigned long long base = (unsigned long long)GetModuleHandleA(nullptr);
    unsigned long long hookRva = (unsigned long long)p_lua_tointegerx(L, 1, nullptr);
    unsigned long long resumeRva = (unsigned long long)p_lua_tointegerx(L, 2, nullptr);
    bool ok = InstallVelocityBlendGuardHook(base + hookRva, base + resumeRva);
    p_lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

// get_velocity_blend_guard_skip_count() -> count(integer)
// Diagnostic readback for live testing -- how many times the guard has
// skipped a call this session because the descriptor was unreadable.
extern "C" int l_get_velocity_blend_guard_skip_count(void* L) {
    p_lua_pushinteger(L, (long long)g_velocityBlendGuardSkipCount);
    return 1;
}

// --- PARTY ABILITY ARRAY-INDEX GUARD HOOK ---
// Fix for a crash in an unrelated per-party ability-data rebuild routine,
// inside a tiny shared helper that resolves a cached handle and indexes
// into the result with no bounds check:
//   rax = *param1; rcx = *rax; ecx = *(rcx+8);
//   rax = fnc_resolve_resource_handle(ecx);
//   return *(int*)(rax + param2*4 + 0x10);   // <-- the fault: no bounds check
// Both real callers already treat a negative return as "not found" -- -1 on
// an unreadable read is the existing contract, reached safely.
static bool g_partyAbilityIndexGuardHookInstalled = false;
static volatile uint64_t g_partyAbilityIndexGuardSkipCount = 0;

// Returns the real int value if readable, or -1 if not.
static int32_t CheckPartyAbilityIndexSafe(uint64_t basePtr, uint64_t index) {
    __try {
        return *(int32_t*)(uintptr_t)(basePtr + index * 4 + 0x10);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        char msg[160];
        snprintf(msg, sizeof(msg),
            "PartyAbilityIndexGuard: basePtr=0x%llX index=%llu unreadable, returning -1 (skipsSoFar=%llu)",
            (unsigned long long)basePtr, (unsigned long long)index,
            (unsigned long long)g_partyAbilityIndexGuardSkipCount + 1);
        LogDebug(msg);
        ++g_partyAbilityIndexGuardSkipCount;
        return -1;
    }
}

// Hooks at the CALL to fnc_resolve_resource_handle (5 bytes) rather than
// the 4-byte faulting read after it -- the stub replicates the resolve
// call then feeds its result into the safety-checked read.
static bool InstallPartyAbilityIndexGuardHook(unsigned long long hookAddr, unsigned long long resumeAddr,
                                                unsigned long long resolveHandleFnAddr) {
    if (g_partyAbilityIndexGuardHookInstalled) return true;

    unsigned char* hookPtr = (unsigned char*)(uintptr_t)hookAddr;
    static const unsigned char expected[5] = { 0xE8, 0xC9, 0x4A, 0x1B, 0x00 };
    if (memcmp(hookPtr, expected, 5) != 0) {
        LogDebug("InstallPartyAbilityIndexGuardHook: unexpected original bytes at hook address, aborting");
        return false;
    }

    void* cave = AllocateNear((void*)(uintptr_t)hookAddr, 4096);
    if (!cave) {
        LogDebug("InstallPartyAbilityIndexGuardHook: failed to allocate a nearby code cave");
        return false;
    }

    unsigned char stub[64] = {};
    size_t off = 0;
    uint64_t checkFnAddr = (uint64_t)(uintptr_t)&CheckPartyAbilityIndexSafe;

    stub[off++] = 0x48; stub[off++] = 0xB8; // mov rax, resolveHandleFnAddr
    memcpy(stub + off, &resolveHandleFnAddr, 8); off += 8;
    stub[off++] = 0xFF; stub[off++] = 0xD0; // call rax  (rax <- resolved pointer, same effect as the original call)

    stub[off++] = 0x48; stub[off++] = 0x89; stub[off++] = 0xC1; // mov rcx, rax  (arg1 = resolved pointer)
    stub[off++] = 0x48; stub[off++] = 0x89; stub[off++] = 0xDA; // mov rdx, rbx  (arg2 = index)

    stub[off++] = 0x48; stub[off++] = 0x83; stub[off++] = 0xEC; stub[off++] = 0x20; // sub rsp, 0x20

    stub[off++] = 0x48; stub[off++] = 0xB8; // mov rax, checkFnAddr
    memcpy(stub + off, &checkFnAddr, 8); off += 8;

    stub[off++] = 0xFF; stub[off++] = 0xD0; // call rax  (eax <- safe result)

    stub[off++] = 0x48; stub[off++] = 0x83; stub[off++] = 0xC4; stub[off++] = 0x20; // add rsp, 0x20

    stub[off++] = 0xE9; // jmp rel32 -> resumeAddr (past both replaced instructions)
    size_t jmpOperand = off; off += 4;

    size_t stubLen = off;
    uintptr_t caveBase = (uintptr_t)cave;

    int32_t jmpRel = (int32_t)((int64_t)resumeAddr - (int64_t)(caveBase + jmpOperand + 4));
    memcpy(stub + jmpOperand, &jmpRel, 4);

    unsigned char patch[5];
    patch[0] = 0xE9;
    int32_t hookRel = (int32_t)((int64_t)caveBase - (int64_t)(hookAddr + 5));
    memcpy(patch + 1, &hookRel, 4);

    std::vector<HANDLE> threads = SuspendOtherThreads();

    memcpy(cave, stub, stubLen);

    DWORD oldProtect = 0;
    VirtualProtect((void*)(uintptr_t)hookAddr, 5, PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((void*)(uintptr_t)hookAddr, patch, 5);
    VirtualProtect((void*)(uintptr_t)hookAddr, 5, oldProtect, &oldProtect);

    FlushInstructionCache(GetCurrentProcess(), (void*)(uintptr_t)hookAddr, 5);
    FlushInstructionCache(GetCurrentProcess(), cave, stubLen);

    ResumeThreads(threads);

    g_partyAbilityIndexGuardHookInstalled = true;
    LogDebug("InstallPartyAbilityIndexGuardHook: installed successfully");
    return true;
}

// --- SKELETON HANDLE DEREF GUARD HOOK ---
// Fix for a real, live-repro'd crash (confirmed 2026-08-04 via minidump
// register/exception analysis, twice, with two different creatures --
// Darkball and Search Ghost, both crashing at the identical instruction) in
// the engine's own per-frame skeleton/bone animation-blend code, which this
// DLL never calls directly -- there's no call site of ours to wrap in
// SafeCall/SEH here. The vulnerable pattern is the same shared idiom
// PartyAbilityIndexGuard above already fixes at a different call site:
//   rax = *param1; rcx = *rax; ecx = *(rcx+8);
//   rax = fnc_resolve_resource_handle(ecx);
//   ebx = *(uint32_t*)(rax + 0x20);   // <-- the fault: no validity check
// Both confirmed crashes read a never-minted handle (bit 31, the "real
// handle" marker every fnc_mint_resource_handle result carries, unset --
// i.e. raw uninitialized memory, not a stale-but-valid handle) that
// resolves to an unmapped address. Root cause (why this specific
// resource never gets minted for a creature loaded cold in certain rooms)
// is still unknown -- see KH1-LUA-LIBRARY memory:
// project_spawn_enemy_cold_spawn_crash_containment.md. This hook doesn't
// fix that; it makes the resulting bad read safe instead of fatal, exactly
// like the other three guard hooks in this file already do for their own
// known-risky reads.
// --- KEYFRAME LIST ENTRY GUARD HOOK ---
// Fixes the very FIRST crash site found this session (2026-08-04, before
// any of the other guard hooks below existed): a keyframe/blend-list
// lookup helper (FUN_140394690 in Ghidra) resolves a handle and
// immediately checks *(int32_t*)resolvedPtr against -1 (its own
// list-terminator sentinel) to know whether to keep scanning -- if the
// handle was never minted for this creature, the resolved pointer is
// invalid and this read faults. Confirmed live TWICE: once early this
// session (small-garbage handle) and once again after several of the
// other guard hooks were already installed (this time a literal NULL
// handle) -- this specific site is in a different call tree from the
// SkeletonHandleGuard/SkeletonBlendCallGuard hooks below and was never
// covered by them. See KH1-LUA-LIBRARY memory:
// project_spawn_enemy_cold_spawn_crash_containment.md.
static bool g_keyframeListEntryGuardHookInstalled = false;
static volatile uint64_t g_keyframeListEntryGuardSkipCount = 0;

// Returns the real dword value if readable, or -1 (the original code's own
// "end of list" sentinel) if not -- callers already handle -1 correctly.
static int32_t CheckKeyframeListEntrySafe(uint64_t ptr) {
    __try {
        return *(int32_t*)(uintptr_t)ptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        char msg[160];
        snprintf(msg, sizeof(msg),
            "KeyframeListEntryGuard: ptr=0x%llX unreadable, treating as end-of-list sentinel (skipsSoFar=%llu)",
            (unsigned long long)ptr, (unsigned long long)g_keyframeListEntryGuardSkipCount + 1);
        LogDebug(msg);
        ++g_keyframeListEntryGuardSkipCount;
        return -1;
    }
}

// Hooks at the CALL to fnc_resolve_resource_handle (5 bytes) through the
// JZ that follows the original CMP (13 bytes total: call + "mov rdi,rax"
// [rdi is read later in the real function] + "cmp dword ptr [rax],-1" +
// jz) -- only the leading 5 bytes actually get patched (an E9 jmp), same
// as InstallPartyAbilityIndexGuardHook's own 9-byte case -- the remaining
// original bytes are simply jumped over, never executed, and don't need
// to be overwritten.
static bool InstallKeyframeListEntryGuardHook(unsigned long long hookAddr, unsigned long long resumeAddr,
                                                unsigned long long notFoundAddr,
                                                unsigned long long resolveHandleFnAddr) {
    if (g_keyframeListEntryGuardHookInstalled) return true;

    unsigned char* hookPtr = (unsigned char*)(uintptr_t)hookAddr;
    static const unsigned char expected[13] = {
        0xE8, 0xFC, 0x66, 0xFF, 0xFF,       // call fnc_resolve_resource_handle
        0x48, 0x8B, 0xF8,                   // mov rdi, rax
        0x83, 0x38, 0xFF,                   // cmp dword ptr [rax], -1
        0x74, 0x1E                          // jz +0x1e
    };
    if (memcmp(hookPtr, expected, 13) != 0) {
        LogDebug("InstallKeyframeListEntryGuardHook: unexpected original bytes at hook address, aborting");
        return false;
    }

    void* cave = AllocateNear((void*)(uintptr_t)hookAddr, 4096);
    if (!cave) {
        LogDebug("InstallKeyframeListEntryGuardHook: failed to allocate a nearby code cave");
        return false;
    }

    unsigned char stub[96] = {};
    size_t off = 0;
    uint64_t checkFnAddr = (uint64_t)(uintptr_t)&CheckKeyframeListEntrySafe;

    stub[off++] = 0x48; stub[off++] = 0xB8; // mov rax, resolveHandleFnAddr
    memcpy(stub + off, &resolveHandleFnAddr, 8); off += 8;
    stub[off++] = 0xFF; stub[off++] = 0xD0; // call rax  (rax <- resolved pointer, same effect as the original call)

    stub[off++] = 0x48; stub[off++] = 0x89; stub[off++] = 0xC7; // mov rdi, rax  (replicate "mov rdi,rax" from the original)

    stub[off++] = 0x48; stub[off++] = 0x89; stub[off++] = 0xC1; // mov rcx, rax  (arg1 = pointer to safety-check)

    stub[off++] = 0x48; stub[off++] = 0x83; stub[off++] = 0xEC; stub[off++] = 0x20; // sub rsp, 0x20

    stub[off++] = 0x48; stub[off++] = 0xB8; // mov rax, checkFnAddr
    memcpy(stub + off, &checkFnAddr, 8); off += 8;

    stub[off++] = 0xFF; stub[off++] = 0xD0; // call rax  (eax <- safe dword or -1 sentinel)

    stub[off++] = 0x48; stub[off++] = 0x83; stub[off++] = 0xC4; stub[off++] = 0x20; // add rsp, 0x20

    stub[off++] = 0x83; stub[off++] = 0xF8; stub[off++] = 0xFF; // cmp eax, -1

    stub[off++] = 0x0F; stub[off++] = 0x84; // jz rel32 (patched below) -> notFoundAddr
    size_t jzOperand = off; off += 4;

    stub[off++] = 0xE9; // jmp rel32 (patched below) -> resumeAddr
    size_t jmpOperand = off; off += 4;

    size_t stubLen = off;
    uintptr_t caveBase = (uintptr_t)cave;

    int32_t jzRel = (int32_t)((int64_t)notFoundAddr - (int64_t)(caveBase + jzOperand + 4));
    memcpy(stub + jzOperand, &jzRel, 4);

    int32_t jmpRel = (int32_t)((int64_t)resumeAddr - (int64_t)(caveBase + jmpOperand + 4));
    memcpy(stub + jmpOperand, &jmpRel, 4);

    unsigned char patch[5];
    patch[0] = 0xE9;
    int32_t hookRel = (int32_t)((int64_t)caveBase - (int64_t)(hookAddr + 5));
    memcpy(patch + 1, &hookRel, 4);

    std::vector<HANDLE> threads = SuspendOtherThreads();

    memcpy(cave, stub, stubLen);

    DWORD oldProtect = 0;
    VirtualProtect((void*)(uintptr_t)hookAddr, 5, PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((void*)(uintptr_t)hookAddr, patch, 5);
    VirtualProtect((void*)(uintptr_t)hookAddr, 5, oldProtect, &oldProtect);

    FlushInstructionCache(GetCurrentProcess(), (void*)(uintptr_t)hookAddr, 5);
    FlushInstructionCache(GetCurrentProcess(), cave, stubLen);

    ResumeThreads(threads);

    g_keyframeListEntryGuardHookInstalled = true;
    LogDebug("InstallKeyframeListEntryGuardHook: installed successfully");
    return true;
}

static bool g_skeletonHandleGuardHookInstalled = false;
static volatile uint64_t g_skeletonHandleGuardSkipCount = 0;

// Returns the real dword value if [resolvedPtr+0x20] is readable, or 0 if
// not -- 0 is the same "nothing here" value a genuinely empty/never-set
// slot would already read as, so callers already handle it.
static uint32_t CheckSkeletonHandleDerefSafe(uint64_t resolvedPtr) {
    __try {
        return *(uint32_t*)(uintptr_t)(resolvedPtr + 0x20);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        char msg[160];
        snprintf(msg, sizeof(msg),
            "SkeletonHandleGuard: resolvedPtr=0x%llX+0x20 unreadable, returning 0 (skipsSoFar=%llu)",
            (unsigned long long)resolvedPtr, (unsigned long long)g_skeletonHandleGuardSkipCount + 1);
        LogDebug(msg);
        ++g_skeletonHandleGuardSkipCount;
        return 0;
    }
}

// Hooks at the CALL to fnc_resolve_resource_handle (5 bytes) plus the
// 3-byte "MOV EBX, dword ptr [RAX+0x20]" faulting read right after it (8
// bytes total) -- same technique as InstallPartyAbilityIndexGuardHook: the
// stub replicates the resolve call (absolute, since it's relocated to a
// code cave) then feeds its result into the safety-checked read.
static bool InstallSkeletonHandleGuardHook(unsigned long long hookAddr, unsigned long long resumeAddr,
                                             unsigned long long resolveHandleFnAddr) {
    if (g_skeletonHandleGuardHookInstalled) return true;

    unsigned char* hookPtr = (unsigned char*)(uintptr_t)hookAddr;
    static const unsigned char expected[5] = { 0xE8, 0xC4, 0xAC, 0xFF, 0xFF };
    if (memcmp(hookPtr, expected, 5) != 0) {
        LogDebug("InstallSkeletonHandleGuardHook: unexpected original bytes at hook address, aborting");
        return false;
    }

    void* cave = AllocateNear((void*)(uintptr_t)hookAddr, 4096);
    if (!cave) {
        LogDebug("InstallSkeletonHandleGuardHook: failed to allocate a nearby code cave");
        return false;
    }

    unsigned char stub[64] = {};
    size_t off = 0;
    uint64_t checkFnAddr = (uint64_t)(uintptr_t)&CheckSkeletonHandleDerefSafe;

    stub[off++] = 0x48; stub[off++] = 0xB8; // mov rax, resolveHandleFnAddr
    memcpy(stub + off, &resolveHandleFnAddr, 8); off += 8;
    stub[off++] = 0xFF; stub[off++] = 0xD0; // call rax  (rax <- resolved pointer, same effect as the original call)

    stub[off++] = 0x48; stub[off++] = 0x89; stub[off++] = 0xC1; // mov rcx, rax  (arg1 = resolved pointer)

    stub[off++] = 0x48; stub[off++] = 0x83; stub[off++] = 0xEC; stub[off++] = 0x20; // sub rsp, 0x20

    stub[off++] = 0x48; stub[off++] = 0xB8; // mov rax, checkFnAddr
    memcpy(stub + off, &checkFnAddr, 8); off += 8;

    stub[off++] = 0xFF; stub[off++] = 0xD0; // call rax  (eax <- safe result)

    stub[off++] = 0x48; stub[off++] = 0x83; stub[off++] = 0xC4; stub[off++] = 0x20; // add rsp, 0x20

    stub[off++] = 0x89; stub[off++] = 0xC3; // mov ebx, eax  (replicates "MOV EBX, dword ptr [RAX+0x20]")

    stub[off++] = 0xE9; // jmp rel32 -> resumeAddr (past both replaced instructions)
    size_t jmpOperand = off; off += 4;

    size_t stubLen = off;
    uintptr_t caveBase = (uintptr_t)cave;

    int32_t jmpRel = (int32_t)((int64_t)resumeAddr - (int64_t)(caveBase + jmpOperand + 4));
    memcpy(stub + jmpOperand, &jmpRel, 4);

    unsigned char patch[5];
    patch[0] = 0xE9;
    int32_t hookRel = (int32_t)((int64_t)caveBase - (int64_t)(hookAddr + 5));
    memcpy(patch + 1, &hookRel, 4);

    std::vector<HANDLE> threads = SuspendOtherThreads();

    memcpy(cave, stub, stubLen);

    DWORD oldProtect = 0;
    VirtualProtect((void*)(uintptr_t)hookAddr, 5, PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((void*)(uintptr_t)hookAddr, patch, 5);
    VirtualProtect((void*)(uintptr_t)hookAddr, 5, oldProtect, &oldProtect);

    FlushInstructionCache(GetCurrentProcess(), (void*)(uintptr_t)hookAddr, 5);
    FlushInstructionCache(GetCurrentProcess(), cave, stubLen);

    ResumeThreads(threads);

    g_skeletonHandleGuardHookInstalled = true;
    LogDebug("InstallSkeletonHandleGuardHook: installed successfully");
    return true;
}

// --- SKELETON BLEND CALL GUARD HOOK ---
// SkeletonHandleGuard above fixes ONE specific unsafe read; live testing
// then found a SECOND, different unsafe read (an out-of-bounds array
// index, not a bad handle) elsewhere in the same huge per-entity
// animation/skeleton-blend update function -- confirmed via 3 separate
// live crash repros at 2 different instructions inside its call tree
// (Darkball, Search Ghost, Bouncywild). That function is large and dense
// enough that patching individual unsafe reads doesn't scale -- there's
// no way to be confident we've found the last one.
//
// This hooks the ENTRY of the whole per-entity update function instead
// (called once per active entity per frame from 15+ different call
// sites -- hooking its own entry covers all of them from one place,
// same as how the other guard hooks in this file hook a shared callee's
// entry rather than every caller). Every call to it now goes through a
// __try/__except wrapper, so ANY crash anywhere in its call tree --
// including ones not yet found -- gets caught, and that one entity's
// animation update is skipped for that frame instead of taking the whole
// process down.
//
// Structurally different from the other guard hooks above: those replace
// 1-2 risky instructions with a safe equivalent and continue inline in
// the SAME function. This one can't do that -- SEH protection only
// applies within an actual compiled __try block's stack frame, so simply
// jumping into/out of the middle of unprotected machine code doesn't
// retroactively protect it. Instead this builds TWO trampolines:
//   1. A "replay" cave that replays the original 5 bytes then resumes the
//      real function body at hookAddr+5 -- this is what gets CALLED (as a
//      real subroutine, preserving a normal return address) from inside
//      the __try block below, so the real function's full execution
//      (including everything it calls) happens within SEH's protection.
//   2. A "redirect" cave that the patched entry jumps to, which long-jumps
//      (tail-call, no extra stack frame) into the compiled C++ wrapper
//      below -- this DLL is very likely outside +/-2GB of the game
//      module, so a direct 5-byte rel32 jmp can't reach it.
static bool g_skeletonBlendCallGuardHookInstalled = false;
static volatile uint64_t g_skeletonBlendCallGuardSkipCount = 0;

typedef void (*SkeletonBlendOriginalFn)(int64_t, uint64_t*, float, int64_t);
static SkeletonBlendOriginalFn g_skeletonBlendOriginalTrampoline = nullptr;

// Tail-jumped into from the redirect cave with the exact register state
// (RCX/RDX/XMM2/R9) the real function's 15+ callers set up -- same
// signature as the real function, so the compiler-generated prologue sees
// a normal call-entry stack (return address already on top from whichever
// real caller invoked us).
static void CallSkeletonBlendSafe(int64_t param_1, uint64_t* param_2, float param_3, int64_t param_4) {
    __try {
        g_skeletonBlendOriginalTrampoline(param_1, param_2, param_3, param_4);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        char msg[192];
        snprintf(msg, sizeof(msg),
            "SkeletonBlendCallGuard: exception during per-entity animation update, skipping this entity this frame (skipsSoFar=%llu)",
            (unsigned long long)g_skeletonBlendCallGuardSkipCount + 1);
        LogDebug(msg);
        ++g_skeletonBlendCallGuardSkipCount;
    }
}

static bool InstallSkeletonBlendCallGuardHook(unsigned long long hookAddr) {
    if (g_skeletonBlendCallGuardHookInstalled) return true;

    unsigned char* hookPtr = (unsigned char*)(uintptr_t)hookAddr;
    static const unsigned char expected[5] = { 0x4C, 0x8B, 0xDC, 0x53, 0x56 }; // mov r11,rsp; push rbx; push rsi
    if (memcmp(hookPtr, expected, 5) != 0) {
        LogDebug("InstallSkeletonBlendCallGuardHook: unexpected original bytes at hook address, aborting");
        return false;
    }

    void* replayCave = AllocateNear((void*)(uintptr_t)hookAddr, 4096);
    if (!replayCave) {
        LogDebug("InstallSkeletonBlendCallGuardHook: failed to allocate replay code cave");
        return false;
    }
    {
        unsigned char stub[16] = {};
        size_t off = 0;
        memcpy(stub + off, expected, 5); off += 5; // replay original 5 bytes
        stub[off++] = 0xE9; // jmp rel32 -> hookAddr+5 (rest of the real function body)
        size_t jmpOperand = off; off += 4;
        size_t stubLen = off;
        uintptr_t caveBase = (uintptr_t)replayCave;
        int32_t jmpRel = (int32_t)((int64_t)(hookAddr + 5) - (int64_t)(caveBase + jmpOperand + 4));
        memcpy(stub + jmpOperand, &jmpRel, 4);
        memcpy(replayCave, stub, stubLen);
        FlushInstructionCache(GetCurrentProcess(), replayCave, stubLen);
    }
    g_skeletonBlendOriginalTrampoline = (SkeletonBlendOriginalFn)(uintptr_t)replayCave;

    void* redirectCave = AllocateNear((void*)(uintptr_t)hookAddr, 4096);
    if (!redirectCave) {
        LogDebug("InstallSkeletonBlendCallGuardHook: failed to allocate redirect code cave");
        return false;
    }
    {
        unsigned char stub[16] = {};
        size_t off = 0;
        uint64_t wrapperAddr = (uint64_t)(uintptr_t)&CallSkeletonBlendSafe;
        stub[off++] = 0x48; stub[off++] = 0xB8; // mov rax, wrapperAddr
        memcpy(stub + off, &wrapperAddr, 8); off += 8;
        stub[off++] = 0xFF; stub[off++] = 0xE0; // jmp rax
        size_t stubLen = off;
        memcpy(redirectCave, stub, stubLen);
        FlushInstructionCache(GetCurrentProcess(), redirectCave, stubLen);
    }

    unsigned char patch[5];
    patch[0] = 0xE9;
    int32_t hookRel = (int32_t)((int64_t)(uintptr_t)redirectCave - (int64_t)(hookAddr + 5));
    memcpy(patch + 1, &hookRel, 4);

    std::vector<HANDLE> threads = SuspendOtherThreads();

    DWORD oldProtect = 0;
    VirtualProtect((void*)(uintptr_t)hookAddr, 5, PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((void*)(uintptr_t)hookAddr, patch, 5);
    VirtualProtect((void*)(uintptr_t)hookAddr, 5, oldProtect, &oldProtect);

    FlushInstructionCache(GetCurrentProcess(), (void*)(uintptr_t)hookAddr, 5);

    ResumeThreads(threads);

    g_skeletonBlendCallGuardHookInstalled = true;
    LogDebug("InstallSkeletonBlendCallGuardHook: installed successfully");
    return true;
}

// --- ASYNC LOAD JOB RECORD GUARD HOOK ---
// Fix for a crash/freeze: fnc_load_gimmick_assets queues an async job that
// captures a raw (not copied) pointer into the placement table, which the
// game can recompute on a room transition before the job's callback fires
// -- the callback then dereferences the stale pointer as a species index,
// corrupting loadedSpeciesPtrTable. Hooks the callback's entry and
// validates the job's record pointer against the current placement table
// bounds before letting the original body run; a stale pointer skips the
// callback entirely and returns the "job done" exit code (4).
static bool g_jobRecordGuardHookInstalled = false;
static volatile uint64_t g_jobRecordGuardSkipCount = 0;
static unsigned long long g_jobRecordGuardTablePtrRva = 0;
static unsigned long long g_jobRecordGuardTableCountRva = 0;
static unsigned long long g_jobRecordGuardWorldNumRva = 0;
static unsigned long long g_jobRecordGuardAreaNumRva = 0;
static unsigned long long g_jobRecordGuardSetNumRva = 0;

// __except turns a hardware fault into a clean "treat as stale" instead of
// taking the process down. Re-resolves the current table bounds from the
// live globals on every call.
static int CheckJobRecordPointerSafe(uint64_t jobNodePtr) {
    __try {
        unsigned long long base = (unsigned long long)GetModuleHandleA(nullptr);
        uint8_t* recordPtr = *(uint8_t**)(uintptr_t)(jobNodePtr + 0x20);

        // Only apply the staleness check to a job we ourselves queued
        // (tagged via g_lastQueuedGimmickRecordPtr) -- every other job
        // (this callback is shared, engine-wide infrastructure) is let
        // through untouched, since skipping an unrelated job's completion
        // callback can freeze the game.
        if (recordPtr != (uint8_t*)(uintptr_t)g_lastQueuedGimmickRecordPtr) {
            return 1;
        }

        // Room identity, not placementTablePtr, is the real staleness
        // signal -- l_spawn_enemy's own timeout-refusal path rolls
        // placementTablePtr back for unrelated reasons, which would make a
        // table-bounds check alone false-positive on our own job.
        if (g_lastQueuedGimmickRoomIdentityValid &&
            g_jobRecordGuardWorldNumRva != 0 && g_jobRecordGuardAreaNumRva != 0 && g_jobRecordGuardSetNumRva != 0) {
            int32_t curWorld = *(int32_t*)(uintptr_t)(base + g_jobRecordGuardWorldNumRva);
            int32_t curArea = *(int32_t*)(uintptr_t)(base + g_jobRecordGuardAreaNumRva);
            int32_t curSet = *(int32_t*)(uintptr_t)(base + g_jobRecordGuardSetNumRva);
            if (curWorld != g_lastQueuedGimmickWorld || curArea != g_lastQueuedGimmickArea ||
                curSet != g_lastQueuedGimmickSet) {
                char msg[192];
                snprintf(msg, sizeof(msg),
                    "JobRecordGuard: room changed since this job was queued (was %d/%d/%d, now %d/%d/%d) -- genuinely stale, skipping (skipsSoFar=%llu)",
                    g_lastQueuedGimmickWorld, g_lastQueuedGimmickArea, g_lastQueuedGimmickSet,
                    curWorld, curArea, curSet, (unsigned long long)g_jobRecordGuardSkipCount + 1);
                LogDebug(msg);
                ++g_jobRecordGuardSkipCount;
                return 0;
            }
            // Same room the whole time -- still valid.
            return 1;
        }

        // Fallback for a build where room-identity RVAs aren't wired up
        // yet: the older, more conservative table-bounds heuristic.
        uint8_t* tableStart = *(uint8_t**)(uintptr_t)(base + g_jobRecordGuardTablePtrRva);
        int32_t tableCount = *(int32_t*)(uintptr_t)(base + g_jobRecordGuardTableCountRva);
        if (tableCount <= 0 || tableCount > 4096 || tableStart == nullptr) {
            ++g_jobRecordGuardSkipCount;
            LogDebug("JobRecordGuard: current placement table looks invalid, skipping job as stale");
            return 0;
        }
        uint8_t* tableEnd = tableStart + (size_t)tableCount * PLACEMENT_RECORD_SIZE;
        if (recordPtr < tableStart || recordPtr >= tableEnd ||
            (size_t)(recordPtr - tableStart) % PLACEMENT_RECORD_SIZE != 0) {
            char msg[192];
            snprintf(msg, sizeof(msg),
                "JobRecordGuard: job's record ptr=0x%llX outside current placement table [0x%llX, 0x%llX) -- stale, skipping job (skipsSoFar=%llu)",
                (unsigned long long)recordPtr, (unsigned long long)tableStart, (unsigned long long)tableEnd,
                (unsigned long long)g_jobRecordGuardSkipCount + 1);
            LogDebug(msg);
            ++g_jobRecordGuardSkipCount;
            return 0;
        }
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LogDebug("JobRecordGuard: exception probing job/table pointers, skipping job as stale");
        ++g_jobRecordGuardSkipCount;
        return 0;
    }
}

// Hooks the function's entry (expected: 5-byte "mov qword ptr [rsp+8],
// rbx"), redirecting through CheckJobRecordPointerSafe.
static bool InstallJobRecordGuardHook(unsigned long long hookAddr, unsigned long long resumeAddr,
                                        unsigned long long tablePtrRva, unsigned long long tableCountRva,
                                        unsigned long long worldNumRva, unsigned long long areaNumRva,
                                        unsigned long long setNumRva) {
    if (g_jobRecordGuardHookInstalled) return true;

    g_jobRecordGuardTablePtrRva = tablePtrRva;
    g_jobRecordGuardTableCountRva = tableCountRva;
    g_jobRecordGuardWorldNumRva = worldNumRva;
    g_jobRecordGuardAreaNumRva = areaNumRva;
    g_jobRecordGuardSetNumRva = setNumRva;

    unsigned char* hookPtr = (unsigned char*)(uintptr_t)hookAddr;
    static const unsigned char expected[5] = { 0x48, 0x89, 0x5C, 0x24, 0x08 };
    if (memcmp(hookPtr, expected, 5) != 0) {
        LogDebug("InstallJobRecordGuardHook: unexpected original bytes at hook address, aborting");
        return false;
    }

    void* cave = AllocateNear((void*)(uintptr_t)hookAddr, 4096);
    if (!cave) {
        LogDebug("InstallJobRecordGuardHook: failed to allocate a nearby code cave");
        return false;
    }

    unsigned char stub[96] = {};
    size_t off = 0;
    uint64_t checkFnAddr = (uint64_t)(uintptr_t)&CheckJobRecordPointerSafe;

    stub[off++] = 0x51; // push rcx

    stub[off++] = 0x48; stub[off++] = 0x83; stub[off++] = 0xEC; stub[off++] = 0x20; // sub rsp, 0x20

    stub[off++] = 0x48; stub[off++] = 0xB8; // mov rax, checkFnAddr
    memcpy(stub + off, &checkFnAddr, 8); off += 8;

    stub[off++] = 0xFF; stub[off++] = 0xD0; // call rax

    stub[off++] = 0x48; stub[off++] = 0x83; stub[off++] = 0xC4; stub[off++] = 0x20; // add rsp, 0x20

    stub[off++] = 0x85; stub[off++] = 0xC0; // test eax, eax

    stub[off++] = 0x59; // pop rcx

    stub[off++] = 0x0F; stub[off++] = 0x84; // jz rel32 (patched below) -> skipCall
    size_t jzOperand = off; off += 4;

    memcpy(stub + off, hookPtr, 5); off += 5; // replay original mov [rsp+8],rbx

    stub[off++] = 0xE9; // jmp rel32 (patched below) -> resumeAddr
    size_t jmpBackOperand = off; off += 4;

    size_t skipCallPos = off;
    stub[off++] = 0xB8; // mov eax, 4
    uint32_t four = 4;
    memcpy(stub + off, &four, 4); off += 4;
    stub[off++] = 0xC3; // ret

    size_t stubLen = off;
    uintptr_t caveBase = (uintptr_t)cave;

    int32_t jzRel = (int32_t)((int64_t)skipCallPos - (int64_t)(jzOperand + 4));
    memcpy(stub + jzOperand, &jzRel, 4);

    int32_t jmpBackRel = (int32_t)((int64_t)resumeAddr - (int64_t)(caveBase + jmpBackOperand + 4));
    memcpy(stub + jmpBackOperand, &jmpBackRel, 4);

    unsigned char patch[5];
    patch[0] = 0xE9;
    int32_t hookRel = (int32_t)((int64_t)caveBase - (int64_t)(hookAddr + 5));
    memcpy(patch + 1, &hookRel, 4);

    std::vector<HANDLE> threads = SuspendOtherThreads();

    memcpy(cave, stub, stubLen);

    DWORD oldProtect = 0;
    VirtualProtect((void*)(uintptr_t)hookAddr, 5, PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((void*)(uintptr_t)hookAddr, patch, 5);
    VirtualProtect((void*)(uintptr_t)hookAddr, 5, oldProtect, &oldProtect);

    FlushInstructionCache(GetCurrentProcess(), (void*)(uintptr_t)hookAddr, 5);
    FlushInstructionCache(GetCurrentProcess(), cave, stubLen);

    ResumeThreads(threads);

    g_jobRecordGuardHookInstalled = true;
    LogDebug("InstallJobRecordGuardHook: installed successfully");
    return true;
}

// install_job_record_guard_hook(hookRva, resumeRva, tablePtrRva, tableCountRva,
//     worldNumRva, areaNumRva, setNumRva) -> ok(boolean)
// Idempotent. hookRva should be the async load callback's entry, resumeRva
// its entry+5. Pass 0 for worldNumRva/areaNumRva/setNumRva to force the
// table-bounds fallback.
extern "C" int l_install_job_record_guard_hook(void* L) {
    unsigned long long base = (unsigned long long)GetModuleHandleA(nullptr);
    unsigned long long hookRva = (unsigned long long)p_lua_tointegerx(L, 1, nullptr);
    unsigned long long resumeRva = (unsigned long long)p_lua_tointegerx(L, 2, nullptr);
    unsigned long long tablePtrRva = (unsigned long long)p_lua_tointegerx(L, 3, nullptr);
    unsigned long long tableCountRva = (unsigned long long)p_lua_tointegerx(L, 4, nullptr);
    unsigned long long worldNumRva = (unsigned long long)p_lua_tointegerx(L, 5, nullptr);
    unsigned long long areaNumRva = (unsigned long long)p_lua_tointegerx(L, 6, nullptr);
    unsigned long long setNumRva = (unsigned long long)p_lua_tointegerx(L, 7, nullptr);
    bool ok = InstallJobRecordGuardHook(base + hookRva, base + resumeRva, tablePtrRva, tableCountRva,
                                          worldNumRva, areaNumRva, setNumRva);
    p_lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

// get_job_record_guard_skip_count() -> count(integer)
// Diagnostic readback for live testing -- how many times the guard has
// skipped a job this session because its captured record pointer had
// fallen outside the current placement table.
extern "C" int l_get_job_record_guard_skip_count(void* L) {
    p_lua_pushinteger(L, (long long)g_jobRecordGuardSkipCount);
    return 1;
}

// Table of every function we expose to lua,
// registered all at once via luaL_setfuncs.
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
    {"install_velocity_blend_guard_hook", reinterpret_cast<void*>(l_install_velocity_blend_guard_hook)},
    {"get_velocity_blend_guard_skip_count", reinterpret_cast<void*>(l_get_velocity_blend_guard_skip_count)},
    {"install_job_record_guard_hook", reinterpret_cast<void*>(l_install_job_record_guard_hook)},
    {"get_job_record_guard_skip_count", reinterpret_cast<void*>(l_get_job_record_guard_skip_count)},
    {nullptr, nullptr}
};

// Exports we need to find on whichever module
// turns out to be the real lua54 module.
static const char* const kRequiredLuaExports[] = {
    "lua_gettop", "lua_tointegerx", "lua_tonumberx", "lua_tolstring", "lua_pushinteger",
    "lua_pushboolean", "lua_pushstring", "luaL_setfuncs", "lua_createtable",
    "lua_rawlen", "lua_rawgeti", "lua_settop",
};

// Checks whether a given module exports every
// lua function we need.
static bool ModuleExportsAllRequired(HMODULE mod) {
    if (!mod) return false;
    for (const char* name : kRequiredLuaExports) {
        if (!GetProcAddress(mod, name)) return false;
    }
    return true;
}

// Fallback: walks every loaded module in the
// process looking for one that exports the
// lua API, in case lua54.dll isn't the name
// actually used.
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

// Tries the expected module name first, then
// falls back to scanning the process if that
// fails.
static HMODULE FindLuaModule() {
    HMODULE bundled = GetModuleHandleA("lua54.dll");
    if (ModuleExportsAllRequired(bundled)) {
        LogDebug("FindLuaModule: resolved via bundled dll/lua54.dll");
        return bundled;
    }

    LogDebug("FindLuaModule: bundled lua54.dll not found or incomplete, falling back to process scan");
    return FindLuaModuleByProcessScan();
}

// Entry point lua calls when it requires this
// module. Resolves the real lua function
// pointers, then registers our functions.
extern "C" __declspec(dllexport) int luaopen_kh1_native(void* L) {
    LogDebug("luaopen_kh1_native called");

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

    if (!p_lua_gettop || !p_lua_tointegerx || !p_lua_tonumberx || !p_lua_tolstring || !p_lua_pushinteger || !p_lua_pushboolean ||
        !p_lua_pushstring || !p_luaL_setfuncs || !p_lua_createtable ||
        !p_lua_rawlen || !p_lua_rawgeti || !p_lua_settop) {
        LogDebug("luaopen_kh1_native: failed to resolve Lua API exports, aborting safely");
        return 0;
    }

    p_lua_createtable(L, 0, 2);
    p_luaL_setfuncs(L, kh1_native_lib, 0);
    return 1;
}

// Standard dll entry point. On attach, figures
// out our own directory (for logging) and loads
// ourselves again by full path so we stay pinned
// in memory rather than getting unloaded.
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        GetModuleFileNameA(hModule, g_dllDir, MAX_PATH);
        char* last = strrchr(g_dllDir, '\\');
        if (last) *(last + 1) = '\0';
        char selfPath[MAX_PATH];
        GetModuleFileNameA(hModule, selfPath, MAX_PATH);
        LoadLibraryA(selfPath);
    } else if (reason == DLL_PROCESS_DETACH) {
        // Best-effort diagnostic only -- lpReserved != NULL means the process
        // itself is terminating (not a plain FreeLibrary), and Microsoft
        // doesn't guarantee CRT/Kernel32 calls are safe at that point.
        // Logging anyway since it's the only signal available for a crash
        // that leaves no Windows Event Log entry and no LogDebug line from
        // anywhere else in this file (2026-08-04, Sniperwild cold-spawn
        // instant-vanish incident) -- if this line shows up right before
        // such a crash, the process was still unwinding normally, which
        // rules out a fail-fast/stack-corruption kill; if it never appears,
        // teardown never ran at all.
        LogDebug(lpReserved ? "DllMain: DLL_PROCESS_DETACH (process terminating)" : "DllMain: DLL_PROCESS_DETACH (FreeLibrary)");
    }
    return TRUE;
}
