-- Named save-block addresses. Loaded by VersionCheck after the version file, so
-- saveData1 / saveData2 are already set for the running game version.
-- EVDL side: save_data[N] = saveData1 + N, save_data2[N] = saveData2 + N.
-- Keep names in sync with KH1-EVDL-TOOLS/save_data_labels.json.

-- save_data1
NAVI_GUMMI_OBTAINED             = saveData1 + 0x040 -- RANDO ONLY
EARTHSHINE_IN_BAG               = saveData1 + 0x041 -- RANDO ONLY
DI_DEFEAT_RIKU_DAY_1_REWARD     = saveData1 + 0x042 -- RANDO ONLY
DI_DEFEAT_TRIO_DAY_2_REWARD     = saveData1 + 0x043 -- RANDO ONLY
DI_GATHER_COCONUT_REWARD        = saveData1 + 0x044 -- RANDO ONLY
DI_KAIRI_POTION_DAY_1_REWARD    = saveData1 + 0x045 -- RANDO ONLY
DI_KAIRI_HI_POTION_DAY_1_REWARD = saveData1 + 0x046 -- RANDO ONLY
DI_RACE_RIKU_DAY_2_REWARD       = saveData1 + 0x047 -- RANDO ONLY
DI_KAIRI_POTION_DAY_2_REWARD    = saveData1 + 0x048 -- RANDO ONLY
DI_KAIRI_HI_POTION_DAY_2_REWARD = saveData1 + 0x049 -- RANDO ONLY
DAY_2_MATERIALS_REQUIRED        = saveData1 + 0x04A -- RANDO ONLY
HOMECOMING_MATERIALS_REQUIRED   = saveData1 + 0x04B -- RANDO ONLY
HOMECOMING_ARRIVAL_PENDING      = saveData1 + 0x04D -- RANDO ONLY, EV-to-EV handshake: set to 1 by the Destiny Islands Day 2 ending (di03) right before it warps to End of the World; ew36's arrival fades in from black and clears it. Lua must not write it.
CID_PREREQUISITE                = saveData1 + 0x11A
NATURESPARK_IN_BAG              = saveData1 + 0x128
WATERGLEAM_IN_BAG               = saveData1 + 0x129
FIREGLOW_IN_BAG                 = saveData1 + 0x12A
EARTHSHINE_RETURNED             = saveData1 + 0x12B
NATURESPARK_RETURNED            = saveData1 + 0x12C
WATERGLEAM_RETURNED             = saveData1 + 0x12D
FIREGLOW_RETURNED               = saveData1 + 0x12E
HYPERION_PREREQUISITE           = saveData1 + 0x12F
CID_BLUEPRINT_GIVEN             = saveData1 + 0x130
AERITH_BLUEPRINT_GIVEN          = saveData1 + 0x131
YUFFIE_BLUEPRINT_GIVEN          = saveData1 + 0x132
LEON_BLUEPRINT_GIVEN            = saveData1 + 0x133
CACTUAR_BLUEPRINT_GIVEN         = saveData1 + 0x134
CHOCOBO_BLUEPRINT_GIVEN         = saveData1 + 0x135
HYPERION_BLUEPRINT_GIVEN        = saveData1 + 0x136
GEPPETTO_VISITS                 = saveData1 + 0x137
CAN_PROGRESS_DI_DAY_2           = saveData1 + 0x400
CAN_PROGRESS_DI_DAY_1           = saveData1 + 0x412
FINISHED_RACE_WITH_RIKU         = saveData1 + 0x422
SORA_VS_RIKU_SORA_SCORE         = saveData1 + 0x436
SORA_VS_RIKU_RIKU_SCORE         = saveData1 + 0x438
KAIRI_GIVES_HINT                = saveData1 + 0x444
KAIRI_SAYS_YOURE_HOPELESS       = saveData1 + 0x445
SLIDE_1_TURNED_IN               = saveData1 + 0x607
SLIDE_2_TURNED_IN               = saveData1 + 0x608
SLIDE_3_TURNED_IN               = saveData1 + 0x609
SLIDE_4_TURNED_IN               = saveData1 + 0x60A
SLIDE_5_TURNED_IN               = saveData1 + 0x60B
SLIDE_6_TURNED_IN               = saveData1 + 0x60C
TRAVERSE_TOWN_PROGRESS          = saveData1 + 0x904
MONSTRO_PROGRESS                = saveData1 + 0x909

-- save_data2
DIALOG_STATE                    = saveData2 + 0x003
GIFT_TABLE_ITEM                 = saveData2 + 0x3C4
DI01_SET_NUMBER                 = saveData2 + 0x473 -- room set-number table entry; 0 = Day 1 map, 2 = Day 2
DI04_SET_NUMBER                 = saveData2 + 0x476 -- room set-number table entry
