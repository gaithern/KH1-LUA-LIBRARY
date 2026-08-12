-- Current Versions are 1.0.0.2 (Steam) and 1.0.0.10 Epic Games Store
-- shared
animSpeed = 0x233FBCC
beepHack = 0x26E20C
blackFade = 0x4DC718
buttonTypes = 0x2E20548
closeMenu = 0x2E941C0
cutscene = 0x233E808
cutsceneFlagBase = 0x2DEA865
deathPointer = 0x2382568
dest = 0x507580
dialog = 0x299BB08
fadeBase = 0x233FDD0
gummiSelect = 0x50707C
gummiStart = 0x506F90
hookship = 0xED6A3E
inCutscene = 0x23AB2D0
inGummi = 0x5075A8 -- differs to cover a change on linux systems
inputAddress = 0x23407B4
maxHP = 0x2DE9366
menu = 0x232DFA0
menuButtonCount = 0x2E2055C
menuFunction = 0x2E204F8
menuItemSlotCount = 0x2E92DF2
menuMaxButtonCount = 0x2E92DF0
minitimer = 0x232E000
reports = 0x2DEAD20
room = 0x233FE8C
saveOpenAddress = 0x232DFA4
skippable = 0x2382594
soraHP = 0x2D5CC4C
soraHUD = 0x281249C
fnc_apply_actor_status_effect = 0x2AE160
stateFlag = 0x2867364
summoning = 0x2D60FAC
textProg = 0x232DF74
warpTrigger = 0x22EC0AC
warpType1 = 0x233FBC0
warpType2 = 0x22EC0B0
white = 0x233FE1C
world = 0x233FE94

-- rando shared
ardOff = 0x2382C60
khamaActive = 0x2D380B0
OCseed = 0x23BBBF0
roomWarp = 0x233FEBC
soraPointer = 0x2537E48
soraStats = 0x2DE9396
theonActive = 0x2D39820
worldFlagBase = 0x2DEB4B0
worldWarp = 0x233FEB8

-- 4:3
height = 0x3B3514

-- achievements
ach = 0x21AAE28

-- auto attack
attackCommand = 0x52890C
attInp = 0x23407B5
fireState1 = 0x23D3F80
fireState2 = 0x232DDC4
swapped = 0x4D8632

-- rando chaos
anims = 0x2D2D730
attackElement = 0x2D278B8
commandMenuPointer = 0x2D36D50
donaldPointer = 0x2D37288
goofyPointer = 0x2D37290
moveSpeed = 0x2D5CB18
musicBaseSpeed = 0
musicPointer = 0x232DF20
musicSpeedHack = 0xA7A5B
soraResist = 0x2D5CB88
weaponSize = 0xD2E4C0

-- consistent finishers
gravBreak = 0x3EC5A8
zantHack = 0x2A6FD8
zantValue = 0

-- early skip
skipArray1 = 0x17A7DC
skipArray2 = 0x17A7FC
skipArray3 = 0x17F663
skipFlag1 = 0x233FEE8
skipFlag2 = 0x23404D0

-- rando enemy
bossAdjustAddresses = {
	0x6DBB74, 0x70D804, 0x2340A94, 0x2D372A0, 0x2D37750,
	0x2D37C00, 0x2D38574, 0x2D38EC0, 0x2D38ED4, 0x2D3A630,
	0x2D3AAE0, 0x2D3AF90, 0x2D3B440, 0x2D3C700, 0x2D3D060,
	0x2D3D9C0, 0x2D424C0, 0x2D461B0, 0x2D5CD4C, 0x2D5CF4C,
	0x2D5D04C, 0x2D5D44C
}
enemyAddresses = {
	0x7E48C0, 0x838FC0, 0x8556C0, 0x8A8640, 0x8A8680,
	0x8A86C0, 0x8CA900, 0x8D31C0, 0x8D4900, 0x8E4640,
	0x918340, 0x91E3C0, 0x926580, 0x951500, 0x953E40,
	0x957140, 0x95D240, 0x95D440, 0x95FAC0, 0x9605C0,
	0x960680, 0x970640, 0x976440, 0x97F3C0, 0x97F400,
	0x9838C0, 0x98C180, 0x98C1C0, 0x98C200, 0x996700,
	0x996740, 0x996780, 0x997300, 0x99FBC0, 0x9A8320,
	0x9A8A00, 0x9A8A80, 0x9ABC40, 0x9B0440, 0x9B6AC0,
	0x9BC2C0, 0x9C4100, 0x9C7BC0, 0x9C7C40, 0x9C7C80,
	0x9C7CC0, 0x9CE040, 0x9CE380, 0x9CEF00, 0x9D2B00,
	0x9D2C40, 0x9D4D80, 0x9D50E0, 0x9D63C0, 0x9DB0C0,
	0x9E1EA0, 0x9E3980, 0x9E3A40, 0x9E7020, 0x9F6BC0,
	0x9F7E00, 0x9FB900, 0x9FDF00, 0xA057C0, 0xA0A840,
	0xA23C00, 0xA23C80, 0xA25D00, 0xA2E2C0, 0xA4BF40,
	0xA57D40, 0xA663C0, 0xA85EC0, 0xA90780, 0xA908C0,
	0xA9B000, 0xA9B040, 0xAC4000, 0xAC4040, 0xAC4100,
	0xAC4140, 0xAC4180, 0xAD3D40, 0xAD3D80, 0xAD3DC0,
	0xAD3E00, 0xAD3E40, 0xAD3E80, 0xAD3EC0, 0xAD4000,
	0xAD40A0, 0xAE9480, 0xB0E3C0, 0xB0E400, 0xB0E4A0,
	0xB1A580, 0xB1AA40, 0xB1CDC0, 0xB5E2C0, 0xB5FA40,
	0xB5FD00, 0xBB43C0
}
bittestRender = 0x232DE00
combo = 0x2DE5E34
cutsceneFlags = 0x2DEA860
floorStatus = 0x528C7C
glideBarrier = 0x506DE8
inTournament = 0x237F4E0
wall = 0x6DBB94

-- fast camera
cameraCenter = 0x2537EEC
cameraInputH = 0x23407E0
cameraInputV = 0x23407E4
curSpeedH = 0x25380F0
curSpeedV = 0x25380EC
speed = 0x506CDC

-- faster dialogue
textSpeed = 0x233FBDC
textTrans = 0x22EC114

-- hook ship
debug1Value = 2
neverland = 0x2DEB257
posDebug1 = 0x232DD20
posDebug2 = 0x2537E40
posDebugString = 0x3EA388

-- instant gummi
monstro = 0x2DEB25A
normalDrive = 0x268986C
worldWarpBase = 0x50ABBA
worldWarpBase2 = 0x50AC2A

-- rando some logic
animHalfPointersAddress = 0x2869E18
ardoffsetClock = 0x2382FC0
battleLevel = 0x2DEAD24
characters = 0x2DEAA44
chestTable = 0x528D60
chestsOpened = 0x2DE9978
chronicles = 0x2DEACFB
collectedFruits = 0x232E004
currentTerminus = 0x2380A14
donaldAbilityTable = 0x2D26C60
donaldStatTable = 0x2D26BF8
emblemCount = 0x2DEB20D
emblemDoor = 0x2DEB21C
enableRC = 0x2DE9BD4
evidence = 0x2DEA168
evidenceActiveBizarre = 0x2D3CBB0
evidenceActiveForest = 0x2D3D510
experienceMult = 0x2D5CB00
goofyAbilityTable = 0x2D26DF8
goofyStatTable = 0x2D26D90
gummiFlagBase = 0x2DEB250
halfPointersAddress = 0x2EE3980
infoBoxNotVisible = 0x23D41F0
instantGummiFix = 0x2538058
inventory = 0x2DE97FA
itemDropID = 0x284D948
itemTable = 0x2D24798
jumpHeights = 0x2D22DEC
language = 0x2E1B52B
libraryFlag = 0x2DEB483
lockMenu = 0x232DF80
magicFlags = 0x2DEAF7E
magicLevels = 0x2DE97F2
menuCheck = 0x2E92838
mermaidKickSpeed = 0x3EFA4C
OCCupDialog = 0x2384760
OCCupUnlock = 0x2DEB160
oppositeState = 0x2DEB018
oppositeTrigger = 0x2DEA07D
party2 = 0x2E205A5
poohProgress = 0x2DEB0A8
poohProgress2 = 0x2DEA780
randoInitCheck = 0x7AC788
RCName = 0x2866D10
report1 = 0x1D06DA4
rewardTable = 0x2D2F3E8
roomWarpRead = 0x232DF18
savedFruits = 0x2DEB09E
sharedAbilities = 0x2DE98F9
shopTableBase = 0x4FEE04
shortcuts = 0x2DE9BA4
slideActive = 0x2D403F0
sliderProgress = 0x2DEB099
soraAbilityTable = 0x2D26938
soraAbilityTable2 = 0x2D26868
soraAbilityTable3 = 0x2D268D0
soraStatTable = 0x2D26800
summons = 0x2DE9B30
summonsReturned = 0x2DEA08C
superglideBaseSpeed = 0
superglideSpeedHack = 0x2B2A84
synthItems = 0x5478A0
synthRequirements = 0x5476C0
terminusTeleUsable = 0x2380954 -- On: 4378 Off: 4294957296
terminusTeleVisible = 0x2678448 -- On: 1166594048 Off: 3323740160
textBox = 0x23D4364
textPointerBase = 0x2B9C280
trinityUnlock = 0x2DEAF7B
unequipBlacklist = 0x545330
unlockedWarps = 0x2DEB25F
warpCount = 0x50AC8C
warpDefinitions = 0x232DF10
waterwayCutsceneFlag = 0x2DEA072
waterwayGate = 0x2DEAFCD
waterwayTrinity = 0x2DEB011
weaponTable = 0x2D2C268
worldMapLines = 0x2DEB272
worldWarps = 0x50ABA8

-- ASL shared
gummiInventory = 0x2DF51DC
magicUnlock = 0x2DE93D4
party1 = 0x2DE97EF
soraCurAbilities = 0x2DE93A4

-- save anywhere
cam = 0x506CD8
config = 0x2DFF760
continue = 0x2DFFF60
cutSceneAspect = 0x4DD562
deathCheck = 0x29C0B0
saveAnywhere = 0x2354854
title = 0x23404E8
titlescreenamvtimer = 0x2EE8BA4
titlescreenpicture = 0x2EE8BB8

-- unlock 0 volume
volumeZero = 0x3D8B64

-- write synthesis item names
synthItemSelected = 0x2E99F40

-- atlantica clams in battle and locking
evdlPointer = 0x234E780

-- bambi rewards
bambi = 0x1AFC824

-- summon anywhere
summonanywhere1 = 0x288F9A
summonanywhere2 = 0x288FAC
summonanywhere3 = 0x294819

-- air items
airitems1 = 0x2943DF
airitems2 = 0x2A376D

-- current hits
currentHits = 0x296B221

-- summon time
summonTime = 0x2892B1

-- interaction
chests_interaction = 0x2B5AA4
examine_interaction = 0x294B89
talk_interaction = 0x294BC9
trinity_interaction = 0x1A4EFF
syscall_battle_check = 0x1AD52E

-- show prompt
boxMemory = 0x283B390
textMemory = 0x2DC2668
colorBox = 0x527A10
colorText = 0x527A50

fnc_enqueue_levelup_prompt = 0x272540

-- giftTableScale fix
giftTableScale = 0x1B7E5C 

-- battleModeCheck
battleModeCheck = 0x1AD526

-- synthesisItemNames
UK_Word = 0x2E1AC60

fnc_spawn_prize = 0x2BFF00
fnc_update_widget_queue = 0x2AB8E0

fnc_spawn_world_gimmick_entity = 0x290D60
placementTablePtr = 0x296B630
placementTableCount = 0x296B628
g_WorldNumber = 0x233FE84
g_AreaNumber = 0x233FE8C
g_SetNumber = 0x233FE90
-- [HOOK TOGGLE: OFF] fnc_async_load_job_callback = 0x286420
-- [HOOK TOGGLE: OFF] fnc_velocity_blend_util = 0x2B5E50
fnc_iterate_live_entities = 0x291B20
-- [HOOK TOGGLE: OFF] fnc_party_ability_index_resolve_call = 0x1D62F2
fnc_load_gimmick_assets = 0x285EE0
-- The game's own release routine for a run of species slots: (species, runLen) -> sets each slot's
-- owner byte to the 0xFF unclaimed sentinel and wipes the matching resource-blob range. Used by
-- spawn_enemy to hand back slots on a room change so a session stops running out of them.
fnc_release_species_slot_run = 0x2858E0
loadedSpeciesPtrTable = 0x2869E18
fnc_mint_resource_handle = 0x38AD90
fnc_resolve_resource_handle = 0x38ADC0
speciesResourceTable = 0xD2ADA0
-- CALL-to-fnc_resolve_resource_handle inside the engine's own skeleton/bone
-- animation-blend code (see InstallSkeletonHandleGuardHook's comment in
-- dllmain.cpp) -- confirmed live via two real crash repros
-- (Darkball, Search Ghost). Steam-only for now: EGS's equivalent function
-- wasn't found (fuzzy-match against the EGS Ghidra project came back empty)
-- -- spawn_enemy degrades gracefully (no guard, same crash risk as before)
-- on any build where this isn't set, so leaving it unset on EGS is safe,
-- just unprotected.
-- [HOOK TOGGLE: OFF] fnc_skeleton_handle_deref_hook = 0x3900F7
-- Entry of the whole per-entity animation/skeleton-blend update function
-- (see InstallSkeletonBlendCallGuardHook's comment in dllmain.cpp) --
-- confirmed live via a THIRD crash repro (Bouncywild) at a
-- different instruction inside the same call tree, after the hook above
-- fixed the first one. Steam-only, same reasoning as the address above.
-- [HOOK TOGGLE: OFF] fnc_skeleton_blend_call_hook = 0x391C70
-- CALL-to-fnc_resolve_resource_handle inside the keyframe/blend-list
-- lookup helper (see InstallKeyframeListEntryGuardHook's comment in
-- dllmain.cpp) -- this was the FIRST crash site found this session,
-- confirmed live TWICE (small-garbage handle, then later a literal NULL
-- handle), in a different call tree from the two hooks above. Steam-only,
-- same reasoning as the addresses above.
-- [HOOK TOGGLE: OFF] fnc_keyframe_list_entry_hook = 0x3946BF
-- fnc_keyframe_list_lookup's own entry point ("MOV ECX,[RCX+0x3C]" + "MOV ESI,EDX", 5 bytes) --
-- see InstallKeyframeListEntryParamGuardHook's comment in dllmain.cpp. CRASH SITE #6, confirmed
-- live via WER minidump: fault reading param_1+0x3C when the caller
-- (fnc_entity_animation_blend_advance) passes an invalid record pointer (observed as literal -1).
-- This is upstream of (and NOT covered by) fnc_keyframe_list_entry_hook above. Steam-only for now,
-- same reasoning as the other animation-subsystem hook addresses in this file.
-- [HOOK TOGGLE: OFF] fnc_keyframe_list_lookup_param_hook = 0x3946A5
-- Diagnostic-only tap inside fnc_entity_animation_blend_advance (RVA 0x29FB30), right after it
-- computes the value that (when bad) becomes crash site #6's -1. Logs entity+0x1D0/+0x1D4/+0x168
-- to kh1_native.log whenever that computed value is -1, to capture the live field values behind
-- the still-unconfirmed root cause. See InstallAnimBlendAdvanceDiagHook's comment in dllmain.cpp.
-- Does not change behavior..
-- [HOOK TOGGLE: OFF] fnc_anim_blend_advance_diag_hook = 0x29FB80
-- Section-2 size check inside fnc_link_model_resource_data ("MOV EAX,[RBP+0xC]" / "SUB EAX,ECX" /
-- "TEST EAX,EAX" / "JLE", 9 bytes) -- REAL FIX for crash site #6's root cause, not another catch
-- hook. Corrects the game's own "section has data" check from "size > 0" to "size >= 0x34" (the
-- minimum needed to safely read the dword at section+0x30 that becomes entity+0x1D4). See
-- InstallSection2SizeGuardHook's comment in dllmain.cpp..
-- [HOOK TOGGLE: OFF] fnc_link_model_resource_data_section2_check = 0x288593
-- fnc_resolve_resource_handle_impl's bucket-index lookup ("SHR RAX,0x19" through "OR RAX,RCX",
-- 17 bytes) -- THE master fix, not another per-call-site guard. The shared 64-bucket
-- resource-handle table is never freed and can be exhausted by heavy Crowd Control spawn_enemy
-- churn; once exhausted, ANY caller of fnc_resolve_resource_handle anywhere in the game can get
-- back a raw -1 "pointer". This corrects the lookup itself so it returns 0 (the same "no data"
-- sentinel already handled everywhere) instead. See InstallResolveHandleBucketGuardHook's comment
-- in dllmain.cpp..
-- [HOOK TOGGLE: OFF] fnc_resolve_resource_handle_impl_bucket_check = 0x38AF5C
-- Base of the 64-entry resource-handle bucket table itself (g_apResourceHandleBuckets in Ghidra).
-- Used by InstallBucketMemoryWatcherThread (dllmain.cpp) -- a background thread, independent of
-- the hot-path fix above, that periodically VirtualQuery's each claimed bucket and proactively
-- resets any whose backing memory was freed back to the -1 "unclaimed" sentinel, so the hot-path
-- guard hook (which already treats -1 as "return 0") catches it with zero added per-call cost.
-- Closes the one gap the hot-path fix's own comment left open (a bucket pointing at real,
-- once-valid memory the OS later freed) -- a per-call VirtualQuery check was tried and reverted
-- the same day for freezing the game; this sidesteps that by checking rarely, off the hot thread.
resource_handle_bucket_table = 0x2EE3980
-- CALL instruction (memcpy import thunk) inside KH1_BehaviorScriptInterpreter's
-- class-0/sub-op-0xA opcode handler ("load block via resolved handle"). Resolves a
-- handle then feeds the raw result straight into memcpy as the source with zero
-- validation -- crash site #6, live-crashed after heavy spawn_enemy churn.
-- See InstallBehaviorScriptCopyGuardHook (dllmain.cpp) for the full writeup.
-- [HOOK TOGGLE: OFF] fnc_behavior_script_copy_guard_hook = 0x2CCFED
-- Entry of FUN_1402a0350 -- a joint/bone remap-table setup dispatcher, reached on a
-- creature's first motion transition, that either blends or (cold path) calls
-- FUN_140394080 (30+ unchecked fnc_resolve_resource_handle calls). Crash site #7,
-- live-crashed immediately after crash site #6 was fixed -- confirms the
-- bucket-exhaustion "return 0" fix surfaces as null derefs scattered across the
-- animation system, not just one site. Wrapped whole (not patched inline) since this
-- function has 5+ callers plus indirect table references -- see
-- InstallJointRemapSetupGuardHook (dllmain.cpp) for the full writeup.
-- [HOOK TOGGLE: OFF] fnc_joint_remap_setup_guard_hook = 0x2A0350
-- Entry of fnc_velocity_blend_neighbor_UNGUARDED -- walks every other live entity looking
-- for one to blend velocity against, chaining through fnc_resolve_resource_handle results
-- with FOUR unguarded dereferences in a single loop. Crash site #4: known since
--, live-crashed twice at the identical instruction (RVA 0x2B5BF0) six days
-- apart, and the top remaining crash site as of. Wrapped whole rather than
-- patched inline -- its own "no neighbor found" return (0, param_2 untouched) is a clean,
-- in-band failure result, unlike most sites here. See
-- InstallVelocityBlendNeighborGuardHook (dllmain.cpp) for the full writeup.
-- [HOOK TOGGLE: OFF] fnc_velocity_blend_neighbor_guard_hook = 0x2B5AF0
-- PATH B entry inside fnc_resource_entry_lookup_with_fallback (0x1DD650). Path B is the silent
-- fallback that returns a 16-byte slice of the HEADER OBJECT ITSELF as if it were a resource
-- table entry; callers then read its +4/+8 as resource handles, which is the documented source
-- of the deterministic float-shaped bogus handles behind crash sites #6/#7/#8/#10/#11.
-- DIAGNOSTIC ONLY -- the hook logs which of path B's three triggers fired (magic absent /
-- blob handle unresolvable / *blob == 0), the one thing never verified live, and does not alter
-- the path taken. Hooked at path B rather than the function entry because the function itself is
-- a hot lookup. See InstallResourceEntryFallbackGuardHook (dllmain.cpp).
-- Bytes at this address: 48 8D 47 01 (lea rax,[rdi+1]) 48 C1 E0 04 (shl rax,4).
-- [HOOK TOGGLE: OFF] fnc_resource_entry_fallback_hook = 0x1DD6AB
-- ENTRY of the same function. Counts calls, so "path B fired 0 times" can be distinguished from
-- "this function never ran" -- the first path-B session logged zero hits and it was not possible
-- to tell which. Bytes here: 48 89 5C 24 08 (mov [rsp+8], rbx), exactly 5, so the JMP replaces
-- one whole instruction. DIAGNOSTIC ONLY.
-- [HOOK TOGGLE: OFF] fnc_resource_entry_lookup_counter_hook = 0x1DD650
-- Boss-defeat / slow-motion engine state. Used by spawn_enemy to refuse while the
-- game is in the post-boss slowdown: the 20:56 crash was a spawn that completed cleanly and then
-- had its species blob wiped by the engine's battle-end teardown ~5s later, killing the game in
-- fnc_text_slot_layout_prepare. Unlike every earlier sighting of that wipe, there was NO room
-- transition -- battle-end fires the same release path.
-- RVAs from the community "Slowmode" script (KSX), which drives the effect by writing these.
-- Confirmed in Ghidra that the game itself writes them too, so they are real engine state:
-- g_GameSpeed is a genuine timescale float (1.0 normal, 0.1 during the effect) read by
-- fnc_get_number_of_seconds_played among others, and g_BossDefeatEffectStart is written by
-- fnc_01F_blur_on / fnc_0B8_rotate_blur (EVDL effect opcodes).
-- ONLY g_GameSpeed gates the refusal; the other two are logged for diagnosis until a real normal
-- value has been observed for them. See the gate in l_spawn_enemy (dllmain.cpp).
-- fnc_entity_teardown -- the engine's own entity erase (reached normally via
-- fnc_entity_erase_by_id at 0x292370). spawn_enemy calls this DIRECTLY, by entity pointer, to
-- retire its own already-dead spawns before the engine can wipe the species blob underneath them.
-- Passing the pointer rather than the id is deliberate: the by-id front door resolves whatever
-- entity currently holds that id, and ids are reused after a room reload, so a native could
-- inherit a dead spawn's id.
-- *** KILL SWITCH: comment this line out to disable the eager teardown with no rebuild. ***
-- *** STAGE 2 DISABLED AGAIN after a live regression. ***
-- Stage 2 ran and the evacuation fired 3 times; the first two were harmless, but the third tore
-- down a creature that was only 3 SECONDS OLD and actively being fought, and a crash in
-- fnc_text_slot_layout_prepare (crash site #11 -- the TEXT/kill-popup path) followed immediately.
-- The user reported "random crash just killing a heartless - that's new".
-- Two problems to solve before re-arming:
--   1. A reentrancy bug in the hook (fixed in the DLL, but unproven): the engine's release routine
--      calls ITSELF with only one argument, so the inner call's runLen is garbage.
--   2. The evacuation is too aggressive -- it kills healthy creatures the player is fighting, and
--      doing that during death/EXP processing is a plausible new crash trigger in its own right.
-- *** RE-ENABLED for despawn_spawned_enemies -- a DIFFERENT consumer. ***
-- The regression that disabled this was the release PRE-HOOK's evacuation, which fired on any slot
-- release and so destroyed creatures mid-combat. That hook is currently toggled OFF
-- (fnc_release_species_slot_run_prehook, below), so this address no longer arms it.
-- What it arms now is despawn_spawned_enemies: teardown ONLY as a transition begins (cutscene-flag
-- rising edge or an actual room change), which is when the creature was going to be destroyed
-- anyway. That is the engine's own erase path -- the same one fnc_1AA_erase_all_enemies uses --
-- standing in for the EVDL script that would have owned the creature.
-- *** DISABLED AGAIN -- made REDUNDANT, not reverted for a fault. ***
-- despawn_spawned_enemies triggered on a cutscene-flag edge or a world/area/set change, to remove
-- our creatures before a transition. Both halves of that turned out to be unnecessary:
--   * on a real AREA CHANGE the game already walks the room's spawn list and erases our creatures
--     (the area's own EVDL script calls the erase opcode), so there was nothing for us to do; and
--   * the gummi-ship warp is NOT an area change, so this never fired for the one case that
--     actually froze -- and that case is now prevented at source by the battle-state gate in
--     1fmRandoBoardGummi.lua (KH1-RANDOMIZER), which simply does not offer the gummi option while
--     the game considers you in battle. Restoring the condition vanilla relies on beats removing
--     entities behind the game's back.
-- Left commented rather than deleted: uncommenting re-arms despawn_spawned_enemies AND
-- RetireDeadTrackedSpawns, if a transition ever turns up that the engine does not clean up itself.
-- fnc_entity_teardown = 0x292390
-- ENTRY of fnc_release_species_slot_run -- same address as fnc_release_species_slot_run above, but
-- a separate entry so the pre-hook has its OWN kill switch (disabling the hook must not also
-- disable our reclamation's normal use of that function).
-- The hook evacuates any of OUR still-live creatures out of a slot run the ENGINE is about to
-- release, BEFORE the blob is wiped. Live-confirmed: the engine released a run
-- 5s after we spawned into it while the creature was still alive, and the crash followed.
-- Hooking the callee (not its ~30 callers) is deliberate -- one hook covers every release path.
-- *** KILL SWITCH: comment this line out to disable the pre-hook with no rebuild. ***
-- [HOOK TOGGLE: OFF] fnc_release_species_slot_run_prehook = 0x2858E0
g_GameSpeed = 0x233FBCC
g_BossDefeatEffect = 0x233FBE8
g_BossDefeatEffectStart = 0x233FDFC
-- Status-effect floating-text stale-pointer fix -- properly re-derives the value
-- instead of guarding a bad read. Three coordinated hook points inside
-- fnc_status_effect_activate_by_type / FUN_1401eba70, plus the shared 256-slot text table's own
-- base address (used to index our side table by slot). See
-- InstallTextSlotHandleCaptureHook/InstallTextSlotHandleRecordHook/InstallTextSlotFreshResolveHook
-- in dllmain.cpp for the full writeup.
-- [HOOK TOGGLE: OFF] fnc_status_effect_handle_capture_hook = 0x1E014D
-- [HOOK TOGGLE: OFF] fnc_status_effect_handle_record_hook = 0x1E0198
-- [HOOK TOGGLE: OFF] fnc_text_slot_fresh_resolve_hook = 0x1EBA78
text_slot_table_base = 0x2678280
-- Base of the shared 96-slot global entity pool (stride 0x4B0/1200 bytes) --
-- Sora, party members, doors/chests/markers, and every
-- fnc_spawn_world_gimmick_entity-constructed creature live in it. Confirmed
-- live: soraPointer's resolved struct landed exactly on this
-- pool's slot-4 boundary. Used by AnyEntityTooCloseToSora in dllmain.cpp to
-- refuse spawn_enemy when something is already sitting on top of Sora.
entityPoolBase = 0x2D372A0
-- Sora + up to 2 active party members' entity pointers (a contiguous
-- 3-pointer array, confirmed live -- g_SoraObjPtr's resolved
-- value matched soraPointer's exactly). Used by AnyEnemyTooCloseToSora to
-- exclude the whole party (not just Sora) from the "too close" check --
-- otherwise a party member following Sora around would always count as
-- an enemy nearby, since they share the same entity kind byte.
g_SoraObjPtr = 0x2D37280
g_PartyMember1ObjectPtr = 0x2D37288
g_PartyMember2ObjPtr = 0x2D37290

fnc_show_item_message = 0x273410
fnc_item_popup_text_hook = 0x27358C
fnc_item_popup_text_resume = 0x273594
fnc_item_popup_text_call_target = 0x27E430
fnc_item_popup_tick = 0x2739D0
fnc_item_popup_tick_resume = 0x2739D6
g_item_popup_state = 0x284D940
fnc_000_open_window = 0x1B93F0
fnc_001_display_message = 0x1B8DD0
fnc_002_close_window = 0x1B8100
fnc_0B1_open_window_no_close = 0x1B9170
fnc_005_set_window_type = 0x1B98F0
fnc_003_set_window_position = 0x1B9770
fnc_004_set_window_size = 0x1B97D0
fnc_116_game_over = 0x1ACC00
fnc_trigger_ko_event_script = 0x1C43D0
fnc_display_message_text_hook = 0x1B8E6C
fnc_display_message_text_resume = 0x1B8E74
g_pEVStringDataPtr = 0x237C4E0
fnc_display_message_anim_hook = 0x1B9133
fnc_display_message_anim_resume = 0x1B913C
fnc_display_message_anim_call_target = 0x1764E0
fnc_play_se2 = 0x1787D0