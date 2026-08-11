#include "pch.h"
#include <tlhelp32.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>
#include <intrin.h> // _AddressOfReturnAddress, for the bad-handle caller diagnostic

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
// Slot+0 and slot+1 (i.e. ptr-0x48 / ptr-0x47, since the state byte at slot+3 is ptr-0x45):
// FUN_140286190 stamps OWNER=species into slot+0 and RUN LENGTH=record[0x56] into slot+1 for EVERY
// slot in a claimed run. Critically it does NOT touch the state byte at slot+3, which only the
// primary slot's own load progression sets -- so a slot claimed as a run MEMBER still reads
// state==0 and looks completely free. Live-proved 2026-08-10: after a creature claimed a run of 4
// starting at 24, the next two spawns were handed 25 and 26 (both inside that run) and the game
// crashed. Occupancy therefore has to test the RUN-LENGTH byte, not just the state byte. It is a
// reliable signal because this table is BSS (zero-initialised) and FUN_140286190 only ever writes a
// non-zero count, so slot+1 != 0 means "claimed by somebody's run".
static const int LOADED_SPECIES_OWNER_OFFSET_FROM_PTR = -0x48;
static const int LOADED_SPECIES_RUNLEN_OFFSET_FROM_PTR = -0x47;
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
// Upper bound is SPECIES_SLOT_ALLOC_MAX, not RESOURCE_BLOB_MAX_SPECIES (2026-08-10): the comment
// above has always said slots ~48-64 are avoided as hardcoded-reserved by unrelated engine
// subsystems, but the loop bound never actually implemented that -- it ran to 64 inclusive and
// would hand out a reserved slot as soon as 20..47 were all busy. Found by reading, NOT the cause
// of the bogus-handle crashes being investigated 2026-08-10 (those pick slot 20, well inside the
// intended range) -- fixed here because it's a real latent bug, not because it fixes those.
// RESOURCE_BLOB_MAX_SPECIES stays 64: that IS the true table extent, and the resource-blob bounds
// check elsewhere still needs it. This is a narrower *allocation* policy on top of it.
static const int SPECIES_SLOT_ALLOC_MAX = 47;

// slotRunLen (2026-08-10): the loader claims a RUN of consecutive slots, not one slot --
// fnc_async_load_job_callback passes record+0x56 to FUN_140286190, which stamps owner/run bytes
// into slotRunLen consecutive 0x50-stride entries starting at the chosen species. Live-confirmed
// this evening: the crashing creature's record+0x56 is 5, so picking slot 20 was really claiming
// slots 20-24, and slot 21 was occupied by a live creature -- silently corrupting its resource
// state, which is what produced the deterministic bogus float-shaped resource handles this whole
// investigation was chasing. Searching for a single free slot was never sufficient; the ENTIRE run
// has to be free, which is what this now checks.
static bool FindFreeLoadedSlot(unsigned long long base, unsigned long long loadedPtrTableRva, const bool* speciesInUseByLiveEntity, int slotRunLen, uint8_t* outSpecies) {
    if (slotRunLen < 1) slotRunLen = 1; // a signed-char <= 0 makes FUN_140286190 claim nothing
    for (int s = 20; s + slotRunLen - 1 <= SPECIES_SLOT_ALLOC_MAX; ++s) {
        bool runFree = true;
        for (int k = 0; k < slotRunLen; ++k) {
            int slot = s + k;
            if (slot > RESOURCE_BLOB_MAX_SPECIES) { runFree = false; break; }
            if (speciesInUseByLiveEntity && speciesInUseByLiveEntity[slot]) { runFree = false; break; }
            if (IsSpeciesTriggeredThisSession((uint8_t)slot)) { runFree = false; break; }
            volatile uint8_t* stateAddr = (volatile uint8_t*)(uintptr_t)(base + loadedPtrTableRva + LOADED_SPECIES_STATE_OFFSET_FROM_PTR + (size_t)slot * LOADED_SPECIES_STRIDE);
            if (*stateAddr != 0) { runFree = false; break; }
            // Also reject a slot already claimed as a MEMBER of somebody else's run -- those keep
            // state==0 and are invisible to the check above. See the offset constants' comment.
            volatile uint8_t* runLenAddr = (volatile uint8_t*)(uintptr_t)(base + loadedPtrTableRva + LOADED_SPECIES_RUNLEN_OFFSET_FROM_PTR + (size_t)slot * LOADED_SPECIES_STRIDE);
            if (*runLenAddr != 0) { runFree = false; break; }
        }
        if (runFree) {
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
static bool InstallKeyframeListEntryParamGuardHook(unsigned long long hookAddr, unsigned long long resumeAddr);
static bool InstallAnimBlendAdvanceDiagHook(unsigned long long hookAddr, unsigned long long resumeAddr,
                                              unsigned long long resolveHandleFnAddr);
static bool InstallSection2SizeGuardHook(unsigned long long hookAddr, unsigned long long resumeAddr,
                                           unsigned long long emptyPathAddr);
static bool InstallResolveHandleBucketGuardHook(unsigned long long hookAddr, unsigned long long resumeAddr);
static bool InstallBucketMemoryWatcherThread(unsigned long long bucketTableAddr);
static bool InstallBehaviorScriptCopyGuardHook(unsigned long long hookAddr, unsigned long long resumeAddr);
static bool InstallJointRemapSetupGuardHook(unsigned long long hookAddr);
static bool InstallFreezeWatchdogThread();
// Live address of the 64-entry resource-handle bucket table, stashed by l_spawn_enemy's headroom
// check so LogResolveHandleBucketGuard (which runs from a code cave and can't be handed it) can
// report how full the table actually was at the moment of a bad-handle hit -- the single most
// important number for telling a genuine exhaustion apart from the unrelated bad-handle bug.
// 0 if this build has no bucket-table RVA configured.
static volatile uint64_t g_resourceHandleBucketTableAddr = 0;
static uint64_t g_textSlotTableBase = 0;
static uint64_t g_resolveHandleFnAddrForTextSlot = 0;
static bool InstallTextSlotHandleCaptureHook(unsigned long long hookAddr, unsigned long long resumeAddr);
static bool InstallTextSlotHandleRecordHook(unsigned long long hookAddr, unsigned long long resumeAddr);
static bool InstallTextSlotFreshResolveHook(unsigned long long hookAddr, unsigned long long resumeAddr);

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
    // Entry point of fnc_keyframe_list_lookup itself (the risky
    // record+0x3C read at the function's own +0x15, BEFORE
    // InstallKeyframeListEntryGuardHook's own patch point at +0x2F -- see
    // InstallKeyframeListEntryParamGuardHook's own comment). Optional, same
    // reasoning as the other animation-subsystem hook RVAs above.
    unsigned long long keyframeListEntryParamHookFnRva = (unsigned long long)p_lua_tointegerx(L, 32, nullptr);
    // Entry point of "MOV RBX,RAX" + "CALL fnc_resolve_resource_handle" inside
    // fnc_entity_animation_blend_advance, right after it computes the value
    // that (when bad) eventually becomes the -1 fed into
    // fnc_keyframe_list_lookup -- see InstallAnimBlendAdvanceDiagHook's own
    // comment. Diagnostic-only (doesn't fix anything, just logs). Optional.
    unsigned long long animBlendAdvanceDiagHookFnRva = (unsigned long long)p_lua_tointegerx(L, 33, nullptr);
    // Entry point of the section-2 size check inside fnc_link_model_resource_data
    // (the "MOV EAX,[RBP+0xC]" through "JLE" sequence right before the copy into
    // entity+0x1D4) -- see InstallSection2SizeGuardHook's own comment. This is a
    // REAL FIX, not a catch-after-the-fact guard: it corrects the game's own
    // "section has data" check from "size > 0" to "size >= 0x34" (the minimum
    // needed to safely read the dword at section+0x30 that becomes entity+0x1D4).
    // Optional, same degrade-gracefully convention as the other hook RVAs above.
    unsigned long long section2SizeGuardHookFnRva = (unsigned long long)p_lua_tointegerx(L, 34, nullptr);
    // Entry point of fnc_resolve_resource_handle_impl's own bucket-table lookup
    // (the "SHR RAX,0x19" through "OR RAX,RCX" sequence) -- see
    // InstallResolveHandleBucketGuardHook's own comment. THE master fix: the
    // resource-handle system has a hard 64-bucket ceiling, shared across the
    // whole game and never freed -- once exhausted (confirmed live 2026-08-05,
    // heavy Crowd Control spawn testing), EVERY caller of
    // fnc_resolve_resource_handle anywhere in the game can get back a raw -1
    // "pointer" with no way to detect it, which is almost certainly the real
    // root cause behind crash sites #1-#6 and the unrelated text-rendering UAF
    // found the same session -- all of them were symptoms of THIS. Fixing it
    // here protects every caller at once, not just the handful of call sites
    // this file has individually hooked so far. Optional, same
    // degrade-gracefully convention as the other hook RVAs above.
    unsigned long long resolveHandleBucketGuardHookFnRva = (unsigned long long)p_lua_tointegerx(L, 35, nullptr);
    // Status-effect floating-text UAF fix (2026-08-05) -- see
    // InstallTextSlotHandleCaptureHook/InstallTextSlotHandleRecordHook/
    // InstallTextSlotFreshResolveHook's own comments. Three coordinated hooks:
    // capture the ORIGINAL handle inside fnc_status_effect_activate_by_type
    // before it gets resolved and cached as a raw pointer, record it into a
    // per-slot side table, then have the one CONFIRMED crash read-site
    // (FUN_1401eba70) re-resolve fresh from that handle instead of trusting the
    // long-cached raw pointer. All optional -- degrade gracefully (falls back to
    // the original cached-pointer behavior) if unconfigured.
    unsigned long long textSlotHandleCaptureHookFnRva = (unsigned long long)p_lua_tointegerx(L, 36, nullptr);
    unsigned long long textSlotHandleRecordHookFnRva = (unsigned long long)p_lua_tointegerx(L, 37, nullptr);
    unsigned long long textSlotFreshResolveHookFnRva = (unsigned long long)p_lua_tointegerx(L, 38, nullptr);
    unsigned long long textSlotTableBaseRva = (unsigned long long)p_lua_tointegerx(L, 39, nullptr);
    // Base of the 64-entry resource-handle bucket table itself (g_apResourceHandleBuckets /
    // DAT_142ee3980 in Ghidra). Originally added for InstallBucketMemoryWatcherThread
    // (reverted the same day for a false-positive bug -- see its own comment), now used
    // for a much simpler read-only headroom check below, right after `base` is computed.
    // Optional, same degrade-gracefully convention as the other hook RVAs above.
    unsigned long long resourceHandleBucketTableRva = (unsigned long long)p_lua_tointegerx(L, 40, nullptr);
    // Entry point of the memcpy call inside KH1_BehaviorScriptInterpreter's
    // class-0/sub-op-0xA handler (see InstallBehaviorScriptCopyGuardHook's own
    // comment) -- fixes crash site #6, a resolved-but-invalid handle used as a
    // memcpy source with zero validation. Optional, same degrade-gracefully
    // convention as the other hook RVAs above.
    unsigned long long behaviorScriptCopyGuardHookFnRva = (unsigned long long)p_lua_tointegerx(L, 41, nullptr);
    // Entry point of FUN_1402a0350 (see InstallJointRemapSetupGuardHook's own
    // comment) -- fixes crash site #7, a null deref inside a joint/bone
    // remap-table builder reached on a fresh motion transition. Optional,
    // same degrade-gracefully convention as the other hook RVAs above.
    unsigned long long jointRemapSetupGuardHookFnRva = (unsigned long long)p_lua_tointegerx(L, 42, nullptr);

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

    // Resource-handle bucket-table headroom check (2026-08-10 follow-up). Live
    // testing this evening found THREE more, completely unrelated call sites
    // (crash sites #6/#7/#8, spread across a behavior-script interpreter, a
    // joint-remap builder, and a keyframe lookup) that all share the same shape:
    // fnc_resolve_resource_handle returns 0 once the shared 64-entry bucket table
    // is exhausted (the 2026-08-05 fix's own intended behavior), and the caller
    // dereferences the result with zero validation. Patching each one as it's
    // found isn't converging -- there are evidently many more of these scattered
    // through the animation/keyframe system than can be hunted in one evening.
    // Rather than keep chasing symptoms, refuse to spawn once the table is
    // getting full, BEFORE the game's own code has a chance to hit real
    // exhaustion from spawn_enemy's own churn -- the actual trigger every crash
    // tonight traced back to. This is a pure read-only count of non-(-1)
    // entries, nothing is written -- unlike the reverted watcher thread
    // (see InstallBucketMemoryWatcherThread's own comment), there is no
    // false-positive risk here, since correctness only depends on the sentinel
    // value, not on guessing whether an address is still valid memory.
    // NOTE: this only prevents spawn_enemy itself from being the thing that
    // pushes the table over the edge -- it can't undo exhaustion already caused
    // by other gameplay, and other native-content resource claims could still
    // exhaust it independently. Harm reduction for the demonstrated trigger, not
    // a complete fix for the underlying scattered-unguarded-callers problem.
    if (resourceHandleBucketTableRva != 0) {
        const int kBucketCount = 64;
        const int kBucketSafeHeadroom = 6; // refuse once fewer than this many buckets remain free
        volatile int64_t* bucketTable = (volatile int64_t*)(uintptr_t)(base + resourceHandleBucketTableRva);
        // Stash it for LogResolveHandleBucketGuard, which runs from a code cave with no way to be
        // handed this address itself but badly needs it to report real table fullness on a hit.
        g_resourceHandleBucketTableAddr = (uint64_t)(uintptr_t)bucketTable;
        int claimed = 0;
        for (int i = 0; i < kBucketCount; ++i) {
            if (bucketTable[i] != -1) ++claimed;
        }
        if (claimed >= kBucketCount - kBucketSafeHeadroom) {
            char msg[160];
            snprintf(msg, sizeof(msg),
                "spawn_enemy: refusing -- resource-handle bucket table nearly full (%d/%d claimed), spawning more risks a downstream crash",
                claimed, kBucketCount);
            LogDebug(msg);
            p_lua_pushboolean(L, 0);
            p_lua_pushstring(L, "spawn_enemy: too many distinct resources loaded this session (bucket table nearly full) -- refusing to risk a crash");
            return 2;
        }
    }

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
    // How many consecutive species slots this creature's asset load will claim (record+0x56, read
    // through a signed char exactly as the game does). Needed BEFORE slot selection so we can find
    // a free RUN instead of a single free slot -- see FindFreeLoadedSlot's own comment.
    const int templateSlotRunLen = (int)(int8_t)templateRec[0x56];

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
    if (keyframeListEntryParamHookFnRva != 0) {
        if (!InstallKeyframeListEntryParamGuardHook(base + keyframeListEntryParamHookFnRva, base + keyframeListEntryParamHookFnRva + 5)) {
            LogDebug("spawn_enemy: InstallKeyframeListEntryParamGuardHook failed to install -- proceeding without it");
        }
    }
    // DISABLED 2026-08-10, the same evening it was written -- it FALSE-POSITIVED constantly,
    // reporting "the game appears FROZEN" every few seconds during completely normal play and
    // flooding kh1_native.log badly enough to bury the spawn diagnostics we actually rely on.
    //
    // Why it was wrong: GetThreadContext on a RUNNING thread returns undefined/stale data (MSDN is
    // explicit that the thread must be suspended for a valid context). In practice it returns the
    // thread's last kernel-entry address, which does NOT change while the thread is busy -- so the
    // detector's core premise ("a thread executing exe code shows a changing RIP") is simply false
    // without suspending. Every sampled RIP sat in an unchanging ntdll wait, so "no exe-code thread
    // made progress" was always true. The deliberate choice NOT to suspend threads (made to avoid
    // deadlock risk) is exactly what made the measurement meaningless.
    //
    // This is the SAME failure shape as InstallBucketMemoryWatcherThread (reverted 2026-08-05): an
    // off-hot-path checker that validates the wrong thing because the value it needs isn't
    // available where it's looking. Left in the file, inert, per this file's convention -- a v2
    // needs a real per-frame heartbeat (a counter incremented from a hooked per-frame function) for
    // DETECTION, and should only suspend/sample thread contexts once that heartbeat says the loop
    // has actually stopped. Do not re-enable as-is.
    // InstallFreezeWatchdogThread();
    (void)&InstallFreezeWatchdogThread;
    if (animBlendAdvanceDiagHookFnRva != 0) {
        if (!InstallAnimBlendAdvanceDiagHook(base + animBlendAdvanceDiagHookFnRva, base + animBlendAdvanceDiagHookFnRva + 8, base + resolveHandleFnRva)) {
            LogDebug("spawn_enemy: InstallAnimBlendAdvanceDiagHook failed to install -- proceeding without it");
        }
    }
    if (section2SizeGuardHookFnRva != 0) {
        // resumeAddr = hookAddr+9 ("ADD RCX,RBP", continuing the "has real data" path);
        // emptyPathAddr = hookAddr+0x2F ("XOR ECX,ECX" empty-section path). Both fixed
        // offsets from this exact build's disassembly -- see the hook's own comment.
        if (!InstallSection2SizeGuardHook(base + section2SizeGuardHookFnRva, base + section2SizeGuardHookFnRva + 9, base + section2SizeGuardHookFnRva + 0x2F)) {
            LogDebug("spawn_enemy: InstallSection2SizeGuardHook failed to install -- proceeding without it");
        }
    }
    if (resolveHandleBucketGuardHookFnRva != 0) {
        if (!InstallResolveHandleBucketGuardHook(base + resolveHandleBucketGuardHookFnRva, base + resolveHandleBucketGuardHookFnRva + 17)) {
            LogDebug("spawn_enemy: InstallResolveHandleBucketGuardHook failed to install -- proceeding without it");
        }
    }
    // DISABLED 2026-08-05, same day it was added -- live-crashed the game on its very
    // first sweep. Root cause: the check below validates the raw BUCKET BASE (the
    // handle's pointer with its low 25 bits truncated to 0), not a real resolved
    // address -- that truncated base is essentially never itself the start of a real
    // allocation (allocations don't generally start on exact 32MB boundaries), so
    // VirtualQuery on it routinely reports unmapped/free even for a perfectly valid,
    // heavily-used bucket. Confirmed live: bucket 0 (base 0x7FF790000000, the 32MB
    // truncation of the game's own exe module base 0x7FF790BE0000 that session) was
    // invalidated in the very first sweep, immediately after install -- collateral
    // damage from that alone (every future resolve of any handle in that whole 32MB
    // region silently degrading to "no data") is a very plausible explanation for the
    // crash that followed. ValidateResolvedBucketAddress above avoided this because it
    // validates the actual resolved base|offset pointer at real resolve time, which
    // this off-hot-path design never has access to (only the bucket table, not
    // individual handles). Left installed-but-inert (never called) rather than deleted,
    // matching this file's existing convention for a tried-and-reverted approach -- do
    // not re-enable without a real fix for what to actually validate.
    (void)resourceHandleBucketTableRva;
    if (behaviorScriptCopyGuardHookFnRva != 0) {
        if (!InstallBehaviorScriptCopyGuardHook(base + behaviorScriptCopyGuardHookFnRva, base + behaviorScriptCopyGuardHookFnRva + 5)) {
            LogDebug("spawn_enemy: InstallBehaviorScriptCopyGuardHook failed to install -- proceeding without it");
        }
    }
    if (jointRemapSetupGuardHookFnRva != 0) {
        if (!InstallJointRemapSetupGuardHook(base + jointRemapSetupGuardHookFnRva)) {
            LogDebug("spawn_enemy: InstallJointRemapSetupGuardHook failed to install -- proceeding without it");
        }
    }
    if (textSlotTableBaseRva != 0) {
        g_textSlotTableBase = base + textSlotTableBaseRva;
    }
    if (resolveHandleFnRva != 0) {
        g_resolveHandleFnAddrForTextSlot = base + resolveHandleFnRva;
    }
    if (textSlotHandleCaptureHookFnRva != 0) {
        if (!InstallTextSlotHandleCaptureHook(base + textSlotHandleCaptureHookFnRva, base + textSlotHandleCaptureHookFnRva + 7)) {
            LogDebug("spawn_enemy: InstallTextSlotHandleCaptureHook failed to install -- proceeding without it");
        }
    }
    if (textSlotHandleRecordHookFnRva != 0) {
        if (!InstallTextSlotHandleRecordHook(base + textSlotHandleRecordHookFnRva, base + textSlotHandleRecordHookFnRva + 7)) {
            LogDebug("spawn_enemy: InstallTextSlotHandleRecordHook failed to install -- proceeding without it");
        }
    }
    if (textSlotFreshResolveHookFnRva != 0) {
        if (!InstallTextSlotFreshResolveHook(base + textSlotFreshResolveHookFnRva, base + textSlotFreshResolveHookFnRva + 7)) {
            LogDebug("spawn_enemy: InstallTextSlotFreshResolveHook failed to install -- proceeding without it");
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
    } else if (FindFreeLoadedSlot(base, loadedPtrTableRva, speciesInUseByLiveEntity, templateSlotRunLen, &species)) {
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
        // --- SLOT-RUN SAFETY CHECK (2026-08-10) ---
        // fnc_async_load_job_callback calls FUN_140286190(species, (int)(signed char)record[0x56]),
        // and that function claims record[0x56] CONSECUTIVE species slots -- writing owner=species
        // and runLen=count into slot[species], slot[species+1], ... each 0x50 apart. So triggering
        // a load does NOT touch just the one slot we carefully verified free: it can stomp the
        // owner/run bytes of however many slots follow, which live creatures may be using right
        // now. FindFreeLoadedSlot/RoomHasNativeSpecies only ever validated the FIRST slot, so this
        // was completely unchecked. record+0x56 is inherited unsanitized from the cloned fallback
        // template -- unlike +0x55 (species), +0x4c (charId) and +0x59 (weight), which we force.
        //
        // This is the concrete mechanism behind session 14's long-standing theory that the load
        // path "force-resets a room-shared, session-global per-slot state with no check for whether
        // some other currently-alive entity depends on that slot right now", and it plausibly
        // explains the bogus float-shaped resource handles being chased 2026-08-10: clobbering a
        // live species' slot bytes leaves its resource table inconsistent, which sends
        // fnc_resource_entry_lookup_with_fallback down its silent header-slice fallback path.
        //
        // Refuse rather than trigger a load that would clobber an occupied slot. Note the game
        // casts +0x56 through a SIGNED char, so a value >= 0x80 goes negative and FUN_140286190
        // claims nothing at all -- only 1..127 actually claim a run.
        int slotRunLen = (int)(int8_t)newRec[0x56];
        {
            char msg[224];
            snprintf(msg, sizeof(msg),
                "spawn_enemy: load-trigger species=%d, record+0x56 slot-run length = %d (the loader "
                "will claim that many CONSECUTIVE species slots starting at %d)",
                species, slotRunLen, species);
            LogDebug(msg);
        }
        if (slotRunLen > 1) {
            int conflictSlot = -1;
            for (int s = species + 1; s < species + slotRunLen; ++s) {
                if (s > RESOURCE_BLOB_MAX_SPECIES) { conflictSlot = s; break; }
                volatile uint8_t* runStateAddr = (volatile uint8_t*)(uintptr_t)(base + loadedPtrTableRva +
                    LOADED_SPECIES_STATE_OFFSET_FROM_PTR + (size_t)s * LOADED_SPECIES_STRIDE);
                if (*runStateAddr != 0) { conflictSlot = s; break; }
                // Same run-member blind spot as FindFreeLoadedSlot had: a slot claimed as part of
                // another creature's run keeps state==0, so the run-length byte must be tested too.
                volatile uint8_t* runLenAddr = (volatile uint8_t*)(uintptr_t)(base + loadedPtrTableRva +
                    LOADED_SPECIES_RUNLEN_OFFSET_FROM_PTR + (size_t)s * LOADED_SPECIES_STRIDE);
                if (*runLenAddr != 0) { conflictSlot = s; break; }
            }
            if (conflictSlot >= 0) {
                char msg[256];
                snprintf(msg, sizeof(msg),
                    "spawn_enemy: refusing -- loading species=%d would claim %d consecutive slots "
                    "and slot %d is already in use (or out of range); triggering this load would "
                    "corrupt a live creature's resource state",
                    species, slotRunLen, conflictSlot);
                LogDebug(msg);
                *tablePtrAddr = oldTable;
                *tableCountAddr = oldCount;
                p_lua_pushboolean(L, 0);
                p_lua_pushstring(L, "spawn_enemy: this creature's asset load would claim several consecutive species slots and one of them is already in use -- refusing to avoid corrupting a live creature");
                return 2;
            }
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

// --- FREEZE WATCHDOG (2026-08-10) ---
// Diagnoses the silent freeze that happens a few seconds after DEFEATING a spawned creature (see
// the memory file project_spawn_enemy_cold_spawn_crash_containment.md). That freeze produces ZERO
// log output from every existing guard hook, so nothing currently in this file can see it.
//
// Why in-process instead of a debugger: Cheat Engine's MCP bridge cannot sample per-thread
// contexts -- debug_break_thread(tid) followed by debug_get_context() returns the same cached
// context no matter which thread was named (verified live 2026-08-10 on two different threads), and
// worse, CE's debugger CANNOT BE DETACHED once attached (debug_detach returned detached:false), so
// attaching to a live test session permanently contaminates it. From inside the process we can just
// call GetThreadContext on every thread ourselves, which sidesteps all of that.
//
// SAFETY, and this is deliberate: this thread NEVER suspends a thread and NEVER writes any game
// state -- it only reads thread contexts and appends to our log. Two specific reasons:
//   1. SuspendThread on 90+ threads once a second is a real deadlock risk (a suspended thread can
//      hold a lock we'd then need), and would itself be capable of causing the very freeze we're
//      trying to diagnose.
//   2. The hard lesson from InstallBucketMemoryWatcherThread (reverted 2026-08-05): a background
//      thread that WRITES game state based on a check that turns out to be wrong can destroy a
//      session instantly. Read-only removes that whole class of risk.
// GetThreadContext on a running thread is technically undefined, but that's fine here: a genuinely
// stuck thread isn't moving, so its RIP reads back stable, which is exactly the signal we want.
//
// Freeze detection needs no game knowledge: on a healthy frame, at least one thread executing exe
// code makes progress (its RIP changes between samples). If NO exe-code thread has moved for
// several consecutive seconds, the game loop has stopped. Then dump the stuck threads, including a
// heuristic scan of each stack for exe-range return addresses -- so a thread parked in a kernel
// wait still tells us which game code led it there.
static bool g_freezeWatchdogStarted = false;
static const int FREEZE_WATCHDOG_INTERVAL_MS = 1000;
static const int FREEZE_STALL_SAMPLES = 5;  // ~5s of no exe-code progress before reporting
static const int FREEZE_MAX_WATCHED_THREADS = 256;
static const int FREEZE_MAX_DUMPED_THREADS = 24; // keep a 90+-thread process from flooding the log

struct WatchdogThreadSample {
    DWORD tid;
    uint64_t lastRip;
    int sameCount;
};

// Logs one stuck thread: its RIP (as an exe RVA when it's in game code) plus up to 8 exe-range
// return addresses found on its stack. Separate function so the stack walk's __try doesn't
// constrain the caller.
static void LogStuckThread(DWORD tid, uint64_t rip, uint64_t rsp, int sameCount,
                           uint64_t exeBase, uint64_t exeSize) {
    char msg[512];
    int written;
    if (rip >= exeBase && rip < exeBase + exeSize) {
        written = snprintf(msg, sizeof(msg),
            "FreezeWatchdog:   tid=%lu stuck %ds RIP=exe+0x%llX callers:",
            (unsigned long)tid, sameCount, (unsigned long long)(rip - exeBase));
    } else {
        written = snprintf(msg, sizeof(msg),
            "FreezeWatchdog:   tid=%lu stuck %ds RIP=0x%llX (outside exe) callers:",
            (unsigned long)tid, sameCount, (unsigned long long)rip);
    }
    if (written <= 0) return;

    int found = 0;
    __try {
        uintptr_t* sp = (uintptr_t*)(uintptr_t)rsp;
        for (int i = 0; i < 256 && found < 8; ++i) {
            uintptr_t v = sp[i];
            if ((uint64_t)v > exeBase && (uint64_t)v < exeBase + exeSize) {
                int n = snprintf(msg + written, sizeof(msg) - (size_t)written, " 0x%llX",
                                 (unsigned long long)((uint64_t)v - exeBase));
                if (n <= 0 || (size_t)(written + n) >= sizeof(msg)) break;
                written += n;
                ++found;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        // A thread's stack can be unreadable mid-teardown -- log what we have.
    }
    if (found == 0) {
        snprintf(msg + written, sizeof(msg) - (size_t)written, " (none on stack)");
    }
    LogDebug(msg);
}

static DWORD WINAPI FreezeWatchdogThreadProc(LPVOID) {
    uint64_t exeBase = (uint64_t)(uintptr_t)GetModuleHandleA(nullptr);
    uint64_t exeSize = 0;
    if (exeBase) {
        IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)(uintptr_t)exeBase;
        IMAGE_NT_HEADERS64* nt = (IMAGE_NT_HEADERS64*)(uintptr_t)(exeBase + dos->e_lfanew);
        exeSize = (uint64_t)nt->OptionalHeader.SizeOfImage;
    }
    if (!exeBase || !exeSize) return 0;

    const DWORD myTid = GetCurrentThreadId();
    const DWORD pid = GetCurrentProcessId();

    static WatchdogThreadSample samples[FREEZE_MAX_WATCHED_THREADS] = {};
    int sampleCount = 0;
    int stalledRounds = 0;
    bool alreadyReported = false;

    for (;;) {
        Sleep(FREEZE_WATCHDOG_INTERVAL_MS);

        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap == INVALID_HANDLE_VALUE) continue;

        THREADENTRY32 te = {};
        te.dwSize = sizeof(te);
        bool anyExeProgress = false;

        if (Thread32First(snap, &te)) {
            do {
                if (te.th32OwnerProcessID != pid || te.th32ThreadID == myTid) continue;
                HANDLE h = OpenThread(THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
                if (!h) continue;

                alignas(16) CONTEXT ctx = {};
                ctx.ContextFlags = CONTEXT_CONTROL; // RIP + RSP, all we need
                if (GetThreadContext(h, &ctx)) {
                    uint64_t rip = (uint64_t)ctx.Rip;
                    bool inExe = (rip >= exeBase && rip < exeBase + exeSize);

                    int idx = -1;
                    for (int i = 0; i < sampleCount; ++i) {
                        if (samples[i].tid == te.th32ThreadID) { idx = i; break; }
                    }
                    if (idx < 0 && sampleCount < FREEZE_MAX_WATCHED_THREADS) {
                        idx = sampleCount++;
                        samples[idx].tid = te.th32ThreadID;
                        samples[idx].lastRip = rip;
                        samples[idx].sameCount = 0;
                    } else if (idx >= 0) {
                        if (samples[idx].lastRip == rip) {
                            ++samples[idx].sameCount;
                        } else {
                            samples[idx].sameCount = 0;
                            samples[idx].lastRip = rip;
                            if (inExe) anyExeProgress = true;
                        }
                    }
                }
                CloseHandle(h);
            } while (Thread32Next(snap, &te));
        }
        CloseHandle(snap);

        if (anyExeProgress) {
            stalledRounds = 0;
            alreadyReported = false; // re-arm for a future freeze
            continue;
        }

        ++stalledRounds;
        if (stalledRounds < FREEZE_STALL_SAMPLES || alreadyReported) continue;
        alreadyReported = true;

        char header[224];
        snprintf(header, sizeof(header),
            "FreezeWatchdog: no exe-code thread has made progress for ~%ds -- the game appears "
            "FROZEN. Dumping stuck threads (RIP + exe-range return addresses per stack):",
            stalledRounds);
        LogDebug(header);

        HANDLE snap2 = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap2 != INVALID_HANDLE_VALUE) {
            THREADENTRY32 te2 = {};
            te2.dwSize = sizeof(te2);
            int dumped = 0;
            if (Thread32First(snap2, &te2)) {
                do {
                    if (te2.th32OwnerProcessID != pid || te2.th32ThreadID == myTid) continue;
                    if (dumped >= FREEZE_MAX_DUMPED_THREADS) break;
                    int idx = -1;
                    for (int i = 0; i < sampleCount; ++i) {
                        if (samples[i].tid == te2.th32ThreadID) { idx = i; break; }
                    }
                    if (idx < 0 || samples[idx].sameCount < FREEZE_STALL_SAMPLES - 1) continue;

                    HANDLE h = OpenThread(THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te2.th32ThreadID);
                    if (!h) continue;
                    alignas(16) CONTEXT ctx = {};
                    ctx.ContextFlags = CONTEXT_CONTROL;
                    if (GetThreadContext(h, &ctx)) {
                        LogStuckThread(te2.th32ThreadID, (uint64_t)ctx.Rip, (uint64_t)ctx.Rsp,
                                       samples[idx].sameCount, exeBase, exeSize);
                        ++dumped;
                    }
                    CloseHandle(h);
                } while (Thread32Next(snap2, &te2));
            }
            CloseHandle(snap2);
            if (dumped >= FREEZE_MAX_DUMPED_THREADS) {
                LogDebug("FreezeWatchdog: (dump capped -- more stuck threads existed than were logged)");
            }
        }
        LogDebug("FreezeWatchdog: end of stuck-thread dump");
    }
}

static bool InstallFreezeWatchdogThread() {
    if (g_freezeWatchdogStarted) return true;
    HANDLE h = CreateThread(nullptr, 0, FreezeWatchdogThreadProc, nullptr, 0, nullptr);
    if (!h) {
        LogDebug("InstallFreezeWatchdogThread: CreateThread failed -- proceeding without freeze diagnostics");
        return false;
    }
    CloseHandle(h);
    g_freezeWatchdogStarted = true;
    LogDebug("InstallFreezeWatchdogThread: started (read-only: samples thread RIPs once a second, "
             "never suspends a thread, never writes game state)");
    return true;
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

// --- KEYFRAME LIST ENTRY *PARAM* GUARD HOOK ---
// Fixes CRASH SITE #6 (found 2026-08-05 via a live WER minidump, after the hook above already
// covered crash site #1 in this same function): fnc_keyframe_list_lookup's own entry point reads
// *(int32_t*)(param_1 + 0x3C) -- this is a DIFFERENT read than InstallKeyframeListEntryGuardHook
// covers (that one guards the CALL to fnc_resolve_resource_handle at +0x2F and the read of its
// RESULT; this one is the read of param_1 itself at +0x15, three instructions earlier). Confirmed
// live via crash register dump: RCX (param_1) == -1 (0xFFFFFFFFFFFFFFFF), fault address == 0x3B
// (== -1 + 0x3C truncated to 32 bits) -- i.e. the CALLER passed an invalid record pointer, not
// merely an unminted handle. Traced (not yet fully root-caused) to
// fnc_entity_animation_blend_advance's own entity+0x1D4/+0x168 animation-table lookup -- see that
// function's plate comment. This crash happens during ordinary per-frame animation update of an
// already-spawned entity, NOT inside spawn_enemy's own construction call -- unlike every other
// guard hook in this file, it isn't gated behind a required RVA (nothing to safely "refuse"; the
// entities affected already exist).
//
// Safe value on failure is 0 (a real handle value, not a sentinel): fnc_resolve_resource_handle(0)
// already returns 0 via its own early-out, and the EXISTING KeyframeListEntryGuardHook already
// treats a resolved pointer of 0 as unreadable and safely returns "not found" -- so returning 0
// here reuses that already-proven path instead of duplicating its logic.
static bool g_keyframeListEntryParamGuardHookInstalled = false;
static volatile uint64_t g_keyframeListEntryParamGuardSkipCount = 0;

static int32_t CheckKeyframeListEntryParamSafe(uint64_t recordPtr) {
    __try {
        return *(int32_t*)(uintptr_t)(recordPtr + 0x3C);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        char msg[176];
        snprintf(msg, sizeof(msg),
            "KeyframeListEntryParamGuard: recordPtr=0x%llX unreadable (likely -1 propagated from "
            "fnc_entity_animation_blend_advance's animation-table lookup), treating as handle=0 "
            "(skipsSoFar=%llu)",
            (unsigned long long)recordPtr, (unsigned long long)g_keyframeListEntryParamGuardSkipCount + 1);
        LogDebug(msg);
        ++g_keyframeListEntryParamGuardSkipCount;
        return 0;
    }
}

// Hooks the 5 bytes at fnc_keyframe_list_lookup+0x15: "MOV ECX,[RCX+0x3C]" (3 bytes) + the
// following "MOV ESI,EDX" (2 bytes) -- exactly 5 bytes, no partial-instruction overwrite needed.
// EDX (needed later, both as ESI's new value here and again for EBX at +0x1A) is stashed in EBX
// across the CALL: EBX is callee-saved (its incoming value was already spilled to the stack by
// this function's own prologue at +0x0, so it's free to reuse here) and the original code doesn't
// read EBX again until +0x1A, where it gets freshly overwritten from EDX anyway.
static bool InstallKeyframeListEntryParamGuardHook(unsigned long long hookAddr, unsigned long long resumeAddr) {
    if (g_keyframeListEntryParamGuardHookInstalled) return true;

    unsigned char* hookPtr = (unsigned char*)(uintptr_t)hookAddr;
    static const unsigned char expected[5] = {
        0x8B, 0x49, 0x3C,   // mov ecx, [rcx+0x3C]
        0x8B, 0xF2          // mov esi, edx
    };
    if (memcmp(hookPtr, expected, 5) != 0) {
        LogDebug("InstallKeyframeListEntryParamGuardHook: unexpected original bytes at hook address, aborting");
        return false;
    }

    void* cave = AllocateNear((void*)(uintptr_t)hookAddr, 4096);
    if (!cave) {
        LogDebug("InstallKeyframeListEntryParamGuardHook: failed to allocate a nearby code cave");
        return false;
    }

    unsigned char stub[64] = {};
    size_t off = 0;
    uint64_t checkFnAddr = (uint64_t)(uintptr_t)&CheckKeyframeListEntryParamSafe;

    stub[off++] = 0x89; stub[off++] = 0xD3; // mov ebx, edx  (stash param_2 across the call)

    stub[off++] = 0x48; stub[off++] = 0x83; stub[off++] = 0xEC; stub[off++] = 0x20; // sub rsp, 0x20

    stub[off++] = 0x48; stub[off++] = 0xB8; // mov rax, checkFnAddr
    memcpy(stub + off, &checkFnAddr, 8); off += 8;

    stub[off++] = 0xFF; stub[off++] = 0xD0; // call rax  (rcx already holds param_1; eax <- safe dword)

    stub[off++] = 0x48; stub[off++] = 0x83; stub[off++] = 0xC4; stub[off++] = 0x20; // add rsp, 0x20

    stub[off++] = 0x89; stub[off++] = 0xC1; // mov ecx, eax  (arg for the upcoming resolve_resource_handle call)
    stub[off++] = 0x89; stub[off++] = 0xDA; // mov edx, ebx  (restore param_2)
    stub[off++] = 0x89; stub[off++] = 0xD6; // mov esi, edx  (replicate the original "mov esi,edx")

    stub[off++] = 0xE9; // jmp rel32 (patched below) -> resumeAddr
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

    g_keyframeListEntryParamGuardHookInstalled = true;
    LogDebug("InstallKeyframeListEntryParamGuardHook: installed successfully");
    return true;
}

// --- ANIM BLEND ADVANCE DIAGNOSTIC HOOK (2026-08-05) ---
// Diagnostic-only, NOT a fix -- doesn't change behavior, just logs. Purpose: capture the LIVE
// field values behind the crash site #6 root cause the next time it happens, instead of more
// static guessing. fnc_entity_animation_blend_advance (RVA 0x29FB30) computes:
//   uVar1 = entity+0x168 (ushort index)
//   lVar3 = fnc_resolve_resource_handle(entity+0x1D4)   (table base)
//   uVar4 = fnc_resolve_resource_handle(*(lVar3 + uVar1*4))   <- this is what becomes -1
// This hook sits at RVA+0x50 ("MOV RBX,RAX" + "CALL fnc_resolve_resource_handle", 8 bytes total,
// the instruction boundary immediately after uVar4 is computed into RAX) -- at that point RSI
// still holds the entity pointer (set at the function's own +0x2E and untouched since), EBX still
// holds uVar1, and RAX holds uVar4. When uVar4 == -1, logs the entity pointer plus its raw
// (unresolved) +0x1D0/+0x1D4/+0x168 bytes -- answers whether the field is genuinely zeroed (fresh
// pool slot, never linked) or non-zero-but-wrong (stale handle surviving a pool-slot reuse). See
// fnc_entity_animation_blend_advance's own plate comment in Ghidra for the fuller writeup.
static bool g_animBlendAdvanceDiagHookInstalled = false;
static volatile uint64_t g_animBlendAdvanceDiagLogCount = 0;

static void LogAnimBlendAdvanceDiag(uint64_t entityPtr, uint32_t uVar1Index, uint64_t uVar4) {
    __try {
        uint32_t f1d0 = *(uint32_t*)(uintptr_t)(entityPtr + 0x1D0);
        uint32_t f1d4 = *(uint32_t*)(uintptr_t)(entityPtr + 0x1D4);
        uint8_t kind = *(uint8_t*)(uintptr_t)(entityPtr + 0x6);
        char msg[256];
        snprintf(msg, sizeof(msg),
            "AnimBlendAdvanceDiag: entity=0x%llX kind=%u entity+0x1D0=0x%08X entity+0x1D4=0x%08X "
            "entity+0x168(idx)=0x%04X uVar4(result)=0x%llX (hitsSoFar=%llu)",
            (unsigned long long)entityPtr, (unsigned)kind, f1d0, f1d4, uVar1Index,
            (unsigned long long)uVar4, (unsigned long long)g_animBlendAdvanceDiagLogCount + 1);
        LogDebug(msg);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        char msg[128];
        snprintf(msg, sizeof(msg),
            "AnimBlendAdvanceDiag: entity=0x%llX itself unreadable while logging (hitsSoFar=%llu)",
            (unsigned long long)entityPtr, (unsigned long long)g_animBlendAdvanceDiagLogCount + 1);
        LogDebug(msg);
    }
    ++g_animBlendAdvanceDiagLogCount;
}

static bool InstallAnimBlendAdvanceDiagHook(unsigned long long hookAddr, unsigned long long resumeAddr,
                                              unsigned long long resolveHandleFnAddr) {
    if (g_animBlendAdvanceDiagHookInstalled) return true;

    unsigned char* hookPtr = (unsigned char*)(uintptr_t)hookAddr;
    static const unsigned char expected[8] = {
        0x48, 0x8B, 0xD8,               // mov rbx, rax
        0xE8, 0x38, 0xB2, 0x0E, 0x00    // call fnc_resolve_resource_handle (rel32, this build's offset)
    };
    if (memcmp(hookPtr, expected, 8) != 0) {
        LogDebug("InstallAnimBlendAdvanceDiagHook: unexpected original bytes at hook address, aborting");
        return false;
    }

    void* cave = AllocateNear((void*)(uintptr_t)hookAddr, 4096);
    if (!cave) {
        LogDebug("InstallAnimBlendAdvanceDiagHook: failed to allocate a nearby code cave");
        return false;
    }

    unsigned char stub[128] = {};
    size_t off = 0;
    uint64_t logFnAddr = (uint64_t)(uintptr_t)&LogAnimBlendAdvanceDiag;

    stub[off++] = 0x48; stub[off++] = 0x89; stub[off++] = 0xC3; // mov rbx, rax  (uVar4, callee-saved across everything below)
    stub[off++] = 0x89; stub[off++] = 0xCF;                     // mov edi, ecx  (stash entity+0x15c arg, RDI callee-saved & free here)

    stub[off++] = 0x48; stub[off++] = 0x83; stub[off++] = 0xFB; stub[off++] = 0xFF; // cmp rbx, -1
    stub[off++] = 0x75; // jnz rel8 (patched below) -> skip_log
    size_t jnzOperand = off; off += 1;

    // log path: LogAnimBlendAdvanceDiag(entityPtr=RSI, uVar1Index=EBX, uVar4=RBX)
    stub[off++] = 0x48; stub[off++] = 0x89; stub[off++] = 0xF1; // mov rcx, rsi
    stub[off++] = 0x89; stub[off++] = 0xDA;                     // mov edx, ebx
    stub[off++] = 0x4C; stub[off++] = 0x8B; stub[off++] = 0xC3; // mov r8, rbx   (arg3 = uVar4)
    stub[off++] = 0x48; stub[off++] = 0x83; stub[off++] = 0xEC; stub[off++] = 0x20; // sub rsp, 0x20
    stub[off++] = 0x48; stub[off++] = 0xB8; // mov rax, logFnAddr
    memcpy(stub + off, &logFnAddr, 8); off += 8;
    stub[off++] = 0xFF; stub[off++] = 0xD0; // call rax
    stub[off++] = 0x48; stub[off++] = 0x83; stub[off++] = 0xC4; stub[off++] = 0x20; // add rsp, 0x20

    size_t skipLogTarget = off;
    stub[jnzOperand] = (unsigned char)(int8_t)(skipLogTarget - (jnzOperand + 1));

    stub[off++] = 0x89; stub[off++] = 0xF9; // mov ecx, edi  (restore entity+0x15c arg)

    stub[off++] = 0x48; stub[off++] = 0x83; stub[off++] = 0xEC; stub[off++] = 0x20; // sub rsp, 0x20
    stub[off++] = 0x48; stub[off++] = 0xB8; // mov rax, resolveHandleFnAddr
    memcpy(stub + off, &resolveHandleFnAddr, 8); off += 8;
    stub[off++] = 0xFF; stub[off++] = 0xD0; // call rax  (rax <- uVar5, matches original CALL's effect)
    stub[off++] = 0x48; stub[off++] = 0x83; stub[off++] = 0xC4; stub[off++] = 0x20; // add rsp, 0x20

    stub[off++] = 0xE9; // jmp rel32 (patched below) -> resumeAddr
    size_t jmpOperand = off; off += 4;

    size_t stubLen = off;
    uintptr_t caveBase = (uintptr_t)cave;

    int32_t jmpRel = (int32_t)((int64_t)resumeAddr - (int64_t)(caveBase + jmpOperand + 4));
    memcpy(stub + jmpOperand, &jmpRel, 4);

    unsigned char patch[8];
    patch[0] = 0xE9;
    int32_t hookRel = (int32_t)((int64_t)caveBase - (int64_t)(hookAddr + 5));
    memcpy(patch + 1, &hookRel, 4);
    // Bytes 5-7 of the original 8-byte region are simply jumped over, never executed --
    // same convention as the other hooks' partial-overwrite case -- but fill with NOPs
    // rather than leaving stale opcode bytes behind, for cleanliness under a disassembler.
    patch[5] = 0x90; patch[6] = 0x90; patch[7] = 0x90;

    std::vector<HANDLE> threads = SuspendOtherThreads();

    memcpy(cave, stub, stubLen);

    DWORD oldProtect = 0;
    VirtualProtect((void*)(uintptr_t)hookAddr, 8, PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((void*)(uintptr_t)hookAddr, patch, 8);
    VirtualProtect((void*)(uintptr_t)hookAddr, 8, oldProtect, &oldProtect);

    FlushInstructionCache(GetCurrentProcess(), (void*)(uintptr_t)hookAddr, 8);
    FlushInstructionCache(GetCurrentProcess(), cave, stubLen);

    ResumeThreads(threads);

    g_animBlendAdvanceDiagHookInstalled = true;
    LogDebug("InstallAnimBlendAdvanceDiagHook: installed successfully");
    return true;
}

// --- SECTION-2 SIZE GUARD HOOK (2026-08-05) -- REAL FIX, not a catch-after-the-fact guard ---
// Root cause found via static tracing (no live process): entity+0x1D4 (the field behind crash
// site #6 -- see fnc_keyframe_list_lookup's plate comment) is a raw copy of a dword living inside
// the SHARED per-species 256KB resource blob, at (blob's own "section 2" base)+0x30. That base is
// computed from the blob's own header offset table (dwords at blob+8 and blob+0xC): if
// blob+0xC - blob+8 (the section's size) is POSITIVE, the game reads a dword at section+0x30 and
// copies it into entity+0x1D4 via fnc_copy_blob_section2_anim_handle -- but ">0" only proves the
// section is non-empty, never that it's large enough to actually CONTAIN a dword at +0x30 (needs
// >= 0x34 bytes). Species resource blob slots are reused across creatures within a session (see
// project_spawn_enemy_cold_spawn_crash_containment.md) -- a small-but-nonzero section 2 left over
// from a earlier, differently-shaped occupant of that same blob slot would pass this check yet
// still cause a read of stale/adjacent bytes.
//
// This hook corrects the check itself (inside fnc_link_model_resource_data, the "MOV EAX,[RBP+0xC]
// / SUB EAX,ECX / TEST EAX,EAX / JLE" sequence computing section size) from "size > 0" to
// "size >= 0x34". Sizes in between (nonzero but too small -- the case this fixes) now take the
// same safe "no section-2 data" path as a genuinely empty section (entity+0x1D4 stays 0, exactly
// like fnc_resolve_resource_handle(0) already handles cleanly), and get logged so we can confirm
// live whether this is really what's happening. Sizes >= 0x34 behave identically to the original
// code -- this hook changes nothing for the common case.
static bool g_section2SizeGuardHookInstalled = false;
static volatile uint64_t g_section2SizeGuardCorrectionCount = 0;

static void LogSection2SizeGuardCorrection(uint32_t rawSize) {
    char msg[192];
    snprintf(msg, sizeof(msg),
        "Section2SizeGuard: model resource blob's section-2 size=0x%X is nonzero but smaller than "
        "the 0x34 bytes needed to safely read entity+0x1D4's animation-table handle at section+0x30 "
        "-- treating as empty instead of reading past the section (correctionsSoFar=%llu)",
        rawSize, (unsigned long long)g_section2SizeGuardCorrectionCount + 1);
    LogDebug(msg);
    ++g_section2SizeGuardCorrectionCount;
}

// Hooks the 9 bytes at fnc_link_model_resource_data's "MOV EAX,[RBP+0xC]" (3) + "SUB EAX,ECX" (2)
// + "TEST EAX,EAX" (2) + "JLE" (2). ECX (blob+8, needed later by "ADD RCX,RBP" at resumeAddr) is
// never touched by this hook -- only EAX/EDX are used as scratch.
static bool InstallSection2SizeGuardHook(unsigned long long hookAddr, unsigned long long resumeAddr,
                                           unsigned long long emptyPathAddr) {
    if (g_section2SizeGuardHookInstalled) return true;

    unsigned char* hookPtr = (unsigned char*)(uintptr_t)hookAddr;
    static const unsigned char expected[9] = {
        0x8B, 0x45, 0x0C,   // mov eax, [rbp+0xC]
        0x2B, 0xC1,          // sub eax, ecx
        0x85, 0xC0,           // test eax, eax
        0x7E, 0x26            // jle +0x26 (original short-form jump to the empty path)
    };
    if (memcmp(hookPtr, expected, 9) != 0) {
        LogDebug("InstallSection2SizeGuardHook: unexpected original bytes at hook address, aborting");
        return false;
    }

    void* cave = AllocateNear((void*)(uintptr_t)hookAddr, 4096);
    if (!cave) {
        LogDebug("InstallSection2SizeGuardHook: failed to allocate a nearby code cave");
        return false;
    }

    unsigned char stub[96] = {};
    size_t off = 0;
    uint64_t logFnAddr = (uint64_t)(uintptr_t)&LogSection2SizeGuardCorrection;

    stub[off++] = 0x8B; stub[off++] = 0x45; stub[off++] = 0x0C; // mov eax, [rbp+0xC]
    stub[off++] = 0x2B; stub[off++] = 0xC1;                     // sub eax, ecx  (eax = raw section2 size)
    stub[off++] = 0x89; stub[off++] = 0xC2;                     // mov edx, eax  (save raw size)
    stub[off++] = 0x85; stub[off++] = 0xC0;                     // test eax, eax

    stub[off++] = 0x0F; stub[off++] = 0x8E; // jle rel32 (patched below) -> tgtEmpty (raw size <= 0, original behavior)
    size_t jleOperand = off; off += 4;

    stub[off++] = 0x83; stub[off++] = 0xF8; stub[off++] = 0x34; // cmp eax, 0x34

    stub[off++] = 0x0F; stub[off++] = 0x8D; // jge rel32 (patched below) -> tgtResume (size >= 0x34, unchanged behavior)
    size_t jgeOperand = off; off += 4;

    // 1 <= raw size < 0x34: the case this hook actually corrects. Log it, then fall through
    // into tgtEmpty (same landing point the jle above uses).
    stub[off++] = 0x89; stub[off++] = 0xD1;                     // mov ecx, edx  (arg1 = raw size)
    stub[off++] = 0x48; stub[off++] = 0x83; stub[off++] = 0xEC; stub[off++] = 0x20; // sub rsp, 0x20
    stub[off++] = 0x48; stub[off++] = 0xB8; // mov rax, logFnAddr
    memcpy(stub + off, &logFnAddr, 8); off += 8;
    stub[off++] = 0xFF; stub[off++] = 0xD0; // call rax
    stub[off++] = 0x48; stub[off++] = 0x83; stub[off++] = 0xC4; stub[off++] = 0x20; // add rsp, 0x20

    size_t tgtEmpty = off;
    stub[off++] = 0xE9; // jmp rel32 (patched below) -> emptyPathAddr
    size_t jmpEmptyOperand = off; off += 4;

    size_t tgtResume = off;
    stub[off++] = 0xE9; // jmp rel32 (patched below) -> resumeAddr
    size_t jmpResumeOperand = off; off += 4;

    size_t stubLen = off;
    uintptr_t caveBase = (uintptr_t)cave;

    int32_t jleRel = (int32_t)((int64_t)(caveBase + tgtEmpty) - (int64_t)(caveBase + jleOperand + 4));
    memcpy(stub + jleOperand, &jleRel, 4);

    int32_t jgeRel = (int32_t)((int64_t)(caveBase + tgtResume) - (int64_t)(caveBase + jgeOperand + 4));
    memcpy(stub + jgeOperand, &jgeRel, 4);

    int32_t jmpEmptyRel = (int32_t)((int64_t)emptyPathAddr - (int64_t)(caveBase + jmpEmptyOperand + 4));
    memcpy(stub + jmpEmptyOperand, &jmpEmptyRel, 4);

    int32_t jmpResumeRel = (int32_t)((int64_t)resumeAddr - (int64_t)(caveBase + jmpResumeOperand + 4));
    memcpy(stub + jmpResumeOperand, &jmpResumeRel, 4);

    unsigned char patch[9];
    patch[0] = 0xE9;
    int32_t hookRel = (int32_t)((int64_t)caveBase - (int64_t)(hookAddr + 5));
    memcpy(patch + 1, &hookRel, 4);
    patch[5] = 0x90; patch[6] = 0x90; patch[7] = 0x90; patch[8] = 0x90;

    std::vector<HANDLE> threads = SuspendOtherThreads();

    memcpy(cave, stub, stubLen);

    DWORD oldProtect = 0;
    VirtualProtect((void*)(uintptr_t)hookAddr, 9, PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((void*)(uintptr_t)hookAddr, patch, 9);
    VirtualProtect((void*)(uintptr_t)hookAddr, 9, oldProtect, &oldProtect);

    FlushInstructionCache(GetCurrentProcess(), (void*)(uintptr_t)hookAddr, 9);
    FlushInstructionCache(GetCurrentProcess(), cave, stubLen);

    ResumeThreads(threads);

    g_section2SizeGuardHookInstalled = true;
    LogDebug("InstallSection2SizeGuardHook: installed successfully");
    return true;
}

// --- RESOLVE-HANDLE BUCKET GUARD HOOK (2026-08-05) -- THE master fix ---
// fnc_resolve_resource_handle_impl (RVA 0x38AF40) resolves a minted handle via a 64-entry
// "bucket" table (DAT_142ee3980): top bits of the handle select a bucket (a distinct 32MB-aligned
// memory region, assigned lazily and NEVER FREED), low 25 bits are an offset within it, combined
// via bitwise OR (relies on 32MB alignment so the two never overlap).
//
// fnc_mint_resource_handle's own bucket-table walk (FUN_14038aee0) has a hard ceiling: if all 64
// buckets are already claimed by 64 distinct 32MB-aligned regions, minting a handle for a NEW,
// unmatched region silently returns a handle encoding bucket-index 64 -- one past the end of the
// real table -- WITHOUT ever registering it. There is no eviction/LRU; the table only ever grows.
// Later, resolving that handle reads bucket_table[64], past the array's actual bounds, and ORs
// whatever garbage sits there with the offset -- confirmed LIVE (WER-adjacent game-internal
// CrashDump.dmp, 2026-08-05) to often just be -1, since bucket_table's own init routine
// (fnc_resource_handle_bucket_table_init) fills ALL slots to -1 as a "not yet claimed" sentinel,
// and ORing anything with -1 always yields -1 -- callers then dereference that -1 "pointer"
// directly with zero validity checking anywhere in the game.
//
// This is almost certainly the TRUE root cause behind every -1-handle crash traced this session
// (crash sites #1-#6, all in the animation/keyframe subsystem) AND the completely unrelated
// text-rendering use-after-free found the same session -- both are just different callers of
// fnc_resolve_resource_handle hitting the same underlying exhaustion. The 64-bucket table is
// shared, global, and never reclaimed, so heavy resource churn from repeated Crowd Control
// spawn_enemy calls (each minting ~20+ handles) is a very plausible way to exhaust in a single
// session what normal, room-paced gameplay never would.
//
// Fix: after the bucket-index lookup, if the index is out of the real 0-63 range OR the specific
// bucket slot is still the unclaimed -1 sentinel, return 0 instead of a garbage/-1 value. 0 is the
// SAME "no data" sentinel fnc_resolve_resource_handle's own fast path already returns for a literal
// 0 handle, so every caller that already checks "resolved == 0" (the overwhelming majority, per
// this whole session's tracing) degrades to "no data" cleanly instead of crashing. This protects
// EVERY caller of fnc_resolve_resource_handle in the entire game at once, not just the handful of
// individual call sites this file has hooked so far.
static bool g_resolveHandleBucketGuardHookInstalled = false;
static volatile uint64_t g_resolveHandleBucketGuardCount = 0;
static const int RESOLVE_BUCKET_COUNT = 64;

// Scans our own stack for return addresses that land inside the game exe's image and logs them as
// RVAs, nearest frame first. Deliberately a heuristic scan rather than a real unwind: the callers
// of fnc_resolve_resource_handle are frame-pointer-less, heavily-optimized game code that a proper
// backtrace API does not walk reliably here, and all we actually need is "which function asked for
// this handle" -- a short list of candidate call sites is enough to identify that in Ghidra. Only
// ever runs on the already-rare bad-handle path, so it adds nothing to the hot path.
static void LogResolveHandleBucketGuardCallers() {
    uint64_t exeBase = (uint64_t)(uintptr_t)GetModuleHandleA(nullptr);
    if (!exeBase) return;

    char msg[512] = {};
    __try {
        IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)(uintptr_t)exeBase;
        IMAGE_NT_HEADERS64* nt = (IMAGE_NT_HEADERS64*)(uintptr_t)(exeBase + dos->e_lfanew);
        uint64_t exeSize = (uint64_t)nt->OptionalHeader.SizeOfImage;

        int written = snprintf(msg, sizeof(msg),
            "ResolveHandleBucketGuard: candidate caller RVAs (nearest frame first):");
        int found = 0;
        uintptr_t* sp = (uintptr_t*)_AddressOfReturnAddress();
        for (int i = 0; i < 512 && found < 10; ++i) {
            uintptr_t v = sp[i];
            if (v > exeBase && v < exeBase + exeSize) {
                int n = snprintf(msg + written, sizeof(msg) - (size_t)written, " 0x%llX",
                                 (unsigned long long)((uint64_t)v - exeBase));
                if (n <= 0 || (size_t)(written + n) >= sizeof(msg)) break;
                written += n;
                ++found;
            }
        }
        if (found == 0) {
            snprintf(msg, sizeof(msg),
                "ResolveHandleBucketGuard: stack scan found no exe-range return addresses");
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        snprintf(msg, sizeof(msg), "ResolveHandleBucketGuard: stack scan faulted, no caller info");
    }
    LogDebug(msg);
}

static void LogResolveHandleBucketGuard(uint32_t maskedHandle) {
    // Only the -1 "never claimed" branch can actually reach here. At the hook point RAX holds the
    // bit31-cleared handle, so RAX>>25 is structurally always 0..63 and the stub's own ">=64
    // out-of-range" test is unreachable dead code. Live-verified 2026-08-10 (CE read of the table
    // at the moment of a hit) that a hit is NOT exhaustion -- only 8/64 buckets were claimed --
    // so this deliberately no longer claims the table is "very likely exhausted", which is what
    // sent an earlier session down the wrong root cause. See the memory file
    // project_spawn_enemy_cold_spawn_crash_containment for the full account.
    unsigned bucketIdx = (unsigned)((maskedHandle & 0x7FFFFFFFu) >> 25);
    unsigned offset = (unsigned)(maskedHandle & 0x1FFFFFFu);

    float asFloat = 0.0f;
    memcpy(&asFloat, &maskedHandle, sizeof(asFloat));

    int claimed = -1;
    uint64_t tableAddr = g_resourceHandleBucketTableAddr;
    if (tableAddr) {
        __try {
            volatile int64_t* table = (volatile int64_t*)(uintptr_t)tableAddr;
            int n = 0;
            for (int i = 0; i < RESOLVE_BUCKET_COUNT; ++i) {
                if (table[i] != -1) ++n;
            }
            claimed = n;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { claimed = -1; }
    }

    char msg[400];
    snprintf(msg, sizeof(msg),
        "ResolveHandleBucketGuard: handle=0x%08X decodes to bucket=%u (offset=0x%X), which is NOT "
        "claimed -- returning 0 instead of a garbage/-1 pointer. Buckets claimed right now: %d/%d. "
        "Reinterpreted as a float this handle is %g -- a float-shaped or otherwise non-handle value "
        "here means something read a non-handle field AS a handle, which is a different bug from "
        "table exhaustion. (hitsSoFar=%llu)",
        maskedHandle, bucketIdx, offset, claimed, RESOLVE_BUCKET_COUNT, (double)asFloat,
        (unsigned long long)g_resolveHandleBucketGuardCount + 1);
    LogDebug(msg);

    // Caller identification for the first few hits only -- that is all it takes to name the
    // function, and it keeps the log readable if a hit ever starts recurring every frame.
    if (g_resolveHandleBucketGuardCount < 5) {
        LogResolveHandleBucketGuardCallers();
    }
    ++g_resolveHandleBucketGuardCount;
}

// --- Bucket memory-validity check (2026-08-05) -- extends the same master fix ---
// A THIRD failure mode, distinct from "exhausted" (index 64) and "never claimed" (-1 sentinel):
// live-confirmed via game-internal CrashDump.dmp that a bucket can hold a genuinely real,
// once-valid 32MB-aligned base whose backing memory was later released -- nothing in the bucket
// table itself can distinguish that from a still-good bucket, since a mint only ever WRITES an
// entry when a real pointer was passed in at claim time; there's no unclaim/eviction. The only way
// to actually know is to ask the OS whether the resulting address is still backed by committed,
// readable memory -- this is completing fnc_resolve_resource_handle's own contract (it hands back
// an address without ever having verified the address is real), not a try/except guard bolted onto
// a caller.
//
// VirtualQuery is real kernel-call overhead, and this function is extremely hot (500k+ calls/session
// just from one caller observed today), so each bucket's validity is cached and only re-checked
// once every BUCKET_REVALIDATE_INTERVAL accesses TO THAT SPECIFIC BUCKET -- not wall-clock time,
// not a global counter across all buckets. A bucket can flip from invalid back to valid (a freed
// region can get legitimately reclaimed by a new, unrelated allocation later), so an invalid result
// is cached too, not treated as permanent.
// (RESOLVE_BUCKET_COUNT is defined further up, next to the bad-handle logger that also needs it.)
static const uint64_t BUCKET_REVALIDATE_INTERVAL = 1024;
static volatile uint64_t g_bucketMemCheckTick = 0;
static uint64_t g_bucketLastCheckTick[RESOLVE_BUCKET_COUNT] = {};
static bool g_bucketLastCheckOk[RESOLVE_BUCKET_COUNT] = {};
static volatile uint64_t g_bucketMemInvalidCount = 0;
static volatile uint64_t g_bucketMemCheckCount = 0;

static uint64_t ValidateResolvedBucketAddress(uint64_t bucketIdx, uint64_t candidateAddr) {
    if (bucketIdx >= (uint64_t)RESOLVE_BUCKET_COUNT) {
        return candidateAddr; // already guarded elsewhere; be conservative, don't invent new behavior here
    }
    uint64_t tick = ++g_bucketMemCheckTick;
    uint64_t elapsed = tick - g_bucketLastCheckTick[bucketIdx];
    if (elapsed < BUCKET_REVALIDATE_INTERVAL && g_bucketLastCheckTick[bucketIdx] != 0) {
        return g_bucketLastCheckOk[bucketIdx] ? candidateAddr : 0;
    }

    MEMORY_BASIC_INFORMATION mbi = {};
    SIZE_T res = VirtualQuery((LPCVOID)(uintptr_t)candidateAddr, &mbi, sizeof(mbi));
    bool ok = (res != 0) && (mbi.State == MEM_COMMIT) && (mbi.Protect != PAGE_NOACCESS) &&
              ((mbi.Protect & PAGE_GUARD) == 0);
    g_bucketLastCheckTick[bucketIdx] = tick;
    g_bucketLastCheckOk[bucketIdx] = ok;
    ++g_bucketMemCheckCount;
    if (!ok) {
        char msg[224];
        snprintf(msg, sizeof(msg),
            "ResolveHandleMemValidation: bucket=%llu addr=0x%llX not committed/readable "
            "(VirtualQuery result=%llu state=0x%X protect=0x%X) -- underlying memory was very likely "
            "freed after this bucket was claimed. Returning 0 instead (invalidSoFar=%llu, "
            "checksSoFar=%llu)",
            (unsigned long long)bucketIdx, (unsigned long long)candidateAddr, (unsigned long long)res,
            (unsigned)mbi.State, (unsigned)mbi.Protect, (unsigned long long)g_bucketMemInvalidCount + 1,
            (unsigned long long)g_bucketMemCheckCount);
        LogDebug(msg);
        ++g_bucketMemInvalidCount;
        return 0;
    }
    return candidateAddr;
}

// Hooks the 17 bytes at fnc_resolve_resource_handle_impl's "SHR RAX,0x19" (4) + "AND ECX,0x1FFFFFF"
// (6) + "MOV RAX,[RDX+RAX*8]" (4) + "OR RAX,RCX" (3). At hook entry: EAX/RAX already holds the
// bit31-cleared handle (zero-extended), RBX holds the same value, RDX holds the bucket table base
// (&DAT_142ee3980). RBX is safe to clobber -- the shared epilogue at resumeAddr POPs the caller's
// real saved RBX from the stack regardless of this function's own use of the register.
static bool InstallResolveHandleBucketGuardHook(unsigned long long hookAddr, unsigned long long resumeAddr) {
    if (g_resolveHandleBucketGuardHookInstalled) return true;

    unsigned char* hookPtr = (unsigned char*)(uintptr_t)hookAddr;
    static const unsigned char expected[17] = {
        0x48, 0xC1, 0xE8, 0x19,                   // shr rax, 0x19
        0x81, 0xE1, 0xFF, 0xFF, 0xFF, 0x01,        // and ecx, 0x1FFFFFF
        0x48, 0x8B, 0x04, 0xC2,                     // mov rax, [rdx+rax*8]
        0x48, 0x0B, 0xC1                              // or rax, rcx
    };
    if (memcmp(hookPtr, expected, 17) != 0) {
        LogDebug("InstallResolveHandleBucketGuardHook: unexpected original bytes at hook address, aborting");
        return false;
    }

    void* cave = AllocateNear((void*)(uintptr_t)hookAddr, 4096);
    if (!cave) {
        LogDebug("InstallResolveHandleBucketGuardHook: failed to allocate a nearby code cave");
        return false;
    }

    unsigned char stub[96] = {};
    size_t off = 0;
    uint64_t logFnAddr = (uint64_t)(uintptr_t)&LogResolveHandleBucketGuard;

    stub[off++] = 0x48; stub[off++] = 0xC1; stub[off++] = 0xE8; stub[off++] = 0x19; // shr rax, 0x19

    stub[off++] = 0x48; stub[off++] = 0x83; stub[off++] = 0xF8; stub[off++] = 0x40; // cmp rax, 0x40
    stub[off++] = 0x0F; stub[off++] = 0x83; // jae rel32 (patched below) -> badHandle
    size_t jaeOperand = off; off += 4;

    stub[off++] = 0x48; stub[off++] = 0x8B; stub[off++] = 0x0C; stub[off++] = 0xC2; // mov rcx, [rdx+rax*8]
    stub[off++] = 0x48; stub[off++] = 0x83; stub[off++] = 0xF9; stub[off++] = 0xFF; // cmp rcx, -1
    stub[off++] = 0x0F; stub[off++] = 0x84; // jz rel32 (patched below) -> badHandle
    size_t jzOperand = off; off += 4;

    // good handle: rax = bucket_table[idx] | (ebx & 0x1FFFFFF)
    // NOTE: the VirtualQuery-based memory-validity layer (ValidateResolvedBucketAddress) was
    // tried here and REVERTED 2026-08-05 -- it caused a real, reproducible game freeze in live
    // testing, almost certainly OS-level VAD-lock contention from calling VirtualQuery this
    // frequently on a multi-threaded hot path (fnc_resolve_resource_handle is called extremely
    // often, from more than one thread). A frozen game is worse than the crash this was meant to
    // prevent. ValidateResolvedBucketAddress/its caching globals are left in the file, unused, in
    // case a lower-risk way to apply the same idea comes up later (e.g. checked far more rarely,
    // or off the hot thread entirely) -- do not wire it back into this hot path without solving
    // the contention risk first.
    stub[off++] = 0x81; stub[off++] = 0xE3; stub[off++] = 0xFF; stub[off++] = 0xFF; stub[off++] = 0xFF; stub[off++] = 0x01; // and ebx, 0x1FFFFFF
    stub[off++] = 0x48; stub[off++] = 0x89; stub[off++] = 0xC8; // mov rax, rcx
    stub[off++] = 0x48; stub[off++] = 0x0B; stub[off++] = 0xC3; // or rax, rbx
    stub[off++] = 0xE9; // jmp rel32 (patched below) -> resumeAddr
    size_t jmpGoodOperand = off; off += 4;

    size_t badHandleTarget = off;
    stub[off++] = 0x89; stub[off++] = 0xD9;                     // mov ecx, ebx  (arg1 = masked handle)
    stub[off++] = 0x48; stub[off++] = 0x83; stub[off++] = 0xEC; stub[off++] = 0x20; // sub rsp, 0x20
    stub[off++] = 0x48; stub[off++] = 0xB8; // mov rax, logFnAddr
    memcpy(stub + off, &logFnAddr, 8); off += 8;
    stub[off++] = 0xFF; stub[off++] = 0xD0; // call rax
    stub[off++] = 0x48; stub[off++] = 0x83; stub[off++] = 0xC4; stub[off++] = 0x20; // add rsp, 0x20
    stub[off++] = 0x31; stub[off++] = 0xC0; // xor eax, eax
    stub[off++] = 0xE9; // jmp rel32 (patched below) -> resumeAddr
    size_t jmpBadOperand = off; off += 4;

    size_t stubLen = off;
    uintptr_t caveBase = (uintptr_t)cave;

    int32_t jaeRel = (int32_t)((int64_t)(caveBase + badHandleTarget) - (int64_t)(caveBase + jaeOperand + 4));
    memcpy(stub + jaeOperand, &jaeRel, 4);

    int32_t jzRel = (int32_t)((int64_t)(caveBase + badHandleTarget) - (int64_t)(caveBase + jzOperand + 4));
    memcpy(stub + jzOperand, &jzRel, 4);

    int32_t jmpGoodRel = (int32_t)((int64_t)resumeAddr - (int64_t)(caveBase + jmpGoodOperand + 4));
    memcpy(stub + jmpGoodOperand, &jmpGoodRel, 4);

    int32_t jmpBadRel = (int32_t)((int64_t)resumeAddr - (int64_t)(caveBase + jmpBadOperand + 4));
    memcpy(stub + jmpBadOperand, &jmpBadRel, 4);

    unsigned char patch[17];
    patch[0] = 0xE9;
    int32_t hookRel = (int32_t)((int64_t)caveBase - (int64_t)(hookAddr + 5));
    memcpy(patch + 1, &hookRel, 4);
    for (int i = 5; i < 17; ++i) patch[i] = 0x90;

    std::vector<HANDLE> threads = SuspendOtherThreads();

    memcpy(cave, stub, stubLen);

    DWORD oldProtect = 0;
    VirtualProtect((void*)(uintptr_t)hookAddr, 17, PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((void*)(uintptr_t)hookAddr, patch, 17);
    VirtualProtect((void*)(uintptr_t)hookAddr, 17, oldProtect, &oldProtect);

    FlushInstructionCache(GetCurrentProcess(), (void*)(uintptr_t)hookAddr, 17);
    FlushInstructionCache(GetCurrentProcess(), cave, stubLen);

    ResumeThreads(threads);

    g_resolveHandleBucketGuardHookInstalled = true;
    LogDebug("InstallResolveHandleBucketGuardHook: installed successfully");
    return true;
}

// --- BUCKET MEMORY WATCHER THREAD (2026-08-05 follow-up) -- closes the remaining gap ---
// InstallResolveHandleBucketGuardHook's own comment (and ValidateResolvedBucketAddress just above
// it) already documents a THIRD failure mode neither that hook nor the exhaustion fix covers: a
// bucket can hold a genuinely real, once-valid 32MB-aligned base whose backing memory was later
// freed by the OS -- indistinguishable from a still-good bucket by inspecting the table alone.
// ValidateResolvedBucketAddress implements the right check (VirtualQuery) but wiring it into the
// resolve hot path itself caused a real, reproducible game freeze (VAD-lock contention from
// VirtualQuery on a path called from multiple threads extremely often) and was reverted the same
// day. That comment explicitly invited "a lower-risk way to apply the same idea... checked far
// more rarely, or off the hot thread entirely."
//
// This is that: move the check entirely OFF the hot path onto a dedicated background thread that
// sweeps all 64 buckets on a slow timer (default 1s) instead of on every one of
// fnc_resolve_resource_handle's 500k+ calls/session. A freed bucket sits invalid for at most one
// sweep interval before being proactively reset to the -1 "unclaimed" sentinel -- which the
// ALREADY-SHIPPED InstallResolveHandleBucketGuardHook hook already treats as "return 0" on the hot
// path, so this needs zero hot-path code changes to benefit from. 64 VirtualQuery calls once a
// second is negligible, and this thread never touches the resolve path or blocks any game thread.
//
// A CompareExchange (not a blind write) guards the one real race: if the game's own claim function
// legitimately re-mints this exact slot between our read and our write (e.g. the freed region gets
// reused by a new, unrelated allocation and re-claimed for it), we must not clobber that fresh,
// valid value with a stale -1 -- the exchange only applies if the slot still holds the value we
// just checked.
static bool g_bucketWatcherThreadInstalled = false;
static volatile uint64_t g_bucketWatcherSweepCount = 0;
static volatile uint64_t g_bucketWatcherInvalidatedCount = 0;
static const DWORD BUCKET_WATCHER_SLEEP_MS = 1000;

static DWORD WINAPI BucketMemoryWatcherThreadProc(LPVOID param) {
    volatile int64_t* bucketTable = (volatile int64_t*)param;
    for (;;) {
        for (int i = 0; i < RESOLVE_BUCKET_COUNT; ++i) {
            int64_t candidate = bucketTable[i];
            if (candidate == -1) continue; // already unclaimed, nothing to check

            MEMORY_BASIC_INFORMATION mbi = {};
            SIZE_T res = VirtualQuery((LPCVOID)(uintptr_t)candidate, &mbi, sizeof(mbi));
            bool ok = (res != 0) && (mbi.State == MEM_COMMIT) && (mbi.Protect != PAGE_NOACCESS) &&
                      ((mbi.Protect & PAGE_GUARD) == 0);
            if (!ok) {
                int64_t prev = InterlockedCompareExchange64((volatile LONG64*)&bucketTable[i], -1, candidate);
                if (prev == candidate) {
                    char msg[192];
                    snprintf(msg, sizeof(msg),
                        "BucketMemoryWatcher: bucket=%d addr=0x%llX no longer committed/readable -- "
                        "proactively reset to unclaimed (invalidatedSoFar=%llu, sweepsSoFar=%llu)",
                        i, (unsigned long long)candidate,
                        (unsigned long long)g_bucketWatcherInvalidatedCount + 1,
                        (unsigned long long)g_bucketWatcherSweepCount);
                    LogDebug(msg);
                    ++g_bucketWatcherInvalidatedCount;
                }
            }
        }
        ++g_bucketWatcherSweepCount;
        Sleep(BUCKET_WATCHER_SLEEP_MS);
    }
}

static bool InstallBucketMemoryWatcherThread(unsigned long long bucketTableAddr) {
    if (g_bucketWatcherThreadInstalled) return true;
    HANDLE hThread = CreateThread(nullptr, 0, BucketMemoryWatcherThreadProc,
                                    (LPVOID)(uintptr_t)bucketTableAddr, 0, nullptr);
    if (!hThread) {
        LogDebug("InstallBucketMemoryWatcherThread: CreateThread failed");
        return false;
    }
    CloseHandle(hThread); // fire-and-forget -- the thread outlives this handle
    g_bucketWatcherThreadInstalled = true;
    LogDebug("InstallBucketMemoryWatcherThread: installed successfully");
    return true;
}

// --- BEHAVIOR-SCRIPT RESOLVED-BLOCK COPY GUARD HOOK (2026-08-06) ---
// KH1_BehaviorScriptInterpreter (Steam RVA 0x2cc620, the .bd AI-script bytecode VM run
// once per active creature per frame) has an opcode handler (class 0/sub-op 0xA --
// "load a block via a resolved resource handle") that resolves a handle then feeds the
// raw result straight into memcpy as the SOURCE with zero validation -- unlike a
// sibling opcode in the very same function (class 0/sub-op 3, RVA 0x2cc6e7) which
// explicitly checks the resolve result for null before using it.
//
// Live-crashed 2026-08-06 after 8 rapid spawn_enemy calls (heavy churn, several
// simultaneously-alive Heartless each running their own behavior script every frame):
// the memcpy source was non-null -- not caught by even a null check -- but genuinely
// unmapped memory, roughly 27MB below the exe's own module base that session.
// Root-caused via the game's own CrashDump.dmp (Python `minidump` + manual
// disassembly against the on-disk exe, cross-referenced with Ghidra once its MCP
// connection came back). This is crash site #6 in
// project_spawn_enemy_cold_spawn_crash_containment.md, and is exactly the "third
// case" (a bucket pointing at real, once-valid memory the OS later freed) that file
// has documented as open since 2026-08-05 -- the same gap two earlier attempts (a
// reverted per-call VirtualQuery check, and this same day's failed
// InstallBucketMemoryWatcherThread) both tried and failed to close from different
// angles.
//
// Unlike those two attempts, this doesn't try to validate a handle/bucket ahead of
// time -- it wraps the actual risky operation (the memcpy itself) in a real compiled
// __try/__except, the same technique already proven safe in this file (see
// InstallSkeletonBlendCallGuardHook). SEH costs nothing on the success path -- no
// VirtualQuery, no per-call kernel transition -- and only pays a cost when the copy
// genuinely faults, avoiding the exact contention/freeze risk that sank the
// VirtualQuery-based attempts. This replaces a single 5-byte CALL instruction (the
// call to the memcpy import thunk at RVA 0x2ccfed) with a CALL to a compiled wrapper
// that does the same copy inside __try/__except; RCX/RDX/R8 are already loaded with
// dest/src/size by the untouched instructions immediately before this call site,
// matching the x64 calling convention exactly, so no trampoline/register-shuffling is
// needed -- just a normal call, then falls through to the same resume point (RVA
// 0x2ccff2) the original memcpy's return would have reached. RSI/RDI/RBX (needed by
// the resumed code right after) are non-volatile in the x64 ABI, so any compliant
// compiled function -- including this wrapper -- preserves them automatically.
static bool g_behaviorScriptCopyGuardHookInstalled = false;
static volatile uint64_t g_behaviorScriptCopyGuardFaultCount = 0;

static void SafeBehaviorScriptBlockCopy(void* dest, const void* src, size_t size) {
    __try {
        memcpy(dest, src, size);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        char msg[192];
        snprintf(msg, sizeof(msg),
            "BehaviorScriptCopyGuard: memcpy from a resolved-but-invalid handle faulted -- "
            "skipped, destination left unwritten (faultsSoFar=%llu)",
            (unsigned long long)g_behaviorScriptCopyGuardFaultCount + 1);
        LogDebug(msg);
        ++g_behaviorScriptCopyGuardFaultCount;
    }
}

static bool InstallBehaviorScriptCopyGuardHook(unsigned long long hookAddr, unsigned long long resumeAddr) {
    if (g_behaviorScriptCopyGuardHookInstalled) return true;

    unsigned char* hookPtr = (unsigned char*)(uintptr_t)hookAddr;
    static const unsigned char expected[5] = { 0xE8, 0x04, 0x80, 0x0B, 0x00 }; // call <memcpy thunk>
    if (memcmp(hookPtr, expected, 5) != 0) {
        LogDebug("InstallBehaviorScriptCopyGuardHook: unexpected original bytes at hook address, aborting");
        return false;
    }

    void* cave = AllocateNear((void*)(uintptr_t)hookAddr, 4096);
    if (!cave) {
        LogDebug("InstallBehaviorScriptCopyGuardHook: failed to allocate a nearby code cave");
        return false;
    }

    unsigned char stub[32] = {};
    size_t off = 0;
    uint64_t wrapperAddr = (uint64_t)(uintptr_t)&SafeBehaviorScriptBlockCopy;
    stub[off++] = 0x48; stub[off++] = 0xB8; // mov rax, wrapperAddr
    memcpy(stub + off, &wrapperAddr, 8); off += 8;
    stub[off++] = 0xFF; stub[off++] = 0xD0; // call rax
    stub[off++] = 0xE9; // jmp rel32 -> resumeAddr
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

    g_behaviorScriptCopyGuardHookInstalled = true;
    LogDebug("InstallBehaviorScriptCopyGuardHook: installed successfully");
    return true;
}

// --- JOINT/MOTION-BLEND SETUP GUARD HOOK (2026-08-06) ---
// While retesting InstallBehaviorScriptCopyGuardHook above (crash site #6), heavy
// spawn_enemy churn crashed the game again -- a DIFFERENT, unrelated function this
// time, confirming the bucket-exhaustion "return 0" fix (2026-08-05) surfaces as null
// derefs in many scattered, previously-never-hit places across the animation/skeleton
// system, not just one. Root-caused via the game's own CrashDump.dmp: FUN_140394080
// (Steam RVA 0x394080, likely a joint/bone remap-table builder run when an entity's
// animation first transitions to a fresh motion -- calls fnc_resolve_resource_handle
// 30+ times with essentially no null-checking anywhere in it) dereferenced a null
// param_2[1] (RVA 0x3941e0, `CMP dword ptr [RAX+0x10],R12D` with RAX=0) -- the null
// most likely originates from an upstream resolve failure in a field this function
// assumes is always populated by the time it runs.
//
// FUN_140394080 has exactly one caller, FUN_1402a0350 (Steam RVA 0x2a0350) -- itself
// called from 5+ distinct sites across 3 different functions plus indirect
// function-pointer-table references (matching the same "shared choke point, not worth
// hooking every caller individually" shape as InstallSkeletonBlendCallGuardHook
// above), and internally either takes a "blend in progress" branch or, on a cold/
// fresh transition (`*(float*)(param_1+0x170) <= 0.0`), calls FUN_140394080 directly
// -- exactly the path that crashed. Rather than trace the exact upstream resolve that
// produced the null (would only fix this one field) or patch inside
// FUN_140394080/FUN_140393b30 individually (whack-a-mole, same conclusion as the
// skeleton-blend precedent above), this wraps FUN_1402a0350's own entry in a real
// __try/__except using the SAME two-trampoline technique as
// InstallSkeletonBlendCallGuardHook -- catches a crash anywhere in this function's
// whole call tree (both the cold-transition and blend-in-progress branches) at once,
// including causes not yet individually identified.
static bool g_jointRemapSetupGuardHookInstalled = false;
static volatile uint64_t g_jointRemapSetupGuardSkipCount = 0;

typedef void (*JointRemapSetupOriginalFn)(int64_t, int64_t, uint64_t, uint32_t, float);
static JointRemapSetupOriginalFn g_jointRemapSetupOriginalTrampoline = nullptr;

// Tail-jumped into with the exact register/stack state (RCX/RDX/R8/R9/stack-XMM) the
// real function's 5+ callers set up -- same signature as the real function (matching
// Ghidra's own decompiled signature shape: two pointers, a qword, a dword, a stack
// float), so the compiler-generated prologue sees a normal call-entry stack.
static void CallJointRemapSetupSafe(int64_t param_1, int64_t param_2, uint64_t param_3, uint32_t param_4, float param_5) {
    __try {
        g_jointRemapSetupOriginalTrampoline(param_1, param_2, param_3, param_4, param_5);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        char msg[192];
        snprintf(msg, sizeof(msg),
            "JointRemapSetupGuard: exception during motion-transition setup, skipping for this entity/frame (skipsSoFar=%llu)",
            (unsigned long long)g_jointRemapSetupGuardSkipCount + 1);
        LogDebug(msg);
        ++g_jointRemapSetupGuardSkipCount;
    }
}

static bool InstallJointRemapSetupGuardHook(unsigned long long hookAddr) {
    if (g_jointRemapSetupGuardHookInstalled) return true;

    unsigned char* hookPtr = (unsigned char*)(uintptr_t)hookAddr;
    // mov r11,rsp; push rbx; push r14
    static const unsigned char expected[6] = { 0x4C, 0x8B, 0xDC, 0x53, 0x41, 0x56 };
    if (memcmp(hookPtr, expected, 6) != 0) {
        LogDebug("InstallJointRemapSetupGuardHook: unexpected original bytes at hook address, aborting");
        return false;
    }

    void* replayCave = AllocateNear((void*)(uintptr_t)hookAddr, 4096);
    if (!replayCave) {
        LogDebug("InstallJointRemapSetupGuardHook: failed to allocate replay code cave");
        return false;
    }
    {
        unsigned char stub[16] = {};
        size_t off = 0;
        memcpy(stub + off, expected, 6); off += 6; // replay original 6 bytes
        stub[off++] = 0xE9; // jmp rel32 -> hookAddr+6 (rest of the real function body)
        size_t jmpOperand = off; off += 4;
        size_t stubLen = off;
        uintptr_t caveBase = (uintptr_t)replayCave;
        int32_t jmpRel = (int32_t)((int64_t)(hookAddr + 6) - (int64_t)(caveBase + jmpOperand + 4));
        memcpy(stub + jmpOperand, &jmpRel, 4);
        memcpy(replayCave, stub, stubLen);
        FlushInstructionCache(GetCurrentProcess(), replayCave, stubLen);
    }
    g_jointRemapSetupOriginalTrampoline = (JointRemapSetupOriginalFn)(uintptr_t)replayCave;

    void* redirectCave = AllocateNear((void*)(uintptr_t)hookAddr, 4096);
    if (!redirectCave) {
        LogDebug("InstallJointRemapSetupGuardHook: failed to allocate redirect code cave");
        return false;
    }
    {
        unsigned char stub[16] = {};
        size_t off = 0;
        uint64_t wrapperAddr = (uint64_t)(uintptr_t)&CallJointRemapSetupSafe;
        stub[off++] = 0x48; stub[off++] = 0xB8; // mov rax, wrapperAddr
        memcpy(stub + off, &wrapperAddr, 8); off += 8;
        stub[off++] = 0xFF; stub[off++] = 0xE0; // jmp rax
        size_t stubLen = off;
        memcpy(redirectCave, stub, stubLen);
        FlushInstructionCache(GetCurrentProcess(), redirectCave, stubLen);
    }

    unsigned char patch[6];
    patch[0] = 0xE9;
    int32_t hookRel = (int32_t)((int64_t)(uintptr_t)redirectCave - (int64_t)(hookAddr + 5));
    memcpy(patch + 1, &hookRel, 4);
    patch[5] = 0x90; // nop, pad the 6th original byte

    std::vector<HANDLE> threads = SuspendOtherThreads();

    DWORD oldProtect = 0;
    VirtualProtect((void*)(uintptr_t)hookAddr, 6, PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((void*)(uintptr_t)hookAddr, patch, 6);
    VirtualProtect((void*)(uintptr_t)hookAddr, 6, oldProtect, &oldProtect);

    FlushInstructionCache(GetCurrentProcess(), (void*)(uintptr_t)hookAddr, 6);

    ResumeThreads(threads);

    g_jointRemapSetupGuardHookInstalled = true;
    LogDebug("InstallJointRemapSetupGuardHook: installed successfully");
    return true;
}

// --- STATUS-EFFECT TEXT SLOT STALE-POINTER FIX (2026-08-05) -- properly re-derive, not a guard ---
// Root cause (static tracing, confirmed against two live crashes at the identical instruction):
// fnc_status_effect_activate_by_type resolves a handle from its per-status-type resource table and
// caches the RESULT -- a raw resolved pointer, not the handle -- into a text-display slot's own
// +0x98 field, once, at activation time. FUN_1401eba70 (crashed twice today, RVA 0x1EBAAB both
// times) later dereferences that cached pointer directly, never re-resolving it. Every OTHER
// resource access traced this whole session re-resolves its handle fresh on every use; this is the
// one place found that doesn't, which is why it alone goes stale when the underlying memory is
// later reused for anything else -- the handle itself would still resolve correctly, only the
// frozen-in-time raw pointer doesn't.
//
// Real fix, not a try/except: capture the ORIGINAL handle (not the resolved pointer) at the moment
// it's minted into a slot, keep it in our own per-slot side table (256 slots, indexed by
// (slotAddr-tableBase)/0xE0 -- can't safely repurpose bytes in the game's own struct without
// knowing the full layout), and have the confirmed crash read-site re-resolve fresh from that
// handle every time instead of trusting the cached pointer -- exactly matching how the rest of the
// engine already behaves. Falls back to the original cached pointer for any slot this doesn't have
// a captured handle for, so untracked slots behave exactly as before (no regression risk).
static const uint64_t TEXT_SLOT_STRIDE = 0xE0;
static const int TEXT_SLOT_COUNT = 256;
static uint32_t g_textSlotHandles[TEXT_SLOT_COUNT] = {};
static bool g_textSlotHandleValid[TEXT_SLOT_COUNT] = {};
static volatile uint32_t g_pendingStatusEffectHandle = 0;

static volatile uint64_t g_textSlotFreshResolveCount = 0;
static volatile uint64_t g_textSlotFreshResolveDivergedCount = 0;

// Called from the capture hook, right before fnc_status_effect_activate_by_type resolves the
// handle into a raw pointer -- just remembers it for the record hook a few instructions later in
// the same, non-reentrant activation call.
static void CaptureStatusEffectHandle(uint32_t handle) {
    g_pendingStatusEffectHandle = handle;
}

// Called from the record hook, right after the game stores the resolved pointer into slotPtr+0x98
// -- files the (slot -> original handle) mapping away in our own side table.
static void RecordTextSlotHandle(uint64_t slotPtr) {
    if (g_textSlotTableBase == 0 || slotPtr < g_textSlotTableBase) return;
    uint64_t idx = (slotPtr - g_textSlotTableBase) / TEXT_SLOT_STRIDE;
    if (idx >= (uint64_t)TEXT_SLOT_COUNT) return;
    g_textSlotHandles[idx] = g_pendingStatusEffectHandle;
    g_textSlotHandleValid[idx] = true;
}

// Called from the fresh-resolve hook at FUN_1401eba70's entry. cachedRawPtr is whatever the game
// itself already loaded from slotPtr+0x98 -- used as the fallback for any slot we have no captured
// handle for (never captured, or captured by a code path other than fnc_status_effect_activate_by_type).
static uint64_t ResolveTextSlotHandleFresh(uint64_t slotPtr, uint64_t cachedRawPtr) {
    if (g_textSlotTableBase != 0 && slotPtr >= g_textSlotTableBase && g_resolveHandleFnAddrForTextSlot != 0) {
        uint64_t idx = (slotPtr - g_textSlotTableBase) / TEXT_SLOT_STRIDE;
        if (idx < (uint64_t)TEXT_SLOT_COUNT && g_textSlotHandleValid[idx]) {
            typedef uint64_t (*ResolveFn)(uint32_t);
            ResolveFn resolveFn = (ResolveFn)(uintptr_t)g_resolveHandleFnAddrForTextSlot;
            uint64_t fresh = resolveFn(g_textSlotHandles[idx]);
            ++g_textSlotFreshResolveCount;
            if (fresh != cachedRawPtr) {
                char msg[192];
                snprintf(msg, sizeof(msg),
                    "TextSlotFreshResolve: slot=0x%llX cachedPtr=0x%llX freshPtr=0x%llX DIVERGED -- "
                    "cached pointer was stale, fresh resolve avoided a likely crash "
                    "(divergedSoFar=%llu, totalSoFar=%llu)",
                    (unsigned long long)slotPtr, (unsigned long long)cachedRawPtr,
                    (unsigned long long)fresh, (unsigned long long)g_textSlotFreshResolveDivergedCount + 1,
                    (unsigned long long)g_textSlotFreshResolveCount);
                LogDebug(msg);
                ++g_textSlotFreshResolveDivergedCount;
            }
            return fresh;
        }
    }
    return cachedRawPtr;
}

// Hooks "MOV ECX,[RBX]" (2) + "CALL fnc_resolve_resource_handle" (5) inside
// fnc_status_effect_activate_by_type, right where it loads the per-status-type handle it's about
// to resolve and cache. Purely additive -- replicates the original load+call exactly, just taps
// the handle value on the way through. RBX/the call's behavior are otherwise untouched.
static bool InstallTextSlotHandleCaptureHook(unsigned long long hookAddr, unsigned long long resumeAddr) {
    static bool installed = false;
    if (installed) return true;

    unsigned char* hookPtr = (unsigned char*)(uintptr_t)hookAddr;
    static const unsigned char expected[7] = {
        0x8B, 0x0B,                                 // mov ecx, [rbx]
        0xE8, 0x6C, 0xAC, 0x1A, 0x00                // call fnc_resolve_resource_handle (this build's rel32)
    };
    if (memcmp(hookPtr, expected, 7) != 0) {
        LogDebug("InstallTextSlotHandleCaptureHook: unexpected original bytes at hook address, aborting");
        return false;
    }

    void* cave = AllocateNear((void*)(uintptr_t)hookAddr, 4096);
    if (!cave) {
        LogDebug("InstallTextSlotHandleCaptureHook: failed to allocate a nearby code cave");
        return false;
    }

    unsigned char stub[64] = {};
    size_t off = 0;
    uint64_t captureFnAddr = (uint64_t)(uintptr_t)&CaptureStatusEffectHandle;
    uint64_t resolveHandleFnAddr = g_resolveHandleFnAddrForTextSlot;

    stub[off++] = 0x8B; stub[off++] = 0x0B; // mov ecx, [rbx]  (arg for the capture call)

    stub[off++] = 0x48; stub[off++] = 0x83; stub[off++] = 0xEC; stub[off++] = 0x20; // sub rsp, 0x20
    stub[off++] = 0x48; stub[off++] = 0xB8; // mov rax, captureFnAddr
    memcpy(stub + off, &captureFnAddr, 8); off += 8;
    stub[off++] = 0xFF; stub[off++] = 0xD0; // call rax
    stub[off++] = 0x48; stub[off++] = 0x83; stub[off++] = 0xC4; stub[off++] = 0x20; // add rsp, 0x20

    stub[off++] = 0x8B; stub[off++] = 0x0B; // mov ecx, [rbx]  (re-read; RBX unchanged, replicates original arg)

    stub[off++] = 0x48; stub[off++] = 0xB8; // mov rax, resolveHandleFnAddr
    memcpy(stub + off, &resolveHandleFnAddr, 8); off += 8;
    stub[off++] = 0xFF; stub[off++] = 0xD0; // call rax  (replicates original CALL fnc_resolve_resource_handle)

    stub[off++] = 0xE9; // jmp rel32 (patched below) -> resumeAddr
    size_t jmpOperand = off; off += 4;

    size_t stubLen = off;
    uintptr_t caveBase = (uintptr_t)cave;
    int32_t jmpRel = (int32_t)((int64_t)resumeAddr - (int64_t)(caveBase + jmpOperand + 4));
    memcpy(stub + jmpOperand, &jmpRel, 4);

    unsigned char patch[7];
    patch[0] = 0xE9;
    int32_t hookRel = (int32_t)((int64_t)caveBase - (int64_t)(hookAddr + 5));
    memcpy(patch + 1, &hookRel, 4);
    patch[5] = 0x90; patch[6] = 0x90;

    std::vector<HANDLE> threads = SuspendOtherThreads();
    memcpy(cave, stub, stubLen);
    DWORD oldProtect = 0;
    VirtualProtect((void*)(uintptr_t)hookAddr, 7, PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((void*)(uintptr_t)hookAddr, patch, 7);
    VirtualProtect((void*)(uintptr_t)hookAddr, 7, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), (void*)(uintptr_t)hookAddr, 7);
    FlushInstructionCache(GetCurrentProcess(), cave, stubLen);
    ResumeThreads(threads);

    installed = true;
    LogDebug("InstallTextSlotHandleCaptureHook: installed successfully");
    return true;
}

// Hooks "MOV [RCX+0x98],R15" (7 bytes) inside fnc_status_effect_activate_by_type, right where the
// resolved pointer gets stored into the slot. Replicates the store, then additionally records
// (slot=rcx -> the handle CaptureStatusEffectHandle saw a few instructions earlier) into our side
// table. RCX isn't read again by the original code until it's freshly reloaded from the stack, so
// it's safe to let our own call clobber it.
static bool InstallTextSlotHandleRecordHook(unsigned long long hookAddr, unsigned long long resumeAddr) {
    static bool installed = false;
    if (installed) return true;

    unsigned char* hookPtr = (unsigned char*)(uintptr_t)hookAddr;
    static const unsigned char expected[7] = {
        0x4C, 0x89, 0xB9, 0x98, 0x00, 0x00, 0x00 // mov [rcx+0x98], r15
    };
    if (memcmp(hookPtr, expected, 7) != 0) {
        LogDebug("InstallTextSlotHandleRecordHook: unexpected original bytes at hook address, aborting");
        return false;
    }

    void* cave = AllocateNear((void*)(uintptr_t)hookAddr, 4096);
    if (!cave) {
        LogDebug("InstallTextSlotHandleRecordHook: failed to allocate a nearby code cave");
        return false;
    }

    unsigned char stub[48] = {};
    size_t off = 0;
    uint64_t recordFnAddr = (uint64_t)(uintptr_t)&RecordTextSlotHandle;

    stub[off++] = 0x4C; stub[off++] = 0x89; stub[off++] = 0xB9; // mov [rcx+0x98], r15
    stub[off++] = 0x98; stub[off++] = 0x00; stub[off++] = 0x00; stub[off++] = 0x00;

    stub[off++] = 0x48; stub[off++] = 0x83; stub[off++] = 0xEC; stub[off++] = 0x20; // sub rsp, 0x20
    stub[off++] = 0x48; stub[off++] = 0xB8; // mov rax, recordFnAddr
    memcpy(stub + off, &recordFnAddr, 8); off += 8;
    stub[off++] = 0xFF; stub[off++] = 0xD0; // call rax  (rcx already = slot ptr)
    stub[off++] = 0x48; stub[off++] = 0x83; stub[off++] = 0xC4; stub[off++] = 0x20; // add rsp, 0x20

    stub[off++] = 0xE9; // jmp rel32 (patched below) -> resumeAddr
    size_t jmpOperand = off; off += 4;

    size_t stubLen = off;
    uintptr_t caveBase = (uintptr_t)cave;
    int32_t jmpRel = (int32_t)((int64_t)resumeAddr - (int64_t)(caveBase + jmpOperand + 4));
    memcpy(stub + jmpOperand, &jmpRel, 4);

    unsigned char patch[7];
    patch[0] = 0xE9;
    int32_t hookRel = (int32_t)((int64_t)caveBase - (int64_t)(hookAddr + 5));
    memcpy(patch + 1, &hookRel, 4);
    patch[5] = 0x90; patch[6] = 0x90;

    std::vector<HANDLE> threads = SuspendOtherThreads();
    memcpy(cave, stub, stubLen);
    DWORD oldProtect = 0;
    VirtualProtect((void*)(uintptr_t)hookAddr, 7, PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((void*)(uintptr_t)hookAddr, patch, 7);
    VirtualProtect((void*)(uintptr_t)hookAddr, 7, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), (void*)(uintptr_t)hookAddr, 7);
    FlushInstructionCache(GetCurrentProcess(), cave, stubLen);
    ResumeThreads(threads);

    installed = true;
    LogDebug("InstallTextSlotHandleRecordHook: installed successfully");
    return true;
}

// Hooks "MOV RSI,[RCX+0x98]" (7 bytes) at FUN_1401eba70's entry -- the confirmed crash site (RVA
// 0x1EBAAB, hit twice live 2026-08-05). Replaces the raw cached-pointer load with a call to
// ResolveTextSlotHandleFresh(slotPtr=rcx, cachedRawPtr=[rcx+0x98]), which re-resolves fresh from
// our captured handle when available, or falls back to the exact original behavior otherwise. RCX
// (param_1) must survive to the resume point (read again at "MOV R11,[RCX+0xa0]" two instructions
// later) -- stashed in RBX, which is free scratch here (its incoming value is already preserved by
// the function's own prologue via a MOV into shadow space, not a register the function itself
// needs again until it's freshly overwritten much later).
static bool InstallTextSlotFreshResolveHook(unsigned long long hookAddr, unsigned long long resumeAddr) {
    static bool installed = false;
    if (installed) return true;

    unsigned char* hookPtr = (unsigned char*)(uintptr_t)hookAddr;
    static const unsigned char expected[7] = {
        0x48, 0x8B, 0xB1, 0x98, 0x00, 0x00, 0x00 // mov rsi, [rcx+0x98]
    };
    if (memcmp(hookPtr, expected, 7) != 0) {
        LogDebug("InstallTextSlotFreshResolveHook: unexpected original bytes at hook address, aborting");
        return false;
    }

    void* cave = AllocateNear((void*)(uintptr_t)hookAddr, 4096);
    if (!cave) {
        LogDebug("InstallTextSlotFreshResolveHook: failed to allocate a nearby code cave");
        return false;
    }

    unsigned char stub[48] = {};
    size_t off = 0;
    uint64_t resolveFreshFnAddr = (uint64_t)(uintptr_t)&ResolveTextSlotHandleFresh;

    stub[off++] = 0x48; stub[off++] = 0x8B; stub[off++] = 0xB1; // mov rsi, [rcx+0x98]  (fallback value)
    stub[off++] = 0x98; stub[off++] = 0x00; stub[off++] = 0x00; stub[off++] = 0x00;

    stub[off++] = 0x48; stub[off++] = 0x89; stub[off++] = 0xCB; // mov rbx, rcx  (stash param_1)
    stub[off++] = 0x48; stub[off++] = 0x89; stub[off++] = 0xF2; // mov rdx, rsi  (arg2 = cachedRawPtr)
    // arg1 = rcx, already correct

    stub[off++] = 0x48; stub[off++] = 0x83; stub[off++] = 0xEC; stub[off++] = 0x20; // sub rsp, 0x20
    stub[off++] = 0x48; stub[off++] = 0xB8; // mov rax, resolveFreshFnAddr
    memcpy(stub + off, &resolveFreshFnAddr, 8); off += 8;
    stub[off++] = 0xFF; stub[off++] = 0xD0; // call rax
    stub[off++] = 0x48; stub[off++] = 0x83; stub[off++] = 0xC4; stub[off++] = 0x20; // add rsp, 0x20

    stub[off++] = 0x48; stub[off++] = 0x89; stub[off++] = 0xC6; // mov rsi, rax  (result, matches original target reg)
    stub[off++] = 0x48; stub[off++] = 0x89; stub[off++] = 0xD9; // mov rcx, rbx  (restore param_1)

    stub[off++] = 0xE9; // jmp rel32 (patched below) -> resumeAddr
    size_t jmpOperand = off; off += 4;

    size_t stubLen = off;
    uintptr_t caveBase = (uintptr_t)cave;
    int32_t jmpRel = (int32_t)((int64_t)resumeAddr - (int64_t)(caveBase + jmpOperand + 4));
    memcpy(stub + jmpOperand, &jmpRel, 4);

    unsigned char patch[7];
    patch[0] = 0xE9;
    int32_t hookRel = (int32_t)((int64_t)caveBase - (int64_t)(hookAddr + 5));
    memcpy(patch + 1, &hookRel, 4);
    patch[5] = 0x90; patch[6] = 0x90;

    std::vector<HANDLE> threads = SuspendOtherThreads();
    memcpy(cave, stub, stubLen);
    DWORD oldProtect = 0;
    VirtualProtect((void*)(uintptr_t)hookAddr, 7, PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((void*)(uintptr_t)hookAddr, patch, 7);
    VirtualProtect((void*)(uintptr_t)hookAddr, 7, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), (void*)(uintptr_t)hookAddr, 7);
    FlushInstructionCache(GetCurrentProcess(), cave, stubLen);
    ResumeThreads(threads);

    installed = true;
    LogDebug("InstallTextSlotFreshResolveHook: installed successfully");
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
