-- Current Versions are 1.0.0.2 (Steam) and 1.0.0.10 Epic Games Store
-- shared
animSpeed = 0x233FBCC
beepHack = 0x26E20C
blackFade = 0x4DC718
buttonTypes = 0x2E20548
closeMenu = 0x2E941C0
cutscene = 0x233E808
cutsceneFlagBase = 0x2DEA865
damageCapBranch = 0x2BFC58
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
saveData1 = 0x2DE9F60
saveData2 = 0x2DEAF60
savedFruits = 0x2DEB09E
sharedAbilities = 0x2DE98F9
shopTableBase = 0x4FEE04
shopTableGummiBase = 0x4FEAB0
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
superglideSpeedTable = 0x42F964
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
fnc_spawn_prize_call_enemy_drops = 0x2ABE6F
fnc_spawn_enemy_drops = 0x2ABC40
fnc_spawn_enemy_drops_jump_bd_7a = 0x2BC67D
fnc_spawn_enemy_drops_jump_bd_d7 = 0x2BC6E5
g_prize_pool = 0x2DE8E20
g_prize_serial = 0x2DE8E10
fnc_update_widget_queue = 0x2AB8E0
fnc_spawn_world_gimmick_entity = 0x290D60
placementTablePtr = 0x296B630
placementTableCount = 0x296B628
g_WorldNumber = 0x233FE84
g_AreaNumber = 0x233FE8C
g_SetNumber = 0x233FE90
fnc_iterate_live_entities = 0x291B20
fnc_load_gimmick_assets = 0x285EE0
fnc_async_load_job_callback = 0x286420
fnc_alloc_async_job_slot = 0x28A900
gimmick_job_pool = 0x2868BB0
fnc_species_model_file_loaded = 0x286290
fnc_reverse_blob_to_slot_ptr = 0x285DB0
fnc_reverse_blob_to_slot_ptr_call_1 = 0x2879DA
fnc_reverse_blob_to_slot_ptr_call_2 = 0x2886AE
species_owner_table = 0x2869DD0
fnc_release_species_slot_run = 0x2858E0
loadedSpeciesPtrTable = 0x2869E18
fnc_mint_resource_handle = 0x38AD90
fnc_resolve_resource_handle = 0x38ADC0
fnc_queue_file_load_to_address = 0x28BC10
fnc_blob_reloc = 0x1D7C30
fnc_blob_fixup_a = 0x1E0080
fnc_blob_fixup_b = 0x2B6E00
fnc_motion_fixup = 0x286110
speciesResourceTable = 0xD2ADA0
resource_handle_bucket_table = 0x2EE3980
g_GameSpeed = 0x233FBCC
g_BossDefeatEffect = 0x233FBE8
g_BossDefeatEffectStart = 0x233FDFC
text_slot_table_base = 0x2678280
fnc_claim_species_slot_run = 0x286190
entityPoolBase = 0x2D372A0
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
fnc_item_popup_name_call = 0x27357D
fnc_resolve_item_name_ptr = 0x28F9B0
fnc_000_open_window = 0x1B93F0
fnc_001_display_message = 0x1B8DD0
fnc_002_close_window = 0x1B8100
fnc_0B1_open_window_no_close = 0x1B9170
g_EVSyscallTable = 0x4FA870
fnc_end_event_skip = 0x1A2130
fnc_005_set_window_type = 0x1B98F0
fnc_003_set_window_position = 0x1B9770
fnc_004_set_window_size = 0x1B97D0
fnc_16D_set_window_width_auto = 0x1B7C80
fnc_050_set_window_tail_type = 0x1B98B0
fnc_116_game_over = 0x1ACC00
fnc_trigger_ko_event_script = 0x1C43D0
fnc_display_message_text_hook = 0x1B8E6C
fnc_display_message_text_resume = 0x1B8E74
g_pEVStringDataPtr = 0x237C4E0
g_EVScriptLoaded = 0x237F4E0
g_EVWindowState = 0x237F4F4
g_EVWindowMsgCount = 0x2380500
g_EVWindowMsgQueue = 0x237F500
g_EVWindowMsgPosition = 0x23D41F0
g_EVWindowActivity = 0x237F4F0
g_EVWindowStateArray = 0x2380520
g_EVWindowTemplates = 0x23D4560
g_EVScreenBoxes = 0x23D4220
g_EVCutsceneCheck = 0x23404EC
g_EVWindowWidthPad = 0x22EC1C4
fnc_display_message_anim_hook = 0x1B9133
fnc_display_message_anim_resume = 0x1B913C
fnc_display_message_anim_call_target = 0x1764E0
fnc_play_se2 = 0x1787D0
fnc_apply_allset_row = 0x17CC70
g_SummonStepTable = 0x528230
fnc_summon_camera_swing_call = 0x289D2F
fnc_summon_fade_out_call = 0x289D1E
fnc_summon_fade_in_call = 0x289869
fnc_config_menu_update = 0x2FA210
fnc_config_help_update = 0x2FA1A0
g_ConfigMenuDescriptor = 0x5463A0
g_ConfigHelpDescriptor = 0x546160
g_ConfigHelpMsgTable = 0x5463D0
g_ConfigValueColors = 0x546440
g_ConfigMenuRow = 0x2E98F18
g_MenuPadState = 0x2E95940
g_MenuRowHeight = 0x545360
g_MenuRowGap = 0x545368
fnc_menu_window_is_active = 0x2E5220
fnc_menu_draw_text = 0x2D7910
fnc_menu_draw_selection_bar = 0x2F2B80
fnc_menu_play_sound = 0x17A040
g_ConfigLabelMsgTable = 0x546410
g_ConfigDifficultyMsgTable = 0x546430
g_MenuSpriteInfo = 0x2E91610
fnc_menu_render_sprite = 0x2EEEE0
fnc_menu_create_window = 0x2E5A40
fnc_menu_last_child = 0x2E5510
fnc_menu_close_window = 0x2E3D10
fnc_menu_set_child = 0x2E2DE0
fnc_menu_set_parent = 0x2E2E40
fnc_menu_bring_to_front = 0x2E5E40
fnc_menu_list_cursor_update = 0x2EF670
fnc_menu_list_cursor_row = 0x2EF470
fnc_menu_move_cursor_sprite = 0x2D9920
fnc_menu_draw_string = 0x2EBCD0
g_MenuTextStyles = 0x52A8D0
g_MenuTextSize = 0x545364
g_MenuConfirmMask = 0x5456A0
g_MenuCancelMask = 0x5456A4
g_HelpTextY = 0x2E957A8
g_HelpTextSizeObject = 0x2E957A0
fnc_pad_write_buttons = 0x28B430
fnc_pad_write_buttons_call = 0x28B19A
fnc_main_menu_update = 0x2D3840
g_MainMenuDescriptor = 0x52AF20
g_MainMenuItems = 0x2E1F4A8
g_MainMenuItemCount = 0x2E1F4C8
g_MainMenuItemDescriptors = 0x52AD60
g_MainMenuItemOpeners = 0x52ADA0
g_ConfigHeaderDescriptor = 0x546190
fnc_menu_draw_pill_text = 0x2F3950
fnc_main_menu_help_update = 0x2D37D0
g_MainMenuHelpDescriptor = 0x52AD10
g_MainMenuItemHelpMsgTable = 0x52AFB8
g_MainMenuCursorRow = 0x2E1F4CC
g_MenuWindowPool = 0x2E92780
g_pTextListHead = 0x2B9C280
g_apResourceHandleBuckets = 0x2EE3980
g_nAppMode = 0x233FBC0
g_nTitleState = 0x2EE8BB8
g_nMenuActive = 0x2E1F470
g_nTitleIdleTimer = 0x2EE8BA4
g_dwScreenClearColor = 0x233FDC0
fnc_add_menu_job = 0x284670
fnc_menu_reset_windows = 0x2E5870
fnc_menu_set_active = 0x2D52C0
fnc_menu_update_windows = 0x2E3D90
fnc_menu_open_window_count = 0x2E52B0
fnc_menu_pad_replay = 0x2F6740
fnc_menu_cursor_init_colors = 0x2D93D0
fnc_menu_cursor_reset = 0x2D9FB0
fnc_menu_cursor_set_position = 0x2D9A60
fnc_menu_cursor_fade_in = 0x2D9EE0
fnc_menu_cursor_hide = 0x2D9F70
fnc_menu_cursor_draw = 0x2D9B40
