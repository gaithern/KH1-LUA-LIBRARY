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
static const float MIN_SPAWN_DISTANCE_FROM_SORA = 100.0f; // live-tuned (was 500, then 100)

// Refuse if another actor is already at the spawn point -- two entities in the same place is a
// known crash. Sora and party members are excluded since they are also kind==3.
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
// come from kh1_creature_data.lua. Resource blob is 256KB per slot, species 0..64.
static const size_t RESOURCE_BLOB_SIZE = 0x40000;
static const int RESOURCE_BLOB_MAX_SPECIES = 0x40;

// Debugging aid: entity id of the most recent spawn_enemy call to reach the
// constructor, for an external CE hook to target. Not read in this DLL.
static volatile uint32_t g_lastSpawnAttemptId = 0;

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

// Last room we spawned in -- used only to detect a room change.
static int32_t g_lastSpawnRoomWorld = -1;
static int32_t g_lastSpawnRoomArea = -1;
// Nothing in the engine ever clears a creature block's ownership. It reclaims purely by EVICTION --
// the next room's loader tiles from slot 0 and overwrites what it needs, leaving the rest stale --
// so our runs stay marked occupied forever and the window exhausts. Releasing them ourselves at room
// change is safe: our entities are destroyed by then. fnc_release_species_slot_run also wipes the
// blob, which a hand-rolled reset would miss.
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
    // Room-change detection is LAZY -- it fires on the first spawn_enemy call in the new room, by
    // which point a creature from the old room may still be alive. Releasing a block out from under
    // one hangs the game: fnc_blob_command_list_walk_and_patch (0x1401D8073) re-reads the same tag
    // forever when it is not 0/1/2/0x11/0x8000, and stale blob bytes are exactly that.
    // Live-confirmed 2026-08-13: released species 30 while entity 0 still referenced it -> spin at
    // exe+0x1D8078 with R14 in blob slot 30 and tag 9.
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

static int32_t g_lastSpawnRoomSet = -1;
static bool g_lastSpawnRoomValid = false;




// Our own record of slots we loaded. Unlike the live-entity scan it does not depend on handle
// resolution, which fails exactly when handles start going bad.
static bool IsSpeciesTriggeredThisSession(uint8_t species) {
    for (int i = 0; i < g_triggeredLoadCount; ++i) {
        if (g_triggeredLoads[i].species == species) return true;
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

// Finds a slot genuinely holding this model. The cached name and state are NEVER cleared when a room
// stops using a slot, so a run MEMBER keeps the name of whatever was last a start there -- matching
// on name alone returned those stale labels, and the reuse gate then rejected them, dropping a
// perfectly reusable creature into the fresh-load path and a slots_full refusal.
// SlotHoldsLiveSpecies additionally requires a valid blob header, which only a real run start has.
// Also bounded to the real 64-entry table; this used to scan 256 and read past its end.
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

// Is every slot of this run still owned by that species?
// Still a live run start, with nothing else having claimed part of its run since.
//
// Ownership is deliberately NOT part of this. A loaded species routinely carries owner==0xFF
// ("loaded, unowned" -- observed on many slots across many dumps), so the old `owner == species`
// test rejected reusable creatures outright and sent them to the fresh-load path, where they then
// failed for want of free slots. What actually invalidates a reuse is another asset having loaded
// over part of the run, and a slot with a valid blob header IS such an asset's start.
static bool SpeciesRunStillIntact(unsigned long long base, unsigned long long loadedPtrTableRva,
                                  unsigned long long speciesResourceTableRva,
                                  uint8_t species, int runLen) {
    if (!SlotHoldsLiveSpecies(base, loadedPtrTableRva, speciesResourceTableRva, species)) return false;
    for (int k = 1; k < runLen && species + k <= RESOURCE_BLOB_MAX_SPECIES; ++k)
        if (SlotHoldsLiveSpecies(base, loadedPtrTableRva, speciesResourceTableRva, species + k))
            return false;
    return true;
}

// Every slot of a run must carry the SAME owner byte. A partly-claimed run means the tail slots
// were never populated, so data living there is whatever the previous occupant left.
// Uniform 0xFF occurs on healthy loaded runs, so only DISAGREEMENT is the defect -- not 0xFF itself.
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
        "blob header is gone or another asset has loaded inside it. Refusing to reuse it; falling "
        "through to a fresh load.",
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

// Allocation window is 30..49.
//
// 50..63 is ENGINE SCRATCH and must be avoided. fnc_release_species_slot_run returns the blob base,
// so subsystems call it purely to grab that buffer; all 18 call sites cover 50-51, 52-55, 52-59,
// 52-63, 54-55, 56-59, 60-63. Crucially they fire MID-ROOM, not just at transitions (approaching a
// door triggers fnc_1BD_load_next_map_texture), so a creature there is clobbered while still alive.
// Proof: slot 56's bytes are byte-identical (sha 55425c83) across three dumps hours apart in which
// the slot table claimed three DIFFERENT creature models -- our data was replaced by the same fixed
// atlas every time. Being clobbered in 30..49 is harmless by contrast: only the next room's loader
// tiles there, and our entities are destroyed by then.
static const int SPECIES_SLOT_ALLOC_MAX = 49;

// owner == 0xFF is the engine's own unclaimed test. State and run-length bytes are both
// unreliable: run members keep state==0, and released slots keep a stale run length.
static const uint8_t SPECIES_SLOT_UNCLAIMED_OWNER = 0xFF;

// A creature claims a RUN of record+0x56 consecutive slots, so the WHOLE run must be free.
// Scans TOP-DOWN: the engine tiles upward from slot 0, so allocating high stays out of its way
// while still letting us drop lower when the upper end is full.
//
// No hardcoded floor. The low slots usually hold Sora + party (measured 0-11 xa_ex_0010,
// 12-20 xa_ex_0040, 21-29 xa_ex_0050) but the roster VARIES -- guest members, and rooms without
// Donald/Goofy -- so a fixed floor either wastes space or is wrong. BuildSlotOccupancy already
// rejects anything actually resident, which is the real protection; unlike 50..63 there is no
// evidence of anything grabbing low slots MID-ROOM, so being tiled over at the next room load is
// harmless (our entities are gone by then).
static const int SPECIES_SLOT_ALLOC_MIN = 0;
// Slots that were already free when we entered this room. The engine only re-tiles slot runs at
// ROOM LOAD, when everything referencing them has been torn down -- a run it releases MID-room may
// still be pointed at (live-confirmed: a display slot held a pointer into species 53's blob after
// its run was released, and loading over it crashed the game). owner==0xFF alone cannot tell the
// two apart, so we only ever allocate runs that were free at room entry.
static bool g_freeAtRoomEntry[SPECIES_SLOT_COUNT] = {};
static bool g_roomEntrySnapshotValid = false;

static void SnapshotFreeSlotsAtRoomEntry(unsigned long long base, unsigned long long loadedPtrTableRva) {
    if (loadedPtrTableRva == 0) return;
    int freeCount = 0;
    for (int s = 0; s < SPECIES_SLOT_COUNT; ++s) {
        bool isFree = false;
        if (s <= SPECIES_SLOT_ALLOC_MAX) {
            volatile uint8_t* ownerAddr = (volatile uint8_t*)(uintptr_t)(base + loadedPtrTableRva +
                LOADED_SPECIES_OWNER_OFFSET_FROM_PTR + (size_t)s * LOADED_SPECIES_STRIDE);
            isFree = (*ownerAddr == SPECIES_SLOT_UNCLAIMED_OWNER);
        }
        g_freeAtRoomEntry[s] = isFree;
        if (isFree && s >= SPECIES_SLOT_ALLOC_MIN) ++freeCount;
    }
    g_roomEntrySnapshotValid = true;
    char msg[144];
    snprintf(msg, sizeof(msg),
        "spawn_enemy: room-entry snapshot -- %d of %d slots in %d..%d were free and are allocatable",
        freeCount, SPECIES_SLOT_ALLOC_MAX - SPECIES_SLOT_ALLOC_MIN + 1,
        SPECIES_SLOT_ALLOC_MIN, SPECIES_SLOT_ALLOC_MAX);
    LogDebug(msg);
}

// owner==0xFF is NOT sufficient to call a slot free: a loaded species can carry no owner stamp at
// all (live-confirmed). Release wipes the blob but leaves state and the cached name, so a VALID
// blob header is what separates "still live" from "released". Live-confirmed: slot 61 read
// owner=0xFF/state=6/xa_ex_2110 with real data, was treated as free, and got loaded over.
static const int32_t BLOB_HEADER_FIRST_SECTION = 128;   // blob+4 on every valid header observed

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
    int32_t sec4 = 0;
    __try {
        memcpy(&sec4, (const void*)(uintptr_t)(base + speciesResourceTableRva +
               (unsigned long long)slot * RESOURCE_BLOB_SIZE + 4), sizeof(sec4));
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return sec4 == BLOB_HEADER_FIRST_SECTION;
}

// Marks every slot a live species occupies, SPAN included. Run members of an unowned run carry
// owner=0xFF, state=0 and no cached name -- indistinguishable from free when judged one slot at a
// time. Live-confirmed: species 55 held 55..59 unowned, slots 56..59 looked free, and allocating
// species 56 there loaded straight over it.
// The room's placement table is an AUTHORED list of every creature this room can place, each record
// carrying its species index (+0x55) and run length (+0x56). That is the room's own reserved set.
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
// through ComputeSlotAvailability/RunIsAvailable.
//
// DO NOT add owner / state / cached-model-name tests anywhere else. The engine never clears any of
// those when a room stops using a slot -- it reclaims by EVICTION, overwriting only what the next
// room needs -- so they record history, not occupancy. Two separate checks built on them had to be
// removed on 2026-08-13 after each independently rejected slots this map correctly frees:
// the g_freeAtRoomEntry gate, and a post-selection "slot is CLAIMED by a different creature" test.
// ============================================================================================
static const char* const WHY_ROOM_ROSTER = "in this room's placement roster";
static const char* const WHY_LIVE_ENTITY = "a live entity references it";
static const char* const WHY_OUR_LOAD    = "we triggered a load into it this session";
static const char* const WHY_RUN_SPAN    = "inside a live species' run span";

struct SlotAvailability {
    bool occupied[SPECIES_SLOT_COUNT];
    const char* why[SPECIES_SLOT_COUNT];   // why a slot is unavailable, for diagnostics
};

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
        if (IsSpeciesTriggeredThisSession((uint8_t)s))    take(s, WHY_OUR_LOAD);
    }

    // Expand anything taken to its whole run -- a creature's blob spans runLen slots, so loading
    // over any one of them corrupts it.
    //
    // TRUNCATE the span at the next run start. A slot with a valid blob header IS a start, so
    // another creature's blob physically cannot span it (that slot's base would be mid-blob and
    // read arbitrary bytes, not 128). A runLen reaching past one is therefore stale. Without this,
    // a leftover run whose head happens to overlap the roster dragged slots beyond the next start
    // with it -- live: slot 42 (runLen 5) overlapped roster entry 39-43 and held 44-46 hostage,
    // reported as "3 run-span" while the viewer correctly showed them reclaimable.
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

// Picks a free run that also avoids the room's own records, walking past rejected candidates.
// Returning only the first free run deadlocked spawning whenever that one run collided.
// A text slot caches a raw pointer into the creature blob its text belongs to, so overwriting that
// blob makes the next text layout read garbage. Live-confirmed: slot 13 pointed into species 53's
// blob, our next load claimed 52..56 over it, and fnc_text_slot_layout_prepare crashed on approach
// to a door. owner==0xFF is necessary but NOT sufficient -- the engine releases a run while the text
// system still references it.
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

// Tracked spawns: entity pointer + id + species/run, recorded at post-construct.
static unsigned long long g_slotMapBase = 0;
static unsigned long long g_slotMapLoadedPtrTableRva = 0;

// Defined further down with the entity-snapshot helpers; needed here for the liveness check.
static bool TryReadU32(uint64_t addr, uint32_t* out);

struct SpawnedEntityRecord {
    unsigned long long entityPtr;
    uint32_t id;
    uint8_t species;
    uint8_t runLen;
    // Consecutive dead observations before acting, so a transient tick gap is ignored.
    uint8_t deadObservations;
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
    rec.deadObservations = 0;
    strncpy_s(rec.modelPath, modelPath, _TRUNCATE);
    ++g_trackedSpawnCount;
}






// Live address of the 64-entry bucket table, stashed for the headroom check.
static volatile uint64_t g_resourceHandleBucketTableAddr = 0;

// Most recent spawn, re-read after construction to confirm it survived.
static uint64_t g_lastSpawnedEntityPtr = 0;
static uint32_t g_lastSpawnedEntityId = 0;

static bool TryReadU32(uint64_t addr, uint32_t* out) {
    __try {
        *out = *(volatile uint32_t*)(uintptr_t)addr;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Resolve-function address, stashed for diagnostics.
static uint64_t g_resolveHandleFnAddrForDiag = 0;



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




// Every refusal returns a short machine-readable code plus a message, so Lua can react to the
// reason rather than parse prose.
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
    unsigned long long entityIterFnRva = (unsigned long long)p_lua_tointegerx(L, 17, nullptr);
    long long luaCharId = (long long)p_lua_tointegerx(L, 18, nullptr);
    long long luaWeight = (long long)p_lua_tointegerx(L, 19, nullptr);
    size_t luaTemplateLen = 0;
    const char* luaTemplateRecord = p_lua_tolstring(L, 20, &luaTemplateLen);
    unsigned long long entityPoolBaseRva = (unsigned long long)p_lua_tointegerx(L, 21, nullptr);
    unsigned long long soraPointerRva = (unsigned long long)p_lua_tointegerx(L, 22, nullptr);
    unsigned long long soraObjPtrRva = (unsigned long long)p_lua_tointegerx(L, 23, nullptr);
    unsigned long long partyMember1PtrRva = (unsigned long long)p_lua_tointegerx(L, 24, nullptr);
    unsigned long long partyMember2PtrRva = (unsigned long long)p_lua_tointegerx(L, 25, nullptr);
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
    // Lua-supplied RVAs -- can read the slot table for the eviction test. Read-only use.
    g_slotMapBase = base;
    g_slotMapLoadedPtrTableRva = loadedPtrTableRva;
    // Diagnostics-only; lets LogEntitySnapshot walk entity+0x134's shape descriptor.
    g_resolveHandleFnAddrForDiag = base + resolveHandleFnRva;

    // Re-read the previous spawn: an unreadable or mismatched id means its pool slot was recycled.

    // Refuse during the post-boss slow-motion: g_GameSpeed is 1.0 normally, 0.1 during the effect.
    // Fails open -- an unreadable or normal-looking value never blocks a spawn.
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

        // Log the first reading (the baseline) and every abnormal one -- not every call.
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
            // Nothing is allocated or published yet, so returning here is a true no-op.
            return RefuseSpawn(L, "boss_defeat_slowdown",
                "spawn_enemy: the game is in a boss-defeat/slow-motion sequence right now -- refusing "
                "so the spawn does not land while the engine is tearing down battle resources; retry shortly");
        }
    }

    // On a room change only clear our triggered-load bookkeeping -- the engine re-tiles slots, so a
    // stale entry would let the reuse path build against a slot it has reassigned. It frees the runs.
    if (worldNumRva != 0 && areaNumRva != 0 && setNumRva != 0) {
        int32_t curWorld = *(int32_t*)(uintptr_t)(base + worldNumRva);
        int32_t curArea = *(int32_t*)(uintptr_t)(base + areaNumRva);
        int32_t curSet = *(int32_t*)(uintptr_t)(base + setNumRva);
        if (g_lastSpawnRoomValid &&
            (curWorld != g_lastSpawnRoomWorld || curArea != g_lastSpawnRoomArea || curSet != g_lastSpawnRoomSet)) {
            // Release BEFORE clearing the bookkeeping -- it is what identifies our runs.
            ReleaseOurTriggeredRuns(base, loadedPtrTableRva, releaseSlotRunFnRva,
                                    entityIterFnRva, resolveHandleFnRva, speciesResourceTableRva);
            g_triggeredLoadCount = 0;
            LogDebug("spawn_enemy: room changed -- triggered-load bookkeeping cleared");
            SnapshotFreeSlotsAtRoomEntry(base, loadedPtrTableRva);
            // A creature from the previous room cannot still be alive; stop re-reading its pointer.
            g_lastSpawnedEntityPtr = 0;
            g_lastSpawnedEntityId = 0;
        }
        if (!g_roomEntrySnapshotValid) SnapshotFreeSlotsAtRoomEntry(base, loadedPtrTableRva);
        g_lastSpawnRoomWorld = curWorld;
        g_lastSpawnRoomArea = curArea;
        g_lastSpawnRoomSet = curSet;
        g_lastSpawnRoomValid = true;

        // Matches world+area only -- Set is the evdl variant suffix, not a distinct room.
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

    // Refuse once the 64-entry resource-handle bucket table is nearly full. Read-only count of
    // non-(-1) entries; only stops spawn_enemy's own churn exhausting it, not other gameplay.
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

    // Refuse if another enemy already occupies the spawn point; Sora and party are excluded.
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

    // kh1_creature_data.lua is the only source of char-id/weight/template.
    if (luaCharId == 0 || !luaTemplateRecord || luaTemplateLen != PLACEMENT_RECORD_SIZE) {
        return RefuseSpawn(L, "no_creature_data",
            "spawn_enemy: no offline fallback data for this creature (missing from kh1_creature_data.lua)");
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
    // A failed reuse gate falls through to a fresh load rather than refusing.
    if (FindTriggeredLoad(modelPath, &species) &&
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
        // the number always agrees with why the search failed (an owner-based count here previously
        // reported figures that matched neither).
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


        // Carry the numbers in the CODE, not the message: the reason code is what reliably survives
        // the native/Lua boundary (see the note above SPAWN_FAILURE_MESSAGES).
        char code[48];
        snprintf(code, sizeof(code), "slots_full/%d/%d", templateSlotRunLen, LongestFreeRun(av));
        return RefuseSpawn(L, code,
            "spawn_enemy: no creature slots free right now -- each creature type claims several. They come back as spawned enemies are defeated, or when you change rooms.");
    }
    // NO post-selection "slot is CLAIMED by a different creature" check here.
    // It refused on owner!=0xFF && state!=0 && cached-name mismatch -- which is exactly what a STALE
    // block looks like, since the engine never clears ownership, state or the cached name when a room
    // stops using a slot. That is the very thing roster-based occupancy is meant to hand back, so the
    // check rejected almost every slot the allocator legitimately chose (live: "slot 38 is CLAIMED
    // (owner=38) ... doesn't match xa_ex_2021.mdls"). It is also redundant: anything reaching here is
    // already outside this room's roster, unreferenced by live entities, and not triggered by us.

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
            // Re-verify immediately before the destructive load, through the SAME availability map
            // the search used. Built from oldTable so it is the room's OWN roster, excluding the
            // record we just spliced for ourselves. (An owner-based test here rejected exactly the
            // reclaimable stale slots the map hands out.)
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

        // Claim the run as a proper block BEFORE loading, the way the engine does. Its room-unload
        // sweep only releases blocks whose owner == their own index; skipping this leaves the run
        // unowned, so it is never reclaimed and other blocks tile straight over it.
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

    // Snapshot the new entity immediately so a success-but-nothing-appeared report can be pinned.
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
