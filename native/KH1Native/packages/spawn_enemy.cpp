#include "pch.h"
#include <cstdio>
#include <cstring>
#include <cstdint>

#include "kh1_native.h"

// --- ENEMY SPAWN (PLACEMENT-TABLE SPLICE) ---
// spawn_enemy(...) -> ok, entityPtr | errorMessage. Splices a synthetic record into the room's
// live placement table and calls the game's own constructor. Keyed by model/motion filename.
static const size_t PLACEMENT_RECORD_SIZE = 0x78;
static const int PLACEMENT_SPECIES_OFFSET = 0x55;
static const int PLACEMENT_POS_X_OFFSET = 0x1C;
static const int PLACEMENT_POS_Y_OFFSET = 0x20;
static const int PLACEMENT_POS_Z_OFFSET = 0x24;
static const int PLACEMENT_MODEL_HANDLE_OFFSET = 0x60;
static const int PLACEMENT_MOTION_HANDLE_OFFSET = 0x64;

// Shared 96-slot entity pool (stride 1200) holding Sora, party, room objects and our spawns.
// Position is 3 floats at +0x10; +0x374 bit 0 is the occupied flag.
static const unsigned long long ENTITY_POOL_STRIDE = 0x4B0;
static const int ENTITY_POOL_COUNT = 96;
static const int ENTITY_OCCUPIED_FLAG_OFFSET = 0x374;
static const int ENTITY_POS_X_OFFSET = 0x10;
static const int ENTITY_POS_Y_OFFSET = 0x14;
static const int ENTITY_POS_Z_OFFSET = 0x18;
static const int ENTITY_KIND_OFFSET = 0x6;
// Sora, party and every constructed creature are kind==3; room furniture uses other values.
static const uint8_t ENTITY_KIND_ACTOR = 3;

// Per-species asset-load state. State byte (+3) climbs as a load progresses; cached filename
// (+4) distinguishes reuse from collision.
static const int LOADED_SPECIES_STRIDE = 0x50;
static const int LOADED_SPECIES_STATE_OFFSET_FROM_PTR = -0x45;
// 6 = fully loaded and safe to construct from; anything lower is still in flight.
static const uint8_t SPECIES_SLOT_STATE_READY = 6;
// slot+0 = owner species, slot+1 = run length, stamped into EVERY slot of a claimed run. The
// state byte is only set on the primary, so run members read state==0 and look free.
static const int LOADED_SPECIES_OWNER_OFFSET_FROM_PTR = -0x48;
static const int LOADED_SPECIES_RUNLEN_OFFSET_FROM_PTR = -0x47;
static const int LOADED_SPECIES_MODEL_NAME_OFFSET_FROM_PTR = -0x44;
static const int LOADED_SPECIES_MODEL_NAME_SIZE = 0x20;
static const int SPECIES_SLOT_COUNT = 256; // species/slot index is a uint8_t (record+0x55) -- the full addressable range

// species (record+0x55) is a room-local slot index, not a creature id; char-id/weight/template
// come from kh1_lua_library/creature_data.lua. Resource blob is 256KB per slot, species 0..64.
static const size_t RESOURCE_BLOB_SIZE = 0x40000;
static const int RESOURCE_BLOB_MAX_SPECIES = 0x40;

// Debugging aid: entity id of the most recent spawn_enemy call to reach the
// constructor, for an external CE hook to target. Not read in this DLL.

// Placement record tagged before it is handed to fnc_load_gimmick_assets.
static volatile uint64_t g_lastQueuedGimmickRecordPtr = 0;

// Room identity snapshotted with the record pointer -- the real staleness signal.
static volatile int32_t g_lastQueuedGimmickWorld = 0;
static volatile int32_t g_lastQueuedGimmickArea = 0;
static volatile int32_t g_lastQueuedGimmickSet = 0;
static volatile bool g_lastQueuedGimmickRoomIdentityValid = false;

// Loads we triggered this session, so the reuse path can tell our slots from the engine's.
struct BlacklistedRoomRange {
    int32_t world;
    int32_t areaMin;
    int32_t areaMax;
    const char* label;
    const char* why;
};
static const BlacklistedRoomRange g_spawnRoomBlacklist[] = {
    // World 16, areas 2..12, every set. A content decision, not a crash workaround.
    // g_AreaNumber is ZERO-INDEXED -- the range was first written 1-indexed and was off by one.
    { 16, 2, 12, "world 16 areas 2-12", "spawning disabled here by request" },
    { 8, 6, 6, "world 8 area 6", "spawning disabled here by request" },
};

struct TriggeredLoadEntry {
    char modelPath[64];
    uint8_t species;
    // How many consecutive slots this creature's load claimed (record+0x56). Needed so the
    // room-change reclamation below knows the full extent of what we took, not just its first slot.
    uint8_t runLen;
    // The slot's loadedSpeciesPtrTable pointer at load time. Identifies OUR load: filename and
    // start index alone cannot tell it apart from a native load of the same model.
    unsigned long long loadToken;
    // When the load was recorded; only used to report a run's age.
    unsigned long long recordedTick;
    // Room visit this load belongs to. An entry from an earlier visit pins nothing and is never
    // reused: the engine re-tiles slots at room unload, so it describes a room that no longer exists.
    uint32_t visit;
};
static const int MAX_TRIGGERED_LOADS = 64;
static TriggeredLoadEntry g_triggeredLoads[MAX_TRIGGERED_LOADS];
static int g_triggeredLoadCount = 0;

// Counts room VISITS, not room keys: comparing keys alone read an A->B->A round trip as "same
// room" and left the old visit's runs pinned for the rest of the session.
static uint32_t g_roomVisit = 0;
static int32_t g_lastSeenRoomWorld = -1, g_lastSeenRoomArea = -1, g_lastSeenRoomSet = -1;
static bool g_lastSeenRoomValid = false;

// Safe to call from the read-only precheck: it advances the visit counter but never releases
// anything, and the spawn path's release is gated on g_lastSpawnVisit, not on this.
static void NoteRoomIdentity(int32_t world, int32_t area, int32_t set) {
    if (g_lastSeenRoomValid && world == g_lastSeenRoomWorld &&
        area == g_lastSeenRoomArea && set == g_lastSeenRoomSet) return;
    g_lastSeenRoomWorld = world; g_lastSeenRoomArea = area; g_lastSeenRoomSet = set;
    g_lastSeenRoomValid = true;
    ++g_roomVisit;
}

static bool FindTriggeredLoad(const char* modelPath, uint8_t* outSpecies) {
    for (int i = 0; i < g_triggeredLoadCount; ++i) {
        if (g_triggeredLoads[i].visit != g_roomVisit) continue;
        if (strcmp(g_triggeredLoads[i].modelPath, modelPath) == 0) {
            *outSpecies = g_triggeredLoads[i].species;
            return true;
        }
    }
    return false;
}

// The slot's own resource pointer, which changes on every reload -- used as a load identity.
static unsigned long long LoadedSlotToken(unsigned long long base, unsigned long long loadedPtrTableRva,
                                          uint8_t species) {
    return *(volatile unsigned long long*)(uintptr_t)(base + loadedPtrTableRva +
        (size_t)species * LOADED_SPECIES_STRIDE);
}

// The pointer is usually still 0 when a load is triggered, so the token is filled in later, on
// the first call that finds the slot populated.
static void EnsureTriggeredLoadToken(unsigned long long base, unsigned long long loadedPtrTableRva,
                                     uint8_t species) {
    for (int i = 0; i < g_triggeredLoadCount; ++i) {
        if (g_triggeredLoads[i].visit != g_roomVisit) continue;
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
    entry.visit = g_roomVisit;
    g_triggeredLoadCount++;

    char msg[200];
    snprintf(msg, sizeof(msg),
        "spawn_enemy: recorded triggered load species=%d runLen=%d model=%s loadToken=0x%llX visit=%u",
        (int)species, (int)runLen, modelPath, entry.loadToken, g_roomVisit);
    LogDebug(msg);
}

// Visit we last spawned in -- used only to decide when to release and clear our bookkeeping.
static uint32_t g_lastSpawnVisit = 0;
// The engine never clears block ownership -- it reclaims by eviction only, so our runs would stay
// occupied forever. We release them ourselves at room change, once our entities are destroyed.
static void MarkSpeciesInUseByLiveEntities(unsigned long long base, unsigned long long entityIterFnRva,
                                           unsigned long long resolveHandleFnRva,
                                           unsigned long long speciesResourceTableRva,
                                           bool outInUse[RESOURCE_BLOB_MAX_SPECIES + 1]);

static void ReleaseOurTriggeredRuns(unsigned long long base, unsigned long long loadedPtrTableRva,
                                    unsigned long long releaseSlotRunFnRva,
                                    unsigned long long entityIterFnRva,
                                    unsigned long long resolveHandleFnRva,
                                    unsigned long long speciesResourceTableRva) {
    if (releaseSlotRunFnRva == 0 || loadedPtrTableRva == 0 || g_triggeredLoadCount == 0) return;
    // Room-change detection is LAZY, so a creature from the old room may still be alive. Releasing
    // a block out from under one hangs the game in fnc_blob_command_list_walk_and_patch.
    bool inUse[RESOURCE_BLOB_MAX_SPECIES + 1] = {};
    MarkSpeciesInUseByLiveEntities(base, entityIterFnRva, resolveHandleFnRva,
                                   speciesResourceTableRva, inUse);
    int released = 0, skipped = 0, stillUsed = 0;
    for (int i = 0; i < g_triggeredLoadCount; ++i) {
        const TriggeredLoadEntry& e = g_triggeredLoads[i];
        if (e.species > RESOURCE_BLOB_MAX_SPECIES) { ++skipped; continue; }
        // Any slot of the run still referenced by a live entity keeps the whole run.
        bool used = false;
        for (int k = 0; k < (e.runLen ? e.runLen : 1) && e.species + k <= RESOURCE_BLOB_MAX_SPECIES; ++k)
            if (inUse[e.species + k]) { used = true; break; }
        if (used) { ++stillUsed; continue; }
        // Only release what is still ours. A native load of the same model is indistinguishable by
        // filename alone, so also require the load token when we captured one.
        const char* cachedName = (const char*)(uintptr_t)(base + loadedPtrTableRva +
            LOADED_SPECIES_MODEL_NAME_OFFSET_FROM_PTR + (size_t)e.species * LOADED_SPECIES_STRIDE);
        if (strncmp(cachedName, e.modelPath, LOADED_SPECIES_MODEL_NAME_SIZE) != 0) { ++skipped; continue; }
        if (e.loadToken != 0 &&
            LoadedSlotToken(base, loadedPtrTableRva, e.species) != e.loadToken) { ++skipped; continue; }
        unsigned long long args[2] = { (unsigned long long)e.species,
                                       (unsigned long long)(e.runLen ? e.runLen : 1) };
        unsigned long long r = 0;
        if (SafeCall(base + releaseSlotRunFnRva, args, 2, r)) ++released; else ++skipped;
    }
    char msg[240];
    snprintf(msg, sizeof(msg),
        "spawn_enemy: room change -- released %d of %d of our slot runs back to the pool "
        "(%d skipped: re-taken or token mismatch; %d KEPT: a live entity still references them)",
        released, g_triggeredLoadCount, skipped, stillUsed);
    LogDebug(msg);
}

static bool g_lastSpawnVisitValid = false;

// Our own record of slots we loaded. Unlike the live-entity scan it does not depend on handle
// resolution, which fails exactly when handles start going bad.
static bool IsSlotInTriggeredRunThisVisit(int slot) {
    for (int i = 0; i < g_triggeredLoadCount; ++i) {
        if (g_triggeredLoads[i].visit != g_roomVisit) continue;
        int start = (int)g_triggeredLoads[i].species;
        int len = g_triggeredLoads[i].runLen ? (int)g_triggeredLoads[i].runLen : 1;
        if (slot >= start && slot < start + len) return true;
    }
    return false;
}

// Lua strings are only valid for the call, but the async load dereferences them later.
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

// Scans slots for one already holding this creature (state != 0 and filename matches).
static bool SlotHoldsLiveSpecies(unsigned long long base, unsigned long long loadedPtrTableRva,
                                 unsigned long long speciesResourceTableRva, int slot);

// Finds a slot genuinely holding this model. Cached names are never cleared, so the name alone is
// stale -- SlotHoldsLiveSpecies also requires a valid blob header, which only a real run start has.
static bool FindLoadedSlotByFilename(unsigned long long base, unsigned long long loadedPtrTableRva,
                                     unsigned long long speciesResourceTableRva,
                                     const char* modelPath, uint8_t* outSpecies) {
    for (int s = 0; s <= RESOURCE_BLOB_MAX_SPECIES; ++s) {
        if (!SlotHoldsLiveSpecies(base, loadedPtrTableRva, speciesResourceTableRva, s)) continue;
        const char* cachedName = (const char*)(uintptr_t)(base + loadedPtrTableRva + LOADED_SPECIES_MODEL_NAME_OFFSET_FROM_PTR + (size_t)s * LOADED_SPECIES_STRIDE);
        if (strncmp(cachedName, modelPath, LOADED_SPECIES_MODEL_NAME_SIZE) == 0) {
            *outSpecies = (uint8_t)s;
            return true;
        }
    }
    return false;
}

// The engine's recorded run length for a slot. Clamped to >=1 so garbage degrades safely.
static int LoadedSlotRunLen(unsigned long long base, unsigned long long loadedPtrTableRva, uint8_t species) {
    volatile uint8_t* runLenAddr = (volatile uint8_t*)(uintptr_t)(base + loadedPtrTableRva +
        LOADED_SPECIES_RUNLEN_OFFSET_FROM_PTR + (size_t)species * LOADED_SPECIES_STRIDE);
    int runLen = (int)*runLenAddr;
    return runLen < 1 ? 1 : runLen;
}

// Still a live run start, with nothing else having claimed part of its run since. Ownership is
// deliberately not tested: loaded species routinely carry owner==0xFF, which rejected valid reuses.
static bool SpeciesRunStillIntact(unsigned long long base, unsigned long long loadedPtrTableRva,
                                  unsigned long long speciesResourceTableRva,
                                  uint8_t species, int runLen) {
    if (!SlotHoldsLiveSpecies(base, loadedPtrTableRva, speciesResourceTableRva, species)) return false;
    for (int k = 1; k < runLen && species + k <= RESOURCE_BLOB_MAX_SPECIES; ++k)
        if (SlotHoldsLiveSpecies(base, loadedPtrTableRva, speciesResourceTableRva, species + k))
            return false;
    return true;
}

// Every slot of a run must carry the SAME owner byte; disagreement means a partly-claimed run whose
// tail still holds the previous occupant's data. Uniform 0xFF is healthy, not a defect.
static bool RunOwnershipUniform(unsigned long long base, unsigned long long loadedPtrTableRva,
                                uint8_t species, int runLen, int* outBadSlot, uint8_t* outOwners) {
    volatile uint8_t* ownerAt = (volatile uint8_t*)(uintptr_t)(base + loadedPtrTableRva +
        LOADED_SPECIES_OWNER_OFFSET_FROM_PTR + (size_t)species * LOADED_SPECIES_STRIDE);
    uint8_t first = *ownerAt;
    bool uniform = true;
    for (int k = 0; k < runLen; ++k) {
        int slot = (int)species + k;
        if (slot > RESOURCE_BLOB_MAX_SPECIES) break;
        volatile uint8_t* owner = (volatile uint8_t*)(uintptr_t)(base + loadedPtrTableRva +
            LOADED_SPECIES_OWNER_OFFSET_FROM_PTR + (size_t)slot * LOADED_SPECIES_STRIDE);
        uint8_t v = *owner;
        if (outOwners && k < 16) outOwners[k] = v;
        if (v != first && uniform) { uniform = false; if (outBadSlot) *outBadSlot = slot; }
    }
    return uniform;
}

// A reused slot must still be wholly ours. Only meaningful once state==6 -- while a load is in
// flight the owner bytes are not written yet, so an intact run would look broken.
static bool ReuseSlotIsSafe(unsigned long long base, unsigned long long loadedPtrTableRva,
                            unsigned long long speciesResourceTableRva,
                            uint8_t species, const char* modelPath, const char* via) {
    // Only judge run intactness once state==6: owner bytes are not written while a load is in
    // flight, so testing earlier made healthy runs look broken and triggered floods of reloads.
    volatile uint8_t* stateAddr = (volatile uint8_t*)(uintptr_t)(base + loadedPtrTableRva +
        LOADED_SPECIES_STATE_OFFSET_FROM_PTR + (size_t)species * LOADED_SPECIES_STRIDE);
    uint8_t state = *stateAddr;
    if (state != 0 && state < SPECIES_SLOT_STATE_READY) {
        return true; // load in progress -- not ours to second-guess yet
    }

    int runLen = LoadedSlotRunLen(base, loadedPtrTableRva, species);
    if (SpeciesRunStillIntact(base, loadedPtrTableRva, speciesResourceTableRva, species, runLen)) return true;
    char msg[240];
    snprintf(msg, sizeof(msg),
        "spawn_enemy: slot %d (%s, found via %s) is no longer a usable %d-slot run -- either its "
        "blob header is gone or another asset has loaded inside it. Not reusing it.",
        (int)species, modelPath, via, runLen);
    LogDebug(msg);
    return false;
}

// Resolves record+0x60's handle to a filename so slot reuse can be told from collision.
static bool ResolvedModelMatches(unsigned long long resolveFnAddr, uint32_t modelHandle, const char* wantModel) {
    if (modelHandle == 0) return false;
    unsigned long long args[1] = { (unsigned long long)modelHandle };
    unsigned long long resolved = 0;
    if (!SafeCall(resolveFnAddr, args, 1, resolved) || resolved == 0) return false;
    return strncmp((const char*)(uintptr_t)resolved, wantModel, LOADED_SPECIES_MODEL_NAME_SIZE) == 0;
}

// A room's own record of the SAME creature is fine; only a different model is a collision.
// Unresolvable handles are conservatively treated as collisions.
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

// Marks species in use by walking live entities and resolving entity+0x134. Blind to any
// entity whose handle does not resolve -- the counters below measure how often that happens.
static void MarkSpeciesInUseByLiveEntities(unsigned long long base, unsigned long long entityIterFnRva,
                                             unsigned long long resolveHandleFnRva, unsigned long long speciesResourceTableRva,
                                             bool outInUse[RESOURCE_BLOB_MAX_SPECIES + 1]) {
    if (entityIterFnRva == 0 || speciesResourceTableRva == 0) return;
    unsigned long long tableBase = base + speciesResourceTableRva;
    unsigned long long entity = 0;
    unsigned long long iterArgs[1] = { 0 };
    if (!SafeCall(base + entityIterFnRva, iterArgs, 1, entity)) return;
    // resolveFailed is real corruption; outsideBlobTable is benign. Do not lump them.
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
                // resolved==0 is what the bucket guard returns for a bogus handle.
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

// Upper bound of the allocation window. 50..63 is ENGINE SCRATCH -- subsystems grab those blobs
// MID-ROOM (texture loads near doors), so a creature placed there is clobbered while still alive.
static const int SPECIES_SLOT_ALLOC_MAX = 49;

// No hardcoded floor: the low slots usually hold Sora + party, but that roster varies, and the
// availability map already rejects anything actually resident. Allocation scans TOP-DOWN.
static const int SPECIES_SLOT_ALLOC_MIN = 0;
// owner==0xFF is NOT sufficient to call a slot free. Release wipes the blob but leaves state and
// the cached name, so a VALID blob header is what separates "still live" from "released".
static const int32_t BLOB_HEADER_FIRST_SECTION = 128;   // blob+4 on every valid header observed

// The load's relocation pass stamps blob+0xD0 with this marker and stores a handle to the
// relocated sub-record table at blob+0xD4. Release zeroes the handle but leaves the marker, the
// state byte, the cached name AND the section header intact -- marker-present + handle-zero is
// the one reliable signature of a released ("zombie") blob. Constructing from one sends the
// engine's sub-record lookup down its non-relocated fallback path, which reads asset bytes as a
// handle; the walk-and-patch pass then overwrites the blob's own command list and soft-locks
// the game (the Angel Star freezes, 2026-08-18/19).
static const int BLOB_RELOC_MARKER_OFFSET = 0xD0;
static const int BLOB_RELOC_HANDLE_OFFSET = 0xD4;
static const uint32_t BLOB_RELOC_MARKER = 0x96969696;

static bool SlotHoldsLiveSpecies(unsigned long long base, unsigned long long loadedPtrTableRva,
                                 unsigned long long speciesResourceTableRva, int slot) {
    if (speciesResourceTableRva == 0) return false;
    volatile uint8_t* stateAddr = (volatile uint8_t*)(uintptr_t)(base + loadedPtrTableRva +
        LOADED_SPECIES_STATE_OFFSET_FROM_PTR + (size_t)slot * LOADED_SPECIES_STRIDE);
    if (*stateAddr != SPECIES_SLOT_STATE_READY) return false;
    const char* name = (const char*)(uintptr_t)(base + loadedPtrTableRva +
        LOADED_SPECIES_MODEL_NAME_OFFSET_FROM_PTR + (size_t)slot * LOADED_SPECIES_STRIDE);
    if (name[0] == '\0') return false;
    // Section +4 is exactly 128 on every valid header seen, ours and natives alike. A released slot
    // reads 0 or leftover garbage -- testing ">0" instead wrongly locks out reusable slots.
    unsigned long long blobAddr = base + speciesResourceTableRva +
        (unsigned long long)slot * RESOURCE_BLOB_SIZE;
    int32_t sec4 = 0;
    uint32_t relocMarker = 0, relocHandle = 0;
    __try {
        memcpy(&sec4, (const void*)(uintptr_t)(blobAddr + 4), sizeof(sec4));
        memcpy(&relocMarker, (const void*)(uintptr_t)(blobAddr + BLOB_RELOC_MARKER_OFFSET), sizeof(relocMarker));
        memcpy(&relocHandle, (const void*)(uintptr_t)(blobAddr + BLOB_RELOC_HANDLE_OFFSET), sizeof(relocHandle));
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    if (sec4 != BLOB_HEADER_FIRST_SECTION) return false;
    if (relocMarker == BLOB_RELOC_MARKER && relocHandle == 0) return false; // released zombie
    return true;
}

// Marks the room's own reserved set: the placement table authors every creature this room can
// place, each record carrying its species index (+0x55) and run length (+0x56).
static void MarkRoomRosterSlots(const uint8_t* table, int32_t count, bool* roster) {
    if (!table || count <= 0) return;
    for (int32_t i = 0; i < count; ++i) {
        const uint8_t* rec = table + (size_t)i * PLACEMENT_RECORD_SIZE;
        int sp = rec[PLACEMENT_SPECIES_OFFSET];
        int len = (int)(int8_t)rec[0x56];
        if (len < 1) len = 1;
        for (int k = 0; k < len && sp + k <= RESOURCE_BLOB_MAX_SPECIES; ++k) roster[sp + k] = true;
    }
}

// ============================================================================================
// SLOT AVAILABILITY -- THE SINGLE SOURCE OF TRUTH. Every "can we use this slot?" decision goes
// through ComputeSlotAvailability/RunIsAvailable. DO NOT add owner / state / cached-model-name
// tests elsewhere: the engine never clears those, so they record history, not occupancy.
// ============================================================================================
static const char* const WHY_ROOM_ROSTER = "in this room's placement roster";
static const char* const WHY_LIVE_ENTITY = "a live entity references it";
static const char* const WHY_OUR_LOAD    = "we triggered a load into it this room visit";
static const char* const WHY_RUN_SPAN    = "inside a live species' run span";

struct SlotAvailability {
    bool occupied[SPECIES_SLOT_COUNT];
    const char* why[SPECIES_SLOT_COUNT];   // why a slot is unavailable, for diagnostics
};

// No "model the room-change release" flag any more: IsSlotInTriggeredRunThisVisit already drops
// entries from an earlier visit, which is exactly what that release frees.
static void ComputeSlotAvailability(unsigned long long base, unsigned long long loadedPtrTableRva,
                                    unsigned long long speciesResourceTableRva,
                                    const uint8_t* roomTable, int32_t roomCount,
                                    const bool* inUseByLiveEntity, SlotAvailability& av) {
    for (int s = 0; s < SPECIES_SLOT_COUNT; ++s) { av.occupied[s] = false; av.why[s] = nullptr; }
    auto take = [&av](int s, const char* why) {
        if (s < 0 || s >= SPECIES_SLOT_COUNT) return;
        if (!av.occupied[s]) { av.occupied[s] = true; av.why[s] = why; }
    };

    bool roster[SPECIES_SLOT_COUNT] = {};
    MarkRoomRosterSlots(roomTable, roomCount, roster);
    for (int s = 0; s <= RESOURCE_BLOB_MAX_SPECIES; ++s) {
        if (roster[s])                                   take(s, WHY_ROOM_ROSTER);
        if (inUseByLiveEntity && inUseByLiveEntity[s])    take(s, WHY_LIVE_ENTITY);
        if (IsSlotInTriggeredRunThisVisit(s))             take(s, WHY_OUR_LOAD);
    }

    // Expand anything taken to its whole run, since a creature's blob spans runLen slots. TRUNCATE
    // the span at the next run start -- a runLen reaching past another start is stale.
    for (int s = 0; s <= RESOURCE_BLOB_MAX_SPECIES; ++s) {
        if (!SlotHoldsLiveSpecies(base, loadedPtrTableRva, speciesResourceTableRva, s)) continue;
        int end = s + LoadedSlotRunLen(base, loadedPtrTableRva, (uint8_t)s);
        if (end > RESOURCE_BLOB_MAX_SPECIES + 1) end = RESOURCE_BLOB_MAX_SPECIES + 1;
        for (int j = s + 1; j < end; ++j)
            if (SlotHoldsLiveSpecies(base, loadedPtrTableRva, speciesResourceTableRva, j)) { end = j; break; }
        bool anyTaken = false;
        for (int j = s; j < end; ++j)
            if (av.occupied[j]) { anyTaken = true; break; }
        if (!anyTaken) continue;
        for (int j = s; j < end; ++j) take(j, WHY_RUN_SPAN);
    }
}

// Is a whole run of runLen slots starting at species usable? outBlocker/outWhy report the first
// slot that says no.
static bool RunIsAvailable(const SlotAvailability& av, int species, int runLen,
                           int* outBlocker = nullptr, const char** outWhy = nullptr) {
    if (runLen < 1) runLen = 1;
    for (int k = 0; k < runLen; ++k) {
        int s = species + k;
        if (s > RESOURCE_BLOB_MAX_SPECIES || av.occupied[s]) {
            if (outBlocker) *outBlocker = s;
            if (outWhy) *outWhy = (s > RESOURCE_BLOB_MAX_SPECIES) ? "past the end of the slot table"
                                                                 : av.why[s];
            return false;
        }
    }
    return true;
}

static bool FindFreeLoadedSlot(unsigned long long base, unsigned long long loadedPtrTableRva, unsigned long long speciesResourceTableRva, const uint8_t* roomTable, int32_t roomCount, const bool* speciesInUseByLiveEntity, int slotRunLen, uint8_t* outSpecies, int searchFrom = SPECIES_SLOT_ALLOC_MAX) {
    SlotAvailability av;
    ComputeSlotAvailability(base, loadedPtrTableRva, speciesResourceTableRva,
                            roomTable, roomCount, speciesInUseByLiveEntity, av);
    if (slotRunLen < 1) slotRunLen = 1; // a signed-char <= 0 makes FUN_140286190 claim nothing
    // Highest start slot whose whole run still fits inside the allocatable range.
    int startAt = SPECIES_SLOT_ALLOC_MAX - slotRunLen + 1;
    if (searchFrom < startAt) startAt = searchFrom;
    for (int s = startAt; s >= SPECIES_SLOT_ALLOC_MIN; --s) {
        if (RunIsAvailable(av, s, slotRunLen)) {
            *outSpecies = (uint8_t)s;
            return true;
        }
    }
    return false;
}

// Longest run of consecutive usable slots -- what a refusal should actually report, since a
// creature needs its slots CONSECUTIVE. Total-free would overstate what is spawnable.
static int LongestFreeRun(const SlotAvailability& av) {
    int best = 0, cur = 0;
    for (int s = SPECIES_SLOT_ALLOC_MIN; s <= SPECIES_SLOT_ALLOC_MAX; ++s) {
        if (av.occupied[s]) { cur = 0; continue; }
        if (++cur > best) best = cur;
    }
    return best;
}

// Counts what is actually unavailable, and why, for the "slots full" diagnostic.
static int CountUnavailableSlots(const SlotAvailability& av, char* detail, size_t detailSize) {
    int taken = 0, roster = 0, live = 0, ours = 0, span = 0;
    for (int s = SPECIES_SLOT_ALLOC_MIN; s <= SPECIES_SLOT_ALLOC_MAX; ++s) {
        if (!av.occupied[s]) continue;
        ++taken;
        if (av.why[s] == WHY_ROOM_ROSTER)      ++roster;
        else if (av.why[s] == WHY_LIVE_ENTITY) ++live;
        else if (av.why[s] == WHY_OUR_LOAD)    ++ours;
        else                                   ++span;
    }
    if (detail) snprintf(detail, detailSize, "%d room roster, %d live-referenced, %d ours, %d run-span",
                         roster, live, ours, span);
    return taken;
}

// A text slot caches a raw pointer into the creature blob its text belongs to, so loading over that
// blob makes the next text layout read garbage -- the engine releases runs the text system still uses.
static const uint64_t TEXT_SLOT_STRIDE = 0xE0;
static const int TEXT_SLOT_COUNT = 256;

static bool TryReadU64(uint64_t addr, uint64_t* out) {
    __try { *out = *(volatile uint64_t*)(uintptr_t)addr; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool RunReferencedByTextSlot(unsigned long long base, unsigned long long textSlotTableRva,
                                    unsigned long long speciesResourceTableRva,
                                    int species, int slotRunLen, int* outTextSlot) {
    if (textSlotTableRva == 0 || speciesResourceTableRva == 0) return false;
    const uint64_t lo = base + speciesResourceTableRva + (uint64_t)species * RESOURCE_BLOB_SIZE;
    const uint64_t hi = lo + (uint64_t)(slotRunLen < 1 ? 1 : slotRunLen) * RESOURCE_BLOB_SIZE;
    const uint64_t tb = base + textSlotTableRva;
    for (int i = 0; i < TEXT_SLOT_COUNT; ++i) {
        // +0x98 and +0xA0 are the two cached data pointers the layout pass dereferences.
        for (int off = 0x98; off <= 0xA0; off += 8) {
            uint64_t p = 0;
            if (!TryReadU64(tb + (uint64_t)i * TEXT_SLOT_STRIDE + off, &p)) continue;
            if (p >= lo && p < hi) { if (outTextSlot) *outTextSlot = i; return true; }
        }
    }
    return false;
}

// Picks a free run that also avoids the room's own records and any text-slot reference, walking
// past rejected candidates -- returning only the first free run deadlocked spawning.
static bool FindFreeSlotAvoidingRoomNatives(unsigned long long base, unsigned long long loadedPtrTableRva,
                                            const bool* speciesInUseByLiveEntity, int slotRunLen,
                                            const uint8_t* table, int32_t count,
                                            unsigned long long resolveFnAddr, const char* modelPath,
                                            unsigned long long textSlotTableRva,
                                            unsigned long long speciesResourceTableRva,
                                            uint8_t* outSpecies) {
    // Top-down: start at the highest allocatable slot and walk DOWN past rejected candidates.
    // See FindFreeLoadedSlot's comment for why the direction matters.
    int searchFrom = SPECIES_SLOT_ALLOC_MAX;
    uint8_t candidate = 0;
    int rejected = 0;
    while (FindFreeLoadedSlot(base, loadedPtrTableRva, speciesResourceTableRva, table, count, speciesInUseByLiveEntity, slotRunLen,
                              &candidate, searchFrom)) {
        // Test the WHOLE run against the room's table: a long run otherwise swallows room-native slots
        // further along it, which the room then re-claims over a live creature.
        bool runCollides = false;
        for (int k = 0; k < slotRunLen; ++k) {
            int slot = (int)candidate + k;
            if (slot > SPECIES_SLOT_ALLOC_MAX) { runCollides = true; break; }
            if (RoomHasNativeSpecies(table, count, (uint8_t)slot, resolveFnAddr, modelPath)) {
                runCollides = true;
                break;
            }
        }
        int refSlot = -1;
        if (!runCollides &&
            RunReferencedByTextSlot(base, textSlotTableRva, speciesResourceTableRva,
                                    (int)candidate, slotRunLen, &refSlot)) {
            char msg[176];
            snprintf(msg, sizeof(msg),
                "spawn_enemy: skipping run %d..%d -- text slot %d still points into it",
                (int)candidate, (int)candidate + slotRunLen - 1, refSlot);
            LogDebug(msg);
            runCollides = true;
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
        searchFrom = (int)candidate - 1;   // next candidate DOWNWARD -- see the top-down rationale
        if (searchFrom < SPECIES_SLOT_ALLOC_MIN) break;
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

// Live address of the 64-entry bucket table, stashed for the headroom check.
static volatile uint64_t g_resourceHandleBucketTableAddr = 0;

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

    // Fields fnc_link_model_resource_data_kind3 writes only when their blob section is non-empty;
    // an unwritten one silently keeps the previous pool occupant's handle.
    uint32_t h134 = 0, h140 = 0, h13c = 0, h68 = 0;
    bool ok134 = TryReadU32(entity + 0x134, &h134);
    bool ok140 = TryReadU32(entity + 0x140, &h140);
    bool ok13c = TryReadU32(entity + 0x13c, &h13c);
    bool ok68 = TryReadU32(entity + 0x68, &h68);

    char msg[900];
    int n = snprintf(msg, sizeof(msg),
        "spawn_enemy: %s entity=0x%llX readable(id,+138,+144,+374)=%d%d%d%d id=0x%X (expected 0x%X, "
        "%s) +0x138=0x%08X +0x144=0x%08X +0x374=0x%08X perFrameTickBit0x10000=%s "
        "engineEnumerable(+0x374&3==1)=%s | "
        "stale-prone handles readable(134,140,13c,68)=%d%d%d%d "
        "+0x134=0x%08X +0x140=0x%08X +0x13c=0x%08X +0x68=0x%08X",
        when, (unsigned long long)entity,
        (int)okId, (int)okH, (int)ok144, (int)okFlags,
        id, expectedId,
        (okId && id == expectedId) ? "MATCH" : "MISMATCH/unreadable -- entity slot was reused or freed",
        h138, v144, flags374,
        okFlags ? ((flags374 & 0x10000) ? "SET (tick is processing it)" : "CLEAR (tick is SKIPPING it)") : "?",
        // The engine enumerates on (+0x374 & 3) == 1, not the 0x10000 tick bit -- different predicates.
        okFlags ? (((flags374 & 3) == 1) ? "YES" : "no") : "?",
        (int)ok134, (int)ok140, (int)ok13c, (int)ok68,
        h134, h140, h13c, h68);

    LogDebug(msg);
}

// SPAWN GATES -- read-only refusal checks shared by l_spawn_enemy and l_spawn_enemy_precheck so
// the two can't drift. Anything with a side effect stays inline in l_spawn_enemy.

// Post-boss slow-motion. g_GameSpeed is 1.0 normally, 0.1 during the effect. Fails OPEN: an
// unreadable or NaN value reads as normal speed and never blocks a spawn.
static bool GateBossDefeatSlowdown(unsigned long long base, unsigned long long gameSpeedRva,
                                   unsigned long long bossDefeatEffectRva,
                                   unsigned long long bossDefeatEffectStartRva,
                                   bool logging) {
    if (gameSpeedRva == 0) return false;
    uint32_t rawSpeed = 0, effect = 0, effectStart = 0;
    bool okSpeed = TryReadU32(base + gameSpeedRva, &rawSpeed);
    bool okEffect = (bossDefeatEffectRva != 0) && TryReadU32(base + bossDefeatEffectRva, &effect);
    bool okStart = (bossDefeatEffectStartRva != 0) && TryReadU32(base + bossDefeatEffectStartRva, &effectStart);

    float gameSpeed = 1.0f;
    if (okSpeed) memcpy(&gameSpeed, &rawSpeed, sizeof(gameSpeed));

    // Treat only a genuinely sub-normal, finite timescale as "slowed". NaN/garbage compares
    // false here and is therefore treated as normal, which is the fail-open direction.
    const bool slowed = okSpeed && (gameSpeed > 0.0f) && (gameSpeed < 0.9f);

    // Baseline reading plus every abnormal one -- not every call. The precheck passes
    // logging=false so polling can't flood the log while a slowdown lasts.
    static bool s_loggedSpeedBaseline = false;
    if (logging && (!s_loggedSpeedBaseline || slowed)) {
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
    return slowed;
}

// Matches world+area only -- Set is the evdl variant suffix, not a distinct room.
static const BlacklistedRoomRange* GateRoomBlacklisted(int32_t curWorld, int32_t curArea) {
    for (size_t i = 0; i < sizeof(g_spawnRoomBlacklist) / sizeof(g_spawnRoomBlacklist[0]); ++i) {
        const BlacklistedRoomRange& r = g_spawnRoomBlacklist[i];
        if (curWorld == r.world && curArea >= r.areaMin && curArea <= r.areaMax) return &r;
    }
    return nullptr;
}

static const int kHandleBucketCount = 64;
static const int kHandleBucketSafeHeadroom = 6; // refuse once fewer than this many remain free

// Counts claimed (non-(-1)) buckets; -1 means "not configured", never "full". Also stashes the
// table address for LogResolveHandleBucketGuard, which can't be handed it from its code cave.
static int GateCountClaimedHandleBuckets(unsigned long long base, unsigned long long rva) {
    if (rva == 0) return -1;
    volatile int64_t* bucketTable = (volatile int64_t*)(uintptr_t)(base + rva);
    g_resourceHandleBucketTableAddr = (uint64_t)(uintptr_t)bucketTable;
    int claimed = 0;
    for (int i = 0; i < kHandleBucketCount; ++i) {
        if (bucketTable[i] != -1) ++claimed;
    }
    return claimed;
}

static bool HandleBucketsFull(int claimed) {
    return claimed >= 0 && claimed >= kHandleBucketCount - kHandleBucketSafeHeadroom;
}

// A null pointer or an out-of-range count means the room isn't in a spawnable state right now.
static bool GatePlacementTableValid(unsigned long long base, unsigned long long tablePtrRva,
                                    unsigned long long tableCountRva,
                                    uint8_t** outTable, int32_t* outCount) {
    uint8_t* table = *(uint8_t**)(uintptr_t)(base + tablePtrRva);
    int32_t count = *(int32_t*)(uintptr_t)(base + tableCountRva);
    if (outTable) *outTable = table;
    if (outCount) *outCount = count;
    return table != nullptr && count > 0 && count <= 4096;
}

// Every refusal returns a short machine-readable code plus a message, so Lua can react to the
// reason rather than parse prose.
static int RefuseSpawn(void* L, const char* code, const char* message) {
    p_lua_pushboolean(L, 0);
    p_lua_pushstring(L, message);
    p_lua_pushboolean(L, 0);
    p_lua_pushstring(L, code);
    return 4;
}

// "retry" clears on its own (room change, a defeat); "unavailable" needs a restart or is
// unsupported for this creature/build.
static int PrecheckVerdict(void* L, const char* verdict, const char* code, const char* message) {
    p_lua_pushstring(L, verdict);
    p_lua_pushstring(L, code);
    p_lua_pushstring(L, message);
    return 3;
}

// Read-only "would spawn_enemy refuse right now?" -- allocates nothing, loads nothing, constructs
// nothing. Returns verdict ("ready"/"retry"/"unavailable"), short code, message. See the README.
extern "C" int l_spawn_enemy_precheck(void* L) {
    const char* modelPath = p_lua_tolstring(L, 1, nullptr);
    long long luaCharId = (long long)p_lua_tointegerx(L, 2, nullptr);
    size_t luaTemplateLen = 0;
    const char* luaTemplateRecord = p_lua_tolstring(L, 3, &luaTemplateLen);
    unsigned long long gameSpeedRva = (unsigned long long)p_lua_tointegerx(L, 4, nullptr);
    unsigned long long bossDefeatEffectRva = (unsigned long long)p_lua_tointegerx(L, 5, nullptr);
    unsigned long long bossDefeatEffectStartRva = (unsigned long long)p_lua_tointegerx(L, 6, nullptr);
    unsigned long long worldNumRva = (unsigned long long)p_lua_tointegerx(L, 7, nullptr);
    unsigned long long areaNumRva = (unsigned long long)p_lua_tointegerx(L, 8, nullptr);
    unsigned long long setNumRva = (unsigned long long)p_lua_tointegerx(L, 9, nullptr);
    unsigned long long resourceHandleBucketTableRva = (unsigned long long)p_lua_tointegerx(L, 10, nullptr);
    unsigned long long tablePtrRva = (unsigned long long)p_lua_tointegerx(L, 11, nullptr);
    unsigned long long tableCountRva = (unsigned long long)p_lua_tointegerx(L, 12, nullptr);
    unsigned long long loadedPtrTableRva = (unsigned long long)p_lua_tointegerx(L, 13, nullptr);
    unsigned long long speciesResourceTableRva = (unsigned long long)p_lua_tointegerx(L, 14, nullptr);
    unsigned long long entityIterFnRva = (unsigned long long)p_lua_tointegerx(L, 15, nullptr);
    unsigned long long resolveHandleFnRva = (unsigned long long)p_lua_tointegerx(L, 16, nullptr);
    unsigned long long textSlotTableRva = (unsigned long long)p_lua_tointegerx(L, 17, nullptr);
    unsigned long long mintHandleFnRva = (unsigned long long)p_lua_tointegerx(L, 18, nullptr);
    unsigned long long loadAssetsFnRva = (unsigned long long)p_lua_tointegerx(L, 19, nullptr);
    // Shallow: creature-independent gates only, skipping the slot walk (a SafeCall per live
    // entity). Cheap enough to poll on a timer.
    const bool shallowOnly = p_lua_tointegerx(L, 20, nullptr) != 0;

    if (!modelPath && !shallowOnly) {
        return PrecheckVerdict(L, "unavailable", "bad_args", "spawn_enemy: model_path is required");
    }
    // NOT InternPath'd: nothing here outlives the call, and interning burns a fixed-pool slot.
    unsigned long long base = (unsigned long long)GetModuleHandleA(nullptr);

    if (resolveHandleFnRva == 0 || loadedPtrTableRva == 0 || mintHandleFnRva == 0 ||
        loadAssetsFnRva == 0 || tablePtrRva == 0 || tableCountRva == 0) {
        return PrecheckVerdict(L, "unavailable", "unconfigured",
            "spawn_enemy: this game build is missing an address this feature needs");
    }

    // logging=false -- this runs on every effect request.
    if (GateBossDefeatSlowdown(base, gameSpeedRva, bossDefeatEffectRva, bossDefeatEffectStartRva, false)) {
        return PrecheckVerdict(L, "retry", "boss_defeat_slowdown",
            "the game is in a boss-defeat/slow-motion sequence -- it will be spawnable again shortly");
    }

    // Still releases nothing -- it only advances the visit counter, which is what lets a poll (not
    // just a spawn attempt) notice the room changed and stop pinning the old visit's runs.
    if (worldNumRva != 0 && areaNumRva != 0 && setNumRva != 0) {
        int32_t curWorld = *(int32_t*)(uintptr_t)(base + worldNumRva);
        int32_t curArea = *(int32_t*)(uintptr_t)(base + areaNumRva);
        int32_t curSet = *(int32_t*)(uintptr_t)(base + setNumRva);
        // Only once the new room's table is live: mid-load the room globals read as an intermediate
        // combination, and bumping on that would unpin a run whose creature is still alive.
        if (GatePlacementTableValid(base, tablePtrRva, tableCountRva, nullptr, nullptr))
            NoteRoomIdentity(curWorld, curArea, curSet);
        const BlacklistedRoomRange* blocked = GateRoomBlacklisted(curWorld, curArea);
        if (blocked) {
            char msg[224];
            snprintf(msg, sizeof(msg),
                "enemy spawning is turned off in this room (%s) -- it will work again elsewhere", blocked->label);
            return PrecheckVerdict(L, "retry", "room_blacklisted", msg);
        }
    }

    // Session-lifetime, unlike everything else here: buckets are not reclaimed on a room change.
    {
        int claimed = GateCountClaimedHandleBuckets(base, resourceHandleBucketTableRva);
        if (HandleBucketsFull(claimed)) {
            char msg[200];
            snprintf(msg, sizeof(msg),
                "too many distinct resources loaded this session (%d/%d resource handles claimed) "
                "-- enemy spawning needs a game restart to recover", claimed, kHandleBucketCount);
            return PrecheckVerdict(L, "unavailable", "handles_full", msg);
        }
    }

    uint8_t* roomTable = nullptr;
    int32_t roomCount = 0;
    if (!GatePlacementTableValid(base, tablePtrRva, tableCountRva, &roomTable, &roomCount)) {
        return PrecheckVerdict(L, "retry", "table_invalid",
            "the room isn't in a spawnable state right now -- try again in a moment");
    }

    // Everything past here is creature-specific, which is exactly what shallow mode skips.
    if (shallowOnly) {
        return PrecheckVerdict(L, "ready", "", "");
    }

    if (luaCharId == 0 || !luaTemplateRecord || luaTemplateLen != PLACEMENT_RECORD_SIZE) {
        return PrecheckVerdict(L, "unavailable", "no_creature_data",
            "no offline data for this creature (missing from kh1_lua_library/creature_data.lua)");
    }
    const uint8_t* templateRec = (const uint8_t*)luaTemplateRecord;
    int templateSlotRunLen = (int)(int8_t)templateRec[0x56];
    if (templateSlotRunLen < 1) templateSlotRunLen = 1;

    // Mirrors l_spawn_enemy: a creature already in a reusable slot needs no free run, so checking
    // only for a free run would refuse spawns that would have succeeded.
    bool speciesInUseByLiveEntity[RESOURCE_BLOB_MAX_SPECIES + 1] = {};
    MarkSpeciesInUseByLiveEntities(base, entityIterFnRva, resolveHandleFnRva,
                                   speciesResourceTableRva, speciesInUseByLiveEntity);
    // FindTriggeredLoad ignores earlier visits by itself, so no extra room-change guard here.
    uint8_t species = 0;
    const bool ourLoadThisVisit = FindTriggeredLoad(modelPath, &species);
    if ((ourLoadThisVisit &&
         ReuseSlotIsSafe(base, loadedPtrTableRva, speciesResourceTableRva, species, modelPath, "precheck: our own triggered load")) ||
        (!ourLoadThisVisit &&
         FindLoadedSlotByFilename(base, loadedPtrTableRva, speciesResourceTableRva, modelPath, &species) &&
         ReuseSlotIsSafe(base, loadedPtrTableRva, speciesResourceTableRva, species, modelPath, "precheck: loaded-slot filename scan"))) {
        if (RoomHasNativeSpecies(roomTable, roomCount, species, base + resolveHandleFnRva, modelPath)) {
            return PrecheckVerdict(L, "retry", "slot_collision",
                "that creature's slot is in use by a different creature native to this room -- try another room");
        }
        return PrecheckVerdict(L, "ready", "", "");
    }
    // Same rule as the spawn path: while our own load for this model is in flight, its slot is
    // the ONLY acceptable outcome -- reporting "ready" off a free-run search would green-light a
    // spawn the spawn path is going to make wait anyway.
    if (ourLoadThisVisit) {
        return PrecheckVerdict(L, "retry", "our_load_pending",
            "this creature's asset load is still in progress -- it will be spawnable in a moment");
    }

    if (FindFreeSlotAvoidingRoomNatives(base, loadedPtrTableRva, speciesInUseByLiveEntity,
                                        templateSlotRunLen, roomTable, roomCount,
                                        base + resolveHandleFnRva, modelPath,
                                        textSlotTableRva, speciesResourceTableRva, &species)) {
        return PrecheckVerdict(L, "ready", "", "");
    }

    SlotAvailability av;
    ComputeSlotAvailability(base, loadedPtrTableRva, speciesResourceTableRva,
                            roomTable, roomCount, speciesInUseByLiveEntity, av);
    char msg[240];
    snprintf(msg, sizeof(msg),
        "no creature slots free right now -- this creature needs %d consecutive free slots and "
        "only %d are free. They come back as spawned enemies are defeated, or on a room change.",
        templateSlotRunLen, LongestFreeRun(av));
    char code[48];
    snprintf(code, sizeof(code), "slots_full/%d/%d", templateSlotRunLen, LongestFreeRun(av));
    return PrecheckVerdict(L, "retry", code, msg);
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
    unsigned long long entityIterFnRva = (unsigned long long)p_lua_tointegerx(L, 17, nullptr);
    long long luaCharId = (long long)p_lua_tointegerx(L, 18, nullptr);
    long long luaWeight = (long long)p_lua_tointegerx(L, 19, nullptr);
    size_t luaTemplateLen = 0;
    const char* luaTemplateRecord = p_lua_tolstring(L, 20, &luaTemplateLen);
    unsigned long long resourceHandleBucketTableRva = (unsigned long long)p_lua_tointegerx(L, 26, nullptr);

    // ENTRY of the same function (Steam 0x1DD650). Counts calls so a zero path-B count can be
    // told apart from the function never running -- see InstallResourceEntryLookupCounterHook.

    // Boss-defeat slow-motion state. g_GameSpeed is the engine's own timescale: 1.0 normally,
    // 0.1 during the effect.
    unsigned long long gameSpeedRva = (unsigned long long)p_lua_tointegerx(L, 27, nullptr);
    unsigned long long bossDefeatEffectRva = (unsigned long long)p_lua_tointegerx(L, 28, nullptr);
    unsigned long long bossDefeatEffectStartRva = (unsigned long long)p_lua_tointegerx(L, 29, nullptr);
    // Text-slot table base -- see RunReferencedByTextSlot.
    unsigned long long textSlotTableRva = (unsigned long long)p_lua_tointegerx(L, 30, nullptr);
    // Engine's block-claim routine (owner=species + runLen into every slot of the run).
    unsigned long long claimSlotRunFnRva = (unsigned long long)p_lua_tointegerx(L, 31, nullptr);
    // Engine's block-release routine (owner=0xFF + flags=0 + blob wipe), used at room change.
    unsigned long long releaseSlotRunFnRva = (unsigned long long)p_lua_tointegerx(L, 32, nullptr);

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

    // Re-read the previous spawn: an unreadable or mismatched id means its pool slot was recycled.

    // Refuse during the post-boss slow-motion. Nothing is allocated or published yet, so
    // returning here is a true no-op.
    if (GateBossDefeatSlowdown(base, gameSpeedRva, bossDefeatEffectRva, bossDefeatEffectStartRva, true)) {
        return RefuseSpawn(L, "boss_defeat_slowdown",
            "spawn_enemy: the game is in a boss-defeat/slow-motion sequence right now -- refusing "
            "so the spawn does not land while the engine is tearing down battle resources; retry shortly");
    }

    // On a room change only clear our triggered-load bookkeeping -- the engine re-tiles slots, so a
    // stale entry would let the reuse path build against a slot it has reassigned. It frees the runs.
    if (worldNumRva != 0 && areaNumRva != 0 && setNumRva != 0) {
        int32_t curWorld = *(int32_t*)(uintptr_t)(base + worldNumRva);
        int32_t curArea = *(int32_t*)(uintptr_t)(base + areaNumRva);
        int32_t curSet = *(int32_t*)(uintptr_t)(base + setNumRva);
        // Same table-valid guard as the precheck, so both paths number visits identically.
        if (GatePlacementTableValid(base, tablePtrRva, tableCountRva, nullptr, nullptr))
            NoteRoomIdentity(curWorld, curArea, curSet);
        if (g_lastSpawnVisitValid && g_lastSpawnVisit != g_roomVisit) {
            // Release BEFORE clearing the bookkeeping -- it is what identifies our runs.
            ReleaseOurTriggeredRuns(base, loadedPtrTableRva, releaseSlotRunFnRva,
                                    entityIterFnRva, resolveHandleFnRva, speciesResourceTableRva);
            g_triggeredLoadCount = 0;
            LogDebug("spawn_enemy: room changed -- triggered-load bookkeeping cleared");
            // A creature from the previous room cannot still be alive; stop re-reading its pointer.
        }
        g_lastSpawnVisit = g_roomVisit;
        g_lastSpawnVisitValid = true;

        const BlacklistedRoomRange* blocked = GateRoomBlacklisted(curWorld, curArea);
        if (blocked) {
            char msg[224];
            snprintf(msg, sizeof(msg),
                "spawn_enemy: refusing -- room %d/%d/%d is blacklisted (%s covers areas %d..%d): %s",
                curWorld, curArea, curSet, blocked->label, blocked->areaMin, blocked->areaMax, blocked->why);
            LogDebug(msg);
            return RefuseSpawn(L, "room_blacklisted",
                "spawn_enemy: enemy spawning is disabled in this room");
        }
    }

    // Refuse once the 64-entry resource-handle bucket table is nearly full. Read-only count of
    // non-(-1) entries; only stops spawn_enemy's own churn exhausting it, not other gameplay.
    {
        int claimed = GateCountClaimedHandleBuckets(base, resourceHandleBucketTableRva);
        if (HandleBucketsFull(claimed)) {
            char msg[160];
            snprintf(msg, sizeof(msg),
                "spawn_enemy: refusing -- resource-handle bucket table nearly full (%d/%d claimed), spawning more risks a downstream crash",
                claimed, kHandleBucketCount);
            LogDebug(msg);
            return RefuseSpawn(L, "handles_full",
                "spawn_enemy: too many distinct resources loaded this session (bucket table nearly full) -- refusing to risk a crash");
        }
    }

    uint8_t** tablePtrAddr = (uint8_t**)(uintptr_t)(base + tablePtrRva);
    int32_t* tableCountAddr = (int32_t*)(uintptr_t)(base + tableCountRva);

    uint8_t* oldTable = nullptr;
    int32_t oldCount = 0;
    if (!GatePlacementTableValid(base, tablePtrRva, tableCountRva, &oldTable, &oldCount)) {
        return RefuseSpawn(L, "table_invalid",
            "spawn_enemy: placement table not valid right now (wrong room state?)");
    }

    // species (the local slot number) is derived below: reuse an
    // already-loaded slot, or claim a free one and load fresh.
    uint8_t species = 0;
    bool needsLoad = false;

    // kh1_lua_library/creature_data.lua is the only source of char-id/weight/template.
    if (luaCharId == 0 || !luaTemplateRecord || luaTemplateLen != PLACEMENT_RECORD_SIZE) {
        return RefuseSpawn(L, "no_creature_data",
            "spawn_enemy: no offline fallback data for this creature (missing from kh1_lua_library/creature_data.lua)");
    }
    const uint8_t* templateRec = (const uint8_t*)luaTemplateRecord;
    uint16_t charId = (uint16_t)luaCharId;
    uint8_t weight = (uint8_t)luaWeight;
    // How many consecutive slots this creature's load will claim (record+0x56).
    const int templateSlotRunLen = (int)(int8_t)templateRec[0x56];

    if (loadedPtrTableRva == 0) {
        return RefuseSpawn(L, "unconfigured",
            "spawn_enemy: loadedSpeciesPtrTable address not configured for this game build");
    }
    // All guard/diagnostic hooks were removed: they guarded consumers of already-bad
    // data, never converged (~14 sites), and both crash and freeze reproduced with all of them off.

    if (mintHandleFnRva == 0) {
        return RefuseSpawn(L, "unconfigured",
            "spawn_enemy: fnc_mint_resource_handle not configured for this game build -- can't safely reuse a captured template's model/motion handles");
    }
    bool speciesInUseByLiveEntity[RESOURCE_BLOB_MAX_SPECIES + 1] = {};
    MarkSpeciesInUseByLiveEntities(base, entityIterFnRva, resolveHandleFnRva, speciesResourceTableRva, speciesInUseByLiveEntity);
    // A load we already triggered for this model pins the request to ITS slot: reusable once
    // ready, waited on otherwise. Falling through to a fresh load instead claimed a new run
    // every retry frame -- exhausting the slot window and landing loads inside other in-flight
    // runs, overwriting their blobs.
    const bool ourLoadThisVisit = FindTriggeredLoad(modelPath, &species);
    if (ourLoadThisVisit &&
        ReuseSlotIsSafe(base, loadedPtrTableRva, speciesResourceTableRva, species, modelPath, "our own triggered load")) {
        // Already triggered a load for this creature this session -- reuse
        // the species and don't trigger another load.
        if (RoomHasNativeSpecies(oldTable, oldCount, species, base + resolveHandleFnRva, modelPath)) {
            char msg[160];
            snprintf(msg, sizeof(msg), "spawn_enemy: slot %d (already triggered by us this session as %s) is used by a different native record in this room -- refusing", species, modelPath);
            LogDebug(msg);
            return RefuseSpawn(L, "slot_collision",
                "spawn_enemy: chosen slot collides with a different creature already native to this room -- refusing");
        }
    } else if (ourLoadThisVisit) {
        // Queued but not started reads state==0, which ReuseSlotIsSafe cannot tell from a dead
        // slot. Wait it out; the request retries next frame and times out upstream if the load
        // never lands.
        static uint8_t lastWaitSpecies = 0xFF;
        if (species != lastWaitSpecies) {
            lastWaitSpecies = species;
            char msg[224];
            snprintf(msg, sizeof(msg),
                "spawn_enemy: our triggered load for %s (species=%d) is not usable yet -- waiting "
                "on it rather than claiming another slot run", modelPath, (int)species);
            LogDebug(msg);
        }
        p_lua_pushboolean(L, 0);
        p_lua_pushstring(L, "spawn_enemy: asset load still in progress -- call again next frame (this is normal for a creature's first load this session)");
        p_lua_pushboolean(L, 1);
        return 3;
    } else if (FindLoadedSlotByFilename(base, loadedPtrTableRva, speciesResourceTableRva, modelPath, &species) &&
               ReuseSlotIsSafe(base, loadedPtrTableRva, speciesResourceTableRva, species, modelPath, "loaded-slot filename scan")) {
        // Already loaded this session -- reuse it, but check the room's own
        // placement table doesn't already use this slot for something else.
        if (RoomHasNativeSpecies(oldTable, oldCount, species, base + resolveHandleFnRva, modelPath)) {
            char msg[160];
            snprintf(msg, sizeof(msg), "spawn_enemy: slot %d (already loaded elsewhere this session as %s) is used by a different native record in this room -- refusing", species, modelPath);
            LogDebug(msg);
            return RefuseSpawn(L, "slot_collision",
                "spawn_enemy: chosen slot collides with a different creature already native to this room -- refusing");
        }
    // Room-native collision is folded into slot selection; checking after it deadlocked spawning.
    } else if (FindFreeSlotAvoidingRoomNatives(base, loadedPtrTableRva, speciesInUseByLiveEntity,
                                               templateSlotRunLen, oldTable, oldCount,
                                               base + resolveHandleFnRva, modelPath,
                                               textSlotTableRva, speciesResourceTableRva, &species) ||
               // No reclaim retry: releasing a run wipes its blob and the liveness test gating that
               // is unreliable, so refusing when the window is full is safer.
               false) {
        // Required only for a genuinely fresh slot; checked before the table is touched.
        if (loadAssetsFnRva == 0) {
            return RefuseSpawn(L, "unconfigured",
                "spawn_enemy: fnc_load_gimmick_assets not configured for this game build -- can't load a creature with zero presence in this room");
        }
        needsLoad = true;
    } else {
        // Capacity limit, not a bug. Counted through the SAME availability map the search used, so
        // the number always agrees with why the search failed.
        SlotAvailability av;
        ComputeSlotAvailability(base, loadedPtrTableRva, speciesResourceTableRva,
                                oldTable, oldCount, speciesInUseByLiveEntity, av);
        char detail[128];
        int occupied = CountUnavailableSlots(av, detail, sizeof(detail));
        char msg[400];
        snprintf(msg, sizeof(msg),
            "spawn_enemy: refusing -- no free run of %d consecutive species slots in the %d..%d "
            "window (%d of %d unavailable: %s). Clears on the next room change.",
            templateSlotRunLen, SPECIES_SLOT_ALLOC_MIN, SPECIES_SLOT_ALLOC_MAX, occupied,
            SPECIES_SLOT_ALLOC_MAX - SPECIES_SLOT_ALLOC_MIN + 1, detail);
        LogDebug(msg);

        // A roster claiming nearly the whole window may be over-counted: MarkRoomRosterSlots
        // trusts +0x55/+0x56 on EVERY record, but those bytes may only mean species/run-length
        // on actor records (id category 3). Log the composition once per table so the histogram
        // can settle whether roster marking should filter by category.
        static int32_t lastRosterLoggedCount = -1;
        if (occupied >= 40 && oldTable && oldCount > 0 && oldCount != lastRosterLoggedCount) {
            lastRosterLoggedCount = oldCount;
            int catCount[8] = {}; int catSlots[8] = {}; int otherCats = 0;
            for (int32_t i = 0; i < oldCount; ++i) {
                const uint8_t* rec = oldTable + (size_t)i * PLACEMENT_RECORD_SIZE;
                uint32_t rid; memcpy(&rid, rec, 4);
                uint32_t cat = rid >> 16;
                int len = (int)(int8_t)rec[0x56]; if (len < 1) len = 1;
                if (cat < 8) { ++catCount[cat]; catSlots[cat] += len; } else { ++otherCats; }
            }
            char hmsg[320];
            snprintf(hmsg, sizeof(hmsg),
                "spawn_enemy: roster composition (%d records) by id category (records/claimed slots): "
                "c0=%d/%d c1=%d/%d c2=%d/%d c3=%d/%d c4=%d/%d c5=%d/%d c6=%d/%d c7=%d/%d other=%d",
                oldCount,
                catCount[0], catSlots[0], catCount[1], catSlots[1], catCount[2], catSlots[2],
                catCount[3], catSlots[3], catCount[4], catSlots[4], catCount[5], catSlots[5],
                catCount[6], catSlots[6], catCount[7], catSlots[7], otherCats);
            LogDebug(hmsg);
        }

        // Carry the numbers in the CODE, not the message: the reason code is what reliably survives
        // the native/Lua boundary (see the note above SPAWN_FAILURE_MESSAGES).
        char code[48];
        snprintf(code, sizeof(code), "slots_full/%d/%d", templateSlotRunLen, LongestFreeRun(av));
        return RefuseSpawn(L, code,
            "spawn_enemy: no creature slots free right now -- each creature type claims several. They come back as spawned enemies are defeated, or when you change rooms.");
    }
    // NO post-selection "slot is CLAIMED by a different creature" check here: it read stale blocks
    // as claimed and rejected nearly every valid slot. Availability is settled by the map above.

    size_t newSize = (size_t)(oldCount + 1) * PLACEMENT_RECORD_SIZE;
    uint8_t* newTable = (uint8_t*)VirtualAlloc(nullptr, newSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!newTable) {
        return RefuseSpawn(L, "alloc_fail",
            "spawn_enemy: VirtualAlloc failed");
    }

    memcpy(newTable, oldTable, (size_t)oldCount * PLACEMENT_RECORD_SIZE);
    uint8_t* newRec = newTable + (size_t)oldCount * PLACEMENT_RECORD_SIZE;
    memcpy(newRec, templateRec, PLACEMENT_RECORD_SIZE);

    // record+8 caches a resource handle; zero it so the constructor re-resolves.
    memset(newRec + 8, 0, 4);

    // Category 3 (character/actor) drives the constructor's per-kind setup; slot index is the
    // record's own position in the resized table.
    uint32_t newId = ((uint32_t)3 << 16) | ((uint32_t)oldCount & 0xFFFFu);
    memcpy(newRec + 0, &newId, 4);
    memcpy(newRec + PLACEMENT_POS_X_OFFSET, &x, 4);
    memcpy(newRec + PLACEMENT_POS_Y_OFFSET, &y, 4);
    memcpy(newRec + PLACEMENT_POS_Z_OFFSET, &z, 4);

    // Force the species byte to the chosen slot rather than trusting
    // templateRec's own value, which can be stale from a different room.
    newRec[PLACEMENT_SPECIES_OFFSET] = species;

    // char-id 0 would be read as a party-member index and overwrite Sora's own pointer.
    memcpy(newRec + 0x4c, &charId, 2);
    newRec[0x59] = weight;

    // Publish first: fnc_load_gimmick_assets looks the record up by newId. Refusals roll it back.
    *tablePtrAddr = newTable;
    *tableCountAddr = oldCount + 1;

    // record+0x60/+0x64 are session-relative handles, so mint fresh ones rather than reuse.
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
    // The load claims record+0x56 consecutive slots; verify every one is still free before triggering.
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
            // Re-verify immediately before the destructive load. Built from oldTable so it is the
            // room's OWN roster, excluding the record we just spliced for ourselves.
            SlotAvailability av;
            ComputeSlotAvailability(base, loadedPtrTableRva, speciesResourceTableRva,
                                    oldTable, oldCount, speciesInUseByLiveEntity, av);
            int conflictSlot = -1;
            const char* why = nullptr;
            if (!RunIsAvailable(av, species, slotRunLen, &conflictSlot, &why)) {
                char msg[300];
                snprintf(msg, sizeof(msg),
                    "spawn_enemy: refusing -- loading species=%d would claim %d consecutive slots "
                    "and slot %d is unavailable (%s); triggering this load would corrupt it",
                    species, slotRunLen, conflictSlot, why ? why : "unknown");
                LogDebug(msg);
                *tablePtrAddr = oldTable;
                *tableCountAddr = oldCount;
                return RefuseSpawn(L, "run_slot_taken",
                    "spawn_enemy: this creature's asset load would claim several consecutive species slots and one of them is already in use -- refusing to avoid corrupting a live creature");
            }
        }

        // Claim the run as a block BEFORE loading, the way the engine does -- its room-unload sweep
        // only releases blocks whose owner == their own index, so an unclaimed run is never reclaimed.
        if (claimSlotRunFnRva != 0) {
            int claimLen = slotRunLen > 0 ? slotRunLen : 1;
            unsigned long long claimArgs[2] = { (unsigned long long)species, (unsigned long long)claimLen };
            unsigned long long claimResult = 0;
            char cmsg[200];
            if (SafeCall(base + claimSlotRunFnRva, claimArgs, 2, claimResult)) {
                snprintf(cmsg, sizeof(cmsg),
                    "spawn_enemy: claimed species=%d run of %d as a block (engine will release it at room unload)",
                    species, claimLen);
            } else {
                snprintf(cmsg, sizeof(cmsg),
                    "spawn_enemy: species-slot claim CRASHED for species=%d run %d -- continuing unclaimed; "
                    "this run will not be reclaimed at room unload", species, claimLen);
            }
            LogDebug(cmsg);
        }

        // Tail slots of the claimed run keep whatever cached identity their last primary load
        // wrote there (name and state persist across release). The ENGINE trusts that cache when
        // it later wants that model again and will construct from a slot whose content this load
        // is about to overwrite -- observed as a black-screen freeze when a cutscene skip reused
        // a stale xa_di_* identity inside one of our runs. Clear the stale identity so nothing
        // can reuse these slots by name.
        {
            int runSpan = slotRunLen > 0 ? slotRunLen : 1;
            for (int k = 1; k < runSpan; ++k) {
                int s = (int)species + k;
                if (s > RESOURCE_BLOB_MAX_SPECIES) break;
                char* tailName = (char*)(uintptr_t)(base + loadedPtrTableRva +
                    LOADED_SPECIES_MODEL_NAME_OFFSET_FROM_PTR + (size_t)s * LOADED_SPECIES_STRIDE);
                volatile uint8_t* tailState = (volatile uint8_t*)(uintptr_t)(base + loadedPtrTableRva +
                    LOADED_SPECIES_STATE_OFFSET_FROM_PTR + (size_t)s * LOADED_SPECIES_STRIDE);
                if (tailName[0] != '\0' || *tailState != 0) {
                    char smsg[224];
                    snprintf(smsg, sizeof(smsg),
                        "spawn_enemy: clearing stale cached identity on tail slot %d (was '%.32s', "
                        "state=%u) so the engine cannot reuse it by name", s, tailName, (unsigned)*tailState);
                    LogDebug(smsg);
                    tailName[0] = '\0';
                    *tailState = 0;
                }
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

        // Non-blocking: the async job runs on this thread, so waiting here would deadlock it.
        // Requires state==6 -- the first callback sets the pointer but leaves state 0.
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

    // A run whose slots disagree on owner was only partly claimed: the unclaimed tail was never
    // populated, so anything the asset stores there reads as the previous occupant's leftovers.
    if (loadedPtrTableRva != 0 && species >= 0 && species <= RESOURCE_BLOB_MAX_SPECIES) {
        volatile uint8_t* stateAddr = (volatile uint8_t*)(uintptr_t)(base + loadedPtrTableRva +
            LOADED_SPECIES_STATE_OFFSET_FROM_PTR + (size_t)species * LOADED_SPECIES_STRIDE);
        if (*stateAddr == SPECIES_SLOT_STATE_READY) {
            int runLen = LoadedSlotRunLen(base, loadedPtrTableRva, species);
            int badSlot = -1;
            uint8_t owners[16] = {};
            if (!RunOwnershipUniform(base, loadedPtrTableRva, species, runLen, &badSlot, owners)) {
                char od[128]; int n = 0;
                for (int k = 0; k < runLen && k < 16 && n < (int)sizeof(od) - 8; ++k)
                    n += snprintf(od + n, sizeof(od) - n, "%s0x%02X", k ? "," : "", owners[k]);
                char msg[320];
                snprintf(msg, sizeof(msg),
                    "spawn_enemy: species=%d run of %d has MIXED ownership (slots %d..%d owners=%s, "
                    "first mismatch at slot %d) -- the run was only partly claimed, so its tail holds "
                    "stale data. Refusing to construct.",
                    species, runLen, species, species + runLen - 1, od, badSlot);
                LogDebug(msg);
                *tablePtrAddr = oldTable;
                *tableCountAddr = oldCount;
                p_lua_pushboolean(L, 0);
                p_lua_pushstring(L, "spawn_enemy: asset run only partly claimed -- refusing (retry may pick a different slot)");
                p_lua_pushboolean(L, 1);
                return 3;
            }
        }
    }

    // Refuse to construct if the blob's section-offset header is not sane (non-negative,
    // ascending, in-bounds, not all-zero). Bound loosely -- a tight bound blocked all spawning.
    if (speciesResourceTableRva != 0 && species >= 0 && species <= RESOURCE_BLOB_MAX_SPECIES) {
        unsigned long long blobAddr =
            base + speciesResourceTableRva + (unsigned long long)species * RESOURCE_BLOB_SIZE;

        // Bound by the rest of the blob table, not this creature's own run: the blob was written by
        // whichever creature loaded it, which may have claimed a longer one. A tight bound blocked spawns.
        int blobRunLen = (int)(int8_t)newRec[0x56];
        if (blobRunLen < 1) blobRunLen = 1;
        const unsigned long long blobSpan =
            (unsigned long long)(RESOURCE_BLOB_MAX_SPECIES + 1 - species) *
            (unsigned long long)RESOURCE_BLOB_SIZE;

        // Nine dwords (+4..+0x24): the kind-3 constructor path reads out to +0x24. Only the first
        // five are sanity-checked -- the rest have never been verified as monotonic.
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
            // An all-zero header means the blob was never populated or has been wiped.
            if (headerSane && sec[0] == 0 && sec[4] == 0) headerSane = false;
        }

        // Log the header on every attempt, so a bad one can be compared against good ones.
        if (readOk) {
            // Log the derived section sizes: a size of 0 means that entity field is never written and
            // keeps the previous pool occupant's handle.
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

        // Monotonic sections can still be stomped: a load landing inside the run rewrites later
        // offsets equal to earlier ones, collapsing a section to zero size. Entity +0x68/+0x13c/
        // +0x134 are minted from sections 1..2/3..4/4..5, and the engine dereferences all three
        // without null checks -- a zero-size source constructs an entity that crashes on its
        // first tick. Every healthy blob observed carries nonzero sizes for these three.
        if (sec[2] - sec[1] == 0 || sec[4] - sec[3] == 0 || sec[5] - sec[4] == 0) {
            char msg[300];
            snprintf(msg, sizeof(msg),
                "spawn_enemy: species=%d blob sections feeding entity+0x68/+0x13c/+0x134 have "
                "sizes %d/%d/%d -- a zero means the blob was partly overwritten after loading; "
                "refusing to construct (a NULL entity field crashes the engine on its next tick)",
                species, sec[2] - sec[1], sec[4] - sec[3], sec[5] - sec[4]);
            LogDebug(msg);
            *tablePtrAddr = oldTable;
            *tableCountAddr = oldCount;
            return RefuseSpawn(L, "blob_field_stale",
                "spawn_enemy: this creature's resource data is damaged in this room right now -- it will recover on a room change");
        }
    }

    unsigned long long spawnFnAddr = base + spawnFnRva;
    unsigned long long args[1] = { (unsigned long long)newId };
    unsigned long long result = 0;

    // The slot is populated now -- the reliable moment to capture the load token.
    EnsureTriggeredLoadToken(base, loadedPtrTableRva, (uint8_t)species);

    // Log the record's handle fields before constructing: a crash rolls the publish back.
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

    // Snapshot the new entity immediately so a success-but-nothing-appeared report can be pinned.
    LogEntitySnapshot("post-construct:", (uint64_t)result, (uint32_t)newId);

    p_lua_pushboolean(L, 1);
    p_lua_pushinteger(L, (long long)result);
    return 2;
}
