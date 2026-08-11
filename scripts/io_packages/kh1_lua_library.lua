---@diagnostic disable: undefined-global
-- ################################################################################################### --
-- #  ____  __.___ ___  ____  .____                   .____    ._____.                               # --
-- # |    |/ _/   |   \/_   | |    |    __ _______    |    |   |__\_ |______________ _______ ___.__. # --
-- # |      </    ~    \|   | |    |   |  |  \__  \   |    |   |  || __ \_  __ \__  \\_  __ <   |  | # --
-- # |    |  \    Y    /|   | |    |___|  |  // __ \_ |    |___|  || \_\ \  | \// __ \|  | \/\___  | # --
-- # |____|__ \___|_  / |___| |_______ \____/(____  / |_______ \__||___  /__|  (____  /__|   / ____| # --
-- #         \/     \/                \/          \/          \/       \/           \/       \/      # --
-- ################################################################################################### --

--[[
    This is a package that provides helpful functions for working in KH1 memory.
    Tries to use io_packages from
    https://github.com/Denhonator/KHPCSpeedrunTools/tree/main/1FMMods/scripts/io_packages as much as
    possible, but some additional memory addresses may need to added.
--]]

-- kh1_native.dll (see native/KH1Native in this repo) is a native Lua module
-- that bridges into raw game function calls -- something the OpenKH Lua host
-- itself has no primitive for. Functions in the Advanced section that need to
-- call into game code (rather than just read/write memory) go through it.
local kh1_native = require("kh1_native")

-- Per-creature char-id/weight/template-record data extracted offline from
-- every room's own .ard file -- see kh1_creature_data.lua's own header and
-- the heartless field-spawn investigation memory ("Session 21") for how this
-- was derived and validated. Lets spawn_enemy's fallback path work for any
-- creature immediately, without ever needing to visit its native room live
-- first (the old auto-learn-on-native-visit mechanism in dllmain.cpp still
-- exists as a fallback for anything missing here).
local kh1_creature_data = require("kh1_creature_data")

-- ########### --
-- # Helpers # --
-- ########### --

local function GetKHSCII(INPUT)
    --[[Converts text to KHSCII, the game's proprietary
    text encoding.]]
    local _charTable = {
        [' '] =  0x01,
        ['\n'] =  0x02,
        ['-'] =  0x6E,
        ['!'] =  0x5F,
        ['?'] =  0x60,
        ['%'] =  0x62,
        ['/'] =  0x66,
        ['.'] =  0x68,
        [','] =  0x69,
        [';'] =  0x6C,
        [':'] =  0x6B,
        ['\''] =  0x71,
        ['('] =  0x74,
        [')'] =  0x75,
        ['['] =  0x76,
        [']'] =  0x77,
        ['¡'] =  0xCA,
        ['¿'] =  0xCB,
        ['À'] =  0xCC,
        ['Á'] =  0xCD,
        ['Â'] =  0xCE,
        ['Ä'] =  0xCF,
        ['Ç'] =  0xD0,
        ['È'] =  0xD1,
        ['É'] =  0xD2,
        ['Ê'] =  0xD3,
        ['Ë'] =  0xD4,
        ['Ì'] =  0xD5,
        ['Í'] =  0xD6,
        ['Î'] =  0xD7,
        ['Ï'] =  0xD8,
        ['Ñ'] =  0xD9,
        ['Ò'] =  0xDA,
        ['Ó'] =  0xDB,
        ['Ô'] =  0xDC,
        ['Ö'] =  0xDD,
        ['Ù'] =  0xDE,
        ['Ú'] =  0xDF,
        ['Û'] =  0xE0,
        ['Ü'] =  0xE1,
        ['ß'] =  0xE2,
        ['à'] =  0xE3,
        ['á'] =  0xE4,
        ['â'] =  0xE5,
        ['ä'] =  0xE6,
        ['ç'] =  0xE7,
        ['è'] =  0xE8,
        ['é'] =  0xE9,
        ['ê'] =  0xEA,
        ['ë'] =  0xEB,
        ['ì'] =  0xEC,
        ['í'] =  0xED,
        ['î'] =  0xEE,
        ['ï'] =  0xEF,
        ['ñ'] =  0xF0,
        ['ò'] =  0xF1,
        ['ó'] =  0xF2,
        ['ô'] =  0xF3,
        ['ö'] =  0xF4,
        ['ù'] =  0xF5,
        ['ú'] =  0xF6,
        ['û'] =  0xF7,
        ['ü'] =  0xF8
    }

    local _returnArray = {}

    local i = 1
    local z = 1

    while z <= #INPUT do
        local _literalHex = INPUT:match('^{0x(%x%x?)}', z)

        if _literalHex then
            _returnArray[i] = tonumber(_literalHex, 16)
            z = z + 4 + #_literalHex
            i = i + 1
            goto continue
        end

        local _char = INPUT:sub(z, z)

        if _char >= 'a' and _char <= 'z' then
            _returnArray[i] = string.byte(_char) - 0x1C
            z = z + 1
        elseif _char >= 'A' and _char <= 'Z' then
            _returnArray[i] = string.byte(_char) - 0x16
            z = z + 1
        elseif _char >= '0' and _char <= '9' then
            _returnArray[i] = string.byte(_char) - 0x0F
            z = z + 1
        else
            if _charTable[_char] ~= nil then
                _returnArray[i] = _charTable[_char]
                z = z + 1
            else
                _returnArray[i] = 0x01
                z = z + 1
            end
        end

        i = i + 1

        ::continue::
    end

    table.insert(_returnArray, 0x00)
    return _returnArray
end

local function byte_to_bits(byte)
    -- Converts 1 byte to array of 8 bits
    local bits = {}
    for i = 0, 7 do
        bits[i + 1] = (byte >> i) & 1  -- LSB first
    end
    return bits
end

local function bits_to_byte(bits)
    -- Converts array of 8 bits to byte
    assert(#bits == 8, "bits_to_byte expects exactly 8 bits")
    local byte = 0
    for i = 1, 8 do
        byte = byte | ((bits[i] & 1) << (i - 1)) -- LSB first
    end
    return byte
end

local function ReadBits(address, absolute)
    -- Returns array of bits in byte at memory address
    if absolute == nil then absolute = false end
    return byte_to_bits(ReadByte(address, absolute))
end

local function ReadBit(address, bit_num, absolute)
    -- Reads bit from a byte at memory address
    if absolute == nil then absolute = false end
    return byte_to_bits(ReadByte(address, absolute))[bit_num]
end

local function WriteBit(address, bit_num, value, absolute)
    -- Writes bit to a byte at memory address
    if absolute == nil then absolute = false end
    local bits = ReadBits(address, absolute)
    bits[bit_num] = (value ~= 0) and 1 or 0
    WriteByte(address, bits_to_byte(bits), absolute)
end

local function contains(tbl, val)
    -- Returns true if val exists in tbl
    for _, v in ipairs(tbl) do
        if v == val then
            return true
        end
    end
    return false
end

local function get_index(tbl, val)
    -- Gets the idx of val in passed tbl if present
    for k, v in ipairs(tbl) do
        if v == val then
            return k
        end
    end
    return nil
end

local function merge_tables(t1, t2)
    -- Merges two lua tables into one
    for _, value in ipairs(t2) do
        table.insert(t1, value)
    end
    return t1
end

-- ########### --
-- # Getters # --
-- ########### --
local function get_world()
    -- Get current world
    return ReadByte(world)
end

local function get_room()
    -- Get current room
    return ReadByte(room)
end

local function get_gummi_select()
    -- Get current gummi selection
    return ReadByte(gummiSelect)
end

local function get_animation_speed()
    -- Gets Sora's current animation speed
    return ReadFloat(GetPointer(soraHUD - 0xA94) + 0x284, true)
end

local function get_current_animation()
    -- Gets Sora's current animation
    return ReadByte(ReadLong(soraPointer)+0x164, true)
end

local function get_stats_page(entity)
    -- Gets an entity's/actor's refrenced stat page
    if entity == 0 then
        return 0
    end
    local handle = ReadLong(entity + 0x6C, true)
    if handle == 0 then
        return 0
    end
    local bucket_index = (handle & 0x7FFFFFFF) >> 25
    local offset = handle & 0x1FFFFFF
    local bucket_base = ReadLong(halfPointersAddress + bucket_index * 8)
    return bucket_base | offset
end

local function get_ground_combo_length_limit()
    -- Get currently set ground combo length limit
    local stats_page = get_stats_page(ReadLong(soraPointer))
    if stats_page == 0 then
        return 0
    end
    return ReadByte(stats_page + 0xD4, true)
end

local function get_air_combo_length_limit()
    -- Get currently set air combo length limit
    local stats_page = get_stats_page(ReadLong(soraPointer))
    if stats_page == 0 then
        return 0
    end
    return ReadByte(stats_page + 0xD5, true)
end

local function get_animation_time()
    -- Get how long Sora has been in an animation
    return ReadFloat(ReadLong(soraPointer)+0x16C, true)
end

local function get_stock()
    -- Gets full stock as an array
    return ReadArray(inventory, 255)
end

local function get_stock_at_index(index)
    -- Gets the stock qty at item idx
    return ReadByte(inventory + index - 1)
end

local function get_sora_abilities()
    -- Get all Sora abiltiies acquired
    local abilities = {}
    local i = 0
    while ReadByte(soraCurAbilities + i) ~= 0 and i <= 48 do
        local ability_value_bits = byte_to_bits(ReadByte(soraCurAbilities + i))
        ability_value_bits[8] = 0
        local ability_value = bits_to_byte(ability_value_bits)
        abilities[#abilities + 1] = ability_value
        i = i + 1
    end
    return abilities
end

local function get_shared_abilities()
    -- Get all shared abilities acquired
    local shared_abilities = {}
    local i = 0
    while ReadByte(sharedAbilities + i) ~= 0 and i <= 8 do
        local shared_ability_value_bits = byte_to_bits(ReadByte(sharedAbilities + i))
        shared_ability_value_bits[8] = 0
        local shared_ability_value = bits_to_byte(shared_ability_value_bits)
        shared_abilities[#shared_abilities + 1] = shared_ability_value
        i = i + 1
    end
    return shared_abilities
end

local function get_equipped_sora_abilities()
    -- Get the currently equipped Sora abilities
    local abilities = {}
    local i = 0
    while ReadByte(soraCurAbilities + i) ~= 0 and i <= 48 do
        local ability_value_bits = byte_to_bits(ReadByte(soraCurAbilities + i))
        if ability_value_bits[8] == 0 then
            ability_value_bits[8] = 0
            local ability_value = bits_to_byte(ability_value_bits)
            abilities[#abilities + 1] = ability_value
        end
        i = i + 1
    end
    return abilities
end

local function get_equipped_shared_abilities()
    -- Get the currently equipped shared abilities
    local shared_abilities = {}
    local i = 0
    while ReadByte(sharedAbilities + i) ~= 0 and i <= 48 do
        local shared_ability_value_bits = byte_to_bits(ReadByte(sharedAbilities + i))
        if shared_ability_value_bits[8] ~= 0 then
            shared_ability_value_bits[8] = 0
            local shared_ability_value = bits_to_byte(shared_ability_value_bits)
            shared_abilities[#shared_abilities + 1] = shared_ability_value
        end
        i = i + 1
    end
    return shared_abilities
end

local function get_soras_accessory_slots()
    -- Get Sora's max accessory slots
    return ReadByte(maxHP + 0x16)
end

local function get_soras_equipped_accessories()
    -- Get Sora's currently equipped accessories
    return ReadArray(maxHP + 0x17, get_soras_accessory_slots())
end

local function get_inputs()
    -- Get current inputs on input device
    return ReadArray(inputAddress, 4)
end

local function get_spell_effectiveness(spell)
    -- Returns Sora's current spell effectiveness
    local memory_location = nil
        if spell == "Fire"     then memory_location = jumpHeights - 0xAC + 0x5F70 + (0x70 * 0x00)
    elseif spell == "Fira"     then memory_location = jumpHeights - 0xAC + 0x5F70 + (0x70 * 0x01)
    elseif spell == "Firaga"   then memory_location = jumpHeights - 0xAC + 0x5F70 + (0x70 * 0x02)
    elseif spell == "Blizzard" then memory_location = jumpHeights - 0xAC + 0x5F70 + (0x70 * 0x03)
    elseif spell == "Blizzara" then memory_location = jumpHeights - 0xAC + 0x5F70 + (0x70 * 0x04)
    elseif spell == "Blizzaga" then memory_location = jumpHeights - 0xAC + 0x5F70 + (0x70 * 0x05)
    elseif spell == "Thunder"  then memory_location = jumpHeights - 0xAC + 0x5F70 + (0x70 * 0x06)
    elseif spell == "Thundara" then memory_location = jumpHeights - 0xAC + 0x5F70 + (0x70 * 0x07)
    elseif spell == "Thundaga" then memory_location = jumpHeights - 0xAC + 0x5F70 + (0x70 * 0x08)
    elseif spell == "Cure"     then memory_location = jumpHeights - 0xAC + 0x5F70 + (0x70 * 0x09) - 0x34
    elseif spell == "Cura"     then memory_location = jumpHeights - 0xAC + 0x5F70 + (0x70 * 0x0A) - 0x34
    elseif spell == "Curaga"   then memory_location = jumpHeights - 0xAC + 0x5F70 + (0x70 * 0x0B) - 0x34
    elseif spell == "Gravity"  then memory_location = jumpHeights - 0xAC + 0x5F70 + (0x70 * 0x0C)
    elseif spell == "Gravira"  then memory_location = jumpHeights - 0xAC + 0x5F70 + (0x70 * 0x0D)
    elseif spell == "Graviga"  then memory_location = jumpHeights - 0xAC + 0x5F70 + (0x70 * 0x0E)
    elseif spell == "Stop"     then memory_location = jumpHeights - 0xAC + 0x5F70 + (0x70 * 0x0F)
    elseif spell == "Stopra"   then memory_location = jumpHeights - 0xAC + 0x5F70 + (0x70 * 0x10)
    elseif spell == "Stopga"   then memory_location = jumpHeights - 0xAC + 0x5F70 + (0x70 * 0x11)
    elseif spell == "Aero"     then memory_location = jumpHeights - 0xAC + 0x5F70 + (0x70 * 0x12)
    elseif spell == "Aerora"   then memory_location = jumpHeights - 0xAC + 0x5F70 + (0x70 * 0x13)
    elseif spell == "Aeroga"   then memory_location = jumpHeights - 0xAC + 0x5F70 + (0x70 * 0x14) end
    if memory_location then
        return ReadByte(memory_location)
    else
        return 0
    end
end

local function get_current_hits()
    -- Returns Sora's current combo hits
    return ReadByte(currentHits)
end

local function get_sora_pos()
    --Returns a table of Sora's X,Y,Z Coords
    local pos = {}
    local currSoraPointer = GetPointer(soraPointer)
    pos["X"] = ReadFloat(currSoraPointer + 0x10, true)
    pos["Y"] = ReadFloat(currSoraPointer + 0x14, true)
    pos["Z"] = ReadFloat(currSoraPointer + 0x18, true)
    return pos
end

local function get_gummi_qty_at_index(index)
    -- Gets Gummi item qty at a passed idx
    return ReadByte(gummiInventory + index - 1)
end

-- ########### --
-- # Setters # --
-- ########### --
local function set_animation_speed(animation_speed)
    -- Overwrites Sora's current animation speed
    WriteFloat(GetPointer(soraHUD - 0xA94) + 0x284, animation_speed, true)
end

local function set_ground_combo_length_limit(ground_combo_length_limit)
    -- Overwrites Sora's current ground combo length
    local stats_page = get_stats_page(ReadLong(soraPointer))
    if stats_page ~= 0 then
        WriteByte(stats_page + 0xD4, ground_combo_length_limit, true)
    end
end

local function set_air_combo_length_limit(air_combo_length_limit)
    -- Overwrites Sora's current air combo length
    local stats_page = get_stats_page(ReadLong(soraPointer))
    if stats_page ~= 0 then
        WriteByte(stats_page + 0xD5, air_combo_length_limit, true)
    end
end

local function set_stock_at_index(index, qty)
    -- Sets stock qty for a particular item
    WriteByte(inventory + index - 1, math.min(qty, 99))
end

local function set_sora_walk_speed(walk_speed)
    -- Set Sora's walk speed
    WriteFloat(zantHack - 0x2862, walk_speed)
end

local function set_sora_run_speed(run_speed)
    -- Sets Sora's run speed
    WriteFloat(zantHack - 0x285B, run_speed)
end

local function set_spell_effectiveness(spell, value)
    local memory_location = nil
        if spell == "Fire"     then memory_location = jumpHeights - 0xAC + 0x5F70 + (0x00 * 0x70)
    elseif spell == "Fira"     then memory_location = jumpHeights - 0xAC + 0x5F70 + (0x01 * 0x70)
    elseif spell == "Firaga"   then memory_location = jumpHeights - 0xAC + 0x5F70 + (0x02 * 0x70)
    elseif spell == "Blizzard" then memory_location = jumpHeights - 0xAC + 0x5F70 + (0x03 * 0x70)
    elseif spell == "Blizzara" then memory_location = jumpHeights - 0xAC + 0x5F70 + (0x04 * 0x70)
    elseif spell == "Blizzaga" then memory_location = jumpHeights - 0xAC + 0x5F70 + (0x05 * 0x70)
    elseif spell == "Thunder"  then memory_location = jumpHeights - 0xAC + 0x5F70 + (0x06 * 0x70)
    elseif spell == "Thundara" then memory_location = jumpHeights - 0xAC + 0x5F70 + (0x07 * 0x70)
    elseif spell == "Thundaga" then memory_location = jumpHeights - 0xAC + 0x5F70 + (0x08 * 0x70)
    elseif spell == "Cure"     then memory_location = jumpHeights - 0xAC + 0x5F70 + (0x09 * 0x70) - 0x34
    elseif spell == "Cura"     then memory_location = jumpHeights - 0xAC + 0x5F70 + (0x0A * 0x70) - 0x34
    elseif spell == "Curaga"   then memory_location = jumpHeights - 0xAC + 0x5F70 + (0x0B * 0x70) - 0x34
    elseif spell == "Gravity"  then memory_location = jumpHeights - 0xAC + 0x5F70 + (0x0C * 0x70)
    elseif spell == "Gravira"  then memory_location = jumpHeights - 0xAC + 0x5F70 + (0x0D * 0x70)
    elseif spell == "Graviga"  then memory_location = jumpHeights - 0xAC + 0x5F70 + (0x0E * 0x70)
    elseif spell == "Stop"     then memory_location = jumpHeights - 0xAC + 0x5F70 + (0x0F * 0x70)
    elseif spell == "Stopra"   then memory_location = jumpHeights - 0xAC + 0x5F70 + (0x10 * 0x70)
    elseif spell == "Stopga"   then memory_location = jumpHeights - 0xAC + 0x5F70 + (0x11 * 0x70)
    elseif spell == "Aero"     then memory_location = jumpHeights - 0xAC + 0x5F70 + (0x12 * 0x70)
    elseif spell == "Aerora"   then memory_location = jumpHeights - 0xAC + 0x5F70 + (0x13 * 0x70)
    elseif spell == "Aeroga"   then memory_location = jumpHeights - 0xAC + 0x5F70 + (0x14 * 0x70) end
    if memory_location then
        WriteByte(memory_location, value)
    end
end

local function set_spell_cost(spell, value)
    --[[ Sets a spells costs.  Defines what valid costs are below.
     15 = 1/2 CP
     30 =   1 CP
    100 =   1 MP
    200 =   2 MP
    300 =   3 MP
    Costs outside this range behave in unexepected ways.]]
    local possible_magic_costs = {15,30,100,200,300}
    if possible_magic_costs[value] then
        local memory_location = nil
            if spell == "Fire"     then memory_location = jumpHeights - 0xAC + 0x5F58 + (0x00 * 0x70)
        elseif spell == "Fira"     then memory_location = jumpHeights - 0xAC + 0x5F58 + (0x01 * 0x70)
        elseif spell == "Firaga"   then memory_location = jumpHeights - 0xAC + 0x5F58 + (0x02 * 0x70)
        elseif spell == "Blizzard" then memory_location = jumpHeights - 0xAC + 0x5F58 + (0x03 * 0x70)
        elseif spell == "Blizzara" then memory_location = jumpHeights - 0xAC + 0x5F58 + (0x04 * 0x70)
        elseif spell == "Blizzaga" then memory_location = jumpHeights - 0xAC + 0x5F58 + (0x05 * 0x70)
        elseif spell == "Thunder"  then memory_location = jumpHeights - 0xAC + 0x5F58 + (0x06 * 0x70)
        elseif spell == "Thundara" then memory_location = jumpHeights - 0xAC + 0x5F58 + (0x07 * 0x70)
        elseif spell == "Thundaga" then memory_location = jumpHeights - 0xAC + 0x5F58 + (0x08 * 0x70)
        elseif spell == "Cure"     then memory_location = jumpHeights - 0xAC + 0x5F58 + (0x09 * 0x70)
        elseif spell == "Cura"     then memory_location = jumpHeights - 0xAC + 0x5F58 + (0x0A * 0x70)
        elseif spell == "Curaga"   then memory_location = jumpHeights - 0xAC + 0x5F58 + (0x0B * 0x70)
        elseif spell == "Gravity"  then memory_location = jumpHeights - 0xAC + 0x5F58 + (0x0C * 0x70)
        elseif spell == "Gravira"  then memory_location = jumpHeights - 0xAC + 0x5F58 + (0x0D * 0x70)
        elseif spell == "Graviga"  then memory_location = jumpHeights - 0xAC + 0x5F58 + (0x0E * 0x70)
        elseif spell == "Stop"     then memory_location = jumpHeights - 0xAC + 0x5F58 + (0x0F * 0x70)
        elseif spell == "Stopra"   then memory_location = jumpHeights - 0xAC + 0x5F58 + (0x10 * 0x70)
        elseif spell == "Stopga"   then memory_location = jumpHeights - 0xAC + 0x5F58 + (0x11 * 0x70)
        elseif spell == "Aero"     then memory_location = jumpHeights - 0xAC + 0x5F58 + (0x12 * 0x70)
        elseif spell == "Aerora"   then memory_location = jumpHeights - 0xAC + 0x5F58 + (0x13 * 0x70)
        elseif spell == "Aeroga"   then memory_location = jumpHeights - 0xAC + 0x5F58 + (0x14 * 0x70) end
        if memory_location then
            WriteShort(memory_location, possible_magic_costs[value])
        end
    end
end    

local function set_attack_animation_data(index, animation_data)
    -- Overwrites attack animation data
    WriteArray(anims + (index * 20), animation_data)
end

local function set_command_data(index, command_data)
    -- Overwrites command data
    WriteArray(ReadLong(commandMenuPointer) + 16 * (index - 1), command_data, true)
end

local function set_gummi_qty_at_index(index, qty)
    -- Writes to Gummi Inventory
    WriteByte(gummiInventory + index - 1, math.min(qty, 99))
end

-- ############ --
-- # Advanced # --
-- ############ --
local function make_sora_actionable()
    -- Sets a flag on Sora's object that makes him actionable
    local sora_flags = byte_to_bits(ReadByte(ReadLong(soraPointer), true))
    sora_flags[3] = 0
    WriteByte(ReadLong(soraPointer), bits_to_byte(sora_flags), true)
end

local function calculate_ground_combo_limit()
    -- Calculates what Sora's ground combo limit should be, based on his abilities equipped
    local ground_combo_length = 3
    local equipped_abilities = get_equipped_sora_abilities()
    for k,v in pairs(equipped_abilities) do
        if v == 0x6 then
            ground_combo_length = ground_combo_length + 1
        end
    end
    return ground_combo_length
end

local function calculate_air_combo_limit()
    -- Calculates what Sora's air combo limit should be, based on his abilities equipped
    local air_combo_length_limit = 3
    local equipped_abilities = get_equipped_sora_abilities()
    for k,v in pairs(equipped_abilities) do
        if v == 0x7 then
            air_combo_length_limit = air_combo_length_limit + 1
        end
    end
    return air_combo_length_limit
end

local function enable_ability(ability)
    -- Enables an ability even if Sora doesn't have it or it isn't equipped
    local memory_location = nil
    
        if ability == "Vortex"          then memory_location = {soraHP + 0x1FC4,          1}
    elseif ability == "Aerial Sweep"    then memory_location = {soraHP + 0x1FC4,          2}
    elseif ability == "Counterattack"   then memory_location = {soraHP + 0x1FC4,          3}
    elseif ability == "Blitz"           then memory_location = {soraHP + 0x1FC4,          4}
    elseif ability == "Guard"           then memory_location = {soraHP + 0x1FC4,          5}
    elseif ability == "Dodge Roll"      then memory_location = {soraHP + 0x1FC4,          6}
    elseif ability == "Cheer"           then memory_location = {soraHP + 0x1FC5,          3}
    elseif ability == "Slapshot"        then memory_location = {soraHP + 0x1FC6,          8}
    elseif ability == "Sliding Dash"    then memory_location = {soraHP + 0x1FC7,          1}
    elseif ability == "Hurricane Blast" then memory_location = {soraHP + 0x1FC7,          2}
    elseif ability == "Ripple Drive"    then memory_location = {soraHP + 0x1FC7,          3}
    elseif ability == "Stun Impact"     then memory_location = {soraHP + 0x1FC7,          4}
    elseif ability == "Gravity Break"   then memory_location = {soraHP + 0x1FC7,          5}
    elseif ability == "Zantetsuken"     then memory_location = {soraHP + 0x1FC7,          6}
    elseif ability == "Sonic Blade"     then memory_location = {dialog + 0x738,           1}
    elseif ability == "Ars Arcanum"     then memory_location = {dialog + 0x738,           2}
    elseif ability == "Strike Raid"     then memory_location = {dialog + 0x738,           3}
    elseif ability == "Ragnarok"        then memory_location = {dialog + 0x738,           4}
    elseif ability == "Trinity Limit"   then memory_location = {dialog + 0x738,           5}
    elseif ability == "MP Haste"        then memory_location = {experienceMult - 0x94DC,  1}
    elseif ability == "MP Rage"         then memory_location = {experienceMult - 0x94DC,  2}
    elseif ability == "Second Chance"   then memory_location = {experienceMult - 0x94DC,  5}
    elseif ability == "Berserk"         then memory_location = {experienceMult - 0x94DC,  6}
    elseif ability == "Leaf Bracer"     then memory_location = {experienceMult - 0x94DC,  7} end
    
    if memory_location then
        WriteBit(memory_location[1], memory_location[2], 1)
    end
end    

local function force_scan(on)
-- Credits to KSX, ASM target that forces scan on
    if on then
        WriteArray(zantHack - 0x227C, {0x90,0x90,0x90,0x90,0x90,0x90})
    else
        WriteArray(zantHack - 0x227C, {0x0F,0x8E,0xD5,0x00,0x00,0x00})
    end
end

local function force_combo_master(on)
-- Credits to KSX, ASM target that forces combo master on
    if on then
        WriteByte(zantHack + 0x6FB, 0x71)
        WriteByte(zantHack + 0x6FB + 0x18, 0x82)
    else
        WriteByte(zantHack + 0x6FB, 0x72)
        WriteByte(zantHack + 0x6FB + 0x18, 0x84)
    end
end

local function allow_summon_anywhere(on)
    --[[ Credits to KSX, ASM target that allows summons 
    to be used outside of combat.]]
    if on then
        WriteByte(summonanywhere1, 0x72)
        WriteByte(summonanywhere2, 0x72)
        WriteByte(summonanywhere3, 0x72)
    else
        WriteByte(summonanywhere1, 0x74)
        WriteByte(summonanywhere2, 0x74)
        WriteByte(summonanywhere3, 0x75)
    end
end

local function allow_midair_dodge_roll_guard(on)
    -- Credits to KSX, ASM target that allows dodging/blocking in mid air.
    if on then
        WriteByte(zantHack + 0xC08, 0x82)
    else
        WriteByte(zantHack + 0xC08, 0x85)
    end
end

local function allow_air_items(on)
    --[[ Credits to KSX, ASM target that allows using items in mid air.
    Currently messes with spells in mid air too.]]
    if on then
        WriteByte(airitems1, 0x73)
        WriteByte(airitems2, 0x73)
    else
        WriteByte(airitems1, 0x75)
        WriteByte(airitems2, 0x74)
    end
end

local function multiply_summon_time(mult)
    -- Credits to KSX, multiplies the summon time factor.
    local vanilla_value = 3000
    local new_value = math.floor(vanilla_value * mult)
    WriteInt(summonTime, new_value)
end

local function partyActorRVA(z)
    --[[ Helper function for getting RVA values for Sora, Donald,
    or Goofy entity/actor pointers]]
    if z == 1 then return soraPointer end
    if z == 2 then return donaldPointer end
    return goofyPointer
end

local promptBodyRingPos = {0, 0, 0}

local function ClampKHSCII(khscii, maxBytes)
    --[[ Helper function which clamps KHSCII data to not be too long
    for level up prompts]]
    if #khscii <= maxBytes then
        return khscii
    end
    local clamped = {}
    for i = 1, maxBytes - 1 do
        clamped[i] = khscii[i]
    end
    clamped[maxBytes] = 0x00
    return clamped
end

local function show_prompt(input_title, input_party, duration, colour)
    --[[ Credits to Topaz.  Show custom text in the level up box.
    This has been slightly rewritten to use actual exe functions
    in order to use the in game box queue, to have more prompts
    on screen than 1.]]
    if colour == nil then
        colour = 0
    end

    local allOk = true

    for z = 1, 3 do
        local _boxArray = input_party[z]
        if _boxArray then
            local actor_ptr = GetPointer(partyActorRVA(z))
            if actor_ptr == 0 or ReadInt(actor_ptr + 0x130, true) == 0 then
                allOk = false
            else
                local _titleAddress = textMemory + 0x20 * (z - 1)
                local ringPos = promptBodyRingPos[z]
                promptBodyRingPos[z] = (ringPos + 1) % 5
                local _textAddress = (textMemory + 0x70) + (0x140 * (z - 1)) + (0x40 * ringPos)

                if input_title[z] then
                    WriteArray(_titleAddress, ClampKHSCII(GetKHSCII(input_title[z]), 0x20))
                end
                WriteArray(_textAddress, ClampKHSCII(GetKHSCII(_boxArray[1]), 0x20))
                local line1_ptr = BASE_ADDR + _textAddress
                local line2_ptr = 0
                if _boxArray[2] then
                    WriteArray(_textAddress + 0x20, ClampKHSCII(GetKHSCII(_boxArray[2]), 0x20))
                    line2_ptr = BASE_ADDR + _textAddress + 0x20
                end

                local ok = kh1_native.call_function(fnc_enqueue_levelup_prompt, actor_ptr, line1_ptr, line2_ptr)
                if not ok then
                    allOk = false
                else
                    local found = nil
                    for slot = 0, 2 do
                        for q = 0, 4 do
                            local boxAddr = boxMemory + slot * 0x3A20 + q * 0xBA0
                            if ReadLong(boxAddr + 0x20) == line1_ptr then
                                found = boxAddr
                                break
                            end
                        end
                        if found then break end
                    end

                    if found then
                        WriteLong(found + 0x30, BASE_ADDR + _titleAddress)
                        WriteLong(found + 0xB88, BASE_ADDR + colorBox + colour)
                        WriteLong(found + 0xB90, BASE_ADDR + colorText + colour)
                    else
                        allOk = false
                    end
                end
            end
        end
    end

    return allOk
end

local function is_pressed(button_array, only)
    --[[Returns true if the buttons passed in button_array
    are pressed.  If only is true, then returns true if 
    only those are pressed]]
    if only == nil then only = false end
    local input_bits = merge_tables(merge_tables(merge_tables(ReadBits(inputAddress), ReadBits(inputAddress+1)), ReadBits(inputAddress+2)), ReadBits(inputAddress+3))
    local bitmap = {"Select", "L3", "R3", "Start", "DPad U", "DPad R", "DPad D", "DPad L",
              "L2", "R2", "L1", "R1", "Triangle", "Circle", "X", "Square",
              "Unused 1", "Unused 2", "Unused 3", "Unused 4", "Right Analog Stick U", "Right Analog Stick R", "Right Analog Stick D", "Right Analog Stick L",
              "Unused 5", "Unused 6", "Unused 7", "Unused 8", "Left Analog Stick U", "Left Analog Stick R", "Left Analog Stick D", "Left Analog Stick L"}
    local expected_bitmap = {}
    for k,v in pairs(bitmap) do
        if contains(button_array, v) then
            expected_bitmap[k] = 1
        else
            expected_bitmap[k] = 0
        end
    end
    if only then
        for k,v in pairs(input_bits) do
            if input_bits[k] ~= expected_bitmap[k] then
                return false
            end
        end
        return true
    else
        for k,v in pairs(input_bits) do
            if expected_bitmap[k] == 1 then
                if input_bits[k] ~= expected_bitmap[k] then
                    return false
                end
            end
        end
        return true
    end
end

local function is_in_gummi_garage()
    --[[ Need to fix, actually returns if in Gummi ship,
    not just Gummi garage]]
    return ReadInt(inGummi) > 0
end

local function grant_shared_ability(shared_ability_value)
    -- Grants the party a shared ability
    local current_shared_abilities_qty = #get_shared_abilities()
    if current_shared_abilities_qty < 8 then
        WriteByte(sharedAbilities + current_shared_abilities_qty, shared_ability_value + 128)
    end
end

local function grant_sora_ability(ability_value)
    -- Grants Sora an ability
    local current_sora_abilities_qty = #get_sora_abilities()
    if current_sora_abilities_qty < 48 then
        WriteByte(soraCurAbilities + current_sora_abilities_qty, ability_value + 128)
    end
end

local function spawn_prize(item_id)
    --Uses the in game function to spawn a map prize
    local pos = get_sora_pos()
    local pos_ptr = kh1_native.write_floats(pos["X"], pos["Y"], pos["Z"])
    local spawned = kh1_native.call_function(fnc_spawn_prize, item_id, pos_ptr, 0)
    return spawned
end

local function show_custom_item_popup(text)
    --[[ Uses the in game function to force a map prize pickup box.
    Injects bytes to read from injected memory for text, so we
    can have custom text.]]
    kh1_native.install_popup_text_hook(fnc_item_popup_text_hook, fnc_item_popup_text_resume, fnc_item_popup_text_call_target)
    kh1_native.install_popup_completion_hook(fnc_item_popup_tick, fnc_item_popup_tick_resume, g_item_popup_state)
    kh1_native.set_custom_popup_text(GetKHSCII(text))
    return kh1_native.call_function(fnc_show_item_message, 1, 1)
end

local function play_se2(se_id, param_2)
    -- Plays a sound effect using the in game function.
    return kh1_native.call_function(fnc_play_se2, se_id, param_2)
end

local function sora_koed()
    -- Returns if Sora's current HP is 0
    return ReadByte(maxHP - 0x1) == 0
end

local function ko_sora()
    -- Triggers the in game functions to KO Sora.
    if not sora_koed() then
        kh1_native.call_function(fnc_trigger_ko_event_script, 0xC8)
    end
end

local function heartless_angel_sora()
    -- Sets Sora HP to 1 and MP to 0
    if not sora_koed() then
        local stats_page = get_stats_page(ReadLong(soraPointer))
        if stats_page ~= 0 then
            WriteByte(stats_page + 0x3C, 1, true)
            WriteByte(stats_page + 0x44, 0, true)
        end
        WriteByte(maxHP - 0x1, 1)
        WriteByte(maxHP - 0x1 + 2, 0)
    end
end

-- ########################## --
-- # Advanced: Enemy Spawns # --
-- ########################## --

local function spawn_enemy_attempt(model_path, motion_path, x, y, z)
    --[[Single-attempt primitive behind spawn_enemy. Spawns a Heartless (or
    other placement-table-driven entity) at an arbitrary world position via
    kh1_native.spawn_enemy. x/y/z all default to Sora's own live position
    (get_sora_pos()) when omitted -- pass explicit coordinates only if you
    want it somewhere other than on top of Sora. Can return stillLoading if
    the asset isn't ready yet, or if a cutscene is currently playing --
    callers should go through spawn_enemy instead of calling this directly.]]

    -- Don't spawn mid-cutscene
    if ReadInt(inCutscene) ~= 0 then
        return false, "in cutscene", "cutscene"
    end

    if x == nil or y == nil or z == nil then
        local pos = get_sora_pos()
        x = x or pos["X"]
        y = y or pos["Y"]
        z = z or pos["Z"]
    end

    local charId, weight, template = 0, 0, nil
    local known = kh1_creature_data[model_path]
    if known then
        charId = known.charId
        weight = known.weight
        template = known.template
    end
    return kh1_native.spawn_enemy(fnc_spawn_world_gimmick_entity, placementTablePtr, placementTableCount,
        fnc_load_gimmick_assets, loadedSpeciesPtrTable, fnc_mint_resource_handle, fnc_resolve_resource_handle,
        speciesResourceTable, model_path, motion_path, x, y, z,
        g_WorldNumber, g_AreaNumber, g_SetNumber,
        fnc_async_load_job_callback, fnc_velocity_blend_util, fnc_iterate_live_entities,
        fnc_party_ability_index_resolve_call, charId, weight, template,
        fnc_skeleton_handle_deref_hook, fnc_skeleton_blend_call_hook,
        fnc_keyframe_list_entry_hook, entityPoolBase, soraPointer,
        g_SoraObjPtr, g_PartyMember1ObjectPtr, g_PartyMember2ObjPtr,
        fnc_keyframe_list_lookup_param_hook, fnc_anim_blend_advance_diag_hook,
        fnc_link_model_resource_data_section2_check, fnc_resolve_resource_handle_impl_bucket_check,
        fnc_status_effect_handle_capture_hook, fnc_status_effect_handle_record_hook,
        fnc_text_slot_fresh_resolve_hook, text_slot_table_base, resource_handle_bucket_table,
        fnc_behavior_script_copy_guard_hook, fnc_joint_remap_setup_guard_hook,
        fnc_release_species_slot_run)
end

-- Pending spawn_enemy requests
local pending_spawn_requests = {}
local next_spawn_request_id = 1

local function spawn_enemy(model_path, motion_path, x, y, z, callback)
    --[[Spawns a Heartless (or other placement-table-driven entity), handling
    the fallback-spawn case where the asset load hasn't finished yet. Queues
    a request and returns immediately; call update_spawn_enemy() every frame
    from your own script's _OnFrame (harmless/no-op if nothing is pending) to
    actually drive it, the same convention open_text_box/update_text_boxes
    uses.

    Ignored outright (never queued) if a cutscene is already playing --
    callers that want it to happen once the cutscene ends should re-request
    it themselves rather than have this silently wait out an unknown-length
    cutscene.]]

    if ReadInt(inCutscene) ~= 0 then
        if callback then
            callback(false, "spawn_enemy: ignored, player is in a cutscene")
        end
        return
    end

    local id = next_spawn_request_id
    next_spawn_request_id = next_spawn_request_id + 1
    pending_spawn_requests[id] = {
        model_path = model_path,
        motion_path = motion_path,
        x = x, y = y, z = z,
        callback = callback,
        deadline = os.clock() + 10.0,
    }
end

local function update_spawn_enemy()
    --[[Drives every pending spawn_enemy request one step forward. Call this
    every frame from your own script's _OnFrame -- see spawn_enemy's comment
    for why this can't happen automatically.]]
    for id, req in pairs(pending_spawn_requests) do
        local ok, result, stillLoading = spawn_enemy_attempt(req.model_path, req.motion_path, req.x, req.y, req.z)
        if stillLoading then
            if stillLoading == "cutscene" then
                -- A cutscene started after this request was already queued
                -- (spawn_enemy's own upfront check only catches one already
                -- playing at request time) -- drop it rather than wait out
                -- an unknown-length cutscene.
                pending_spawn_requests[id] = nil
                if req.callback then
                    req.callback(false, "spawn_enemy: ignored, player entered a cutscene while this request was pending")
                end
            elseif os.clock() >= req.deadline then
                pending_spawn_requests[id] = nil
                if req.callback then
                    req.callback(false, "spawn_enemy: timed out waiting for asset load")
                end
            end
            -- else: still within budget, leave it queued for next frame.
        else
            pending_spawn_requests[id] = nil
            if req.callback then
                req.callback(ok, result)
            end
        end
    end
end

-- ######################## --
-- # Advanced: Text Boxes # --
-- ######################## --

-- Table for tracking open boxes
local open_text_boxes = {}

local function set_text_box_style(window_id, style)
    -- Controls the style of text box.
    window_id = window_id or 1
    return kh1_native.call_evdl_syscall(fnc_005_set_window_type, {window_id, style})
end

local function set_text_box_position(window_id, x, y)
    -- Controls text box position on screen.
    window_id = window_id or 1
    return kh1_native.call_evdl_syscall(fnc_003_set_window_position, {window_id, x, y})
end

local function set_text_box_size(window_id, width, height)
    -- Controls box width/height.
    window_id = window_id or 1
    return kh1_native.call_evdl_syscall(fnc_004_set_window_size, {window_id, width, height})
end

local function open_text_box(text, window_id, duration_seconds, style, x, y, width, height)
    --[[ Opens text box.  Normally text boxes uses a reference to
    predefined text, so we have to inject ASM and text memory in order
    to use custom text.]]
    window_id = window_id or 1
    if style ~= nil then
        set_text_box_style(window_id, style)
    end
    if x ~= nil and y ~= nil then
        set_text_box_position(window_id, x, y)
    end
    if width ~= nil and height ~= nil then
        set_text_box_size(window_id, width, height)
    end
    kh1_native.install_textbox_hook(fnc_display_message_text_hook, fnc_display_message_text_resume)
    kh1_native.install_textbox_anim_hook(fnc_display_message_anim_hook, fnc_display_message_anim_resume, fnc_display_message_anim_call_target)
    kh1_native.set_textbox_text(GetKHSCII(text))
    local opened = kh1_native.call_evdl_syscall(fnc_0B1_open_window_no_close, {window_id})
    local displayed = kh1_native.call_evdl_syscall(fnc_001_display_message, {window_id, 0})
    kh1_native.set_pending_text_box(window_id, fnc_002_close_window)
    open_text_boxes[window_id] = {
        deadline = (duration_seconds and duration_seconds > 0) and (os.clock() + duration_seconds) or nil,
        open_world = get_world(),
        open_room = get_room(),
    }
    return opened and displayed
end

local function close_text_box(window_id)
    -- Closes text box
    window_id = window_id or 1
    open_text_boxes[window_id] = nil
    kh1_native.clear_pending_text_box()
    return kh1_native.call_evdl_syscall(fnc_002_close_window, {window_id})
end

--[[ If the script is refreshed, clean up any
open boxes]]
kh1_native.close_pending_text_box()

local function update_text_boxes()
--[[ Handles the timing mechanism for open text
boxes.]]
    local now = os.clock()
    local current_world = get_world()
    local current_room = get_room()
    for window_id, box in pairs(open_text_boxes) do
        local timed_out = box.deadline and now >= box.deadline
        local transitioned = current_world ~= box.open_world or current_room ~= box.open_room
        if timed_out or transitioned then
            close_text_box(window_id)
        end
    end
end

return {
    byte_to_bits = byte_to_bits,
    bits_to_byte = bits_to_byte,
    ReadBits = ReadBits,
    ReadBit = ReadBit,
    WriteBit = WriteBit,
    contains = contains,
    get_index = get_index,
    merge_tables = merge_tables,
    get_world = get_world,
    get_room = get_room,
    get_gummi_select = get_gummi_select,
    get_animation_speed = get_animation_speed,
    get_current_animation = get_current_animation,
    get_ground_combo_length_limit = get_ground_combo_length_limit,
    get_air_combo_length_limit = get_air_combo_length_limit,
    get_animation_time = get_animation_time,
    get_stock = get_stock,
    get_stock_at_index = get_stock_at_index,
    get_sora_abilities = get_sora_abilities,
    get_shared_abilities = get_shared_abilities,
    get_equipped_sora_abilities = get_equipped_sora_abilities,
    get_equipped_shared_abilities = get_equipped_shared_abilities,
    get_soras_accessory_slots = get_soras_accessory_slots,
    get_soras_equipped_accessories = get_soras_equipped_accessories,
    get_inputs = get_inputs,
    get_spell_effectiveness = get_spell_effectiveness,
    get_current_hits = get_current_hits,
    get_sora_pos = get_sora_pos,
    get_gummi_qty_at_index = get_gummi_qty_at_index,
    set_animation_speed = set_animation_speed,
    set_ground_combo_length_limit = set_ground_combo_length_limit,
    set_air_combo_length_limit = set_air_combo_length_limit,
    set_stock_at_index = set_stock_at_index,
    set_sora_walk_speed = set_sora_walk_speed,
    set_sora_run_speed = set_sora_run_speed,
    set_spell_effectiveness = set_spell_effectiveness,
    set_spell_cost = set_spell_cost,
    set_attack_animation_data = set_attack_animation_data,
    set_command_data = set_command_data,
    set_gummi_qty_at_index = set_gummi_qty_at_index,
    make_sora_actionable = make_sora_actionable,
    calculate_ground_combo_limit = calculate_ground_combo_limit,
    calculate_air_combo_limit = calculate_air_combo_limit,
    enable_ability = enable_ability,
    force_scan = force_scan,
    force_combo_master = force_combo_master,
    allow_summon_anywhere = allow_summon_anywhere,
    allow_midair_dodge_roll_guard = allow_midair_dodge_roll_guard,
    allow_air_items = allow_air_items,
    multiply_summon_time = multiply_summon_time,
    show_prompt = show_prompt,
    GetKHSCII = GetKHSCII,
    is_pressed = is_pressed,
    is_in_gummi_garage = is_in_gummi_garage,
    give_shared_ability = grant_shared_ability,
    give_sora_ability = grant_sora_ability,
    spawn_prize = spawn_prize,
    spawn_enemy = spawn_enemy,
    update_spawn_enemy = update_spawn_enemy,
    show_custom_item_popup = show_custom_item_popup,
    play_se2 = play_se2,
    open_text_box = open_text_box,
    close_text_box = close_text_box,
    update_text_boxes = update_text_boxes,
    set_text_box_style = set_text_box_style,
    set_text_box_position = set_text_box_position,
    set_text_box_size = set_text_box_size,
    sora_koed = sora_koed,
    ko_sora = ko_sora,
    heartless_angel_sora = heartless_angel_sora
}