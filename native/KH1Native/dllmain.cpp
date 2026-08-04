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
// 2. Identifies the creature by its real, stable identity -- the model/motion
//    filename pair (e.g. "xa_ex_2010.mdls"/"xa_ex_2010.mset" = Soldier) --
//    never by species/slot number, which was proven this investigation to be
//    a per-room-LOCAL index with no fixed meaning (the same number is a
//    different creature in different rooms). char-id/weight/a full template
//    record for this filename come from kh1_creature_data.lua (generated
//    offline from every room's own .ard file -- see generate_creature_data.py);
//    this refuses cleanly rather than guess if the model isn't in that table.
//    Two cases, in order:
//     a. Some local slot is already loaded with this exact creature this
//        session -- reuse that slot's number via the template, no reload
//        needed. See FindLoadedSlotByFilename/FindFreeLoadedSlot above.
//     b. No slot has it loaded yet -- claim the first slot untouched this
//        session (state byte == 0 in the per-species asset-load struct) and
//        go through the full mint-handle + load-trigger path.
//    (Session 21: this used to have a third, first-preference case --
//    cloning a record already native to the current room, via
//    fnc_resolve_resource_handle -- removed once the .ard table made
//    deriving data from a live room visit redundant. See the heartless
//    field-spawn investigation memory, "Session 21 continued".)
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
static const int SPECIES_SLOT_COUNT = 256; // species/slot index is a uint8_t (record+0x55) -- the full addressable range

// species (record+0x55) was proven this investigation to be a per-room-LOCAL
// slot index, not a stable creature ID -- the same numeric value means a
// different creature in different rooms (species=30 is a Shadow in Traverse
// Town 2nd District but a Soldier-adjacent slot in Green Room; species=34 is
// Soldier in both Green Room and Alleyway, but that's a coincidence, not a
// rule -- see KH1-EVDL-TOOLS/docs/enemy_ai/heartless_field_spawn_investigation.md,
// sessions 5-6). The real, stable identity is the creature's model/motion
// filename pair (e.g. "xa_ex_2010.mdls"/"xa_ex_2010.mset" = Soldier,
// confirmed via fnc_resolve_resource_handle). So spawn_enemy no longer takes
// a species number at all -- callers identify a creature by its filename
// pair, and l_spawn_enemy below figures out which local slot number (if any)
// already holds it, or claims a free one, entirely on its own.
//
// Per-creature char-id/weight/template data (session 21 onward) lives
// entirely in kh1_creature_data.lua, generated offline from every room's own
// .ard file (see generate_creature_data.py) -- Lua looks it up and passes it
// into l_spawn_enemy as luaCharId/luaWeight/luaTemplateRecord. This used to
// also be sourced from a hardcoded kKnownCreatures array and a
// live-learned-this-session cache (g_learnedCreatures), both keyed the same
// way but far more limited in coverage; both were removed once the .ard
// table made them redundant -- see the heartless field-spawn investigation
// memory, "Session 21 continued".
//
// A per-species resource-blob table (DAT_140d2ada0 on Steam RVA 0xd2ada0,
// DAT_140d2b880 on EGS RVA 0xd2b880 -- 0x40000/256KB stride per species/slot
// index) that fnc_spawn_world_gimmick_entity's kind==3 setup path
// (FUN_140288460 -> FUN_140287e40 on Steam) reads and parses as a small
// section-offset table. RESOURCE_BLOB_MAX_SPECIES bounds
// MarkSpeciesInUseByLiveEntities/FindFreeLoadedSlot below (FUN_140285db0:
// `(ptr - DAT_140d2ada0) >> 0x12`, refuses if > 0x40 -- 65 valid entries,
// species 0..64). A session 9-era mechanism that captured and re-primed this
// table's content per-species was live-tested, made a crash WORSE, and was
// fully reverted -- the fresh-load path never actually needed it (sessions
// 13-14 found `fnc_load_gimmick_assets` mints record+8 into the correct
// slice itself, unconditionally, before construction).
static const size_t RESOURCE_BLOB_SIZE = 0x40000;
static const int RESOURCE_BLOB_MAX_SPECIES = 0x40;

// Session 16 (2026-07-23) live-debugging aid: the entity id of whichever
// spawn_enemy call most recently reached the constructor. Exists purely so
// an external CE hook at the confirmed fault site (RVA 0x2b5e66, inside
// FUN_1402b5e50) can compare a live entity pointer's own id field
// (entity+4, per session 4) against this value and only intervene for THIS
// specific spawn's entity -- session 15's magnitude-check and
// blanket-substitution attempts both proved unsafe/insufficient because
// they couldn't distinguish our buggy entity from any of the function's
// 11+ other unrelated callers. Not read anywhere in this DLL itself.
static volatile uint32_t g_lastSpawnAttemptId = 0;

// Session 19 fix -- see InstallJobRecordGuardHook's plate comment. Set to the
// exact placement-record pointer we're about to hand to fnc_load_gimmick_assets,
// right before that call. fnc_load_gimmick_assets/FUN_140286420 is genuinely
// shared, engine-wide infrastructure -- EVERY EVDL room script's own gimmick
// asset loads (treasure chests, NPCs, scenery) go through the exact same job
// struct shape, most of them with a real, non-null completion callback at
// job+0x28 that something else in the engine blocks on. A first live test that
// guarded ALL jobs indiscriminately (not just our own) skipped one of those
// unrelated real jobs' callback and hard-froze the game -- confirmed via
// decompile: `fnc_load_gimmick_assets`'s only caller path relevant to us
// (`l_spawn_enemy`, below) always passes `0` for that callback argument, so
// OUR jobs alone are safe to skip without invoking it. The guard hook compares
// the job's own (always-safe-to-read) `job+0x20` pointer VALUE against this
// global -- not a dereference of what it points to -- before ever applying its
// staleness check, so every other caller's job is left completely untouched.
static volatile uint64_t g_lastQueuedGimmickRecordPtr = 0;

// Live-confirmed fix, same session: the check above (comparing the job's
// captured record pointer against the CURRENT placementTablePtr/Count) has a
// real false-positive -- `l_spawn_enemy`'s own 10s-timeout refusal path rolls
// placementTablePtr back to the OLD table (the session-11 anti-phantom-record
// fix) BEFORE this job's callback can possibly fire, which makes our own
// freshly-queued job look "stale" purely because of that self-rollback, with
// zero room transition ever happening. Confirmed live: two consecutive
// timeout attempts both got skipped this way, permanently starving that
// species' background load from ever completing (the callback we skip is
// also what would have written its real completion state). The actual
// question is "did the ROOM change", not "does the record still match
// whatever placementTablePtr says right now" -- so snapshot the room's own
// identity (g_WorldNumber/g_AreaNumber/g_SetNumber, read -- never written --
// by the same room-load routine, FUN_140287c60, that recomputes
// placementTablePtr) at tag time instead, and compare THAT at check time.
static volatile int32_t g_lastQueuedGimmickWorld = 0;
static volatile int32_t g_lastQueuedGimmickArea = 0;
static volatile int32_t g_lastQueuedGimmickSet = 0;
static volatile bool g_lastQueuedGimmickRoomIdentityValid = false;

// Session 20 (2026-08-02): tracks every model_path we've ourselves already
// called fnc_load_gimmick_assets for this session, and which species slot we
// used. Exists because removing the old blocking Sleep-poll (see
// spawn_enemy's comment in kh1_lua_library.lua) means l_spawn_enemy can now
// be re-entered on a LATER real frame while a load is still in flight --
// and fnc_load_gimmick_assets itself zeroes the engine's own state byte at
// the START of a fresh load (only the async callback advances it once it
// actually runs), so FindLoadedSlotByFilename (which skips state==0 slots)
// cannot see an in-flight load of ours and would otherwise let
// FindFreeLoadedSlot hand out a trigger call again on every single retry
// frame until the callback finally runs -- queuing multiple concurrent async
// jobs for the same record. Live-confirmed this is a real crash, not a
// theoretical one: the first per-frame-retry build crashed with zero Windows
// crash telemetry (matching session 17's "no WerFault" signature for a
// stacked/concurrent job corruption, as opposed to the well-understood
// single-job 0x2b5e66 fault sessions 8-19 chased). This table is checked
// BEFORE FindLoadedSlotByFilename so our own in-flight loads are always
// recognized immediately, not just once the engine's table catches up.
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

// Never removed once added (not even once the load completes) -- the
// species assignment remains this session's correct slot for this creature
// for the same reason FindLoadedSlotByFilename's own matches stay valid
// indefinitely once made.
static void RecordTriggeredLoad(const char* modelPath, uint8_t species) {
    if (g_triggeredLoadCount >= MAX_TRIGGERED_LOADS) return;
    TriggeredLoadEntry& entry = g_triggeredLoads[g_triggeredLoadCount];
    strncpy_s(entry.modelPath, modelPath, _TRUNCATE);
    entry.species = species;
    g_triggeredLoadCount++;
}

// model_path/motion_path arrive from Lua as p_lua_tolstring pointers, valid only
// for the duration of this call -- Lua's GC is free to reclaim that string the
// moment l_spawn_enemy returns. But fnc_load_gimmick_assets's async pipeline
// (FUN_140286420, queued via the load-trigger call below) doesn't actually
// dereference the resolved model-path handle until a LATER frame, sometimes
// 10+ seconds later per the poll timeout -- confirmed live 2026-07-23 via a
// hardware breakpoint trace: load_trigger_entry and async_tick_entry each fired
// exactly once, then the process died inside FUN_140286420 itself (never
// reaching the finalize callback or the constructor), for the same
// xa_ex_2010.mdls/.mset spawn on every attempt. Minting a handle straight from
// the raw Lua pointer (as this used to) hands the engine a pointer that can be
// dangling by the time anything actually reads it -- a use-after-free, not an
// engine bug. Fix: copy into DLL-owned static storage (a "must outlive this
// call" cache) and mint handles from that instead.
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

// Fallback path only: scan the per-species
// asset-load state struct across every possible slot for one already
// holding this exact creature (state != 0, cached filename matches --
// reuse it, e.g. a prior fallback spawn of the same creature this session)
// or, failing that, the first slot untouched this session (state == 0 --
// safe to claim). Trying the filename match first means a creature already
// loaded via a non-placement-table path (rare, but the collision guard
// below exists precisely because it's possible) gets reused instead of
// wastefully claiming a second slot for the same asset.
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

// Both FindLoadedSlotByFilename and FindFreeLoadedSlot only ever scan the
// SESSION-GLOBAL loadedSpeciesPtrTable (256 raw slot numbers, no notion of
// which room means what by any of them) -- neither has ever checked whether
// the CURRENT room's own placement table already uses the chosen species
// number for a real, unrelated native record. Species is a per-room-LOCAL
// slot index (see session 5's foundational correction) -- reusing a number
// another creature/object in THIS room already owns splices our clone right
// on top of it. Confirmed live 2026-07-22 (session 11): a fallback spawn in
// Accessory Shop picked up slot 28 (already marked loaded globally from an
// earlier native Soldier spawn elsewhere), which collided with Accessory
// Shop's own unrelated native record at that same local slot -- first call
// spawned something invisible/uninteractable, second call crashed the game.
// record+0x60 holds a handle (same bucket-table encoding as record+8) to the
// model filename string, and fnc_resolve_resource_handle (Steam RVA
// 0x38ADC0, EGS 0x38B0B0 -- confirmed live 2026-07-22 against real Alleyway
// records) resolves it back to a real, in-process-readable string pointer --
// this DLL runs inside the game process, so no external memory-read is
// needed, a plain strncmp against the resolved pointer works.
static bool ResolvedModelMatches(unsigned long long resolveFnAddr, uint32_t modelHandle, const char* wantModel) {
    if (modelHandle == 0) return false;
    unsigned long long args[1] = { (unsigned long long)modelHandle };
    unsigned long long resolved = 0;
    if (!SafeCall(resolveFnAddr, args, 1, resolved) || resolved == 0) return false;
    return strncmp((const char*)(uintptr_t)resolved, wantModel, LOADED_SPECIES_MODEL_NAME_SIZE) == 0;
}

// Session 21 (2026-08-02) fix: a room's own native record of the EXACT
// creature being spawned used to never reach this check at all (caught
// earlier by the now-removed native-clone case). Once that case was removed,
// spawning a creature already native to its own room started false-positive
// refusing here -- RoomHasNativeSpecies only checked "is this species number
// used by ANY record", with no notion of "used by the SAME creature we're
// about to spawn" being perfectly fine (we're about to reuse that exact
// slot). Now resolves each same-species record's own model handle and only
// treats it as a real collision if it belongs to a genuinely DIFFERENT
// creature. Live-confirmed: the false-positive refusal (spawning Soldier in
// a room where Soldier is already native, broken immediately after the
// native-clone case was removed) is fixed by this change.
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

// Session 20 (2026-08-02): walks every live entity in the same 96-slot pool
// and iteration order FUN_1402b6390 itself uses (FUN_140291b20(0) to start,
// FUN_140291b20(prevEntity) to advance, 0 means done -- decompiled and
// confirmed to be a pure, read-only, always-safe walk over a fixed,
// session-lifetime-allocated array, `DAT_142d372a0`..`DAT_142d5349f`, no
// special calling context required). For each entity, reads its own
// `entity+0x134` resource handle (a per-species type-def reference,
// confirmed by session 19's callgraph trace of FUN_1402b6af0/FUN_1402b6c10
// to be exactly what those functions -- and now also the unrelated
// party-ability-rebuild routine found this session -- depend on) and, if it
// resolves into the shared resource-blob table's address range, marks that
// species as "in use by something live right now" using the same
// `(ptr - tableBase) >> 18` slice math FUN_140285db0 itself uses for its own
// bounds check. This is the live-entity walk every prior session since 15
// scoped out as the real fix but never implemented -- FindFreeLoadedSlot
// below now refuses to hand out any species number this marks, in addition
// to its existing "session-global load state" check, closing the collision
// class that starting the scan at 20 (session 19) only partially avoided.
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
// found via static analysis 2026-07-23 (session 15) while investigating the
// fresh-load fallback crash: fnc_load_gimmick_assets itself
// (`&DAT_140d2ada0 + species*0x40000`, FUN_140285ee0) does NO bounds check
// at all before minting a handle to that address, unlike
// fnc_spawn_world_gimmick_entity's own record+8 self-heal path (which goes
// through FUN_140285db0's `> 0x40` refusal first). A species number above 64
// returned here would mint a handle pointing well outside the real 65-entry/
// 16MB resource-blob table, and the async load callback would later write
// real parsed model data through it -- unguarded heap corruption, not a
// clean refusal. SPECIES_SLOT_COUNT (a uint8_t's full range) remains correct
// for FindLoadedSlotByFilename above, which only ever reads existing state,
// never hands out a fresh species number to be used as a table index.
// Starts scanning from 20 instead of 0 (session 19, 2026-08-02): low slot
// numbers (species=1, picked every time from a fresh session) collide with
// something ALREADY relying on that resource-blob slot that isn't tracked
// in the room's placement table at all -- some other live entity's own
// cached `entity+0x134` type-def handle, corrupted by our unconditional
// resource-blob remint, causing an unrelated entity's velocity-blend read
// to underflow (FUN_1402b6af0/FUN_1402b6c10's `count-1`-style loop) minutes
// later -- live-confirmed as the mechanism behind an intermittent crash on
// this whole path. RoomHasNativeSpecies only checks placement-table
// records, not arbitrary live entities, so it can't catch this class of
// collision. Session 20 (2026-08-02) found starting at 20 wasn't a complete
// fix either -- species=20 collided with a completely different live
// system (an unrelated per-party ability-data rebuild routine, crashing
// inside FUN_1401d62e0 with zero relation to entity rendering/velocity at
// all) -- proving there is no fixed "safe" species range; it depends on
// whatever's currently loaded. Session 20 implemented the walk this
// comment already called for: see MarkSpeciesInUseByLiveEntities below,
// which mirrors FUN_1402b6390's own iteration (FUN_140291b20 over the
// 96-slot entity pool, each entity's own +0x134 handle resolved and mapped
// back to a species number) and is now consulted here in addition to the
// engine's own loadedSpeciesPtrTable state. Session 15 found slots ~48-64
// are hardcoded-reserved by unrelated engine subsystems (menu/JP-sysfont,
// voice, event motion/effect), so this stays well clear of that range too.
static bool FindFreeLoadedSlot(unsigned long long base, unsigned long long loadedPtrTableRva, const bool* speciesInUseByLiveEntity, uint8_t* outSpecies) {
    for (int s = 20; s <= RESOURCE_BLOB_MAX_SPECIES; ++s) {
        if (speciesInUseByLiveEntity && speciesInUseByLiveEntity[s]) continue;
        volatile uint8_t* stateAddr = (volatile uint8_t*)(uintptr_t)(base + loadedPtrTableRva + LOADED_SPECIES_STATE_OFFSET_FROM_PTR + (size_t)s * LOADED_SPECIES_STRIDE);
        if (*stateAddr == 0) {
            *outSpecies = (uint8_t)s;
            return true;
        }
    }
    return false;
}

// Forward declarations -- both hooks are defined later in this file (near
// the rest of the code-cave/JMP-trampoline hook infrastructure) but need to
// be installed from here, unconditionally, before any fallback-template
// spawn. See each function's own definition for the full mechanism.
static bool InstallJobRecordGuardHook(unsigned long long hookAddr, unsigned long long resumeAddr,
                                        unsigned long long tablePtrRva, unsigned long long tableCountRva,
                                        unsigned long long worldNumRva, unsigned long long areaNumRva,
                                        unsigned long long setNumRva);
static bool InstallVelocityBlendGuardHook(unsigned long long hookAddr, unsigned long long resumeAddr);
static bool InstallPartyAbilityIndexGuardHook(unsigned long long hookAddr, unsigned long long resumeAddr,
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
    // Room-identity globals (g_WorldNumber/g_AreaNumber/g_SetNumber), added
    // for InstallJobRecordGuardHook's room-identity fix -- see the tag site
    // below and CheckJobRecordPointerSafe's comment. Optional: 0/unset on a
    // build where they haven't been located yet (EGS as of this writing);
    // the guard hook falls back to its older, more conservative table-bounds
    // heuristic in that case.
    unsigned long long worldNumRva = (unsigned long long)p_lua_tointegerx(L, 14, nullptr);
    unsigned long long areaNumRva = (unsigned long long)p_lua_tointegerx(L, 15, nullptr);
    unsigned long long setNumRva = (unsigned long long)p_lua_tointegerx(L, 16, nullptr);
    // Entry points for InstallJobRecordGuardHook/InstallVelocityBlendGuardHook
    // -- installed below (idempotent, once per process) before any fallback-
    // template spawn is attempted. Both are load-bearing fixes for real,
    // live-confirmed crashes on this exact path (session 19); a fallback
    // spawn refuses cleanly rather than proceed unprotected if either RVA
    // is 0 (not configured on this build) or either install fails.
    unsigned long long jobCallbackFnRva = (unsigned long long)p_lua_tointegerx(L, 17, nullptr);
    unsigned long long velocityBlendFnRva = (unsigned long long)p_lua_tointegerx(L, 18, nullptr);
    // Session 20: entry point for FUN_140291b20 (the 96-slot live-entity pool
    // iterator), used by MarkSpeciesInUseByLiveEntities to avoid handing out
    // a species number some live entity's own +0x134 handle already
    // references. Optional (0 = not configured on this build, e.g. EGS as of
    // this writing) -- FindFreeLoadedSlot degrades to session 19's
    // start-at-20 heuristic alone in that case, same as before this session.
    unsigned long long entityIterFnRva = (unsigned long long)p_lua_tointegerx(L, 19, nullptr);
    // Session 20: entry point of the CALL-to-fnc_resolve_resource_handle
    // instruction inside FUN_1401d62e0 (Steam RVA 0x1D62F2, five bytes
    // before the 0x1D62F7 fault site) -- see
    // InstallPartyAbilityIndexGuardHook's comment for the crash this fixes.
    // Optional (0 = not configured on this build) -- a fallback spawn
    // refuses cleanly rather than proceed unprotected, same as the other
    // two guard hooks.
    unsigned long long partyAbilityIndexFnRva = (unsigned long long)p_lua_tointegerx(L, 20, nullptr);
    // Session 21: the only source of fallback char-id/weight/template data,
    // looked up Lua-side (kh1_creature_data.lua, generated from every room's
    // own .ard file -- see generate_creature_data.py and the heartless
    // field-spawn investigation memory, "Session 21"/"Session 21 continued").
    // charId==0 is never a real value (record+0x4c==0 is Sora's own party
    // slot, forced below) so it doubles as "no offline data for this model",
    // matching every other optional trailing arg's 0-means-unset convention
    // in this function.
    // luaTemplateRecord is a raw (possibly-embedded-NUL) byte string, so its
    // real length MUST come from lua_tolstring's own out-param, not strlen --
    // uninitialized here first since a nil Lua value leaves *len untouched.
    long long luaCharId = (long long)p_lua_tointegerx(L, 21, nullptr);
    long long luaWeight = (long long)p_lua_tointegerx(L, 22, nullptr);
    size_t luaTemplateLen = 0;
    const char* luaTemplateRecord = p_lua_tolstring(L, 23, &luaTemplateLen);

    if (!modelPath || !motionPath) {
        p_lua_pushboolean(L, 0);
        p_lua_pushstring(L, "spawn_enemy: model_path/motion_path are required");
        return 2;
    }
    // Copy off the Lua VM's own string storage immediately -- see InternPath's
    // comment above. Everything below (mint calls, resource-blob capture,
    // logging) uses these stable pointers instead of the raw Lua ones.
    modelPath = InternPath(modelPath);
    motionPath = InternPath(motionPath);
    // resolveHandleFnRva is load-bearing for MarkSpeciesInUseByLiveEntities
    // and InstallPartyAbilityIndexGuardHook below. A 0 RVA (this build's
    // Global address table hasn't got it yet -- see EGSGlobal_1_0_0_10.lua's
    // TODO) must refuse cleanly rather than compute `base + 0` and treat the
    // module header as a function address.
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

    // species (the local slot number, record+0x55) is never chosen by the
    // caller -- it's derived below from whichever of two cases applies:
    //  1. Some local slot is already loaded with this exact creature this
    //     session (e.g. a prior spawn of it) -- reuse that slot number, no
    //     reload needed.
    //  2. Neither -- claim the first slot untouched this session and go
    //     through the full mint + load-trigger path.
    // (Session 21: this used to have a third, first-preference case --
    // cloning a record already native to the current room -- removed once
    // kh1_creature_data.lua's offline .ard data made deriving char-id/
    // weight/template from a live room visit redundant. See the heartless
    // field-spawn investigation memory, "Session 21 continued".)
    uint8_t species = 0;
    bool needsLoad = false;

    // kh1_creature_data.lua (looked up Lua-side, generated offline from
    // every room's own .ard file -- see generate_creature_data.py) is now
    // the only source of char-id/weight/template data; refuse cleanly if
    // this model isn't in it rather than guess. luaTemplateRecord is
    // consumed synchronously below (memcpy'd into the new placement record
    // well before this function returns), so pointing directly at Lua's own
    // string storage is safe, unlike modelPath/motionPath (which the async
    // load callback dereferences up to ~10s later -- see InternPath's
    // comment -- and therefore must be interned).
    if (luaCharId == 0 || !luaTemplateRecord || luaTemplateLen != PLACEMENT_RECORD_SIZE) {
        p_lua_pushboolean(L, 0);
        p_lua_pushstring(L, "spawn_enemy: no offline fallback data for this creature (missing from kh1_creature_data.lua)");
        return 2;
    }
    const uint8_t* templateRec = (const uint8_t*)luaTemplateRecord;
    uint16_t charId = (uint16_t)luaCharId;
    uint8_t weight = (uint8_t)luaWeight;

    // loadedPtrTableRva is load-bearing for both slot-scan helpers below --
    // a 0 RVA here (this build's Global address table hasn't got it yet)
    // must refuse cleanly rather than scan garbage at `base + 0 + offset`.
    if (loadedPtrTableRva == 0) {
        p_lua_pushboolean(L, 0);
        p_lua_pushstring(L, "spawn_enemy: loadedSpeciesPtrTable address not configured for this game build");
        return 2;
    }
    // Install all three crash-fixing hooks (idempotent -- a no-op if already
    // installed this process) before touching anything else. All are
    // load-bearing, live-confirmed fixes for real crashes on this exact path
    // (sessions 19-20) -- refuse cleanly rather than proceed unprotected if
    // any RVA is unconfigured for this build or any install itself fails.
    if (jobCallbackFnRva == 0 || velocityBlendFnRva == 0 || partyAbilityIndexFnRva == 0) {
        p_lua_pushboolean(L, 0);
        p_lua_pushstring(L, "spawn_enemy: job-record-guard/velocity-blend-guard/party-ability-index-guard hook addresses not configured for this game build");
        return 2;
    }
    if (!InstallJobRecordGuardHook(base + jobCallbackFnRva, base + jobCallbackFnRva + 5,
                                     tablePtrRva, tableCountRva, worldNumRva, areaNumRva, setNumRva) ||
        !InstallVelocityBlendGuardHook(base + velocityBlendFnRva, base + velocityBlendFnRva + 5) ||
        // +9, not +5: the hook replaces BOTH the 5-byte call and the
        // 4-byte risky read that immediately follows it (see
        // InstallPartyAbilityIndexGuardHook's comment) -- resumeAddr
        // must skip past both, landing at "add rsp,0x20", or the stub's
        // own jmp lands back on the original unpatched read and
        // re-triggers the exact crash this hook exists to prevent.
        !InstallPartyAbilityIndexGuardHook(base + partyAbilityIndexFnRva, base + partyAbilityIndexFnRva + 9, base + resolveHandleFnRva)) {
        p_lua_pushboolean(L, 0);
        p_lua_pushstring(L, "spawn_enemy: failed to install the async-load/velocity-blend/party-ability-index guard hooks -- refusing rather than risk the crash they fix");
        return 2;
    }
    // Session 12: mintHandleFnRva is required regardless of which of the two
    // sub-branches below ends up taken -- see the re-mint block further down
    // for why. Checked here, before the placement table is touched, for the
    // same "keep refusals a true no-op" reason the loadAssetsFnRva check in
    // the FindFreeLoadedSlot branch already exists.
    if (mintHandleFnRva == 0) {
        p_lua_pushboolean(L, 0);
        p_lua_pushstring(L, "spawn_enemy: fnc_mint_resource_handle not configured for this game build -- can't safely reuse a captured template's model/motion handles");
        return 2;
    }
    // Session 20: only needed for the genuinely-fresh-slot branch below
    // (FindFreeLoadedSlot), but computed here, once, regardless of which
    // branch ends up taken -- cheap (a few dozen native calls at most) and
    // keeps the two-way branch below simple. See MarkSpeciesInUseByLiveEntities's
    // comment for what this protects against.
    bool speciesInUseByLiveEntity[RESOURCE_BLOB_MAX_SPECIES + 1] = {};
    MarkSpeciesInUseByLiveEntities(base, entityIterFnRva, resolveHandleFnRva, speciesResourceTableRva, speciesInUseByLiveEntity);
    if (FindTriggeredLoad(modelPath, &species)) {
        // We already called fnc_load_gimmick_assets for this exact
        // creature earlier this session (possibly on a previous frame's
        // retry, possibly still in flight right now) -- reuse that
        // species number and do NOT trigger another load. See
        // FindTriggeredLoad's declaration comment for why re-triggering
        // here on every retry frame is unsafe.
        if (RoomHasNativeSpecies(oldTable, oldCount, species, base + resolveHandleFnRva, modelPath)) {
            char msg[160];
            snprintf(msg, sizeof(msg), "spawn_enemy: slot %d (already triggered by us this session as %s) is used by a different native record in this room -- refusing", species, modelPath);
            LogDebug(msg);
            p_lua_pushboolean(L, 0);
            p_lua_pushstring(L, "spawn_enemy: chosen slot collides with a different creature already native to this room -- refusing");
            return 2;
        }
        // needsLoad stays false -- readiness is verified uniformly by the
        // check further below regardless of which branch got us here.
    } else if (FindLoadedSlotByFilename(base, loadedPtrTableRva, modelPath, &species)) {
        // Already loaded into some slot this session (not via a placement
        // record we can see) -- reuse it, no need to load again. But first:
        // this slot number was chosen purely from the session-global load
        // table, with no idea whether THIS room's own placement table
        // already uses it for a real, different creature/object -- refuse
        // rather than collide (see RoomHasNativeSpecies's comment for the
        // live crash this guard is fixing).
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
        // checked here, before the placement table is touched at all,
        // rather than down in the load-trigger block below (which runs
        // AFTER the table swap and would hit the same phantom-record-on-
        // refusal bug the timeout/crash refusals there still have --
        // see KH1-EVDL-TOOLS's investigation doc, session 6). Refusing
        // this early keeps this particular check a true no-op.
        if (loadAssetsFnRva == 0) {
            p_lua_pushboolean(L, 0);
            p_lua_pushstring(L, "spawn_enemy: fnc_load_gimmick_assets not configured for this game build -- can't load a creature with zero presence in this room");
            return 2;
        }
        // Same room-local collision risk as the reuse branch above: a
        // slot never touched THIS SESSION globally could still be a real
        // native record in the CURRENT room if that room simply hasn't
        // been scanned into the global load table yet for some reason --
        // refuse rather than assume "session-global free" means "room-
        // local free" too.
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
    // Session 9/10 found the fresh-load (needsLoad) case's eventual
    // CONSTRUCTION call is unsafe (root cause still open -- see
    // KH1-EVDL-TOOLS's investigation doc, "Session 9"/"Session 10").
    // Session 11 found this refusal had scope creep: it used to sit here,
    // unconditionally, covering BOTH sub-cases above -- including the
    // `FindLoadedSlotByFilename` reuse case, which needs no new asset load
    // at all and was independently validated safe across sessions 5-8. That
    // reuse case no longer refuses here; the still-unsafe construction is
    // now refused specifically for the `needsLoad` case only, after its own
    // trigger+poll block below (see the `if (needsLoad)` guard right before
    // the constructor call).

    // Collision guard, defense in depth: FindLoadedSlotByFilename/
    // FindFreeLoadedSlot above already choose a slot that should be safe by
    // construction, but re-check right before touching anything, the same
    // way session 6 first added this guard -- if the slot state changed
    // between the scan above and here (or a future change to the scan logic
    // introduces a bug), refuse rather than risk corrupting another
    // creature's in-flight load. Confirmed live 2026-07-21 (session 6, via
    // KH1-LUA-LIBRARY-DEBUG's Forge Species Slot panel) that this refusal
    // path itself works cleanly, no crash. See
    // KH1-EVDL-TOOLS/docs/enemy_ai/heartless_field_spawn_investigation.md.
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
    // for whatever room this actually runs in. Untested against other
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

    // Force the species byte to the slot number chosen above rather than trusting
    // whatever's baked into templateRec at this offset (a stale value from
    // whichever room's .ard record generate_creature_data.py happened to pick
    // as this creature's template). This line guarantees the species byte
    // specifically is always correct regardless of template provenance; it
    // does NOT fix other fields the template might carry from a different
    // room (scale/rotation/etc. -- see kh1_creature_data.lua's own header).
    newRec[PLACEMENT_SPECIES_OFFSET] = species;

    // Force record+0x4c ("character id") and record+0x59 (weight) to the
    // offline-derived, per-creature values instead of whatever the template
    // happened to carry -- this is the critical safety fix for the
    // g_SoraObjPtr hijack documented above: char-id 0 gets treated as an
    // actual PARTY MEMBER index and overwrites g_SoraObjPtr[0] (Sora
    // himself). charId/weight above are already validated non-placeholder
    // (charId != 0) before this point.
    memcpy(newRec + 0x4c, &charId, 2);
    newRec[0x59] = weight;

    // Session 11: publish here, before the load-trigger block below, restoring
    // session 10's original order. A session-11 attempt to defer this publish
    // until after every refusal point (to fix session 6's never-fixed phantom-
    // record-on-refusal bug -- see the rollback comments in the refusal paths
    // below instead) turned out to be wrong: fnc_load_gimmick_assets resolves
    // OUR record by looking it up via `newId` in this same live table
    // (fnc_find_gimmick_type_def), so the record has to already be published
    // for the trigger call to find it. Deferring the publish made the very
    // next live test crash inside the trigger call itself -- not a bug in
    // loadAssetsFnRva (see below), but a self-inflicted regression from
    // deferring this too far. Publish early again; every refusal path from
    // here through the needsLoad block now explicitly rolls this back instead.
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
    // region a never-seen pointer falls in. Only reached when needsLoad is
    // true -- a brand-new claim of a never-touched-this-session slot; the
    // native-match and already-loaded-elsewhere paths above both skip this
    // block entirely, since the asset is already resident either way.
    //
    // Deliberately NOT setting g_EVSystemFlags this time (attempt 2 did,
    // matching the real caller fnc_0B5_load_model) -- that was a raw,
    // unguarded pointer write with no SafeCall/__try protection, unlike
    // everything else in this file, and is the leading theory for attempt 2's
    // crash (zero breakpoint hits were recorded on the ENTIRE call chain,
    // including fnc_load_gimmick_assets's own entry, meaning execution never
    // even reached it -- the flags write was the only new code before that
    // point). No real evidence it's load-bearing for safety here.
    //
    // Session 12 (2026-07-22): broadened from `needsLoad`-only to every
    // spawn, regardless of which of the two slot-selection branches above
    // was taken. Live-confirmed the exact crash this comment already
    // predicted, but for a path believed safe since session 8: the "already
    // loaded elsewhere this session" reuse branch (FindLoadedSlotByFilename)
    // also clones templateRec without ever reaching this mint block, since
    // it was gated on `needsLoad` (false for that branch). record+0x60/+0x64
    // in kh1_creature_data.lua's template are baked in from whatever room's
    // .ard file generate_creature_data.py picked -- meaningless in the
    // CURRENT process's bucket table (a session-relative handle encoding,
    // see FUN_14038ad90/mintHandleFnRva below). Confirmed live (pre-.ard-table
    // era, a kKnownCreatures entry
    // with compile-time-hardcoded handles): a species=28 reuse in tw11
    // crashed the constructor, and reading the live record back showed
    // record+0x60/+0x64 byte-for-byte identical to the hardcoded template's
    // bytes at those offsets -- proof this mint had never run for that call.
    // Re-minting is cheap and always safe (it just registers a fresh handle
    // for OUR OWN caller-supplied model_path/motion_path strings, which are
    // valid regardless of where templateRec's other fields came from), so it
    // runs for every spawn now, not just needsLoad.
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
        // Session 11 correction: an earlier version of this session's work
        // concluded loadAssetsFnRva (0x285EE0) was garbage -- a mid-
        // instruction address, not a real function -- based on Ghidra's
        // static disassembly. That conclusion was WRONG: live disassembly
        // of the actual running game process at this exact address shows a
        // clean, valid function prologue (`mov [rsp+8],rbx`, preceded by
        // proper INT3 alignment padding after the previous function's own
        // end), matching a real function entry, not Ghidra's apparently
        // stale/mismatched database. 0x285EE0 is correct and always was.
        // The real cause of the one crash seen inside this call this
        // session was the table-publish-ordering regression described
        // above (this function looks our record up by `newId` in the live
        // table via fnc_find_gimmick_type_def, which needs it already
        // published) -- now fixed by publishing before this call again.
        // Tag this record pointer as "ours" for InstallJobRecordGuardHook
        // BEFORE queuing the job -- fnc_load_gimmick_assets synchronously
        // captures newRec into the job struct during this same call, so the
        // global must already be set when the guard hook can first see it.
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
        // Record this immediately -- see FindTriggeredLoad's declaration
        // comment. Must happen even though the load hasn't completed yet;
        // that's the entire point (stop a later retry frame from triggering
        // a second, concurrent load for the same record).
        RecordTriggeredLoad(modelPath, species);

        // Session 19 used a Sleep(20ms)*500 (~10s) blocking poll here,
        // confirmed live 2026-08-02 (session 20) to be actively
        // counterproductive, not just slow: fnc_load_gimmick_assets's async
        // job is drained by the game's own per-frame loop, which runs on
        // THIS SAME THREAD (the one this native call executes on, called
        // synchronously from the Lua frame hook). Blocking this thread in a
        // Sleep loop doesn't wait for the load -- it PREVENTS the load from
        // ever starting, since the frame that would drain the job queue can
        // never run while we're inside the loop. A 3-breakpoint live trace
        // (trigger call, the async callback FUN_140286420, and its
        // completion-pointer write at RVA 0x2866EA) proved this
        // definitively: zero callback hits for the entire 10-second window,
        // then the callback (a burst of 9, the whole queue's backlog) and
        // the completion write both landing at the EXACT SAME timestamp,
        // exactly 10.000s after the trigger -- the instant the poll gave up
        // and this function returned control to the game's own thread. This
        // also explains why session 19 always needed exactly one retry,
        // deterministically: the poll's own final check can never observe
        // the completion, because the completion is structurally impossible
        // until after the poll has already given up.
        //
        // Fix: don't block at all. Check once (no Sleep), and if not ready
        // yet, roll back and refuse with a distinct third return value
        // (stillLoading=true) so the caller knows this is a normal
        // "call me again once you've let a real frame run" case, not a hard
        // error -- see kh1_lua_library.lua's spawn_enemy_async, which
        // drives this via the caller's own _OnFrame instead of blocking.
        //
        // Checks state==6, not just loadedPtrAddr!=0 (session 20, revised
        // after live-testing the ptr-only version): a genuinely fresh
        // species's FIRST real callback invocation (FUN_140286420, state==0
        // entry) takes a "first-time registration" branch that writes the
        // completion pointer but leaves state at 0 -- the model-handle scan
        // that would advance state through 1/3/5/6 only succeeds on a LATER
        // invocation, once that same registration has populated whatever
        // table it checks. Live-confirmed: checking ptr!=0 alone let a
        // spawn through one real frame too early, producing a visible-but-
        // functionally-broken entity (invisible, matching session 5's
        // original "position stuck at (0,0,0)" symptom that the old 10s
        // poll existed specifically to avoid) on the first attempt, working
        // correctly only on a second, separate call. Requiring state==6
        // forces the extra non-blocking retry cycle needed to actually
        // reach it.
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
        // Minting the fresh resource-string handles crashed (handlesMinted
        // false) -- previously this fell through and constructed anyway
        // with the template's possibly-stale handles. The readiness check
        // right below now catches this too (a species that was never
        // triggered will read loadedPtrAddr==0), refusing cleanly instead
        // of risking a construct against known-bad handles.
        LogDebug("spawn_enemy: minting a fresh resource-string handle crashed -- refusing rather than constructing with possibly-stale handles");
    }

    // Same readiness check as above (state==6, not just ptr!=0 -- see the
    // needsLoad block's comment for why) -- covers the FindLoadedSlotByFilename
    // reuse-elsewhere branch too, which never triggers a load itself
    // (needsLoad is false there) and, before this session, went straight to
    // construction on nothing stronger than "state != 0" (the load merely
    // STARTED, not necessarily finished).
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

    // Fallback-template construction was refused unconditionally from
    // session 9 through session 18 -- the real root cause spanned two
    // independent bugs, both now fixed and live-verified
    // end-to-end (session 19, 2026-08-02, species=20 in a non-native room:
    // visible, lockable, damageable, first full success this investigation
    // has ever had on this path):
    //   1. InstallVelocityBlendGuardHook (FUN_1402b5e50's entry) -- the
    //      real fault this whole thing chased since session 8. Zero-fills
    //      the caller's uninitialized output buffer on an unreadable
    //      descriptor instead of leaving it as garbage the caller folds
    //      into live entity state; a later TOCTOU fix (session 19) closes a
    //      probe-then-independently-reread race by redirecting the replayed
    //      original code to a stable snapshot instead of the live pointer.
    //   2. InstallJobRecordGuardHook (FUN_140286420's entry) -- the async
    //      asset-load job captures a raw, unrevalidated pointer that can go
    //      stale (a room transition, or this function's own timeout-refusal
    //      rolling placementTablePtr back). Validates against a snapshot of
    //      the room's own identity (g_WorldNumber/g_AreaNumber/g_SetNumber)
    //      taken when the job was queued, not placementTablePtr itself,
    //      which this function mutates for unrelated reasons.
    // Both are installed once per process, automatically, right above in
    // this same function. FindFreeLoadedSlot also starts scanning from
    // species 20 instead of 0, avoiding a live-
    // confirmed collision between low slot numbers and another entity's own
    // cached type-def handle. Full session-by-session history: the
    // heartless field-spawn investigation memory/doc.
    unsigned long long spawnFnAddr = base + spawnFnRva;
    unsigned long long args[1] = { (unsigned long long)newId };
    unsigned long long result = 0;

    // Session 12 temporary diagnostic: log the record's own handle-bearing
    // fields right before the constructor call, since a crash here rolls
    // the table publish back (newTable is never freed, but nothing else
    // still points at newRec once *tablePtrAddr is restored) -- without
    // this, there's no way to inspect what the constructor actually saw.
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
        // Session 12: this refusal never rolled the table publish back,
        // unlike every other refusal path in this function -- confirmed
        // live (species=28/tw11 crash) that it leaves a phantom half-built
        // record in the room's live placement table (count off-by-one from
        // the room's real native total) even though no entity was ever
        // constructed. Roll back to match every sibling refusal.
        *tablePtrAddr = oldTable;
        *tableCountAddr = oldCount;
        p_lua_pushboolean(L, 0);
        p_lua_pushstring(L, "spawn_enemy: exception during constructor call");
        return 2;
    }

    // A clean call with a null return is NOT success -- confirmed live
    // 2026-07-22 and root-caused via decompile: fnc_spawn_world_gimmick_entity
    // itself refuses (returns 0, no crash, table already spliced) when a
    // global concurrent-entity budget (DAT_142d60c98 cap vs DAT_142d60c9c
    // running total, incremented by the species-def's own weight byte at
    // +0x59 -- the same "weight" field kh1_creature_data.lua stores) would be
    // exceeded. Observed directly: spawning into a room with a live,
    // player-untouched ambient Heartless returned a null entity pointer;
    // clearing that Heartless first (freeing budget via its own despawn path)
    // let the identical call succeed. This is a real, global gate distinct
    // from the per-encounter-group ambient-wave budget investigated
    // previously (DAT_142d60ca8/cac) -- see
    // KH1-EVDL-TOOLS/docs/enemy_ai/heartless_field_spawn_investigation.md.
    // Previously this surfaced as a misleading `true, 0` to the caller;
    // now it's a clear refusal instead.
    if (result == 0) {
        char msg[192];
        snprintf(msg, sizeof(msg), "spawn_enemy: constructor call succeeded but returned a null entity -- likely the room's concurrent-entity budget is full (id=0x%x)", newId);
        LogDebug(msg);
        // Session 12: same phantom-record risk as the crash refusal above --
        // no live entity was constructed here either, so the spliced record
        // is equally orphaned. Not separately live-confirmed as broken for
        // this specific branch, but rolling back is free and keeps this
        // refusal consistent with every other one in this function.
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
// Session 17 (2026-07-23) fix for the fresh-load fallback-spawn crash chased
// since session 8: FUN_1402b5e50 (Steam RVA 0x2b5e50, a generic per-frame
// motion/velocity-blend utility called from 11+ unrelated sites, not
// spawn-specific) unconditionally dereferences its second parameter (a
// motion-descriptor pointer, RDX at entry) at six different offsets (+1,
// +2, +4, +8, +0xc, +0x10 -- confirmed via decompile, not just the single
// +2 instruction chased across sessions 8-16) with no validity check at
// all. Session 15 confirmed live the fallback path can leave this pointer
// large/plausible-looking but genuinely dangling; session 16 confirmed the
// same fault can hit a completely unrelated entity's own tick on a later
// frame, not just the entity we just spawned -- some shared state the
// fallback path touches leaves this pointer bad for whoever reads it next.
// Rather than trying to identify which caller is "ours" (session 16's
// approach -- correct for its own entity, but left every OTHER caller of
// this hot shared function still exposed), this hooks the function's
// ENTRY and safely probes whether the whole descriptor range is readable
// before the original body runs at all. A readable descriptor takes the
// exact original path, byte for byte -- zero behavior change for the
// overwhelming majority of calls.
//
// Session 18 (2026-07-23) static re-review, done before any further live
// test per session 17's own note: the original "unreadable -> skip this
// call entirely, a safe no-op" premise was WRONG. Decompiling the real
// caller (FUN_1402998a0) shows `param_3` (R8) is always the CALLER's own
// uninitialized stack-local buffer (`local_78[8]`/`local_70`/`local_64`,
// no prior init) -- FUN_1402b5e50, together with the FUN_14029eb20 sub-call
// it makes at param_3+0x10, is the SOLE writer of that entire 0x20-byte
// range, every single call. Skipping the whole function left that 32 bytes
// as genuine uninitialized garbage, which the caller then unconditionally
// read back and folded straight into real entity state
// (`*(float*)(param_1+0x3d4)`) and a flags word
// (`param_1+0x374 |= 0xc0000000`, gated on a magnitude check downstream).
// A garbage float landing as NaN/huge there is a fully plausible mechanism
// for session 17 test 2's delayed, telemetry-free crash. Fixed by having
// the unreadable case zero-fill the real 0x20-byte output range instead of
// leaving it untouched -- deterministic, finite output, matching what a
// "safe no-op" was supposed to mean in the first place. Not yet live-tested
// as of this writing.
static bool g_velocityBlendGuardHookInstalled = false;
static volatile uint64_t g_velocityBlendGuardSkipCount = 0;

// Session 19 (2026-08-02) live-confirmed fix for a real TOCTOU race in the
// original "probe, then let the original instructions independently re-read
// the same live pointer" design: this game is heavily multithreaded (90+
// threads during an active asset load), and the gap between a successful
// readability probe and the original code's own later read of the exact
// same address was wide enough for another thread to invalidate that memory
// in between -- confirmed live: the probe passed, the hook replayed the
// original instructions believing it safe, and the SAME fault (RVA 0x2b5e66)
// still happened moments later, with the guard fully installed and active.
// Fix: never let the original code re-touch the live pointer at all. Copy
// the probed bytes into a thread-local snapshot (each calling thread gets
// its own, so concurrent callers can't race each other either) and redirect
// the original code to operate on that stable copy instead -- once copied,
// nothing external can invalidate our own memory, closing the window
// entirely rather than just narrowing it.
static thread_local uint8_t g_veloBlendSnapshot[0x14];

// Wrapped the same way SafeCall wraps a risky game call: __except turns the
// hardware fault into a normal zero return instead of taking the process
// down. Probes the whole range the original function actually dereferences
// (up to param2+0x13, the last byte of the +0x10 float read) in one copy.
// Returns 0 if unreadable (and zero-fills param3's real 0x20-byte output
// footprint, in its own nested __try/__except -- param3 is ordinarily just
// the caller's own stack, so this should never fault, but it costs nothing
// to guard the same way every other game-memory touch in this file already
// is). Returns the snapshot's own address on success -- the caller (the
// hook stub) redirects the original code to that address instead of the
// live param2.
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

// Original bytes at the function's entry are expected to be exactly a
// 5-byte "mov qword ptr [rsp+8], rbx" (REX.W 89 /r + disp8) -- confirmed
// via Ghidra disassembly on the Steam build; live-verify before trusting
// on EGS (see [[feedback_ghidra_static_vs_live_process_mismatch]]).
//
// Stub layout (all in the allocated cave):
//   push rcx / push rdx / push r8       ; save the 3 incoming params
//   mov rax, rdx                        ; rax = param2
//   mov rdx, r8                         ; arg2 for the check = param3 (output buffer)
//   mov rcx, rax                        ; arg1 for the check = param2
//   sub rsp, 0x20                       ; shadow space (net 16-aligned before CALL)
//   mov rax, &CheckVelocityBlendParamSafe
//   call rax                            ; rax <- 0 (unreadable) or the snapshot's own address
//   add rsp, 0x20
//   test rax, rax                       ; full 64-bit test -- rax is a real pointer now, not a bool
//   pop r8 / pop rdx / pop rcx          ; restore regardless of outcome (rax untouched by pops)
//   jz skipCall                         ; unreadable -> abandon the whole function (output already zeroed)
//   mov rdx, rax                        ; redirect the replayed original code to the SNAPSHOT, not the
//                                        ; live (possibly now-racy) param2 -- see CheckVelocityBlendParamSafe's
//                                        ; comment for the live-confirmed TOCTOU this closes
//   <original 5 bytes, replayed verbatim -- mov [rsp+8],rbx>
//   jmp resumeAddr
//   skipCall:
//   ret                                 ; nothing pushed/allocated yet -- safe early return
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
// Idempotent. Session 17 diagnostic/fix -- see the plate comment above.
// hookRva should be the function's entry (Steam 0x2b5e50), resumeRva its
// entry+5.
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
// Session 20 (2026-08-02) fix for a crash discovered while testing the
// non-blocking spawn_enemy redesign: a completely unrelated per-party
// ability-data rebuild routine (FUN_1402d94d0/FUN_1402da720, run for each
// of the 3 party members, checking g_pInventory's own equipped-ability
// state) crashed live, reproducibly, at Steam RVA 0x1D62F7, inside a tiny
// shared helper (FUN_1401d62e0) -- confirmed via Windows crash telemetry
// (Get-WinEvent Id 1000) on 2 separate live attempts, byte-identical fault
// offset both times. Full decompile of FUN_1401d62e0:
//   rax = *param1; rcx = *rax; ecx = *(rcx+8);
//   rax = fnc_resolve_resource_handle(ecx);
//   return *(int*)(rax + param2*4 + 0x10);   // <-- the fault: no bounds check
// Both real callers pass param2=0x34 (52) and already treat a negative
// return as this function's own "not found" sentinel (falling back to a
// default/unset state) -- so returning -1 on an unreadable read is not a
// behavior change invented for this fix, it's the function's own existing
// contract, just reached safely instead of by luck.
//
// This is NOT the live-entity-handle collision session 19 found and this
// session initially (wrongly) assumed was the same mechanism -- confirmed
// by testing MarkSpeciesInUseByLiveEntities first and seeing the identical
// crash, at the identical address, with species=20 still not marked in use
// by anything in the 96-slot gimmick-entity pool. The handle here
// (`(&DAT_142e1fe24)[partyMember*0x44]`, a static per-party-member global,
// not any entity's own field) is cached ability-icon data, unrelated to the
// gimmick/heartless entity system entirely. It crashes for the same root
// reason sessions 3-5 already established generally (fnc_mint_resource_handle's
// bucket table assigns handles purely by which 32MB-aligned heap region a
// pointer falls in, shared globally across every subsystem in the game,
// with no isolation between them) -- our fresh species-blob write can
// happen to alias memory ANY unrelated cached handle's bucket points into,
// not just another entity's. Enumerating every possible colliding
// subsystem ahead of time isn't tractable; guarding the actual unsafe read
// (the same approach that already fixed the two other fault sites chased
// since session 8) is.
static bool g_partyAbilityIndexGuardHookInstalled = false;
static volatile uint64_t g_partyAbilityIndexGuardSkipCount = 0;

// __fastcall(rcx=resolved base pointer, rdx=index) -> the real int value if
// readable, or -1 (this function's own pre-existing "not found" sentinel,
// already handled by both real callers) if not.
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

// Hooks at the CALL to fnc_resolve_resource_handle (5 bytes, `E8 xx xx xx xx`)
// rather than the 4-byte faulting read right after it -- the read alone is
// too short for a 5-byte JMP trampoline, but the call+read together (9
// bytes) give enough room, and both get replaced: the stub replicates the
// resolve call itself (via an absolute call through resolveHandleFnAddr,
// avoiding any RIP-relative relocation math) then feeds its result straight
// into the safety-checked read, so nothing about the original 4-byte
// instruction survives to fault. ecx (the resolve call's argument) is set
// by the instruction immediately before this hook and is untouched by
// installing it. rbx (the index) was already computed earlier in the
// function and stays valid across this whole hook.
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

// --- ASYNC LOAD JOB RECORD GUARD HOOK ---
// Session 18 (2026-07-23) fix for the real root cause finally pinned down
// this session, unifying every fresh-load-fallback symptom chased since
// session 7: `fnc_load_gimmick_assets` (FUN_140285ee0) queues an async job
// (drained later by the game's own per-frame job-pool dispatcher) that
// captures a RAW pointer into the CURRENT room's placement table
// (`fnc_find_gimmick_type_def()`'s return value) at `job+0x20` -- not a
// copy. Session 11 already proved this job can sit pending for 10+
// seconds before its callback (FUN_140286420) actually fires, running
// unprotected on the game's own thread. This session proved, via a live
// 37-thread hardware write-watchpoint on `placementTablePtr` during a real
// room transition, that the game recomputes that global fresh from the
// NEWLY loaded room's own data blob on every single room load
// (FUN_140288030, called from the room-load routine FUN_140287c60) -- so
// a job that outlives a room transition holds a pointer into memory that
// no longer belongs to the room it was captured in. FUN_140286420 then
// dereferences that stale pointer unconditionally (`*(char*)(record+0x55)`
// as a species index) and uses the result to index/write the shared,
// global `loadedSpeciesPtrTable`/`DAT_142869dd0` struct -- corrupting
// whatever species slot the garbage index happens to land on. This single
// mechanism plausibly explains the whole diversity of symptoms chased
// since session 7 (crash vs. freeze, our entity vs. an unrelated entity,
// clean telemetry vs. none) -- the outcome depends entirely on what
// garbage lands where and what later reads it.
//
// The fix: hook FUN_140286420's ENTRY (same code-cave/JMP-trampoline idiom
// as InstallVelocityBlendGuardHook/InstallTextBoxHook) and validate the
// job's captured record pointer against the CURRENT placementTablePtr/
// placementTableCount bounds -- read fresh every check, since those
// globals are exactly what changes on a room transition -- before letting
// the original body run at all. A pointer that's still within the live
// room's own table is untouched, byte for byte (the overwhelming majority
// case: same room, job resolves in well under 10s). A pointer that's
// fallen outside those bounds is provably stale -- skip the ENTIRE
// callback (no dereference, no write to the shared table, no invoking the
// completion callback with garbage) and return the same `4` ("job done")
// exit code this function's own terminal paths already use, so the job
// pool doesn't retry it forever.
static bool g_jobRecordGuardHookInstalled = false;
static volatile uint64_t g_jobRecordGuardSkipCount = 0;
static unsigned long long g_jobRecordGuardTablePtrRva = 0;
static unsigned long long g_jobRecordGuardTableCountRva = 0;
static unsigned long long g_jobRecordGuardWorldNumRva = 0;
static unsigned long long g_jobRecordGuardAreaNumRva = 0;
static unsigned long long g_jobRecordGuardSetNumRva = 0;

// Wrapped the same way every other risky-pointer probe in this file is:
// __except turns a hardware fault into a clean "treat as stale" instead of
// taking the process down. Re-resolves the CURRENT table bounds from the
// live globals (not whatever was true when the job was queued) on every
// call -- that's the whole point, since those globals are exactly what a
// room transition changes.
static int CheckJobRecordPointerSafe(uint64_t jobNodePtr) {
    __try {
        unsigned long long base = (unsigned long long)GetModuleHandleA(nullptr);
        uint8_t* recordPtr = *(uint8_t**)(uintptr_t)(jobNodePtr + 0x20);

        // Session 19 fix: only ever apply the staleness check to a job we
        // ourselves queued (l_spawn_enemy tags g_lastQueuedGimmickRecordPtr
        // right before calling fnc_load_gimmick_assets). This is a plain
        // pointer-VALUE comparison, not a dereference of recordPtr -- always
        // safe regardless of whether recordPtr itself is still valid. Every
        // other caller's job (this function is shared, engine-wide asset-
        // load infrastructure -- ordinary EVDL room scripts queue jobs
        // through the exact same path) is let through completely untouched,
        // callback included -- a first live test that skipped ALL jobs
        // indiscriminately hard-froze the game by skipping a real EVDL job's
        // non-null completion callback that something else was blocked on.
        if (recordPtr != (uint8_t*)(uintptr_t)g_lastQueuedGimmickRecordPtr) {
            return 1;
        }

        // Live-confirmed fix: room identity, not placementTablePtr, is the
        // real staleness signal. l_spawn_enemy's own 10s-timeout refusal
        // rolls placementTablePtr back to the OLD table before this exact
        // job's callback can possibly fire -- with the table-bounds check
        // that used to live here, that made our own successfully-queued job
        // look "stale" on every single timeout, with zero room transition
        // involved, permanently starving that species' load (the callback
        // being skipped is also what would have recorded its real
        // completion). g_WorldNumber/g_AreaNumber/g_SetNumber are read, not
        // written, by the same room-load routine that recomputes
        // placementTablePtr -- comparing those instead answers the actual
        // question ("did the room change") instead of a mutable proxy for it.
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
            // Same room the whole time -- the record is exactly as valid as
            // it was the moment it was queued, regardless of what
            // placementTablePtr currently says. Let the real body run.
            return 1;
        }

        // Fallback for a build where room-identity RVAs aren't wired up yet
        // (e.g. EGS as of this writing): the older, more conservative
        // table-bounds heuristic. Known to false-positive on our own
        // timeout-refusal rollback (see above) but still strictly safer
        // than applying no check at all.
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

// Original bytes at the function's entry are expected to be exactly a
// 5-byte "mov qword ptr [rsp+8], rbx" (REX.W 89 /r + disp8) -- same
// instruction, same encoding as the velocity-blend guard's hook point
// (confirmed independently via Ghidra disassembly here); live-verify
// before trusting on EGS.
//
// Stub layout (all in the allocated cave):
//   push rcx                            ; save the job node ptr (also arg1 for the check as-is)
//   sub rsp, 0x20                       ; shadow space (net 16-aligned before CALL)
//   mov rax, &CheckJobRecordPointerSafe
//   call rax                            ; rcx already holds the job node ptr, untouched by push/sub
//   add rsp, 0x20
//   test eax, eax
//   pop rcx                             ; restore original rcx regardless of outcome
//   jz skipCall                         ; stale -> abandon the whole function
//   <original 5 bytes, replayed verbatim -- mov [rsp+8],rbx>
//   jmp resumeAddr
//   skipCall:
//   mov eax, 4                          ; matches this function's own "job done" exit code
//   ret
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
// Idempotent. Session 18 fix, room-identity-corrected same session -- see the
// plate comment above and CheckJobRecordPointerSafe's comment. hookRva should
// be FUN_140286420's entry (Steam 0x286420), resumeRva its entry+5.
// tablePtrRva/tableCountRva are placementTablePtr/placementTableCount, kept
// only as an EGS-era fallback now; worldNumRva/areaNumRva/setNumRva
// (g_WorldNumber/g_AreaNumber/g_SetNumber) are the real staleness signal on a
// build where they're known -- pass 0 for all three to force the fallback.
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
    }
    return TRUE;
}
