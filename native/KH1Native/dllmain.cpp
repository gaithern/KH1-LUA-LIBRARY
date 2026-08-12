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
// The state byte climbs 0..6 across frames as an async asset load progresses; 6 means fully
// loaded and safe to construct from. Everything below is "still in flight" -- the value the
// existing readiness check has always compared against (`*stateAddr != 6`), named here because
// ReuseSlotIsSafe now needs it too.
static const uint8_t SPECIES_SLOT_STATE_READY = 6;
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
// --- ROOM BLACKLIST ---
// Rooms where spawn_enemy refuses outright, matched on world + an INCLUSIVE area range.
// The Set number is deliberately not part of the match -- see the check site's comment.
//
// Add an entry as { world, areaMin, areaMax, "label", "why" }. For a single area, pass the same
// number for min and max. `why` is written into kh1_native.log on every refusal, so record the
// actual reason rather than "requested" -- a future session reading this needs to know whether
// the room can ever be un-blacklisted.
//
// This replaces the old COLD_SPAWN_CRASH_WORLD/AREA/SET single-room guard (removed 2026-08-05
// once the bucket-exhaustion root cause was fixed). That one was a hardcoded triple compared
// inline; this is a table so entries can be added without touching the check.
struct BlacklistedRoomRange {
    int32_t world;
    int32_t areaMin;
    int32_t areaMax;
    const char* label;
    const char* why;
};
static const BlacklistedRoomRange g_spawnRoomBlacklist[] = {
    // Requested by the user 2026-08-11. World 16 areas 3..13, every set.
    // NOT diagnosed as a crash -- added as a deliberate content decision, not a workaround for a
    // known bug, so do not remove it on the grounds that "the underlying crash is fixed".
    { 16, 3, 13, "world 16 areas 3-13", "spawning disabled here by request" },
};

struct TriggeredLoadEntry {
    char modelPath[64];
    uint8_t species;
    // How many consecutive slots this creature's load claimed (record+0x56). Needed so the
    // room-change reclamation below knows the full extent of what we took, not just its first slot.
    uint8_t runLen;
    // LOAD-IDENTITY TOKEN (2026-08-11) -- the loadedSpeciesPtrTable pointer this slot held
    // immediately after OUR load triggered. See LoadedSlotToken/TriggeredLoadStillOurs.
    //
    // Why: before this, "is this run still ours?" was answered by model filename + start index
    // + owner bytes. None of those distinguish OUR load from a NATIVE one that happens to have
    // the same model at the same start index -- they would be byte-identical. That matters
    // because ReclaimOwnSlotRuns runs LAZILY (only on the next spawn_enemy call, by which point
    // the new room has already loaded and re-tiled the slot table), so it can walk a stale
    // entry against a freshly-loaded room and release a run the ROOM owns, wiping its resource
    // blob. Not observed in any log so far -- it needs the same model AND the same start index
    // to collide -- but nothing in the old checks prevents it.
    //
    // A pointer is the right shape for this: a fresh load allocates new backing memory, so a
    // reload into the same slot changes it. Cross-checked against real log data before being
    // written -- species 57 carried rec+0x60=0xa6a67680, then 0xa6a67280 after a reload, then
    // 0xa6a67280 again on a reuse with no reload.
    unsigned long long loadToken;
    // GetTickCount64() when this load was recorded. Only used to report a run's age in the
    // release log, so a freeze that follows a release can be judged against the measured
    // ~20-second lifetime of a spawned creature -- releasing a 3-second-old run is far more
    // suspicious than releasing a 5-minute-old one.
    unsigned long long recordedTick;
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

// The loadedSpeciesPtrTable pointer a species slot currently holds. This is the slot's own
// per-load resource pointer (the table entry itself -- every other field in this structure is
// addressed at a NEGATIVE offset from it), so it changes whenever the slot is reloaded.
// Used as the load-identity token; see TriggeredLoadEntry::loadToken.
static unsigned long long LoadedSlotToken(unsigned long long base, unsigned long long loadedPtrTableRva,
                                          uint8_t species) {
    return *(volatile unsigned long long*)(uintptr_t)(base + loadedPtrTableRva +
        (size_t)species * LOADED_SPECIES_STRIDE);
}

// Is this recorded entry still describing OUR load, rather than a native one that happens to
// sit at the same slot with the same model? Compares the load-identity token captured when we
// triggered the load against what the slot holds now.
//
// FAILS SAFE BY DESIGN: a mismatch means "don't touch it", so if the token turns out to be less
// stable than believed, the cost is runs we decline to reclaim -- i.e. today's capacity
// behaviour -- and never a wrongly-released run. That asymmetry is deliberate; a false positive
// here wipes a live resource blob, a false negative only leaks a slot.
static bool TriggeredLoadStillOurs(unsigned long long base, unsigned long long loadedPtrTableRva,
                                   const TriggeredLoadEntry& entry) {
    if (entry.loadToken == 0) return false; // not captured yet -- refuse to guess
    return LoadedSlotToken(base, loadedPtrTableRva, entry.species) == entry.loadToken;
}

// RecordTriggeredLoad runs at the moment the load is TRIGGERED, which is several frames before
// the load finishes -- the slot's pointer is typically still 0 then (the log's usual "asset load
// for species=N not ready yet (state=0)" line). Capturing 0 would leave the entry permanently
// unmatched, and since TriggeredLoadStillOurs fails safe that would silently disable ALL
// reclamation. So the token is filled in on the first later call that finds the slot populated,
// which is any spawn that gets far enough to construct.
static void EnsureTriggeredLoadToken(unsigned long long base, unsigned long long loadedPtrTableRva,
                                     uint8_t species) {
    for (int i = 0; i < g_triggeredLoadCount; ++i) {
        if (g_triggeredLoads[i].species != species) continue;
        if (g_triggeredLoads[i].loadToken != 0) return;
        unsigned long long tok = LoadedSlotToken(base, loadedPtrTableRva, species);
        if (tok == 0) return; // still not populated -- try again on a later call
        g_triggeredLoads[i].loadToken = tok;
        char msg[200];
        snprintf(msg, sizeof(msg),
            "spawn_enemy: captured load token for species=%d (%s) once its slot was populated: 0x%llX",
            (int)species, g_triggeredLoads[i].modelPath, tok);
        LogDebug(msg);
        return;
    }
}

// Entries stay valid for the rest of the session, even once the load
// completes.
static void RecordTriggeredLoad(unsigned long long base, unsigned long long loadedPtrTableRva,
                                const char* modelPath, uint8_t species, uint8_t runLen) {
    if (g_triggeredLoadCount >= MAX_TRIGGERED_LOADS) return;
    TriggeredLoadEntry& entry = g_triggeredLoads[g_triggeredLoadCount];
    strncpy_s(entry.modelPath, modelPath, _TRUNCATE);
    entry.species = species;
    entry.runLen = runLen;
    entry.loadToken = LoadedSlotToken(base, loadedPtrTableRva, species);
    entry.recordedTick = GetTickCount64();
    g_triggeredLoadCount++;

    char msg[200];
    snprintf(msg, sizeof(msg),
        "spawn_enemy: recorded triggered load species=%d runLen=%d model=%s loadToken=0x%llX",
        (int)species, (int)runLen, modelPath, entry.loadToken);
    LogDebug(msg);
}

// --- ROOM-CHANGE SLOT RECLAMATION (2026-08-10) ---
// Fixes the hard ~6-8-distinct-creatures-per-session ceiling. Each creature claims a run of 3-5
// consecutive species slots, the safe allocation window is only 20..47, and nothing ever released a
// run -- so spawning eventually refused for the rest of the session regardless of room changes.
//
// The game itself never hits this because it does all of its slot reuse by EVICTION at ROOM-LOAD
// time: fnc_async_load_job_callback compares the slot's cached model filename and, on mismatch,
// evicts and reloads in place. That is safe for the engine purely because nothing from the previous
// room is still alive at that moment. We spawn mid-room while creatures ARE alive, which is why we
// could never simply overwrite -- doing so is exactly the corruption fixed earlier today.
//
// This borrows the engine's own invariant instead of guessing whether a creature is still alive:
// on a room CHANGE, every creature we spawned is already gone (session 4 proved a synthetic entity
// does not survive a room reload -- the entity pool and placement table are both rebuilt from the
// new room's data), so releasing the runs we claimed in the previous room is safe for exactly the
// same reason the engine's own eviction is safe. No dependence on MarkSpeciesInUseByLiveEntities,
// which this file's own comments already flag as untrustworthy for precisely these creatures.
//
// TWO safety checks, because the new room may legitimately have re-taken some of those slots for
// its OWN natives before our first spawn call in it (rooms reuse the same low local slot numbers):
//   1. the run's PRIMARY slot must still hold OUR creature's cached model filename -- if the new
//      room reloaded that slot, the name differs and we leave the whole run alone;
//   2. per member slot, the owner byte must still equal our run's species -- a slot already
//      re-claimed by something else is skipped individually.
// Only ever resets slots to their pristine all-zero (BSS/"never touched") state, which is exactly
// what FindFreeLoadedSlot treats as free. Never writes a non-zero value into game state.
static int32_t g_lastSpawnRoomWorld = -1;
static int32_t g_lastSpawnRoomArea = -1;
static int32_t g_lastSpawnRoomSet = -1;
static bool g_lastSpawnRoomValid = false;

static void ReclaimOwnSlotRuns(unsigned long long base, unsigned long long loadedPtrTableRva,
                               unsigned long long releaseFnRva) {
    if (releaseFnRva == 0) {
        LogDebug("spawn_enemy: room changed but no release-function RVA configured for this build -- cannot reclaim slots");
        g_triggeredLoadCount = 0;
        return;
    }
    int reclaimedRuns = 0, reclaimedSlots = 0, skippedRuns = 0;
    for (int i = 0; i < g_triggeredLoadCount; ++i) {
        const TriggeredLoadEntry& entry = g_triggeredLoads[i];
        int runLen = (int)entry.runLen;
        if (runLen < 1) runLen = 1;

        // Identity first: filename+index alone cannot tell our load from a native one that
        // landed on the same slot with the same model (see TriggeredLoadEntry::loadToken).
        // This check matters most HERE, because this function runs lazily after a room change
        // -- by which point the new room has already re-tiled the table.
        if (!TriggeredLoadStillOurs(base, loadedPtrTableRva, entry)) {
            char idMsg[200];
            snprintf(idMsg, sizeof(idMsg),
                "spawn_enemy: slot %d (%s) no longer carries our load token (0x%llX, now 0x%llX) -- "
                "a different load owns it now, leaving it alone",
                (int)entry.species, entry.modelPath, entry.loadToken,
                LoadedSlotToken(base, loadedPtrTableRva, entry.species));
            LogDebug(idMsg);
            ++skippedRuns;
            continue;
        }

        unsigned long long primaryPtr = base + loadedPtrTableRva + (size_t)entry.species * LOADED_SPECIES_STRIDE;
        const char* cachedName = (const char*)(uintptr_t)(primaryPtr + LOADED_SPECIES_MODEL_NAME_OFFSET_FROM_PTR);
        if (strncmp(cachedName, entry.modelPath, LOADED_SPECIES_MODEL_NAME_SIZE) != 0) {
            ++skippedRuns; // something else owns this slot now -- do not touch it
            continue;
        }
        // Confirm we still own the whole run before releasing any of it -- a partial release would
        // leave the engine's own bookkeeping inconsistent.
        bool stillOurs = true;
        for (int k = 0; k < runLen; ++k) {
            int slot = (int)entry.species + k;
            if (slot > RESOURCE_BLOB_MAX_SPECIES) { stillOurs = false; break; }
            volatile uint8_t* owner = (volatile uint8_t*)(uintptr_t)(base + loadedPtrTableRva +
                LOADED_SPECIES_OWNER_OFFSET_FROM_PTR + (size_t)slot * LOADED_SPECIES_STRIDE);
            if (*owner != entry.species) { stillOurs = false; break; }
        }
        if (!stillOurs) { ++skippedRuns; continue; }

        // Hand it back with the engine's OWN release routine rather than zeroing bytes by hand:
        // it writes the real 0xFF unclaimed sentinel and wipes the species' resource-blob range,
        // which a hand-rolled reset silently skipped.
        unsigned long long releaseArgs[2] = { (unsigned long long)entry.species, (unsigned long long)runLen };
        unsigned long long releaseResult = 0;
        if (!SafeCall(base + releaseFnRva, releaseArgs, 2, releaseResult)) {
            char msg[192];
            snprintf(msg, sizeof(msg),
                "spawn_enemy: fnc_release_species_slot_run(%u,%d) threw -- leaving that run claimed",
                (unsigned)entry.species, runLen);
            LogDebug(msg);
            ++skippedRuns;
            continue;
        }
        reclaimedSlots += runLen;
        ++reclaimedRuns;
    }

    // Drop all of our triggered-load bookkeeping regardless: any entry we could not reclaim is
    // stale anyway (its slot belongs to something else now), and keeping it would let
    // FindTriggeredLoad hand a creature a slot it no longer owns.
    g_triggeredLoadCount = 0;

    char msg[224];
    snprintf(msg, sizeof(msg),
        "spawn_enemy: room changed -- reclaimed %d of our own slot runs (%d slots) for reuse, left %d "
        "alone because the new room re-took them; triggered-load bookkeeping cleared",
        reclaimedRuns, reclaimedSlots, skippedRuns);
    LogDebug(msg);
}

// --- ON-DEMAND SLOT RECLAMATION (2026-08-11) ---
// ReclaimOwnSlotRuns above only fires on a ROOM CHANGE, so within a single room the 20..63
// window fills monotonically and spawning dies with "no free run of N consecutive species
// slots". Live-observed: 42 of 44 slots claimed, every further spawn refused, and a restart
// was the only recovery.
//
// The measurement that makes this fixable: across 53 spawns in one session, every entity was
// activated at construct (+0x374 SET, 53/53), but the follow-up rechecks found +0x374 fully
// ZEROED 338 times vs still-set 13 -- i.e. the game frees our spawned creatures within roughly
// twenty seconds. Their slot runs are therefore almost always pure dead weight, still claimed
// on behalf of an entity that no longer exists. Reclaiming those on demand is what turns a
// hard session cap into a recycling pool.
//
// Only ever releases a run that (a) has NO live entity anywhere in it, (b) still carries our
// own cached model filename, and (c) is still owned end-to-end by us -- the exact ownership
// checks ReclaimOwnSlotRuns already uses, since a partial release would leave the engine's
// bookkeeping inconsistent. Uses the engine's own fnc_release_species_slot_run, so the
// resource-blob range is wiped the way the engine does it (a hand-rolled reset skipped that,
// and stale blob content is itself a documented crash cause -- sessions 9-12).
// Returns the number of slots actually freed.
// DISABLED BY DEFAULT PENDING AN A/B TEST (2026-08-11). Shipped enabled, and the very next
// play session froze silently ~25s later -- log's last line a clean post-construct, file never
// grew again, not one guard hit. That is the exact signature of the long-standing never-root-
// caused freeze (sessions 7/8/16, and the 2026-08-10 post-defeat freeze), so this may well be
// unrelated. But it CANNOT be ruled out as the cause, for a specific reason:
// fnc_release_species_slot_run WIPES the species' resource-blob range, and the liveness test
// gating it (MarkSpeciesInUseByLiveEntities) is explicitly documented a few hundred lines above
// as unreliable -- "even if ... MarkSpeciesInUseByLiveEntities couldn't resolve the occupying
// entity's handle. That resolve failing is exactly the same failure mode as the crashes this
// DLL guards against elsewhere". An entity whose +0x134 fails to resolve is invisible to that
// scan, so its run would be freed and its blob wiped while it is still alive -- a textbook
// freeze. That warning existed before this function was written and should have gated it.
//
// Counter-evidence, which is why this is a flag rather than a deletion: the same blob-wiping
// release already runs on every room change via ReclaimOwnSlotRuns (live-confirmed working
// since 2026-08-10), so the OPERATION is not new here, only its timing.
//
// RE-ENABLED 2026-08-11 (second attempt), now that every release is additionally gated on the
// load-identity token (TriggeredLoadEntry::loadToken). READ THIS BEFORE READING THE RESULT:
//
// The token closes a DIFFERENT hole than the one suspected of causing the freeze above. It stops
// us releasing a run that is not ours at all -- a NATIVE run that happens to sit at the same slot
// with the same model, which the old filename+index+owner checks could not distinguish from our
// own load. That was a real reachable defect, but it is NOT the freeze mechanism described above.
//
// The freeze suspicion is that OUR OWN still-alive creature gets misjudged as dead, because
// MarkSpeciesInUseByLiveEntities cannot see an entity whose handle fails to resolve. The token
// does not help there at all: the entry genuinely IS ours, so the token matches, and the blob is
// wiped under a live creature exactly as before. Do not read "token added" as "freeze fixed".
//
// So this is deliberately a real A/B test, not a fix being shipped as safe. If the freeze
// reappears, the mechanism is now much easier to pin down: every release logs the species, model,
// token and age of the run immediately before fnc_release_species_slot_run is called, so the
// last line before the log stops naming a specific released run is strong evidence FOR this
// function, and its absence is strong evidence AGAINST (i.e. the unrelated long-standing freeze).
// The targeted follow-up if that happens is a grace period -- require a run to look dead for
// several consecutive checks before releasing it -- which attacks the actual suspected cause.
//
// Flip back to false to restore the pre-2026-08-11 behaviour and the ~7-creature-per-session slot
// cap; a refusal is strictly better than a freeze while the cause is unknown.
static const bool ENABLE_ON_DEMAND_SLOT_RECLAIM = true;

static int ReclaimDeadSlotRuns(unsigned long long base, unsigned long long loadedPtrTableRva,
                               unsigned long long releaseFnRva,
                               const bool* speciesInUseByLiveEntity) {
    if (!ENABLE_ON_DEMAND_SLOT_RECLAIM) {
        LogDebug("spawn_enemy: slot window full -- on-demand reclamation is DISABLED pending a "
                 "freeze A/B test (see ENABLE_ON_DEMAND_SLOT_RECLAIM), so this refusal stands");
        return 0;
    }
    if (releaseFnRva == 0) return 0;

    int reclaimedRuns = 0, reclaimedSlots = 0;
    int out = 0;
    for (int i = 0; i < g_triggeredLoadCount; ++i) {
        TriggeredLoadEntry entry = g_triggeredLoads[i]; // by value -- the array is compacted below
        int runLen = (int)entry.runLen;
        if (runLen < 1) runLen = 1;

        // "In use" MUST be tested across the whole run, not just its first slot.
        // MarkSpeciesInUseByLiveEntities derives a species by dividing the resolved blob pointer
        // by the 0x40000 stride, but a creature's blob spans its ENTIRE run -- so an entity whose
        // data sits partway in reports a run MEMBER index, never the run's owner. Testing only
        // entry.species would therefore miss a live creature and free a run out from under it.
        bool inUse = false;
        for (int k = 0; k < runLen; ++k) {
            int slot = (int)entry.species + k;
            if (slot >= 0 && slot <= RESOURCE_BLOB_MAX_SPECIES && speciesInUseByLiveEntity[slot]) {
                inUse = true;
                break;
            }
        }
        if (inUse) { g_triggeredLoads[out++] = entry; continue; }

        // Same identity gate as ReclaimOwnSlotRuns -- see TriggeredLoadEntry::loadToken.
        // Keep tracking the entry rather than dropping it: the slot is not ours to free, but
        // forgetting it would let FindTriggeredLoad hand this model that slot again later.
        if (!TriggeredLoadStillOurs(base, loadedPtrTableRva, entry)) {
            g_triggeredLoads[out++] = entry;
            continue;
        }

        unsigned long long primaryPtr = base + loadedPtrTableRva + (size_t)entry.species * LOADED_SPECIES_STRIDE;
        const char* cachedName = (const char*)(uintptr_t)(primaryPtr + LOADED_SPECIES_MODEL_NAME_OFFSET_FROM_PTR);
        if (strncmp(cachedName, entry.modelPath, LOADED_SPECIES_MODEL_NAME_SIZE) != 0) {
            g_triggeredLoads[out++] = entry; // something else owns it now -- keep tracking, don't touch
            continue;
        }
        bool stillOurs = true;
        for (int k = 0; k < runLen; ++k) {
            int slot = (int)entry.species + k;
            if (slot > RESOURCE_BLOB_MAX_SPECIES) { stillOurs = false; break; }
            volatile uint8_t* owner = (volatile uint8_t*)(uintptr_t)(base + loadedPtrTableRva +
                LOADED_SPECIES_OWNER_OFFSET_FROM_PTR + (size_t)slot * LOADED_SPECIES_STRIDE);
            if (*owner != entry.species) { stillOurs = false; break; }
        }
        if (!stillOurs) { g_triggeredLoads[out++] = entry; continue; }

        // Logged BEFORE the release, deliberately. fnc_release_species_slot_run wipes the
        // species' resource-blob range, and the freeze this function is on trial for leaves no
        // other trace -- the log simply stops with no guard hits. A line written after the call
        // would never appear for the one release that mattered. If the log's final line is this
        // one, this function is the prime suspect and `age` says how plausible "already dead"
        // was; if the log ends on an ordinary spawn line with no release logged, that points
        // away from this function and back at the long-standing unrelated freeze.
        {
            unsigned long long ageMs = GetTickCount64() - entry.recordedTick;
            char relMsg[224];
            snprintf(relMsg, sizeof(relMsg),
                "spawn_enemy: RELEASING dead run species=%d runLen=%d model=%s token=0x%llX age=%llus "
                "-- no live entity found in the run; blob range will be wiped",
                (int)entry.species, runLen, entry.modelPath, entry.loadToken, ageMs / 1000);
            LogDebug(relMsg);
        }

        unsigned long long releaseArgs[2] = { (unsigned long long)entry.species, (unsigned long long)runLen };
        unsigned long long releaseResult = 0;
        if (!SafeCall(base + releaseFnRva, releaseArgs, 2, releaseResult)) {
            char failMsg[192];
            snprintf(failMsg, sizeof(failMsg),
                "spawn_enemy: on-demand fnc_release_species_slot_run(%u,%d) threw -- leaving that run claimed",
                (unsigned)entry.species, runLen);
            LogDebug(failMsg);
            g_triggeredLoads[out++] = entry;
            continue;
        }
        reclaimedSlots += runLen;
        ++reclaimedRuns;
    }
    g_triggeredLoadCount = out;

    if (reclaimedRuns > 0) {
        char msg[224];
        snprintf(msg, sizeof(msg),
            "spawn_enemy: slot window was full -- reclaimed %d run(s) (%d slots) whose spawned "
            "creature is no longer alive; %d still-live run(s) left claimed",
            reclaimedRuns, reclaimedSlots, g_triggeredLoadCount);
        LogDebug(msg);
    }
    return reclaimedSlots;
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

// The engine's own recorded run length for a loaded species slot (the loader writes
// record+0x56's value here). Clamped to >=1 so a zeroed/garbage byte degrades to
// "just the primary slot" rather than skipping the run check entirely.
static int LoadedSlotRunLen(unsigned long long base, unsigned long long loadedPtrTableRva, uint8_t species) {
    volatile uint8_t* runLenAddr = (volatile uint8_t*)(uintptr_t)(base + loadedPtrTableRva +
        LOADED_SPECIES_RUNLEN_OFFSET_FROM_PTR + (size_t)species * LOADED_SPECIES_STRIDE);
    int runLen = (int)*runLenAddr;
    return runLen < 1 ? 1 : runLen;
}

// Is every slot of this species' run still owned by that species?
// Same end-to-end ownership test ReclaimOwnSlotRuns and FindFreeLoadedSlot already apply --
// factored out here so the REUSE paths can apply it too (see ReuseSlotIsSafe).
static bool SpeciesRunStillIntact(unsigned long long base, unsigned long long loadedPtrTableRva,
                                  uint8_t species, int runLen) {
    for (int k = 0; k < runLen; ++k) {
        int slot = (int)species + k;
        if (slot > RESOURCE_BLOB_MAX_SPECIES) return false;
        volatile uint8_t* owner = (volatile uint8_t*)(uintptr_t)(base + loadedPtrTableRva +
            LOADED_SPECIES_OWNER_OFFSET_FROM_PTR + (size_t)slot * LOADED_SPECIES_STRIDE);
        if (*owner != species) return false;
    }
    return true;
}

// Gate on both reuse paths, added 2026-08-11 after a live crash.
//
// FindTriggeredLoad and FindLoadedSlotByFilename both identify a slot by its PRIMARY entry
// alone -- a filename match plus (for the latter) state != 0. Neither re-checks that the
// rest of the creature's run is still ours, even though a creature's resource blob spans
// its WHOLE run. That gap crashed the game on 2026-08-11 08:06:39: species 57 was loaded
// with a 4-slot run (57..60) at 08:01:39; slot 60 was later handed to a different creature
// as the start of ITS run (60..63) at 08:05:49; then at 08:06:39 the filename scan found
// slot 57 still named xa_ex_2230.mdls and reused it with needsLoad=0 and the SAME minted
// handles as five minutes earlier. The blob-header validator passed legitimately -- it reads
// the PRIMARY slot's header, which was intact; the damage was in the run's tail. Construction
// then hit a skeleton-blend exception, a float-shaped bogus handle, and null keyframe lists,
// and the process died. (Bucket table was 34/64 at the time -- this was not exhaustion.)
//
// Returns false to mean "do not reuse", which lets the caller fall through to the fresh-load
// path rather than refusing outright, so this costs a reload rather than a failed spawn.
// The broken run stays claimed and leaks a slot -- accepted, since the alternative is
// releasing a run we have just proven we do not fully own.
static bool ReuseSlotIsSafe(unsigned long long base, unsigned long long loadedPtrTableRva,
                            uint8_t species, const char* modelPath, const char* via) {
    // A SLOT WHOSE LOAD IS STILL IN FLIGHT IS NOT A BROKEN RUN (fixed 2026-08-11).
    //
    // The ownership test below asks "is every slot of this run still owned by us?". That question
    // is only meaningful once the loader has actually CLAIMED the run. Asset loads here are
    // asynchronous -- the state byte climbs 0..6 across many frames -- and the owner bytes are not
    // written until it completes. Applying the test during that window made a still-loading run
    // look broken, so every retry frame abandoned it and triggered a FRESH load into the next free
    // slot. Measured consequence: bursts of 20-35 loads of the SAME model into consecutive slot
    // runs within a single second (xa_ex_2160 alone: 153 loads in one session), which exhausts the
    // slot window, litters memory with half-written copies of the same creature, and makes it a
    // lottery which copy an entity's handles end up bound to.
    //
    // So: only judge intactness once the slot reports fully loaded. While it is still loading,
    // trust our own triggered-load record and let the caller wait for it -- which is what the
    // "asset load not ready yet, caller should retry next frame" path already exists to do.
    volatile uint8_t* stateAddr = (volatile uint8_t*)(uintptr_t)(base + loadedPtrTableRva +
        LOADED_SPECIES_STATE_OFFSET_FROM_PTR + (size_t)species * LOADED_SPECIES_STRIDE);
    uint8_t state = *stateAddr;
    if (state != 0 && state < SPECIES_SLOT_STATE_READY) {
        return true; // load in progress -- not ours to second-guess yet
    }

    int runLen = LoadedSlotRunLen(base, loadedPtrTableRva, species);
    if (SpeciesRunStillIntact(base, loadedPtrTableRva, species, runLen)) return true;
    char msg[240];
    snprintf(msg, sizeof(msg),
        "spawn_enemy: slot %d (%s, found via %s) is NOT owned end-to-end any more -- part of its "
        "%d-slot run was re-taken by something else, so its blob is only partly ours. Refusing to "
        "reuse it; falling through to a fresh load.",
        (int)species, modelPath, via, runLen);
    LogDebug(msg);
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
// NOTE ON FALSE POSITIVES (2026-08-11): ResolvedModelMatches returns false for THREE different
// situations -- the handle is 0, the handle fails to resolve, or it resolves to a genuinely
// different model. Only the third is a real collision; the first two are "we could not tell",
// which this function conservatively also treats as a collision. That is the right default (do
// not squat on a slot we cannot vouch for) but it means a room full of unresolvable handles can
// make many slots look occupied. The diagnostic below exists to tell those cases apart, because
// the refusal text alone never could -- logged only once per (species, model) so a repeatedly
// retried spawn does not flood the file.
static bool RoomHasNativeSpecies(const uint8_t* table, int32_t count, uint8_t species, unsigned long long resolveFnAddr, const char* modelPath) {
    for (int32_t i = 0; i < count; ++i) {
        const uint8_t* rec = table + (size_t)i * PLACEMENT_RECORD_SIZE;
        if (rec[PLACEMENT_SPECIES_OFFSET] != species) continue;
        uint32_t modelHandle;
        memcpy(&modelHandle, rec + PLACEMENT_MODEL_HANDLE_OFFSET, 4);
        if (!ResolvedModelMatches(resolveFnAddr, modelHandle, modelPath)) {
            static uint8_t lastSpecies = 0xFF;
            static char lastModel[64] = {0};
            if (species != lastSpecies || strncmp(lastModel, modelPath, sizeof(lastModel)) != 0) {
                lastSpecies = species;
                strncpy_s(lastModel, modelPath, _TRUNCATE);
                // Resolve once more purely to report WHICH of the three cases this is.
                unsigned long long resolved = 0;
                const char* why;
                if (modelHandle == 0) {
                    why = "record's model handle is 0 (no model recorded)";
                } else {
                    unsigned long long args[1] = { (unsigned long long)modelHandle };
                    if (!SafeCall(resolveFnAddr, args, 1, resolved) || resolved == 0) {
                        why = "record's model handle did NOT resolve -- treated as occupied, but this is 'unknown', not a proven collision";
                    } else {
                        why = "record resolves to a genuinely different model -- a real collision";
                    }
                }
                char msg[256];
                snprintf(msg, sizeof(msg),
                    "spawn_enemy: room placement record #%d claims species=%d while we want %s -- %s "
                    "(handle=0x%08X, resolved=0x%llX)",
                    (int)i, (int)species, modelPath, why, modelHandle, resolved);
                LogDebug(msg);
            }
            return true;
        }
    }
    return false;
}

// Walks the live entity pool (pass 0 to start, pass the previous result to
// advance, 0 means done) and marks each entity's resource-handle as
// "species in use" -- catches an unrelated live entity using a slot with
// no placement-table record.
// BLINDNESS COUNTERS (2026-08-11). This scan is the ONLY thing stopping FindFreeLoadedSlot from
// allocating over a live creature and ReclaimDeadSlotRuns from releasing one out from under
// itself -- and it identifies an entity's species by RESOLVING entity+0x134. An entity whose
// handle does not resolve is therefore invisible to it, and this file has warned about exactly
// that ("the MarkSpeciesInUseByLiveEntities resolve scan cannot be trusted alone") without ever
// measuring how often it happens.
//
// A one-off census on 2026-08-11 found only 3 of 22 live entities resolving to a species. That is
// alarming at face value but ambiguous, because two very different cases were being lumped
// together, and the fix depends entirely on which one dominates:
//   noHandle           -- entity+0x134 == 0. Almost certainly not a creature at all (props,
//                         effects, doors, party members). Harmless; the scan SHOULD ignore these.
//   handleUnresolvable -- entity+0x134 != 0 but does not resolve into the species blob table.
//                         THIS is the dangerous case: a real creature the scan cannot see, whose
//                         slot we may then hand out or release.
//
// The second case matters most because it is self-reinforcing: resolve failure is the core
// symptom of the bug being chased, so the safety check goes blind precisely when handles start
// going bad -- i.e. exactly when it most needs to work.
//
// Counting rides the walk that already happens on every spawn_enemy call. No extra iteration, no
// timer, no sampling during teardown -- deliberately unlike the reverted LiveEntityCensus, whose
// timer-driven walk froze the game by running while the engine was tearing the pool down.
// Consecutive identical summaries are collapsed so the retry bursts during an asset load do not
// flood the file.
static void MarkSpeciesInUseByLiveEntities(unsigned long long base, unsigned long long entityIterFnRva,
                                             unsigned long long resolveHandleFnRva, unsigned long long speciesResourceTableRva,
                                             bool outInUse[RESOURCE_BLOB_MAX_SPECIES + 1]) {
    if (entityIterFnRva == 0 || speciesResourceTableRva == 0) return;
    unsigned long long tableBase = base + speciesResourceTableRva;
    unsigned long long entity = 0;
    unsigned long long iterArgs[1] = { 0 };
    if (!SafeCall(base + entityIterFnRva, iterArgs, 1, entity)) return;
    // resolveFailed vs outsideBlobTable is THE distinction that matters -- see the comment above.
    // Lumping them (as the first version of this counter did) makes a healthy effect entity look
    // identical to a creature with a corrupted handle, and the counts are dominated by the former.
    int walked = 0, resolved_ = 0, noHandle = 0, resolveFailed = 0, outsideBlobTable = 0;
    int guard = 0; // hard cap matching the pool's own real size -- never trust an external loop to self-terminate
    while (entity != 0 && guard < 128) {
        ++guard;
        ++walked;
        uint32_t handle = *(volatile uint32_t*)(uintptr_t)(entity + 0x134);
        if (handle == 0) {
            ++noHandle;
        } else {
            unsigned long long resolveArgs[1] = { (unsigned long long)handle };
            unsigned long long resolved = 0;
            bool ok = SafeCall(base + resolveHandleFnRva, resolveArgs, 1, resolved);
            if (!ok || resolved == 0) {
                // The resolve genuinely failed. resolved==0 is precisely what the bucket guard
                // returns for a bogus handle, so this is the count that measures real corruption
                // -- and any entity in here is a potential creature the in-use check cannot see.
                ++resolveFailed;
            } else if (resolved >= tableBase &&
                       resolved < tableBase + (unsigned long long)(RESOURCE_BLOB_MAX_SPECIES + 1) * RESOURCE_BLOB_SIZE) {
                int species = (int)((resolved - tableBase) >> 18);
                if (species >= 0 && species <= RESOURCE_BLOB_MAX_SPECIES) {
                    outInUse[species] = true;
                    ++resolved_;
                } else {
                    ++outsideBlobTable;
                }
            } else {
                // Resolved to a real address in some OTHER resource table. Healthy, and simply
                // not a species-blob-backed creature -- effects, models, props. Benign.
                ++outsideBlobTable;
            }
        }
        iterArgs[0] = entity;
        if (!SafeCall(base + entityIterFnRva, iterArgs, 1, entity)) break;
    }

    static int lastWalked = -1, lastResolved = -1, lastNoHandle = -1,
               lastResolveFailed = -1, lastOutside = -1;
    if (walked != lastWalked || resolved_ != lastResolved || noHandle != lastNoHandle ||
        resolveFailed != lastResolveFailed || outsideBlobTable != lastOutside) {
        lastWalked = walked; lastResolved = resolved_; lastNoHandle = noHandle;
        lastResolveFailed = resolveFailed; lastOutside = outsideBlobTable;
        char msg[320];
        snprintf(msg, sizeof(msg),
            "LiveEntityScan: walked=%d resolved=%d noHandle=%d outsideBlobTable=%d resolveFAILED=%d%s",
            walked, resolved_, noHandle, outsideBlobTable, resolveFailed,
            resolveFailed > 0
                ? "  <-- resolveFAILED are BROKEN handles: real corruption, and invisible to the in-use check"
                : "  (outsideBlobTable is benign -- healthy non-creature entities)");
        LogDebug(msg);
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
// Raised from 47 to 63 (2026-08-10) on live evidence. The old bound came from a comment claiming
// slots ~48-64 are "hardcoded-reserved by unrelated engine subsystems (menu/JP-sysfont, voice,
// event motion/effect)". Two independent findings contradict that: (1) xref'ing the slot table
// (DAT_142869dd0) shows only SEVEN functions touch it, all in the asset-load family -- no menu,
// font, voice or event subsystem references it at all; (2) a live dump mid-session showed slots
// 50-63 holding owner=0xFF, i.e. unclaimed by the engine's own definition, while 0..49 were fully
// tiled by resident creatures. With the window capped at 47 there was literally never a free run
// available (longest free run in 20..47 measured: ZERO), which is why spawning refused for the rest
// of a session. RESOURCE_BLOB_MAX_SPECIES (64) is still the true table extent and remains the hard
// bound everything else checks against.
static const int SPECIES_SLOT_ALLOC_MAX = 63;

// The engine's real "this slot is unclaimed" sentinel, confirmed two ways: fnc_release_species_slot_run
// (0x1402858e0) writes exactly this when freeing a run, and a live dump showed every never-used slot
// holding it. Testing the state byte or the run-length byte instead -- as this file did earlier today
// -- is wrong in both directions: run MEMBERS keep state==0 (so they looked free when they were not),
// and an engine-released slot can keep a stale run-length byte (so it looks busy when it is free).
static const uint8_t SPECIES_SLOT_UNCLAIMED_OWNER = 0xFF;

// slotRunLen (2026-08-10): the loader claims a RUN of consecutive slots, not one slot --
// fnc_async_load_job_callback passes record+0x56 to FUN_140286190, which stamps owner/run bytes
// into slotRunLen consecutive 0x50-stride entries starting at the chosen species. Live-confirmed
// this evening: the crashing creature's record+0x56 is 5, so picking slot 20 was really claiming
// slots 20-24, and slot 21 was occupied by a live creature -- silently corrupting its resource
// state, which is what produced the deterministic bogus float-shaped resource handles this whole
// investigation was chasing. Searching for a single free slot was never sufficient; the ENTIRE run
// has to be free, which is what this now checks.
// searchFrom lets a caller ask for the NEXT candidate after one it has rejected -- see
// FindFreeSlotAvoidingRoomNatives. Pass 20 for the normal "first free run" behaviour.
static bool FindFreeLoadedSlot(unsigned long long base, unsigned long long loadedPtrTableRva, const bool* speciesInUseByLiveEntity, int slotRunLen, uint8_t* outSpecies, int searchFrom = 20) {
    if (slotRunLen < 1) slotRunLen = 1; // a signed-char <= 0 makes FUN_140286190 claim nothing
    if (searchFrom < 20) searchFrom = 20;
    for (int s = searchFrom; s + slotRunLen - 1 <= SPECIES_SLOT_ALLOC_MAX; ++s) {
        bool runFree = true;
        for (int k = 0; k < slotRunLen; ++k) {
            int slot = s + k;
            if (slot > RESOURCE_BLOB_MAX_SPECIES) { runFree = false; break; }
            if (speciesInUseByLiveEntity && speciesInUseByLiveEntity[slot]) { runFree = false; break; }
            if (IsSpeciesTriggeredThisSession((uint8_t)slot)) { runFree = false; break; }
            // The engine's own definition of free: owner == 0xFF. This correctly rejects run
            // MEMBERS (which carry the owner of their block but state==0) and correctly ACCEPTS a
            // slot the engine has released (which can retain a stale run-length byte).
            volatile uint8_t* ownerAddr = (volatile uint8_t*)(uintptr_t)(base + loadedPtrTableRva +
                LOADED_SPECIES_OWNER_OFFSET_FROM_PTR + (size_t)slot * LOADED_SPECIES_STRIDE);
            if (*ownerAddr != SPECIES_SLOT_UNCLAIMED_OWNER) { runFree = false; break; }
        }
        if (runFree) {
            *outSpecies = (uint8_t)s;
            return true;
        }
    }
    return false;
}

// Picks a free slot run that ALSO does not collide with one of the room's own placement
// records, retrying past each rejected candidate.
//
// Added 2026-08-11 after live evidence that the previous behaviour was a dead end: slot
// selection returned only the FIRST free run, and if the room-native collision check rejected
// it, spawn_enemy refused outright rather than asking for the next one. Because the allocator
// is deterministic, every creature then failed on the same slot forever -- observed as three
// different creatures (Angel Star, Large Body, Pirate) all refused on slot 36 within 20
// seconds of a fresh session start, and previously on slot 35 in an earlier session. The room
// having a record at that one slot number was enough to block ALL spawning in that room.
//
// This is a pre-existing bug that was invisible until the refusal reason reached the caller.
static bool FindFreeSlotAvoidingRoomNatives(unsigned long long base, unsigned long long loadedPtrTableRva,
                                            const bool* speciesInUseByLiveEntity, int slotRunLen,
                                            const uint8_t* table, int32_t count,
                                            unsigned long long resolveFnAddr, const char* modelPath,
                                            uint8_t* outSpecies) {
    int searchFrom = 20;
    uint8_t candidate = 0;
    int rejected = 0;
    while (FindFreeLoadedSlot(base, loadedPtrTableRva, speciesInUseByLiveEntity, slotRunLen,
                              &candidate, searchFrom)) {
        // Check the WHOLE run against the room's placement table, not just the start slot
        // (fixed 2026-08-11). The original only tested `candidate`, so a long run happily
        // swallowed room-native species sitting further along it -- live-observed: slots 30 and
        // 31 were correctly skipped as room-claimed, then a 13-slot run was taken at 32, silently
        // covering 32..44 with no check on 33..44. The room's own creatures then re-claim those
        // slots (the slot table later showed our species-32 slot reading owner=30), overwriting a
        // LIVE creature's model/motion/script data, which is what the behaviour-script
        // interpreter then reads a garbage loop count from.
        //
        // Same lesson as the record+0x56 run-length fix: a creature occupies a RUN, so every
        // per-slot safety test has to cover the whole run. This one was missed.
        bool runCollides = false;
        for (int k = 0; k < slotRunLen; ++k) {
            int slot = (int)candidate + k;
            if (slot > SPECIES_SLOT_ALLOC_MAX) { runCollides = true; break; }
            if (RoomHasNativeSpecies(table, count, (uint8_t)slot, resolveFnAddr, modelPath)) {
                runCollides = true;
                break;
            }
        }
        if (!runCollides) {
            if (rejected > 0) {
                char msg[192];
                snprintf(msg, sizeof(msg),
                    "spawn_enemy: settled on slot %d for %s after skipping %d candidate(s) claimed by "
                    "the room's own placement records",
                    (int)candidate, modelPath, rejected);
                LogDebug(msg);
            }
            *outSpecies = candidate;
            return true;
        }
        ++rejected;
        searchFrom = (int)candidate + 1;
    }
    if (rejected > 0) {
        char msg[192];
        snprintf(msg, sizeof(msg),
            "spawn_enemy: every free slot run for %s (%d candidate(s)) is claimed by one of the room's "
            "own placement records -- no usable slot in this room",
            modelPath, rejected);
        LogDebug(msg);
    }
    return false;
}

// --- ROOM-TRANSITION EVICTION TEST (2026-08-11) ---
// Purpose: prove or disprove one specific hypothesis, and nothing else.
//
// HYPOTHESIS: the game preloads the NEXT room during the door-opening scene -- before
// g_WorldNumber/AreaNumber/SetNumber change, so before our own lazy room-change detection can
// fire -- and that preload EVICTS species slots in place (fnc_async_load_job_callback compares
// the slot's cached filename and reloads on mismatch). That is safe for the engine only because
// a normal transition has already killed every creature. Creatures WE spawned are not part of
// the room roster and are still alive, so their resource blobs get reused underneath them; their
// .bd scripts then read another creature's data, which is what the BehaviorScriptCopyGuard
// faults and the two KH1_BehaviorScriptInterpreter crashes (RVA 0x2CD023 read, 0x2CD064 stack
// smash) both look like.
//
// User-reported timing is what makes this testable: the crash happens while APPROACHING a door,
// not after the room has changed.
//
// THE TEST: remember, for each creature we spawn, the species slot it was constructed from and
// the model filename that slot held at the time. When BehaviorScriptCopyGuard first faults, walk
// those records and re-read the slot's cached filename. If it no longer matches, the slot was
// evicted out from under a live creature and the hypothesis is confirmed outright -- no
// inference needed. If every filename still matches, the hypothesis is WRONG and the corruption
// comes from somewhere else, which is just as useful to know.
//
// Read-only. Nothing here writes game memory or changes behaviour.
static unsigned long long g_slotMapBase = 0;
static unsigned long long g_slotMapLoadedPtrTableRva = 0;

// Defined further down with the entity-snapshot helpers; needed here for the liveness check.
static bool TryReadU32(uint64_t addr, uint32_t* out);

struct SpawnedEntityRecord {
    unsigned long long entityPtr;
    uint32_t id;
    uint8_t species;
    uint8_t runLen;
    char modelPath[LOADED_SPECIES_MODEL_NAME_SIZE + 1];
};
static const int MAX_TRACKED_SPAWNS = 16;
static SpawnedEntityRecord g_trackedSpawns[MAX_TRACKED_SPAWNS];
static int g_trackedSpawnCount = 0;

static void RecordSpawnedEntity(unsigned long long entityPtr, uint32_t id, uint8_t species,
                                uint8_t runLen, const char* modelPath) {
    // Ring buffer -- only the most recent spawns can plausibly still be alive (measured lifetime
    // is ~20s), so overwriting the oldest loses nothing this test needs.
    SpawnedEntityRecord& rec = g_trackedSpawns[g_trackedSpawnCount % MAX_TRACKED_SPAWNS];
    rec.entityPtr = entityPtr;
    rec.id = id;
    rec.species = species;
    rec.runLen = runLen;
    strncpy_s(rec.modelPath, modelPath, _TRUNCATE);
    ++g_trackedSpawnCount;
}

// The decisive check. Re-reads each tracked spawn's slot and reports whether the cached model
// filename still matches what it was when we constructed from it.
// liveRowsOnly: log only rows for creatures that are still LIVE (plus the summary). Used for the
// every-room-change sampling, where the full 16-row dump would be mostly STALE noise and would
// bury the one line that can carry evidence.
//
// WHY THE SAMPLING EXISTS AT ALL (2026-08-11): this check originally ran ONLY from
// NoteFirstGuardFault -- a one-shot that fires at the first guard fault, i.e. only ever when
// something has already gone wrong. Every observation of "LIVE creature + owner==0xFF" therefore
// came from a crash, and was then treated as the cause. That is selection bias: the test never
// once sampled a HEALTHY transition, so there was no denominator. If the same condition also
// occurs on the many transitions that do not crash, it is background noise rather than the
// mechanism. Sampling on every detected room change supplies the missing baseline.
static void LogSpawnedEntitySlotIntegrity(const char* reason, bool liveRowsOnly = false) {
    if (g_slotMapBase == 0 || g_slotMapLoadedPtrTableRva == 0) return;
    int n = g_trackedSpawnCount < MAX_TRACKED_SPAWNS ? g_trackedSpawnCount : MAX_TRACKED_SPAWNS;
    if (n == 0) return;

    if (!liveRowsOnly) {
        char hdr[192];
        snprintf(hdr, sizeof(hdr),
            "spawn_enemy: --- EVICTION TEST (%s): re-checking %d tracked spawn(s) ---", reason, n);
        LogDebug(hdr);
    }

    // LIVENESS IS THE WHOLE POINT (fixed 2026-08-11 -- the first version of this test omitted it
    // and produced a false CONFIRMED). A dead creature's slot being reused later is completely
    // NORMAL -- that is what reclamation is for. The hypothesis is only supported if a slot is
    // reused while its creature is STILL ALIVE. Without this check the test was reporting stale
    // ring-buffer records, provably so: two records showed the same entity pointer AND id with
    // different species, which can only happen once the entity pool has recycled that slot.
    //
    // Three states, and only the third is evidence:
    //   STALE   -- entity+0x04 no longer matches the id we recorded; the pool reused the slot.
    //   RETIRED -- id matches but the +0x374 tick bit is clear; the engine is done with it.
    //   LIVE    -- id matches and the tick bit is set. A filename change HERE is the real thing.
    int evictedUnderLive = 0, live = 0, retired = 0, stale = 0;
    for (int i = 0; i < n; ++i) {
        const SpawnedEntityRecord& rec = g_trackedSpawns[i];
        if (rec.entityPtr == 0) continue;

        uint32_t id = 0, flags = 0;
        bool idOk = TryReadU32(rec.entityPtr + 4, &id) && id == rec.id;
        bool flagsOk = idOk && TryReadU32(rec.entityPtr + ENTITY_OCCUPIED_FLAG_OFFSET, &flags);
        bool isLive = flagsOk && (flags & 0x10000) != 0;
        const char* state;
        if (!idOk)        { state = "STALE  "; ++stale; }
        else if (!isLive) { state = "RETIRED"; ++retired; }
        else              { state = "LIVE   "; ++live; }

        unsigned long long slotPtr = g_slotMapBase + g_slotMapLoadedPtrTableRva +
            (size_t)rec.species * LOADED_SPECIES_STRIDE;
        char nowName[LOADED_SPECIES_MODEL_NAME_SIZE + 1] = {0};
        memcpy(nowName, (const void*)(uintptr_t)(slotPtr + LOADED_SPECIES_MODEL_NAME_OFFSET_FROM_PTR),
               LOADED_SPECIES_MODEL_NAME_SIZE);
        uint8_t owner = *(volatile uint8_t*)(uintptr_t)(slotPtr + LOADED_SPECIES_OWNER_OFFSET_FROM_PTR);

        // TWO independent ways a slot can be pulled out from under a creature, and the second one
        // is the one that actually happens (corrected 2026-08-11 after it hid the root cause):
        //   REUSED   -- the cached filename changed; something else loaded into the slot.
        //   RELEASED -- owner == 0xFF. fnc_release_species_slot_run writes that sentinel AND wipes
        //               the resource blob, but does NOT clear the cached filename. Testing the
        //               filename alone therefore reports "slot unchanged" for the exact case that
        //               destroys the creature's data, which is why this test kept printing
        //               "Hypothesis NOT supported" while `owner=255` sat in its own output.
        bool reused = (strncmp(nowName, rec.modelPath, LOADED_SPECIES_MODEL_NAME_SIZE) != 0);
        bool released = (owner == SPECIES_SLOT_UNCLAIMED_OWNER);
        bool lost = reused || released;
        if (lost && isLive) ++evictedUnderLive;

        const char* verdict;
        if (!lost)              verdict = "slot intact";
        else if (!isLive)       verdict = "slot released/reused (creature already gone -- normal, not evidence)";
        else if (released && reused) verdict = "<<< SLOT RELEASED **AND** REUSED WHILE THIS CREATURE IS STILL LIVE";
        else if (released)      verdict = "<<< SLOT RELEASED (blob WIPED) WHILE THIS CREATURE IS STILL LIVE";
        else                    verdict = "<<< SLOT REUSED WHILE THIS CREATURE IS STILL LIVE";

        if (!liveRowsOnly || isLive) {
            char line[320];
            snprintf(line, sizeof(line),
                "  %s entity=0x%llX id=0x%X species=%d runLen=%d spawned-as=%-20s now=%-20s owner=%-3d  %s",
                state, rec.entityPtr, rec.id, (int)rec.species, (int)rec.runLen, rec.modelPath, nowName,
                (int)owner, verdict);
            LogDebug(line);
        }
    }
    char tail[352];
    snprintf(tail, sizeof(tail),
        "SlotIntegrity[%s]: live=%d retired=%d stale=%d; %d slot(s) RELEASED-or-REUSED under a LIVE "
        "creature.%s",
        reason, live, retired, stale, evictedUnderLive,
        // Deliberately NOT worded as a verdict any more. A single observation cannot distinguish
        // cause from coincidence; what matters is the RATE across crashing vs healthy transitions,
        // which is why this now also samples on every room change. Compare the two before drawing
        // any conclusion from an individual line.
        evictedUnderLive > 0
            ? "  <-- slot pulled from under a LIVE creature (count the healthy transitions too before calling this causal)"
            : "");
    LogDebug(tail);
}

// --- LIVE ENTITY CENSUS (2026-08-11) ---
// Tests one inherited assumption that this whole investigation has been leaning on without ever
// checking it.
//
// The comment above ReclaimDeadSlotRuns asserts the engine's slot eviction at room load "is safe
// for the engine purely because nothing from the previous room is still alive at that moment".
// That was written in an earlier session with no stated evidence, and everything since -- including
// the reverted quiet_spawned_entities containment -- has been built on it. Its only support is
// circular: the engine evicts, the game does not normally crash, therefore nothing must be alive.
// That is inference from absence, not observation. A native creature has never actually been seen
// being retired at a transition.
//
// WHAT THIS ANSWERS: during the door-opening scene, are the ROOM's own creatures still in the live
// entity pool?
//   - If the pool empties to just Sora/party, the assumption holds and creatures we spawned really
//     are the anomaly (they survive when nothing else does).
//   - If native creatures are STILL enumerated mid-transition, the assumption is FALSE, and the
//     difference cannot be lifetime. It would have to be something about our creatures' resource
//     DATA -- e.g. path B firing only for species we cold-loaded, or our minted handles differing
//     from the ones a room's own .ard produces. That redirects the search entirely.
//
// Uses the same fnc_iterate_live_entities walk and the same resolved-blob-to-species arithmetic as
// MarkSpeciesInUseByLiveEntities, including its 128 iteration cap. Read-only; no game state is
// written. `cutsceneFlag` is passed in from Lua (it owns the version-specific address) purely so
// the log line can be correlated with the door scene without guessing.
static void LogLiveEntityCensus(unsigned long long base, unsigned long long entityIterFnRva,
                                unsigned long long resolveHandleFnRva,
                                unsigned long long speciesResourceTableRva,
                                int cutsceneFlag) {
    if (entityIterFnRva == 0 || speciesResourceTableRva == 0 || resolveHandleFnRva == 0) return;
    unsigned long long tableBase = base + speciesResourceTableRva;
    unsigned long long entity = 0;
    unsigned long long iterArgs[1] = { 0 };
    if (!SafeCall(base + entityIterFnRva, iterArgs, 1, entity)) return;

    int total = 0, unresolved = 0, ours = 0;
    char list[600];
    int listLen = 0;
    list[0] = 0;

    int guard = 0;
    while (entity != 0 && guard < 128) {
        ++guard;
        ++total;
        int species = -1;
        uint32_t handle = *(volatile uint32_t*)(uintptr_t)(entity + 0x134);
        if (handle != 0) {
            unsigned long long resolveArgs[1] = { (unsigned long long)handle };
            unsigned long long resolved = 0;
            if (SafeCall(base + resolveHandleFnRva, resolveArgs, 1, resolved) && resolved != 0 &&
                resolved >= tableBase &&
                resolved < tableBase + (unsigned long long)(RESOURCE_BLOB_MAX_SPECIES + 1) * RESOURCE_BLOB_SIZE) {
                species = (int)((resolved - tableBase) >> 18);
            }
        }
        if (species < 0) ++unresolved;

        // Is this one of ours? Match on the entity pointer we recorded at construct, not on
        // species -- species can legitimately be shared with a room native of the same creature.
        bool isOurs = false;
        int nTracked = g_trackedSpawnCount < MAX_TRACKED_SPAWNS ? g_trackedSpawnCount : MAX_TRACKED_SPAWNS;
        for (int i = 0; i < nTracked; ++i) {
            if (g_trackedSpawns[i].entityPtr == entity) { isOurs = true; break; }
        }
        if (isOurs) ++ours;

        if (listLen < (int)sizeof(list) - 24) {
            int w = snprintf(list + listLen, sizeof(list) - listLen, "%s%d%s",
                             listLen ? " " : "", species, isOurs ? "*" : "");
            if (w > 0) listLen += w;
        }

        iterArgs[0] = entity;
        if (!SafeCall(base + entityIterFnRva, iterArgs, 1, entity)) break;
    }

    char msg[800];
    snprintf(msg, sizeof(msg),
        "LiveEntityCensus: cutscene=%d live=%d ours=%d native=%d unresolvedSpecies=%d  species=[%s]  "
        "(* = spawned by us; 'native' here means every live entity that is not one of ours, "
        "including Sora/party)",
        cutsceneFlag, total, ours, total - ours, unresolved, list);
    LogDebug(msg);
}

// Fires the eviction test on the FIRST fault from ANY guard, process-wide.
//
// Originally this was hung off BehaviorScriptCopyGuard alone, because that guard preceded the
// two KH1_BehaviorScriptInterpreter crashes. That was too narrow and the test simply did not run
// on the next crash, which arrived behind a PartyAbilityIndexGuard storm instead (638 faults,
// then death). Whichever guard trips first, the question is the same -- had a live creature's
// species slot been reused? -- so any of them is a valid moment to ask it.
//
// Interlocked one-shot: several of these guards run from more than one thread and can fault in
// bursts, and the answer is only meaningful for the FIRST fault anyway (later ones are downstream
// of whatever already went wrong).
static volatile long g_anyGuardFaultReported = 0;
static void NoteFirstGuardFault(const char* guardName) {
    if (InterlockedCompareExchange(&g_anyGuardFaultReported, 1, 0) != 0) return;
    LogSpawnedEntitySlotIntegrity(guardName);
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
static bool InstallVelocityBlendNeighborGuardHook(unsigned long long hookAddr);
static bool InstallResourceEntryFallbackGuardHook(unsigned long long hookAddr, unsigned long long resumeAddr);
static bool InstallResourceEntryLookupCounterHook(unsigned long long hookAddr, unsigned long long resumeAddr);
// Resolve-fn address for the path-B diagnostic's own re-derivation of which condition fired --
// stashed here because the hook's logger runs from a code cave and can't be handed it.
static uint64_t g_resolveHandleFnAddrForPathB = 0;
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

// --- POST-CONSTRUCTION ENTITY READBACK (2026-08-10) ---
// Answers "spawn_enemy reports success but nothing appears". The constructor provably returns a
// non-null entity (no crash line, no null-entity refusal in the log), so the failure is strictly
// POST-construction -- the same class this investigation has recorded since session 8. This
// distinguishes the three candidates that log evidence alone cannot:
//   (a) entity built but never activated -- the per-frame tick's gate bit stays clear
//   (b) entity culled/freed shortly after -- fields become unreadable or the id stops matching
//   (c) entity alive and activated -- then the fault is rendering/placement, not lifecycle
//
// Only offsets this investigation has ALREADY established are used:
//   entity+0x04  id, written once by the constructor                      (session 4)
//   entity+0x138 a resource handle the constructor resolves for def-kind 2/5 (session 8)
//   entity+0x144 read by the per-frame entity tick FUN_140292470          (session 9)
//   entity+0x374 flags word; that tick gates on bit 0x10000               (session 9)
// Deliberately does NOT guess a position offset -- none has ever been established here, and this
// file's own standing lesson is to not assert a field's meaning that was never verified live.
static uint64_t g_lastSpawnedEntityPtr = 0;
static uint32_t g_lastSpawnedEntityId = 0;

static bool TryWriteU32(uint64_t addr, uint32_t value) {
    __try {
        *(volatile uint32_t*)(uintptr_t)addr = value;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool TryReadU32(uint64_t addr, uint32_t* out) {
    __try {
        *out = *(volatile uint32_t*)(uintptr_t)addr;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static void LogEntitySnapshot(const char* when, uint64_t entity, uint32_t expectedId) {
    if (entity == 0) return;
    uint32_t id = 0, h138 = 0, v144 = 0, flags374 = 0;
    bool okId = TryReadU32(entity + 0x04, &id);
    bool okH = TryReadU32(entity + 0x138, &h138);
    bool ok144 = TryReadU32(entity + 0x144, &v144);
    bool okFlags = TryReadU32(entity + 0x374, &flags374);

    // The four handle fields that fnc_link_model_resource_data_kind3 writes ONLY when their blob
    // section is non-empty, and leaves completely untouched otherwise (no else branch, unlike
    // +0x138/+0x1dc which fall back to mint(0)). Because the 96-slot entity pool is memset only on
    // a slot's FIRST allocation, an unwritten field silently inherits the PREVIOUS occupant's
    // handle. entity+0x134 is the one fnc_velocity_blend_neighbor_scan dereferences and the prime
    // suspect for the 2026-08-11 20:36 crash. Logging these next to the header's field gates makes
    // a stale value directly visible: gate == 0 in the header line and a handle here that is
    // identical to an EARLIER spawn's value is the confirmation.
    uint32_t h134 = 0, h140 = 0, h13c = 0, h68 = 0;
    bool ok134 = TryReadU32(entity + 0x134, &h134);
    bool ok140 = TryReadU32(entity + 0x140, &h140);
    bool ok13c = TryReadU32(entity + 0x13c, &h13c);
    bool ok68 = TryReadU32(entity + 0x68, &h68);

    char msg[640];
    snprintf(msg, sizeof(msg),
        "spawn_enemy: %s entity=0x%llX readable(id,+138,+144,+374)=%d%d%d%d id=0x%X (expected 0x%X, "
        "%s) +0x138=0x%08X +0x144=0x%08X +0x374=0x%08X perFrameTickBit0x10000=%s | "
        "stale-prone handles readable(134,140,13c,68)=%d%d%d%d "
        "+0x134=0x%08X +0x140=0x%08X +0x13c=0x%08X +0x68=0x%08X",
        when, (unsigned long long)entity,
        (int)okId, (int)okH, (int)ok144, (int)okFlags,
        id, expectedId,
        (okId && id == expectedId) ? "MATCH" : "MISMATCH/unreadable -- entity slot was reused or freed",
        h138, v144, flags374,
        okFlags ? ((flags374 & 0x10000) ? "SET (tick is processing it)" : "CLEAR (tick is SKIPPING it)") : "?",
        (int)ok134, (int)ok140, (int)ok13c, (int)ok68,
        h134, h140, h13c, h68);
    LogDebug(msg);
}

// census_live_entities(entityIterFnRva, resolveHandleFnRva, speciesResourceTableRva, cutsceneFlag)
//
// Logs one LiveEntityCensus line. Purely diagnostic and read-only -- see LogLiveEntityCensus for
// the assumption it exists to test. Lua supplies the RVAs (it owns the version-specific address
// tables) and the current cutscene flag, and is responsible for its own call rate; this does no
// throttling of its own so the caller can sample as finely as a transition needs.
extern "C" int l_census_live_entities(void* L) {
    unsigned long long entityIterFnRva = (unsigned long long)p_lua_tointegerx(L, 1, nullptr);
    unsigned long long resolveHandleFnRva = (unsigned long long)p_lua_tointegerx(L, 2, nullptr);
    unsigned long long speciesResourceTableRva = (unsigned long long)p_lua_tointegerx(L, 3, nullptr);
    int cutsceneFlag = (int)p_lua_tointegerx(L, 4, nullptr);
    unsigned long long base = (unsigned long long)GetModuleHandleA(nullptr);
    LogLiveEntityCensus(base, entityIterFnRva, resolveHandleFnRva, speciesResourceTableRva, cutsceneFlag);
    return 0;
}

// quiet_spawned_entities() -> number of creatures quieted
//
// *** TRIED AND REVERTED 2026-08-11. STILL EXPORTED BUT NOTHING CALLS IT. ***
// kh1_lua_library.lua's update_spawn_enemy used to call this on the rising edge of `inCutscene`.
// It did exactly what it says -- kh1_native.log confirms the bit flipping on the right entities --
// and the game crashed anyway, in the same second, at RVA 0x2B01B3 behind a
// "VelocityBlendNeighborGuard: exception walking neighbor entities".
//
// WHY IT DOES NOT WORK: bit 0x10000 gates an entity's OWN per-frame tick. It does not remove the
// entity from the world. Other creatures and systems still enumerate it as a NEIGHBOUR and still
// read its corrupted data, which is the path the surviving crash came through. This contained the
// wrong half of the problem. A real fix needs the engine's own despawn/retire routine so the
// entity stops existing, not a flag that only stops it thinking.
//
// Left in the file, inert, per this project's convention for tried-but-unproven fixes
// (ValidateResolvedBucketAddress, InstallBucketMemoryWatcherThread, InstallFreezeWatchdogThread).
// Do not re-wire it without a genuinely new idea about the neighbour-walk path.
//
// Original description follows.
//
// Clears the per-frame tick bit on every creature WE spawned that is still being ticked, so the
// engine stops running their per-frame update (and therefore their .bd behaviour scripts).
// Intended to be called the moment a cutscene / room transition begins.
//
// WHY THIS EXISTS: live-reproduced repeatedly on 2026-08-11 -- a Crowd-Control-spawned creature
// that is still alive when the player approaches a door crashes the game during the door scene,
// consistently inside code that walks its animation/behaviour data (KH1_BehaviorScriptInterpreter
// RVA 0x2CD023 read and 0x2CD064 stack smash, plus VelocityBlendGuard storms striding a 0x14-byte
// array off a NULL base). The room's own creatures never hit this because a real transition
// retires them first; ours are not part of the room roster and keep being ticked.
//
// WHAT THIS DOES NOT CLAIM: it does not fix why their data goes bad. The slot-eviction theory was
// TESTED AND DISPROVED (see LogSpawnedEntitySlotIntegrity -- 0 of 5 tracked slots reused, and the
// only released runs belonged to creatures already retired by the engine). The actual corruption
// is still the long-standing bogus/zero-handle problem. This is containment: a creature that is
// not ticked cannot run a script over bad data, whatever made the data bad.
//
// SAFETY: only entities this DLL spawned are touched, and only after re-reading entity+0x04 and
// confirming the id still matches what we recorded -- the entity pool recycles slots, so a stale
// pointer would otherwise mean writing into an unrelated creature. Both the read and the write go
// through SEH. Only the single bit 0x10000 is cleared; every other flag is preserved. A record
// that fails any check is dropped from tracking rather than retried.
//
// FIELD PROVENANCE: entity+0x374 bit 0x10000 is the per-frame tick gate, established live in
// session 9 (see LogEntitySnapshot's own comment) -- not inferred here. Every entity snapshot
// logged today agrees: retired creatures read +0x374 == 0, live ones have the bit set.
extern "C" int l_quiet_spawned_entities(void* L) {
    int quieted = 0, dropped = 0, alreadyQuiet = 0;
    int n = g_trackedSpawnCount < MAX_TRACKED_SPAWNS ? g_trackedSpawnCount : MAX_TRACKED_SPAWNS;
    for (int i = 0; i < n; ++i) {
        SpawnedEntityRecord& rec = g_trackedSpawns[i];
        if (rec.entityPtr == 0) continue;

        uint32_t id = 0;
        if (!TryReadU32(rec.entityPtr + 4, &id) || id != rec.id) {
            rec.entityPtr = 0; // pool slot recycled or unreadable -- not ours any more
            ++dropped;
            continue;
        }
        uint32_t flags = 0;
        if (!TryReadU32(rec.entityPtr + ENTITY_OCCUPIED_FLAG_OFFSET, &flags)) {
            rec.entityPtr = 0;
            ++dropped;
            continue;
        }
        if ((flags & 0x10000) == 0) {
            ++alreadyQuiet; // engine already retired it -- nothing to do
            continue;
        }
        if (TryWriteU32(rec.entityPtr + ENTITY_OCCUPIED_FLAG_OFFSET, flags & ~0x10000u)) {
            char msg[192];
            snprintf(msg, sizeof(msg),
                "quiet_spawned_entities: cleared tick bit on entity=0x%llX id=0x%X species=%d (%s) "
                "-- +0x374 0x%08X -> 0x%08X",
                rec.entityPtr, rec.id, (int)rec.species, rec.modelPath, flags, flags & ~0x10000u);
            LogDebug(msg);
            ++quieted;
        } else {
            rec.entityPtr = 0;
            ++dropped;
        }
    }
    if (quieted > 0 || dropped > 0) {
        char msg[192];
        snprintf(msg, sizeof(msg),
            "quiet_spawned_entities: quieted %d, already-retired %d, dropped %d (of %d tracked)",
            quieted, alreadyQuiet, dropped, n);
        LogDebug(msg);
    }
    p_lua_pushinteger(L, (long long)quieted);
    return 1;
}

// Every spawn_enemy refusal goes through here so it returns a SHORT machine-readable
// code alongside its long human message.
//
// Why the code exists at all (2026-08-11): the long message was reaching Lua unusably.
// Measured across kh1_crowdcontrol_native.log, 236 of 319 failure lines printed an EMPTY
// reason and several printed a TRUNCATED one ("...(all 256 in use) -- r", "spawn_enemy:
// placement table not va") -- the same message arriving intact on some calls and blank on
// others, so it is not a code path that pushes nothing. This is the LuaBackend
// native-string defect already documented at the top of KH1-CROWDCONTROL's
// kh1_crowdcontrol.lua: a string pushed from native and then read back through Lua's own
// string library (string.format/tostring/json.encode) comes back empty or cut short,
// while the native C API reads the identical memory correctly. Mid-word truncation is the
// tell.
//
// The fix is to never let display text originate natively: `code` is short and is used
// only as a table key on the Lua side, where the human sentence is a Lua-authored string
// and therefore immune. The long `message` is kept as the second return purely for
// existing callers and as a fallback -- it is still subject to the same defect, so do NOT
// make anything user-facing depend on it.
//
// Return shape: (false, message, false, code). The third slot is the "still loading"
// flag, which a refusal never sets -- pushed as boolean false rather than nil so the
// arity stays fixed at 4 for every refusal.
static int RefuseSpawn(void* L, const char* code, const char* message) {
    p_lua_pushboolean(L, 0);
    p_lua_pushstring(L, message);
    p_lua_pushboolean(L, 0);
    p_lua_pushstring(L, code);
    return 4;
}

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
    // fnc_release_species_slot_run (Steam 0x2858E0) -- the GAME'S OWN release routine for a run of
    // species slots, found 2026-08-10 by xref'ing the slot table. Takes (species, runLen) and, for
    // each slot in the run, sets owner=0xFF (the engine's real unclaimed sentinel) and clears the
    // slot+2 flags byte -- then, crucially, WIPES the matching resource-blob range
    // (DAT_140d2ada0 + species*0x40000, runLen blocks). Our own hand-rolled reclamation missed that
    // blob wipe entirely, and stale blob content is itself a documented crash cause (sessions
    // 9-12). Calling the engine's own routine is strictly better than reproducing it by hand.
    // Optional, same degrade-gracefully convention as the other RVAs: without it we simply cannot
    // reclaim, which is the pre-2026-08-10 behaviour.
    unsigned long long releaseSpeciesSlotRunFnRva = (unsigned long long)p_lua_tointegerx(L, 43, nullptr);
    // Entry point of fnc_velocity_blend_neighbor_UNGUARDED (Steam 0x2B5AF0) -- see
    // InstallVelocityBlendNeighborGuardHook's own comment. Fixes crash site #4, known
    // and unfixed since 2026-08-04 and the TOP remaining crash site as of 2026-08-10.
    // Optional, same degrade-gracefully convention as the other hook RVAs above.
    unsigned long long velocityBlendNeighborGuardHookFnRva = (unsigned long long)p_lua_tointegerx(L, 44, nullptr);

    // Entry of PATH B inside fnc_resource_entry_lookup_with_fallback (Steam 0x1DD6AB) -- the
    // silent fallback that hands callers a slice of the header object as if it were a resource
    // entry. Diagnostic only; see InstallResourceEntryFallbackGuardHook. Optional, same
    // degrade-gracefully convention as every other hook RVA here.
    unsigned long long resourceEntryFallbackHookFnRva = (unsigned long long)p_lua_tointegerx(L, 45, nullptr);

    // ENTRY of the same function (Steam 0x1DD650). Counts calls so a zero path-B count can be
    // told apart from the function never running -- see InstallResourceEntryLookupCounterHook.
    unsigned long long resourceEntryLookupCounterFnRva = (unsigned long long)p_lua_tointegerx(L, 46, nullptr);

    // Boss-defeat / slow-motion state. Steam RVAs sourced from the community "Slowmode" script
    // (KSX), which drives this effect by writing these three globals; cross-checked in Ghidra that
    // the GAME itself writes them too, so they are real engine state and not script-only scratch:
    // g_GameSpeed is a genuine timescale float, and g_BossDefeatEffectStart is written by
    // fnc_01F_blur_on / fnc_0B8_rotate_blur (EVDL effect opcodes) among others.
    // See the 2026-08-11 20:56 crash: a spawn landed inside the post-boss slowdown and the engine
    // wiped the species blob out from under the live creature a few seconds later.
    unsigned long long gameSpeedRva = (unsigned long long)p_lua_tointegerx(L, 47, nullptr);
    unsigned long long bossDefeatEffectRva = (unsigned long long)p_lua_tointegerx(L, 48, nullptr);
    unsigned long long bossDefeatEffectStartRva = (unsigned long long)p_lua_tointegerx(L, 49, nullptr);

    if (!modelPath || !motionPath) {
        return RefuseSpawn(L, "bad_args",
            "spawn_enemy: model_path/motion_path are required");
    }
    // Copy off Lua's own string storage immediately -- see InternPath.
    modelPath = InternPath(modelPath);
    motionPath = InternPath(motionPath);
    if (resolveHandleFnRva == 0) {
        return RefuseSpawn(L, "unconfigured",
            "spawn_enemy: fnc_resolve_resource_handle address not configured for this game build");
    }

    unsigned long long base = (unsigned long long)GetModuleHandleA(nullptr);

    // Stashed so the guard hooks -- which run on the game's own thread with no access to these
    // Lua-supplied RVAs -- can read the slot table for the eviction test. Read-only use.
    g_slotMapBase = base;
    g_slotMapLoadedPtrTableRva = loadedPtrTableRva;

    // Re-read the PREVIOUS spawn's entity before doing anything else. If it was culled, or its pool
    // slot got recycled since, that shows up here as an unreadable or mismatched id -- which
    // separates "never became visible" from "appeared briefly then was silently removed".
    if (g_lastSpawnedEntityPtr != 0) {
        LogEntitySnapshot("previous-spawn recheck:", g_lastSpawnedEntityPtr, g_lastSpawnedEntityId);
    }

    // ---- Boss-defeat / slow-motion refusal -------------------------------------------------
    //
    // WHY: the 2026-08-11 20:56 crash. A spawn completed cleanly at 20:56:08, and ~5s later the
    // ENGINE released and wiped that species' resource blob while our creature was still live
    // (blobHandle at header+0x14 zeroed, magic intact), sending
    // fnc_resource_entry_lookup_with_fallback down path B 20x and killing the game in
    // fnc_text_slot_layout_prepare. Every previous sighting of that engine wipe was at a ROOM
    // TRANSITION; this one had none -- the trigger was a boss-defeat/battle-end cleanup. The real
    // fix is still to remove our tracked entities when the engine tears down, and it must now cover
    // battle-end as well as room change. This is harm reduction at the demonstrated trigger, not
    // that fix.
    //
    // Refusing here is cheap: the caller retries every frame (the crash log shows dozens of
    // retries per second through exactly this window), so a spawn merely lands a moment later
    // instead of inside the teardown.
    //
    // DELIBERATELY FAIL-OPEN, and gated on g_GameSpeed ONLY. This file has twice shipped a
    // validator built on an invariant that was inferred rather than observed, and twice broken the
    // whole feature. g_GameSpeed has a known normal value (1.0) and a known slowed value (0.1), so
    // "positively below normal" is a real observation. The other two bytes have NO observed normal
    // value yet, so they are LOGGED ONLY and gate nothing. If a read fails, or the value looks
    // normal, we proceed exactly as before -- a missed window costs one caught exception, a false
    // positive costs the user the entire feature.
    if (gameSpeedRva != 0) {
        uint32_t rawSpeed = 0, effect = 0, effectStart = 0;
        bool okSpeed = TryReadU32(base + gameSpeedRva, &rawSpeed);
        bool okEffect = (bossDefeatEffectRva != 0) && TryReadU32(base + bossDefeatEffectRva, &effect);
        bool okStart = (bossDefeatEffectStartRva != 0) && TryReadU32(base + bossDefeatEffectStartRva, &effectStart);

        float gameSpeed = 1.0f;
        if (okSpeed) memcpy(&gameSpeed, &rawSpeed, sizeof(gameSpeed));

        // Treat only a genuinely sub-normal, finite timescale as "slowed". NaN/garbage compares
        // false here and is therefore treated as normal, which is the fail-open direction.
        const bool slowed = okSpeed && (gameSpeed > 0.0f) && (gameSpeed < 0.9f);

        // Log on the first call ever (to capture the baseline these thresholds should be judged
        // against) and on every abnormal reading. Not on every normal call -- the caller polls
        // each frame and that would bury the spawn diagnostics, which is a failure mode this
        // file has hit before.
        static bool s_loggedSpeedBaseline = false;
        if (!s_loggedSpeedBaseline || slowed) {
            s_loggedSpeedBaseline = true;
            char spdMsg[320];
            snprintf(spdMsg, sizeof(spdMsg),
                "spawn_enemy: engine state readable(speed,effect,start)=%d%d%d g_GameSpeed=%f "
                "(raw=0x%08X) bossDefeatEffect=%u bossDefeatEffectStart=%u -- slowed=%d "
                "(only g_GameSpeed gates; the other two are diagnostic only)",
                (int)okSpeed, (int)okEffect, (int)okStart, (double)gameSpeed, rawSpeed,
                (unsigned)(effect & 0xFF), (unsigned)(effectStart & 0xFF), (int)slowed);
            LogDebug(spdMsg);
        }

        if (slowed) {
            // Nothing has been allocated or published yet, so returning here is a true no-op --
            // no table swap to roll back, no phantom placement record. Keep it that way if this
            // block ever moves.
            return RefuseSpawn(L, "boss_defeat_slowdown",
                "spawn_enemy: the game is in a boss-defeat/slow-motion sequence right now -- refusing "
                "so the spawn does not land while the engine is tearing down battle resources; retry shortly");
        }
    }

    // Room-change slot reclamation -- see ReclaimOwnSlotRuns' own comment for why a room change is
    // the one moment this is provably safe. Done before any slot selection below, so the freed runs
    // are immediately available to this very call.
    if (worldNumRva != 0 && areaNumRva != 0 && setNumRva != 0) {
        int32_t curWorld = *(int32_t*)(uintptr_t)(base + worldNumRva);
        int32_t curArea = *(int32_t*)(uintptr_t)(base + areaNumRva);
        int32_t curSet = *(int32_t*)(uintptr_t)(base + setNumRva);
        if (g_lastSpawnRoomValid &&
            (curWorld != g_lastSpawnRoomWorld || curArea != g_lastSpawnRoomArea || curSet != g_lastSpawnRoomSet)) {
            // Sample BEFORE ReclaimOwnSlotRuns, so what is logged is the state the ENGINE left
            // behind, not something our own reclaim just did. This is the healthy-transition
            // baseline the guard-fault one-shot could never provide -- see
            // LogSpawnedEntitySlotIntegrity's comment. Live rows only: the full dump here would be
            // mostly stale records and would drown the line that matters.
            LogSpawnedEntitySlotIntegrity("room change", true);

            ReclaimOwnSlotRuns(base, loadedPtrTableRva, releaseSpeciesSlotRunFnRva);
            // A creature we spawned in the previous room cannot still be alive here, so the stale
            // entity pointer from it is meaningless now -- stop re-reading it (it was already
            // producing confusing "MISMATCH" lines from ordinary entity-pool recycling).
            g_lastSpawnedEntityPtr = 0;
            g_lastSpawnedEntityId = 0;
        }
        g_lastSpawnRoomWorld = curWorld;
        g_lastSpawnRoomArea = curArea;
        g_lastSpawnRoomSet = curSet;
        g_lastSpawnRoomValid = true;

        // Room blacklist -- checked HERE, after the reclamation above rather than before it, so
        // that walking into a blacklisted room still hands back the previous room's slot runs.
        // Refusing earlier would skip that and quietly leak a run every time.
        //
        // Matches on world+area only; the Set number is ignored, so every set/variant of a listed
        // area is blocked. Set is the evdl variant suffix (ard0/ardc/ardd...), not a distinct
        // room, so blacklisting per-set would let the same physical room through under a
        // different variant.
        for (size_t i = 0; i < sizeof(g_spawnRoomBlacklist) / sizeof(g_spawnRoomBlacklist[0]); ++i) {
            const BlacklistedRoomRange& r = g_spawnRoomBlacklist[i];
            if (curWorld == r.world && curArea >= r.areaMin && curArea <= r.areaMax) {
                char msg[224];
                snprintf(msg, sizeof(msg),
                    "spawn_enemy: refusing -- room %d/%d/%d is blacklisted (%s covers areas %d..%d): %s",
                    curWorld, curArea, curSet, r.label, r.areaMin, r.areaMax, r.why);
                LogDebug(msg);
                return RefuseSpawn(L, "room_blacklisted",
                    "spawn_enemy: enemy spawning is disabled in this room");
            }
        }
    }

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
            return RefuseSpawn(L, "handles_full",
                "spawn_enemy: too many distinct resources loaded this session (bucket table nearly full) -- refusing to risk a crash");
        }
    }

    uint8_t** tablePtrAddr = (uint8_t**)(uintptr_t)(base + tablePtrRva);
    int32_t* tableCountAddr = (int32_t*)(uintptr_t)(base + tableCountRva);

    uint8_t* oldTable = *tablePtrAddr;
    int32_t oldCount = *tableCountAddr;
    if (!oldTable || oldCount <= 0 || oldCount > 4096) {
        return RefuseSpawn(L, "table_invalid",
            "spawn_enemy: placement table not valid right now (wrong room state?)");
    }

    // See AnyEnemyTooCloseToSora's own comment -- a fresh spawn lands at
    // Sora's own position, so if another ENEMY is already sitting there,
    // refuse rather than risk two enemies overlapping directly (spawning
    // on Sora himself is fine). Checked before any species resolution/load
    // work, since nothing below matters if this refuses.
    if (AnyEnemyTooCloseToSora(base, entityPoolBaseRva, soraPointerRva, soraObjPtrRva,
                                 partyMember1PtrRva, partyMember2PtrRva, MIN_SPAWN_DISTANCE_FROM_SORA)) {
        LogDebug("spawn_enemy: refusing -- another enemy is already too close to Sora's position");
        return RefuseSpawn(L, "too_close",
            "spawn_enemy: another enemy is already too close to Sora -- refusing to avoid spawning on top of it");
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
        return RefuseSpawn(L, "no_creature_data",
            "spawn_enemy: no offline fallback data for this creature (missing from kh1_creature_data.lua)");
    }
    const uint8_t* templateRec = (const uint8_t*)luaTemplateRecord;
    uint16_t charId = (uint16_t)luaCharId;
    uint8_t weight = (uint8_t)luaWeight;
    // How many consecutive species slots this creature's asset load will claim (record+0x56, read
    // through a signed char exactly as the game does). Needed BEFORE slot selection so we can find
    // a free RUN instead of a single free slot -- see FindFreeLoadedSlot's own comment.
    const int templateSlotRunLen = (int)(int8_t)templateRec[0x56];

    if (loadedPtrTableRva == 0) {
        return RefuseSpawn(L, "unconfigured",
            "spawn_enemy: loadedSpeciesPtrTable address not configured for this game build");
    }
    // Install all three crash-fixing hooks (idempotent) before touching
    // anything else.
    if (jobCallbackFnRva == 0 || velocityBlendFnRva == 0 || partyAbilityIndexFnRva == 0) {
        return RefuseSpawn(L, "unconfigured",
            "spawn_enemy: job-record-guard/velocity-blend-guard/party-ability-index-guard hook addresses not configured for this game build");
    }
    if (!InstallJobRecordGuardHook(base + jobCallbackFnRva, base + jobCallbackFnRva + 5,
                                     tablePtrRva, tableCountRva, worldNumRva, areaNumRva, setNumRva) ||
        !InstallVelocityBlendGuardHook(base + velocityBlendFnRva, base + velocityBlendFnRva + 5) ||
        // +9, not +5: the hook replaces both the call and the risky read
        // right after it -- resumeAddr must skip past both.
        !InstallPartyAbilityIndexGuardHook(base + partyAbilityIndexFnRva, base + partyAbilityIndexFnRva + 9, base + resolveHandleFnRva)) {
        return RefuseSpawn(L, "hook_install_fail",
            "spawn_enemy: failed to install the async-load/velocity-blend/party-ability-index guard hooks -- refusing rather than risk the crash they fix");
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
    if (velocityBlendNeighborGuardHookFnRva != 0) {
        if (!InstallVelocityBlendNeighborGuardHook(base + velocityBlendNeighborGuardHookFnRva)) {
            LogDebug("spawn_enemy: InstallVelocityBlendNeighborGuardHook failed to install -- proceeding without it");
        }
    }
    if (resourceEntryFallbackHookFnRva != 0) {
        // Stash the resolve-fn address first -- the hook's logger needs it to tell condition 2
        // (handle won't resolve) apart from condition 3 (blob resolved but empty).
        if (resolveHandleFnRva != 0) g_resolveHandleFnAddrForPathB = base + resolveHandleFnRva;
        // Resume 8 bytes on: the patch covers LEA(4) + SHL(4), both replayed inside the stub.
        if (!InstallResourceEntryFallbackGuardHook(base + resourceEntryFallbackHookFnRva,
                                                    base + resourceEntryFallbackHookFnRva + 8)) {
            LogDebug("spawn_enemy: InstallResourceEntryFallbackGuardHook failed to install -- proceeding without it");
        }
    }
    if (resourceEntryLookupCounterFnRva != 0) {
        // Resume 5 bytes on -- the patch replaces exactly one 5-byte instruction, replayed in the stub.
        if (!InstallResourceEntryLookupCounterHook(base + resourceEntryLookupCounterFnRva,
                                                    base + resourceEntryLookupCounterFnRva + 5)) {
            LogDebug("spawn_enemy: InstallResourceEntryLookupCounterHook failed to install -- proceeding without it");
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
        return RefuseSpawn(L, "unconfigured",
            "spawn_enemy: fnc_mint_resource_handle not configured for this game build -- can't safely reuse a captured template's model/motion handles");
    }
    bool speciesInUseByLiveEntity[RESOURCE_BLOB_MAX_SPECIES + 1] = {};
    MarkSpeciesInUseByLiveEntities(base, entityIterFnRva, resolveHandleFnRva, speciesResourceTableRva, speciesInUseByLiveEntity);
    // Both reuse branches are gated on ReuseSlotIsSafe -- see its comment for the 2026-08-11
    // crash this closes. A failed gate deliberately falls through to the next branch (and
    // ultimately to a fresh load) rather than refusing, so the cost is a reload, not a lost
    // spawn. `species` may hold a stale value after a failed gate; every later branch assigns
    // it again before use.
    if (FindTriggeredLoad(modelPath, &species) &&
        ReuseSlotIsSafe(base, loadedPtrTableRva, species, modelPath, "our own triggered load")) {
        // Already triggered a load for this creature this session -- reuse
        // the species and don't trigger another load.
        if (RoomHasNativeSpecies(oldTable, oldCount, species, base + resolveHandleFnRva, modelPath)) {
            char msg[160];
            snprintf(msg, sizeof(msg), "spawn_enemy: slot %d (already triggered by us this session as %s) is used by a different native record in this room -- refusing", species, modelPath);
            LogDebug(msg);
            return RefuseSpawn(L, "slot_collision",
                "spawn_enemy: chosen slot collides with a different creature already native to this room -- refusing");
        }
    } else if (FindLoadedSlotByFilename(base, loadedPtrTableRva, modelPath, &species) &&
               ReuseSlotIsSafe(base, loadedPtrTableRva, species, modelPath, "loaded-slot filename scan")) {
        // Already loaded this session -- reuse it, but check the room's own
        // placement table doesn't already use this slot for something else.
        if (RoomHasNativeSpecies(oldTable, oldCount, species, base + resolveHandleFnRva, modelPath)) {
            char msg[160];
            snprintf(msg, sizeof(msg), "spawn_enemy: slot %d (already loaded elsewhere this session as %s) is used by a different native record in this room -- refusing", species, modelPath);
            LogDebug(msg);
            return RefuseSpawn(L, "slot_collision",
                "spawn_enemy: chosen slot collides with a different creature already native to this room -- refusing");
        }
    // The room-native collision check is now folded INTO slot selection rather than applied
    // after it. Previously this took the first free run and refused outright if that one run
    // happened to collide -- a dead end, since the allocator is deterministic and would hand
    // back the same colliding slot on every retry. See FindFreeSlotAvoidingRoomNatives.
    } else if (FindFreeSlotAvoidingRoomNatives(base, loadedPtrTableRva, speciesInUseByLiveEntity,
                                               templateSlotRunLen, oldTable, oldCount,
                                               base + resolveHandleFnRva, modelPath, &species) ||
               // Nothing usable on the first pass -- hand back the runs of creatures that have
               // since died (see ReclaimDeadSlotRuns) and look once more. Short-circuit order
               // matters: the reclaim only runs when the window is genuinely full, and the retry
               // only runs when the reclaim actually freed something, so the common path is
               // untouched.
               (ReclaimDeadSlotRuns(base, loadedPtrTableRva, releaseSpeciesSlotRunFnRva, speciesInUseByLiveEntity) > 0 &&
                FindFreeSlotAvoidingRoomNatives(base, loadedPtrTableRva, speciesInUseByLiveEntity,
                                                templateSlotRunLen, oldTable, oldCount,
                                                base + resolveHandleFnRva, modelPath, &species))) {
        // mintHandleFnRva/loadAssetsFnRva are required for this specific
        // case (a genuinely fresh slot, nothing loaded into it yet) --
        // checked here, before the placement table is touched at all, to
        // keep this particular check a true no-op.
        if (loadAssetsFnRva == 0) {
            return RefuseSpawn(L, "unconfigured",
                "spawn_enemy: fnc_load_gimmick_assets not configured for this game build -- can't load a creature with zero presence in this room");
        }
        needsLoad = true;
    } else {
        // This refusal logged NOTHING before 2026-08-10, which is why a long run of failing spawns
        // left no trace in kh1_native.log at all -- ~20 consecutive failures were observed with only
        // an unrelated recheck line between them, making the failure look like it wasn't even
        // reaching native code. Always log a refusal.
        //
        // CAPACITY, and this is a real design limit rather than a bug: slot state is session-global
        // and NOTHING ever releases it (exactly like the resource-handle buckets), and since
        // 2026-08-10 each creature is known to claim a RUN of record+0x56 consecutive slots
        // (typically 3-5, live-observed). The allocation window is 20..SPECIES_SLOT_ALLOC_MAX, i.e.
        // 28 slots, so one session supports only ~6-8 DISTINCT creatures before this refusal is
        // both correct and unavoidable. Entering a new room makes it worse, since the new room's own
        // natives occupy slots in the same window. The old message claimed "all 256 in use", which
        // was never true and pointed diagnosis in the wrong direction.
        int occupied = 0;
        for (int s = 20; s <= SPECIES_SLOT_ALLOC_MAX; ++s) {
            volatile uint8_t* ownerAddr = (volatile uint8_t*)(uintptr_t)(base + loadedPtrTableRva +
                LOADED_SPECIES_OWNER_OFFSET_FROM_PTR + (size_t)s * LOADED_SPECIES_STRIDE);
            if (*ownerAddr != SPECIES_SLOT_UNCLAIMED_OWNER || IsSpeciesTriggeredThisSession((uint8_t)s)) ++occupied;
        }
        // WORDING CORRECTED 2026-08-11: this used to say slot state "is never released, so this
        // does not recover without a restart". That was true only while ENABLE_ON_DEMAND_SLOT_RECLAIM
        // was false. It is now true again, and ReclaimDeadSlotRuns has ALREADY run by the time
        // control reaches here (it is the second half of the branch condition above), so reaching
        // this point means no slots were reclaimable right now -- not that none ever will be.
        // Recovery no longer needs a restart: runs come back as spawned creatures are defeated,
        // and again on a room change. Do not restore the old wording; a message that sends users
        // to restart when defeating an enemy would do is worse than no message.
        char msg[320];
        snprintf(msg, sizeof(msg),
            "spawn_enemy: refusing -- no free run of %d consecutive species slots in the %d..%d "
            "window (%d of %d slots claimed right now; on-demand reclamation already ran and freed "
            "nothing reusable, so every claimed run is either still alive or not ours to release)",
            templateSlotRunLen, 20, SPECIES_SLOT_ALLOC_MAX, occupied,
            SPECIES_SLOT_ALLOC_MAX - 20 + 1);
        LogDebug(msg);

        // Slot-table map, logged at most once every 30s on this refusal (it is several lines and
        // this path can fire every few seconds). Added 2026-08-11 because the occupancy COUNT
        // alone cannot answer the only question that matters when spawning stops working: is the
        // window full of the ROOM's own creatures -- in which case this is a capacity limit and
        // nothing here can reclaim it -- or full of OUR orphaned runs, which would be a real
        // bookkeeping leak worth fixing. Reads owner/runLen/state/name straight out of the
        // engine's own table; pure read-only, nothing is written.
        {
            static unsigned long long lastMapTick = 0;
            unsigned long long now = GetTickCount64();
            if (lastMapTick == 0 || now - lastMapTick > 30000) {
                lastMapTick = now;
                // Deliberately maps the WHOLE table (0..63), not just the 20..63 allocation
                // window, even though nothing may allocate below 20. The occupancy count above
                // only counts the window, which quietly hides where the room's own creatures
                // actually sit -- the engine tiles them upward from slot 0, so most of a room's
                // roster can live in 0..19 and never appear in "N of 44 claimed". The point of
                // this map is to answer whether the window is full of the ROOM's creatures (a
                // capacity limit) or OUR orphaned runs (a leak), and that cannot be judged
                // without seeing the region the count omits.
                //
                // It also shows whether the hardcoded lower bound of 20 is still earning its
                // keep. That bound has no recorded justification anywhere in this file or the
                // investigation notes -- unlike SPECIES_SLOT_ALLOC_MAX, whose raise from 47 to
                // 63 is documented with live evidence. Do NOT lower it on the strength of this
                // map alone: "0..19 looks free right now" is not the same as "nothing claims
                // 0..19", and getting that wrong corrupts a live creature's blob silently rather
                // than failing visibly. Find out what claims that region and when, first.
                LogDebug("spawn_enemy: --- species slot map 0..63 (allocation window is 20..63; 0..19 shown for context) ---");
                for (int s = 0; s <= SPECIES_SLOT_ALLOC_MAX; ) {
                    unsigned long long slotPtr = base + loadedPtrTableRva + (size_t)s * LOADED_SPECIES_STRIDE;
                    uint8_t owner = *(volatile uint8_t*)(uintptr_t)(slotPtr + LOADED_SPECIES_OWNER_OFFSET_FROM_PTR);
                    if (owner == SPECIES_SLOT_UNCLAIMED_OWNER) {
                        int runStart = s;
                        while (s <= SPECIES_SLOT_ALLOC_MAX &&
                               *(volatile uint8_t*)(uintptr_t)(base + loadedPtrTableRva +
                                   (size_t)s * LOADED_SPECIES_STRIDE + LOADED_SPECIES_OWNER_OFFSET_FROM_PTR)
                                   == SPECIES_SLOT_UNCLAIMED_OWNER) ++s;
                        char freeMsg[128];
                        snprintf(freeMsg, sizeof(freeMsg), "  %2d-%2d  FREE (%d slots)%s",
                            runStart, s - 1, s - runStart,
                            (s - 1) < 20 ? "  [below slot 20 -- OUTSIDE the allocation window, unusable today]"
                                         : (runStart < 20 ? "  [straddles slot 20 -- only the 20+ part is usable today]" : ""));
                        LogDebug(freeMsg);
                        continue;
                    }
                    uint8_t runLen = *(volatile uint8_t*)(uintptr_t)(slotPtr + LOADED_SPECIES_RUNLEN_OFFSET_FROM_PTR);
                    uint8_t state = *(volatile uint8_t*)(uintptr_t)(slotPtr + LOADED_SPECIES_STATE_OFFSET_FROM_PTR);
                    const char* name = (const char*)(uintptr_t)(slotPtr + LOADED_SPECIES_MODEL_NAME_OFFSET_FROM_PTR);
                    char nameBuf[LOADED_SPECIES_MODEL_NAME_SIZE + 1] = {0};
                    memcpy(nameBuf, name, LOADED_SPECIES_MODEL_NAME_SIZE);
                    nameBuf[LOADED_SPECIES_MODEL_NAME_SIZE] = 0;
                    char entryMsg[224];
                    snprintf(entryMsg, sizeof(entryMsg),
                        "  %2d     owner=%-3d runLen=%-2d state=%-2d %-20s %s%s",
                        s, (int)owner, (int)runLen, (int)state, nameBuf,
                        IsSpeciesTriggeredThisSession((uint8_t)s) ? "<- OURS (tracked)" : "",
                        s < 20 ? "  [outside allocation window]" : "");
                    LogDebug(entryMsg);
                    ++s;
                }
                LogDebug("spawn_enemy: --- end slot map ---");
            }
        }

        return RefuseSpawn(L, "slots_full",
            "spawn_enemy: no creature slots free right now -- each creature type claims several. They come back as spawned enemies are defeated, or when you change rooms.");
    }
    // Room-exclusion guard for World=3/Area=2/Set=2 intentionally REMOVED
    // 2026-08-04 -- under active investigation now instead of being refused
    // outright. See project_spawn_enemy_cold_spawn_crash_containment.md for
    // the crash this room causes and what's already known about it; git
    // history has the removed check if it needs to come back.
    //
    // Defense in depth: re-check the slot right before touching anything,
    // in case its state changed since the scan above.
    //
    // MUST gate on owner == SPECIES_SLOT_UNCLAIMED_OWNER first (fixed 2026-08-11). This check
    // predates the 2026-08-10 discovery that `owner == 0xFF` is the ONLY authoritative "slot is
    // free" test, and it was never brought in line: it keyed purely on the state byte plus the
    // cached model filename. But `fnc_release_species_slot_run` -- the engine's own release
    // routine, which ReclaimOwnSlotRuns now calls on every room change -- sets owner back to 0xFF
    // and clears the flags byte WITHOUT clearing the state byte or the cached model name. Those
    // fields are therefore STALE, not current, on a genuinely free slot.
    //
    // The result was a direct self-contradiction, caught live in Hundred Acre Wood: the room
    // change reclaimed our run ("reclaimed 1 of our own slot runs (5 slots)"), FindFreeLoadedSlot
    // correctly handed back slot 20 as free, and then this check refused that very slot on the
    // dead creature's leftover filename -- so spawning failed in a room where nothing was
    // actually colliding. Consulting stale metadata about a slot nobody owns is meaningless; only
    // a slot that is really claimed can really collide.
    if (needsLoad) {
        volatile uint8_t* ownerAddr = (volatile uint8_t*)(uintptr_t)(base + loadedPtrTableRva +
            LOADED_SPECIES_OWNER_OFFSET_FROM_PTR + (size_t)species * LOADED_SPECIES_STRIDE);
        volatile uint8_t* stateAddr = (volatile uint8_t*)(uintptr_t)(base + loadedPtrTableRva + LOADED_SPECIES_STATE_OFFSET_FROM_PTR + (size_t)species * LOADED_SPECIES_STRIDE);
        if (*ownerAddr != SPECIES_SLOT_UNCLAIMED_OWNER && *stateAddr != 0) {
            const char* cachedName = (const char*)(uintptr_t)(base + loadedPtrTableRva + LOADED_SPECIES_MODEL_NAME_OFFSET_FROM_PTR + (size_t)species * LOADED_SPECIES_STRIDE);
            if (strncmp(cachedName, modelPath, LOADED_SPECIES_MODEL_NAME_SIZE) != 0) {
                char msg[192];
                snprintf(msg, sizeof(msg), "spawn_enemy: slot %d is CLAIMED (owner=%u) by a different creature this session (cached model file doesn't match %s) -- refusing to avoid corrupting it", species, (unsigned)*ownerAddr, modelPath);
                LogDebug(msg);
                return RefuseSpawn(L, "slot_collision",
                    "spawn_enemy: chosen slot collides with another creature already active in this room -- refusing");
            }
        }
    }

    size_t newSize = (size_t)(oldCount + 1) * PLACEMENT_RECORD_SIZE;
    uint8_t* newTable = (uint8_t*)VirtualAlloc(nullptr, newSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!newTable) {
        return RefuseSpawn(L, "alloc_fail",
            "spawn_enemy: VirtualAlloc failed");
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
                // Same authoritative test as FindFreeLoadedSlot: owner == 0xFF means unclaimed.
                volatile uint8_t* ownerAddr = (volatile uint8_t*)(uintptr_t)(base + loadedPtrTableRva +
                    LOADED_SPECIES_OWNER_OFFSET_FROM_PTR + (size_t)s * LOADED_SPECIES_STRIDE);
                if (*ownerAddr != SPECIES_SLOT_UNCLAIMED_OWNER) { conflictSlot = s; break; }
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
                return RefuseSpawn(L, "run_slot_taken",
                    "spawn_enemy: this creature's asset load would claim several consecutive species slots and one of them is already in use -- refusing to avoid corrupting a live creature");
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
            return RefuseSpawn(L, "load_threw",
                "spawn_enemy: exception during asset-load trigger call");
        }
        // Record immediately, even though the load hasn't completed, to
        // stop a retry frame from triggering a second concurrent load.
        RecordTriggeredLoad(base, loadedPtrTableRva, modelPath, species, (uint8_t)(slotRunLen > 0 ? slotRunLen : 1));

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

    // --- SPECIES RESOURCE-BLOB HEADER VALIDATION (2026-08-11) -- an UPSTREAM fix ---
    // Root cause found by decompiling fnc_link_model_resource_data (Steam 0x140288460), the
    // function the constructor calls right after resolving record+8 to the 256KB species blob.
    // EVERY resource handle it hangs on the new entity is minted straight from the blob's own
    // section-offset header:
    //     entity+0x154 = mint(blob + *(int*)(blob+0x04))
    //     entity+0x1d0 = mint(blob + *(int*)(blob+0x08))   -> fnc_copy_blob_section2_anim_handle
    //     entity+0x134 = mint(blob + *(int*)(blob+0x0c))   -> the collision/shape table
    //     entity+0x68  = mint(blob + *(int*)(blob+0x10))
    // fnc_mint_resource_handle does NOT validate its pointer (session 5 established it is a
    // generic pointer->handle encoder with no requirement that the pointer be a real allocation),
    // so a garbage header mints perfectly well-formed handles pointing into arbitrary memory.
    // Resolving them later returns whatever bytes live there -- and reading +0x14/+0x18 off such
    // an object yields FLOATS, which is exactly the float-shaped bogus handles
    // ResolveHandleBucketGuard has been reporting all along (0.90022, 6, 30, 154.571, 8192, ...).
    // The handles were never corrupted; they were faithfully resolved from a bad blob.
    //
    // This single defect explains, as ONE bug rather than four:
    //   - crash site #4 (fnc_velocity_blend_neighbor_scan)  -> resolve(resolve(entity+0x134)+0x14)
    //   - crash site #9 (FUN_1402b4460, 2026-08-11)         -> resolve(resolve(entity+0x134)+0x18)
    //   - the "exception during constructor call" in Hundred Acre Wood: Meadow (RVA 0x1D6230,
    //     fnc_copy_blob_section2_anim_handle reading resolvedSection2Ptr+0x30)
    //   - the recurring bogus-handle guard hits themselves
    //
    // So rather than add a FIFTH consumer-side guard (sites #6/#7/#8 already proved that does not
    // converge), refuse to construct at all when the blob header is not sane. Deliberately
    // read-only and entirely on our side: no game code is patched, nothing is written, nothing
    // runs on a hot path, and a refusal rolls the table publish back like every other refusal
    // here. That keeps it in the same low-risk category as the bucket-headroom check, and well
    // clear of session 9's blob-PRIMING attempt, which made things worse and was reverted --
    // this never writes blob contents, it only declines to use a bad one.
    //
    // Validity test: the five section offsets must be non-negative, non-decreasing, and inside
    // the blob. That is the exact invariant fnc_link_model_resource_data itself assumes when it
    // computes each section's size as the difference between consecutive entries.
    //
    // THE BLOB SPANS THE WHOLE SLOT RUN, NOT ONE SLOT (corrected 2026-08-11, same evening, from
    // live data). The first version of this check bounded offsets by RESOURCE_BLOB_SIZE
    // (0x40000) and immediately false-positived on every creature, blocking all spawning: real
    // headers came back as e.g. species=20 -> {128, 337920, 1020160, 1020160, 1026688} and
    // species=52 -> {128, 277632, 400768, 400768, 400768}, all perfectly well-formed but far
    // past 0x40000. Cross-checked against each creature's own record+0x56 run length: species 20
    // claims 6 slots (6 * 0x40000 = 1572864 > 1026688) and species 52 claims 5 (1310720 >
    // 400768). Both fit exactly. THAT is why the loader claims a run of CONSECUTIVE slots -- the
    // resource blob is one contiguous region spanning the entire run, so the per-species stride
    // bounds the slot, not the data. Bound by runLen * RESOURCE_BLOB_SIZE accordingly.
    if (speciesResourceTableRva != 0 && species >= 0 && species <= RESOURCE_BLOB_MAX_SPECIES) {
        unsigned long long blobAddr =
            base + speciesResourceTableRva + (unsigned long long)species * RESOURCE_BLOB_SIZE;

        // Bound GENEROUSLY, by everything left in the blob table from this species onward,
        // rather than by this creature's own record+0x56. Reason (2026-08-11): record+0x56 is a
        // property of the CREATURE being spawned, but the blob sitting in the slot was written by
        // whichever creature actually loaded it, which may have claimed a LONGER run -- so a
        // per-creature bound can reject a perfectly good blob. Live data shows how little headroom
        // there is: species 20's real header ends at 1026688 while its own runLen=4 would allow
        // only 1048576. Having already false-positived once here and blocked all spawning, the
        // asymmetry is clear -- a missed bad blob costs a caught SafeCall exception, a false
        // positive costs the user the whole feature. This still rejects genuinely wild values
        // (negative, or hundreds of MB), which is what a garbage header actually looks like.
        int blobRunLen = (int)(int8_t)newRec[0x56];
        if (blobRunLen < 1) blobRunLen = 1;
        const unsigned long long blobSpan =
            (unsigned long long)(RESOURCE_BLOB_MAX_SPECIES + 1 - species) *
            (unsigned long long)RESOURCE_BLOB_SIZE;

        // NINE dwords (+4..+0x24), not five. Reason (2026-08-11 20:36 crash): our spawns are
        // kind==3 entities, so the constructor path they actually take is
        // fnc_link_model_resource_data's DEFAULT branch -> fnc_link_model_resource_data_kind3
        // (Steam 0x140287e40), which reads header entries all the way out to +0x24. The five
        // dwords logged before this change could not even express the gate for entity+0x134 --
        // the field implicated in that crash -- because that gate is (*(+0x18) - *(+0x14)).
        //
        // *** THE SANITY CHECK BELOW DELIBERATELY STILL COVERS ONLY THE FIRST FIVE. ***
        // The non-negative/in-bounds/non-decreasing invariant was live-verified for +4..+0x14
        // only. Nothing has ever observed whether +0x18..+0x24 obey it, and this file's history
        // is emphatic on the cost of guessing: a validator built on an inferred invariant has
        // already blocked all spawning once. The four new entries are LOGGED ONLY. Tighten this
        // later if, and only if, real data says they are monotonic too.
        int32_t sec[9] = {};
        bool readOk = false;
        __try {
            memcpy(sec, (const void*)(uintptr_t)(blobAddr + 4), sizeof(sec));
            readOk = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { readOk = false; }

        bool headerSane = readOk;
        if (headerSane) {
            int32_t prev = 0;
            for (int i = 0; i < 5; ++i) {
                if (sec[i] < 0 || (unsigned long long)sec[i] > blobSpan || sec[i] < prev) {
                    headerSane = false;
                    break;
                }
                prev = sec[i];
            }
            // An all-zero header means the blob was never populated (or was wiped by a room
            // change while our session-global slot bookkeeping still claimed it was loaded --
            // exactly the Hundred Acre Wood case).
            if (headerSane && sec[0] == 0 && sec[4] == 0) headerSane = false;
        }

        // Always log the header, not just on refusal. The Hundred Acre Wood failure is a real,
        // deterministic fault whose blob header has never actually been observed -- the first
        // version of this check guessed at the invariant and got it wrong. Capturing the header
        // on every attempt means the next repro shows what a genuinely bad one looks like
        // side by side with the good ones, instead of another guess.
        if (readOk) {
            // Also log the DERIVED section sizes, because those -- not the raw offsets -- are what
            // fnc_link_model_resource_data_kind3 actually gates each entity field on. A size of 0
            // means that field is NEVER WRITTEN and silently keeps the previous pool occupant's
            // handle (there is no else branch for these five, unlike +0x138/+0x1dc which fall back
            // to mint(0)). entity+0x134 is the field implicated in the 20:36 crash via
            // fnc_velocity_blend_neighbor_scan; +0x140 was already provably stale on 12/12 spawns.
            char hdrMsg[512];
            snprintf(hdrMsg, sizeof(hdrMsg),
                "spawn_enemy: species=%d blob header sections +4..+0x24 = %d, %d, %d, %d, %d, %d, %d, %d, %d "
                "(recRunLen=%d, sane=%d) | field gates: +0x68=%d +0x140=%d +0x13c=%d +0x134=%d +0x4a8=%d "
                "(0 => field left STALE)",
                species, sec[0], sec[1], sec[2], sec[3], sec[4], sec[5], sec[6], sec[7], sec[8],
                blobRunLen, (int)headerSane,
                sec[2] - sec[1],   // entity+0x68
                sec[3] - sec[2],   // entity+0x140
                sec[4] - sec[3],   // entity+0x13c
                sec[5] - sec[4],   // entity+0x134  <-- the crash field
                sec[8] - sec[7]);  // entity+0x4a8
            LogDebug(hdrMsg);
        }

        if (!headerSane) {
            char msg[320];
            if (!readOk) {
                snprintf(msg, sizeof(msg),
                    "spawn_enemy: species=%d resource blob at 0x%llx is unreadable -- refusing to "
                    "construct (would mint handles from garbage; see fnc_link_model_resource_data)",
                    species, blobAddr);
            } else {
                snprintf(msg, sizeof(msg),
                    "spawn_enemy: species=%d resource blob header is invalid "
                    "(sections +4..+0x14 = %d, %d, %d, %d, %d; runLen=%d, blobSpan=%llu) -- "
                    "refusing to construct. The blob was never loaded or was reset by a room "
                    "change while the slot still claimed 'loaded'; constructing would mint "
                    "entity+0x134/+0x1d0/+0x154 from garbage.",
                    species, sec[0], sec[1], sec[2], sec[3], sec[4], blobRunLen, blobSpan);
            }
            LogDebug(msg);
            *tablePtrAddr = oldTable;
            *tableCountAddr = oldCount;
            return RefuseSpawn(L, "blob_invalid",
                "spawn_enemy: this creature's resource data is not valid in this room right now (blob header invalid) -- try again after re-entering the room");
        }
    }

    unsigned long long spawnFnAddr = base + spawnFnRva;
    unsigned long long args[1] = { (unsigned long long)newId };
    unsigned long long result = 0;

    // The slot is loaded and about to be constructed against, so its pointer is populated --
    // the one reliable moment to capture the load-identity token if the trigger-time capture
    // came back 0. No-op once captured, and no-op for a species we never loaded ourselves.
    EnsureTriggeredLoadToken(base, loadedPtrTableRva, (uint8_t)species);

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
        return RefuseSpawn(L, "ctor_threw",
            "spawn_enemy: exception during constructor call");
    }

    // A null return means the constructor refused (room's concurrent-entity
    // budget full), not a crash -- the table is already spliced.
    if (result == 0) {
        char msg[192];
        snprintf(msg, sizeof(msg), "spawn_enemy: constructor call succeeded but returned a null entity -- likely the room's concurrent-entity budget is full (id=0x%x)", newId);
        LogDebug(msg);
        *tablePtrAddr = oldTable;
        *tableCountAddr = oldCount;
        return RefuseSpawn(L, "ctor_refused",
            "spawn_enemy: constructor refused (room's concurrent-entity budget is likely full right now) -- try again after some are cleared");
    }

    // Construction succeeded and returned a non-null entity -- snapshot it immediately, so a
    // "success but nothing appeared" report can be attributed to a specific post-construction
    // state instead of guessed at. Purely read-only.
    LogEntitySnapshot("post-construct:", (uint64_t)result, (uint32_t)newId);
    g_lastSpawnedEntityPtr = (uint64_t)result;
    g_lastSpawnedEntityId = (uint32_t)newId;

    // Remember which species slot this creature was built from, and what model that slot held at
    // the time, so the eviction test can later detect the slot being reused underneath it.
    RecordSpawnedEntity((uint64_t)result, (uint32_t)newId, (uint8_t)species,
                        (uint8_t)LoadedSlotRunLen(base, loadedPtrTableRva, (uint8_t)species),
                        modelPath);

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
        NoteFirstGuardFault("VelocityBlendGuard");
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
        NoteFirstGuardFault("PartyAbilityIndexGuard");
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
        NoteFirstGuardFault("KeyframeListEntryGuard");
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
// --- RESOURCE-ENTRY FALLBACK (PATH B) DIAGNOSTIC (2026-08-11) ---
// Answers the last unverified question upstream of every crash in this investigation.
//
// fnc_resource_entry_lookup_with_fallback (Steam 0x1401dd650) returns a 16-byte "entry record".
// Path A resolves a real table entry. Path B (0x1401dd6ab) silently returns a slice of the HEADER
// OBJECT ITSELF, which callers then read +4/+8 from as resource handles -- which is where the
// deterministic, float-shaped bogus handles come from. That much is already established.
//
// What has NEVER been verified live is WHICH of path B's three triggers actually fires:
//   1. magic absent            -- *(int*)(param_1+0x10) != 0x96969696
//   2. blob handle unresolvable -- resolve(*(int*)(param_1+0x14)) == 0
//   3. empty blob              -- *(void**)resolved == 0
// The fix is completely different for each, which is why guarding consumers has never converged:
// sites #6/#7/#8/#10/#11 are all victims of whatever this is.
//
// Hooked at PATH B's entry rather than the function entry on purpose: this function is a hot
// resource lookup, and instrumenting every call would add a resolve to the hot path. Here the
// cost is only paid when the fallback is actually taken.
//
// Read-only: it re-derives the condition from param_1 and logs. It does not alter the return
// value or the path taken -- the original LEA/SHL are replayed verbatim before resuming.
// g_resolveHandleFnAddrForPathB is defined with the other hook globals near the top of this file.
static bool g_resourceEntryFallbackHookInstalled = false;
static volatile uint64_t g_pathBHitCount = 0;

static void LogResourceEntryFallbackPathB(uint64_t param1, uint64_t idx) {
    uint64_t hit = ++g_pathBHitCount;
    // Full detail for the first 20, then 1-in-1024. Path B can fire extremely often once things
    // go wrong, and burying the first (most diagnostic) hits under a flood would defeat the point.
    if (hit > 20 && (hit & 1023) != 0) return;

    uint32_t magic = 0, blobHandle = 0;
    bool magicOk = TryReadU32(param1 + 0x10, &magic);
    bool handleOk = TryReadU32(param1 + 0x14, &blobHandle);

    const char* why;
    unsigned long long resolved = 0;
    uint64_t blobFirstQword = 0;

    if (!magicOk) {
        why = "COND?: param_1+0x10 unreadable -- param_1 itself is a bad pointer";
    } else if (magic != 0x96969696) {
        why = "COND 1: magic absent (param_1+0x10 != 0x96969696) -- this object is not a resource header at all";
    } else if (!handleOk) {
        why = "COND?: magic ok but param_1+0x14 unreadable";
    } else if (g_resolveHandleFnAddrForPathB == 0) {
        why = "COND 2 or 3 (magic ok; no resolve-fn address configured to tell them apart)";
    } else {
        unsigned long long args[1] = { (unsigned long long)blobHandle };
        if (!SafeCall(g_resolveHandleFnAddrForPathB, args, 1, resolved) || resolved == 0) {
            why = "COND 2: blob handle did NOT resolve -- the handle at param_1+0x14 is itself bad";
        } else {
            uint32_t lo = 0, hi = 0;
            bool ok = TryReadU32(resolved, &lo) && TryReadU32(resolved + 4, &hi);
            blobFirstQword = ok ? (((uint64_t)hi << 32) | lo) : 0;
            if (!ok) {
                why = "COND 3?: blob resolved but its first qword is unreadable";
            } else if (blobFirstQword == 0) {
                why = "COND 3: blob resolved but *blob == 0 -- the table is allocated but EMPTY (load not finished?)";
            } else {
                why = "NONE OF 1/2/3 re-derived -- state changed between the branch and this hook (race), or a 4th path";
            }
        }
    }

    char msg[400];
    snprintf(msg, sizeof(msg),
        "ResourceEntryFallback: PATH B taken (hit #%llu) idx=%llu param_1=0x%llX magic=0x%08X "
        "blobHandle=0x%08X resolved=0x%llX *blob=0x%llX -- %s",
        hit, idx, param1, magic, blobHandle, resolved, blobFirstQword, why);
    LogDebug(msg);
}

// --- RESOURCE-ENTRY LOOKUP CALL COUNTER (2026-08-11) ---
// Settles an ambiguity the path-B hook alone cannot: it recorded ZERO path B hits across a whole
// session that ended in the usual KH1_BehaviorScriptInterpreter crash. That has two readings and
// they point in opposite directions:
//   1. path B genuinely is not taken -- the "bogus handles come from path B" chain does not apply
//      to these crashes, and the null the interpreter reads comes from somewhere else entirely.
//   2. the path-B hook is watching a road with no traffic -- if fnc_resource_entry_lookup_with_
//      fallback is never CALLED, zero hits proves nothing at all.
// "Installed successfully" only means the bytes were patched; it never meant the code ran.
//
// This counts calls at the function's ENTRY and reports the running path-B total alongside, so a
// single log line distinguishes the two readings outright.
//
// Cost: an increment plus a compare on the common path; the log and the magic read happen on call
// #1 and every 4096th thereafter. Registers -- at entry RCX=param_1 and RDX=idx are live and both
// are volatile, so they are pushed around the call; RCX still holds param_1 at the call site, so
// it doubles as the logger's argument for free. Stack: entry RSP%16==8, and push/push/sub 0x28
// lands the CALL on a 16-byte boundary as the ABI requires.
static volatile uint64_t g_resourceEntryLookupCallCount = 0;
static bool g_resourceEntryLookupCounterInstalled = false;

static void LogResourceEntryLookupCall(uint64_t param1) {
    uint64_t n = ++g_resourceEntryLookupCallCount;
    if (n != 1 && (n & 4095) != 0) return;
    uint32_t magic = 0;
    bool ok = TryReadU32(param1 + 0x10, &magic);
    char msg[240];
    snprintf(msg, sizeof(msg),
        "ResourceEntryLookup: call #%llu param_1=0x%llX magic=%s0x%08X -- PATH B hits so far: %llu",
        n, param1, ok ? "" : "(unreadable) ", magic,
        (unsigned long long)g_pathBHitCount);
    LogDebug(msg);
}

static bool InstallResourceEntryLookupCounterHook(unsigned long long hookAddr, unsigned long long resumeAddr) {
    if (g_resourceEntryLookupCounterInstalled) return true;

    unsigned char* hookPtr = (unsigned char*)(uintptr_t)hookAddr;
    // 0x1401dd650: MOV qword ptr [RSP+0x8], RBX -- exactly 5 bytes, so the JMP replaces it whole.
    static const unsigned char expected[5] = { 0x48, 0x89, 0x5C, 0x24, 0x08 };
    if (memcmp(hookPtr, expected, 5) != 0) {
        LogDebug("InstallResourceEntryLookupCounterHook: unexpected original bytes at hook address, aborting");
        return false;
    }

    void* cave = AllocateNear((void*)(uintptr_t)hookAddr, 4096);
    if (!cave) {
        LogDebug("InstallResourceEntryLookupCounterHook: failed to allocate a nearby code cave");
        return false;
    }

    unsigned char stub[96] = {};
    size_t off = 0;
    uint64_t logFnAddr = (uint64_t)(uintptr_t)&LogResourceEntryLookupCall;

    stub[off++] = 0x51;                                         // push rcx  (save param_1)
    stub[off++] = 0x52;                                         // push rdx  (save idx)
    stub[off++] = 0x48; stub[off++] = 0x83; stub[off++] = 0xEC; stub[off++] = 0x28; // sub rsp,0x28
    stub[off++] = 0x48; stub[off++] = 0xB8;                     // mov rax, logFnAddr
    memcpy(stub + off, &logFnAddr, 8); off += 8;
    stub[off++] = 0xFF; stub[off++] = 0xD0;                     // call rax   (arg1 = rcx = param_1)
    stub[off++] = 0x48; stub[off++] = 0x83; stub[off++] = 0xC4; stub[off++] = 0x28; // add rsp,0x28
    stub[off++] = 0x5A;                                         // pop rdx
    stub[off++] = 0x59;                                         // pop rcx

    // Replay the original instruction, now that RSP is back to its entry value.
    stub[off++] = 0x48; stub[off++] = 0x89; stub[off++] = 0x5C; stub[off++] = 0x24; stub[off++] = 0x08;

    stub[off++] = 0xE9; // jmp rel32 -> resumeAddr
    size_t jmpResumeOperand = off; off += 4;

    size_t stubLen = off;
    uintptr_t caveBase = (uintptr_t)cave;

    int32_t jmpResumeRel = (int32_t)((int64_t)resumeAddr - (int64_t)(caveBase + jmpResumeOperand + 4));
    memcpy(stub + jmpResumeOperand, &jmpResumeRel, 4);

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

    g_resourceEntryLookupCounterInstalled = true;
    LogDebug("InstallResourceEntryLookupCounterHook: installed successfully");
    return true;
}

static bool InstallResourceEntryFallbackGuardHook(unsigned long long hookAddr, unsigned long long resumeAddr) {
    if (g_resourceEntryFallbackHookInstalled) return true;

    unsigned char* hookPtr = (unsigned char*)(uintptr_t)hookAddr;
    // 0x1401dd6ab: LEA RAX,[RDI+0x1] ; SHL RAX,0x4   -- 8 bytes, both replayed in the stub.
    static const unsigned char expected[8] = {
        0x48, 0x8D, 0x47, 0x01,   // lea rax, [rdi+1]
        0x48, 0xC1, 0xE0, 0x04    // shl rax, 4
    };
    if (memcmp(hookPtr, expected, 8) != 0) {
        LogDebug("InstallResourceEntryFallbackGuardHook: unexpected original bytes at hook address, aborting");
        return false;
    }

    void* cave = AllocateNear((void*)(uintptr_t)hookAddr, 4096);
    if (!cave) {
        LogDebug("InstallResourceEntryFallbackGuardHook: failed to allocate a nearby code cave");
        return false;
    }

    // Only RBX (param_1) and RDI (idx) are live across this point, and both are non-volatile in
    // the x64 ABI, so a compiled C++ callee preserves them for us -- no manual save/restore.
    // RAX is dead here (the replayed LEA overwrites it). RSP is 16-byte aligned at path B
    // (entry+8, then PUSH RDI, then SUB RSP,0x20), so SUB RSP,0x20 keeps the CALL aligned.
    unsigned char stub[96] = {};
    size_t off = 0;
    uint64_t logFnAddr = (uint64_t)(uintptr_t)&LogResourceEntryFallbackPathB;

    stub[off++] = 0x48; stub[off++] = 0x89; stub[off++] = 0xD9; // mov rcx, rbx   (arg1 = param_1)
    stub[off++] = 0x48; stub[off++] = 0x89; stub[off++] = 0xFA; // mov rdx, rdi   (arg2 = idx)
    stub[off++] = 0x48; stub[off++] = 0x83; stub[off++] = 0xEC; stub[off++] = 0x20; // sub rsp, 0x20
    stub[off++] = 0x48; stub[off++] = 0xB8;                     // mov rax, logFnAddr
    memcpy(stub + off, &logFnAddr, 8); off += 8;
    stub[off++] = 0xFF; stub[off++] = 0xD0;                     // call rax
    stub[off++] = 0x48; stub[off++] = 0x83; stub[off++] = 0xC4; stub[off++] = 0x20; // add rsp, 0x20

    // Replay the two original instructions verbatim, then resume after them.
    stub[off++] = 0x48; stub[off++] = 0x8D; stub[off++] = 0x47; stub[off++] = 0x01; // lea rax, [rdi+1]
    stub[off++] = 0x48; stub[off++] = 0xC1; stub[off++] = 0xE0; stub[off++] = 0x04; // shl rax, 4

    stub[off++] = 0xE9; // jmp rel32 -> resumeAddr
    size_t jmpResumeOperand = off; off += 4;

    size_t stubLen = off;
    uintptr_t caveBase = (uintptr_t)cave;

    int32_t jmpResumeRel = (int32_t)((int64_t)resumeAddr - (int64_t)(caveBase + jmpResumeOperand + 4));
    memcpy(stub + jmpResumeOperand, &jmpResumeRel, 4);

    unsigned char patch[8];
    patch[0] = 0xE9;
    int32_t hookRel = (int32_t)((int64_t)caveBase - (int64_t)(hookAddr + 5));
    memcpy(patch + 1, &hookRel, 4);
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

    g_resourceEntryFallbackHookInstalled = true;
    LogDebug("InstallResourceEntryFallbackGuardHook: installed successfully");
    return true;
}

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

    // Per-DISTINCT-HANDLE bookkeeping (2026-08-11). The previous "first 5 hits only" rule for
    // caller identification looked reasonable but failed in practice the first time it mattered:
    // in the 2026-08-10 crash session the first 5 hits were all the already-known float-shaped
    // effect-emitter handles logged in one burst early on, so the budget was spent long before
    // the handle that actually preceded the fatal crash (0x5665669B, which then hit 1026 times)
    // ever appeared -- and that one was never caller-logged at all. Keying on the distinct handle
    // VALUE instead guarantees every genuinely new bad handle gets its call site named exactly
    // once, which is the whole point of the scan.
    //
    // It also fixes a real second problem: those 1026 byte-identical lines flooded kh1_native.log
    // and buried the spawn diagnostics around them (the same way the reverted freeze watchdog's
    // spam did). Repeats are now collapsed to a periodic line carrying the running count.
    //
    // Racy across threads by design: the worst case is a duplicated log line or a missed
    // dedupe, never a crash or a wrong decision. Nothing here writes game memory, and all of it
    // runs only on the already-rare bad-handle path.
    const int SEEN_CAP = 32;
    static uint32_t seenHandles[SEEN_CAP] = {};
    static uint64_t seenCounts[SEEN_CAP] = {};
    static int seenUsed = 0;

    int slot = -1;
    for (int i = 0; i < seenUsed; ++i) {
        if (seenHandles[i] == maskedHandle) { slot = i; break; }
    }
    bool firstTimeForThisHandle = false;
    uint64_t timesForThisHandle = 0;
    if (slot < 0) {
        if (seenUsed < SEEN_CAP) {
            slot = seenUsed++;
            seenHandles[slot] = maskedHandle;
            seenCounts[slot] = 1;
            firstTimeForThisHandle = true;
            timesForThisHandle = 1;
        } else {
            // Table full -- degrade to the old always-log behaviour rather than go silent.
            firstTimeForThisHandle = true;
            timesForThisHandle = 0;
        }
    } else {
        timesForThisHandle = ++seenCounts[slot];
    }

    char msg[512];
    if (firstTimeForThisHandle) {
        snprintf(msg, sizeof(msg),
            "ResolveHandleBucketGuard: NEW bad handle=0x%08X decodes to bucket=%u (offset=0x%X), "
            "which is NOT claimed -- returning 0 instead of a garbage/-1 pointer. Buckets claimed "
            "right now: %d/%d. Reinterpreted as a float this handle is %g -- a float-shaped or "
            "otherwise non-handle value here means something read a non-handle field AS a handle, "
            "which is a different bug from table exhaustion. (hitsSoFar=%llu, distinctSoFar=%d)",
            maskedHandle, bucketIdx, offset, claimed, RESOLVE_BUCKET_COUNT, (double)asFloat,
            (unsigned long long)g_resolveHandleBucketGuardCount + 1, seenUsed);
        LogDebug(msg);
        // Name the call site once per distinct handle -- this is what turns "some unknown caller"
        // into a named function in a single repro.
        LogResolveHandleBucketGuardCallers();
    } else if ((timesForThisHandle % 1024) == 0) {
        snprintf(msg, sizeof(msg),
            "ResolveHandleBucketGuard: handle=0x%08X (bucket=%u, float %g) seen %llu times now "
            "-- repeats collapsed; call site was logged on its first occurrence.",
            maskedHandle, bucketIdx, (double)asFloat, (unsigned long long)timesForThisHandle);
        LogDebug(msg);
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

        // FIRST fault only. This is the moment the hypothesis is testable: a behaviour-script
        // block has just failed to load, which is the earliest in-process signal that some
        // creature's resource data has gone bad -- and it fired ~1 second before both
        // KH1_BehaviorScriptInterpreter crashes. Checking here, rather than at the next
        // spawn_enemy call, matters because the process may not survive to see another call.
        //
        // Deliberately once: these faults arrive in bursts (8 in one second was observed) and a
        // per-fault dump would bury the answer in its own output.
        if (g_behaviorScriptCopyGuardFaultCount == 1) {
            LogSpawnedEntitySlotIntegrity("BehaviorScriptCopyGuard first fault");
        }
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

// --- VELOCITY-BLEND NEIGHBOR GUARD HOOK (2026-08-11) -- CRASH SITE #4, finally guarded ---
// fnc_velocity_blend_neighbor_UNGUARDED (Steam RVA 0x2B5AF0) has been a known, unfixed
// crash site since 2026-08-04, and was the TOP remaining crash site as of 2026-08-10 --
// it faulted at RVA 0x2B5BF0 in two separate live crashes, six days apart, at the exact
// same instruction. The 2026-08-10 crash came from the game's own CrashDump.dmp:
//     ExceptionCode = 0xC0000005 (read) at 0x7FF7EEA98CB8  -- wild, ~4GB past the image
//     Rip = exe + 0x2B5BF0
//     Rdx = g_apResourceHandleBuckets (the bucket TABLE base) -- i.e. immediately
//           downstream of a resolve, the same family as every other site here.
//
// The function walks every other live entity looking for a nearby one to blend velocity
// against. Per iteration it resolves entity+0x134 and chains through the result with NO
// validation anywhere -- there are FOUR unguarded dereferences of a resolve result in
// this one loop, not just the instruction that happened to fault:
//     0x2B5BC4  MOV ECX,[RAX+0x14]     ; 1st resolve's result, deref'd raw
//     0x2B5BDC  CMP byte ptr [RAX+5],SIL ; 3rd resolve's result -> the loop bound
//     0x2B5BF0  MOVZX ECX,byte ptr [RBX-1] ; RBX = 2nd resolve's result + 2  <-- faulted
//     0x2B5DDD  MOVZX ECX,[RAX+5]      ; loop bound re-read every iteration
// Guarding only the faulting instruction would leave the other three live, and this
// investigation has already established (sites #6/#7/#8) that patching one consumer
// instruction at a time does not converge. So this wraps the WHOLE function, using the
// same two-trampoline __try/__except technique as InstallSkeletonBlendCallGuardHook and
// InstallJointRemapSetupGuardHook.
//
// WHY RETURNING 0 IS SAFE HERE, AND WHY THAT IS UNUSUALLY CLEAN FOR THIS INVESTIGATION:
// unlike InstallVelocityBlendGuardHook -- where session 18 found that "skip" left the
// caller's uninitialized stack buffer to be folded into real entity state, and the fix
// had to zero-fill it -- this function has NO output-parameter side effects on the
// failure path. It writes to param_2 (+4) only on the two success paths, immediately
// before returning the found neighbor. Its own "no neighbor found" path is literally
// `XOR EAX,EAX; ret` with param_2 untouched, which is the overwhelmingly common outcome
// every frame. Its single caller (FUN_14029b4e0) consumes the result as
// `FUN_140297df0(param_1, result)`, which just mints a handle from it into
// entity+0x398 -- and minting 0 ("minted null") is an established, normal pattern in
// this engine. So an exception -> return 0 is not a synthesized fallback value; it is
// exactly the function's own in-band "nothing nearby this frame" result.
static bool g_velocityBlendNeighborGuardHookInstalled = false;
static volatile uint64_t g_velocityBlendNeighborGuardSkipCount = 0;

typedef int64_t (*VelocityBlendNeighborOriginalFn)(int64_t, int64_t);
static VelocityBlendNeighborOriginalFn g_velocityBlendNeighborOriginalTrampoline = nullptr;

// Tail-jumped into with the exact register/stack state the real function's caller set up
// (RCX = entity, RDX = motion/velocity struct), matching Ghidra's decompiled signature.
static int64_t CallVelocityBlendNeighborSafe(int64_t param_1, int64_t param_2) {
    __try {
        return g_velocityBlendNeighborOriginalTrampoline(param_1, param_2);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        char msg[192];
        snprintf(msg, sizeof(msg),
            "VelocityBlendNeighborGuard: exception walking neighbor entities, reporting 'no neighbor' for this entity/frame (skipsSoFar=%llu)",
            (unsigned long long)g_velocityBlendNeighborGuardSkipCount + 1);
        LogDebug(msg);
        NoteFirstGuardFault("VelocityBlendNeighborGuard");
        ++g_velocityBlendNeighborGuardSkipCount;
        return 0;
    }
}

static bool InstallVelocityBlendNeighborGuardHook(unsigned long long hookAddr) {
    if (g_velocityBlendNeighborGuardHookInstalled) return true;

    unsigned char* hookPtr = (unsigned char*)(uintptr_t)hookAddr;
    // mov rax,rsp; mov [rax+0x18],rbx  -- 7 bytes, no RIP-relative operands, so they
    // can be replayed verbatim from the cave. Verified byte-for-byte against the live
    // Ghidra database before shipping (48 8b c4 48 89 58 18).
    static const unsigned char expected[7] = { 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x18 };
    if (memcmp(hookPtr, expected, 7) != 0) {
        LogDebug("InstallVelocityBlendNeighborGuardHook: unexpected original bytes at hook address, aborting");
        return false;
    }

    void* replayCave = AllocateNear((void*)(uintptr_t)hookAddr, 4096);
    if (!replayCave) {
        LogDebug("InstallVelocityBlendNeighborGuardHook: failed to allocate replay code cave");
        return false;
    }
    {
        unsigned char stub[16] = {};
        size_t off = 0;
        memcpy(stub + off, expected, 7); off += 7; // replay original 7 bytes
        stub[off++] = 0xE9; // jmp rel32 -> hookAddr+7 (rest of the real function body)
        size_t jmpOperand = off; off += 4;
        size_t stubLen = off;
        uintptr_t caveBase = (uintptr_t)replayCave;
        int32_t jmpRel = (int32_t)((int64_t)(hookAddr + 7) - (int64_t)(caveBase + jmpOperand + 4));
        memcpy(stub + jmpOperand, &jmpRel, 4);
        memcpy(replayCave, stub, stubLen);
        FlushInstructionCache(GetCurrentProcess(), replayCave, stubLen);
    }
    g_velocityBlendNeighborOriginalTrampoline = (VelocityBlendNeighborOriginalFn)(uintptr_t)replayCave;

    void* redirectCave = AllocateNear((void*)(uintptr_t)hookAddr, 4096);
    if (!redirectCave) {
        LogDebug("InstallVelocityBlendNeighborGuardHook: failed to allocate redirect code cave");
        return false;
    }
    {
        unsigned char stub[16] = {};
        size_t off = 0;
        uint64_t wrapperAddr = (uint64_t)(uintptr_t)&CallVelocityBlendNeighborSafe;
        // Clobbering RAX here is safe: this is a function ENTRY point, and RAX carries
        // no argument under the x64 calling convention. The replayed `mov rax,rsp`
        // re-establishes it inside the cave, against the trampoline call's own (equally
        // valid, shadow-space-backed) frame.
        stub[off++] = 0x48; stub[off++] = 0xB8; // mov rax, wrapperAddr
        memcpy(stub + off, &wrapperAddr, 8); off += 8;
        stub[off++] = 0xFF; stub[off++] = 0xE0; // jmp rax
        size_t stubLen = off;
        memcpy(redirectCave, stub, stubLen);
        FlushInstructionCache(GetCurrentProcess(), redirectCave, stubLen);
    }

    unsigned char patch[7];
    patch[0] = 0xE9;
    int32_t hookRel = (int32_t)((int64_t)(uintptr_t)redirectCave - (int64_t)(hookAddr + 5));
    memcpy(patch + 1, &hookRel, 4);
    patch[5] = 0x90; // nop, pad the remaining 2 original bytes
    patch[6] = 0x90;

    std::vector<HANDLE> threads = SuspendOtherThreads();

    DWORD oldProtect = 0;
    VirtualProtect((void*)(uintptr_t)hookAddr, 7, PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy((void*)(uintptr_t)hookAddr, patch, 7);
    VirtualProtect((void*)(uintptr_t)hookAddr, 7, oldProtect, &oldProtect);

    FlushInstructionCache(GetCurrentProcess(), (void*)(uintptr_t)hookAddr, 7);

    ResumeThreads(threads);

    g_velocityBlendNeighborGuardHookInstalled = true;
    LogDebug("InstallVelocityBlendNeighborGuardHook: installed successfully");
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
        NoteFirstGuardFault("SkeletonBlendCallGuard");
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
    {"quiet_spawned_entities", reinterpret_cast<void*>(l_quiet_spawned_entities)},
    {"census_live_entities", reinterpret_cast<void*>(l_census_live_entities)},
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
