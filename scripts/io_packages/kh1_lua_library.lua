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

-- ########### --
-- # Helpers # --
-- ########### --

local function GetKHSCII(INPUT)
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
    local bits = {}
    for i = 0, 7 do
        bits[i + 1] = (byte >> i) & 1  -- LSB first
    end
    return bits
end

local function bits_to_byte(bits)
    assert(#bits == 8, "bits_to_byte expects exactly 8 bits")
    local byte = 0
    for i = 1, 8 do
        byte = byte | ((bits[i] & 1) << (i - 1)) -- LSB first
    end
    return byte
end

local function ReadBits(address, absolute)
    if absolute == nil then absolute = false end
    return byte_to_bits(ReadByte(address, absolute))
end

local function ReadBit(address, bit_num, absolute)
    if absolute == nil then absolute = false end
    return byte_to_bits(ReadByte(address, absolute))[bit_num]
end

local function WriteBit(address, bit_num, value, absolute)
    if absolute == nil then absolute = false end
    local bits = ReadBits(address, absolute)
    bits[bit_num] = (value ~= 0) and 1 or 0
    WriteByte(address, bits_to_byte(bits), absolute)
end

local function contains(tbl, val)
    for _, v in ipairs(tbl) do
        if v == val then
            return true
        end
    end
    return false
end

local function get_index(tbl, val)
    for k, v in ipairs(tbl) do
        if v == val then
            return k
        end
    end
    return nil
end

local function merge_tables(t1, t2)
    for _, value in ipairs(t2) do
        table.insert(t1, value)
    end
    return t1
end

-- ########### --
-- # Getters # --
-- ########### --
local function get_world()
    return ReadByte(world)
end

local function get_room()
    return ReadByte(room)
end

local function get_gummi_select()
    return ReadByte(gummiSelect)
end

local function get_animation_speed()
    return ReadFloat(GetPointer(soraHUD - 0xA94) + 0x284, true)
end

local function get_current_animation()
    return ReadByte(ReadLong(soraPointer)+0x164, true)
end

local function get_ground_combo_length_limit()
    return ReadByte(soraHP + 0x98)
end

local function get_air_combo_length_limit()
    return ReadByte(soraHP + 0x99)
end

local function get_animation_time()
    return ReadFloat(ReadLong(soraPointer)+0x16C, true)
end

local function get_stock()
    return ReadArray(inventory, 255)
end

local function get_stock_at_index(index)
    return ReadByte(inventory + index - 1)
end

local function get_sora_abilities()
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
    return ReadByte(maxHP + 0x16)
end

local function get_soras_equipped_accessories()
    return ReadArray(maxHP + 0x17, get_soras_accessory_slots())
end

local function get_inputs()
    return ReadArray(inputAddress, 4)
end

local function get_spell_effectiveness(spell)
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
    return ReadByte(gummiInventory + index - 1)
end

-- ########### --
-- # Setters # --
-- ########### --
local function set_animation_speed(animation_speed)
    WriteFloat(GetPointer(soraHUD - 0xA94) + 0x284, animation_speed, true)
end

local function set_ground_combo_length_limit(ground_combo_length_limit)
    WriteByte(soraHP + 0x98, ground_combo_length_limit)
end

local function set_air_combo_length_limit(air_combo_length_limit)
    WriteByte(soraHP + 0x99, air_combo_length_limit)
end

local function set_stock_at_index(index, qty)
    WriteByte(inventory + index - 1, math.min(qty, 99))
end

local function set_sora_walk_speed(walk_speed)
    WriteFloat(zantHack - 0x2862, walk_speed)
end

local function set_sora_run_speed(run_speed)
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
    elseif spell == "Cure"     then memory_location = jumpHeights - 0xAC + 0x5F70 + (0x09 * 0x70)
    elseif spell == "Cura"     then memory_location = jumpHeights - 0xAC + 0x5F70 + (0x0A * 0x70)
    elseif spell == "Curaga"   then memory_location = jumpHeights - 0xAC + 0x5F70 + (0x0B * 0x70)
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
    WriteArray(anims + (index * 20), animation_data)
end

local function set_command_data(index, command_data)
    WriteArray(ReadLong(commandMenuPointer) + 16 * (index - 1), command_data, true)
end

local function set_gummi_qty_at_index(index, qty)
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
    if on then
        WriteArray(zantHack - 0x227C, {0x90,0x90,0x90,0x90,0x90,0x90})
    else
        WriteArray(zantHack - 0x227C, {0x0F,0x8E,0xD5,0x00,0x00,0x00})
    end
end

local function force_combo_master(on)
    if on then
        WriteByte(zantHack + 0x6FB, 0x71)
        WriteByte(zantHack + 0x6FB + 0x18, 0x82)
    else
        WriteByte(zantHack + 0x6FB, 0x72)
        WriteByte(zantHack + 0x6FB + 0x18, 0x84)
    end
end

local function allow_summon_anywhere(on)
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
    if on then
        WriteByte(zantHack + 0xC08, 0x82)
    else
        WriteByte(zantHack + 0xC08, 0x85)
    end
end

local function allow_air_items(on)
    if on then
        WriteByte(airitems1, 0x73)
        WriteByte(airitems2, 0x73)
    else
        WriteByte(airitems1, 0x75)
        WriteByte(airitems2, 0x74)
    end
end

local function multiply_summon_time(mult)
    local vanilla_value = 3000
    local new_value = math.floor(vanilla_value * mult)
    WriteInt(summonTime, new_value)
end

local function show_prompt(input_title, input_party, duration, colour)
    --[[Writes to memory the message to be displayed in a Level Up prompt.]]
    if colour == nil then
        colour = 0
    end

    local _partyOffset = 0x3A20

    for i = 1, #input_title do
        if input_title[i] then
            WriteArray(textMemory + 0x20 * (i - 1), GetKHSCII(input_title[i]))
        end
    end

    for z = 1, 3 do
        local _boxArray = input_party[z];
        
        local _colorBox  = colorBox + colour
        local _colorText = colorText + colour

        if _boxArray then
            local _textAddress = (textMemory + 0x70) + (0x140 * (z - 1)) + (0x40 * 0)
            local _boxAddress = boxMemory + (_partyOffset * (z - 1)) + (0xBA0 * 0)

            -- Write the box count.
            WriteInt(boxMemory - 0x10 + 0x04 * (z - 1), 1)

            -- Write the Title Pointer.
            WriteLong(_boxAddress + 0x30, BASE_ADDR  + textMemory + 0x20 * (z - 1))

            if _boxArray[2] then
                -- String Count is 2.
                WriteInt(_boxAddress + 0x18, 0x02)

                -- Second Line Text.
                WriteArray(_textAddress + 0x20, GetKHSCII(_boxArray[2]))
                WriteLong(_boxAddress + 0x28, BASE_ADDR  + _textAddress + 0x20)
            else
                -- String Count is 1
                WriteInt(_boxAddress + 0x18, 0x01)
            end

            -- First Line Text
            WriteArray(_textAddress, GetKHSCII(_boxArray[1]))
            WriteLong(_boxAddress + 0x20, BASE_ADDR  + _textAddress)

            -- Reset box timers.
            WriteInt(_boxAddress + 0x0C, duration)
            WriteFloat(_boxAddress + 0xB80, 1)

            -- Set box colors.
            WriteLong(_boxAddress + 0xB88, BASE_ADDR  + _colorBox)
            WriteLong(_boxAddress + 0xB90, BASE_ADDR  + _colorText)

            -- Show the box.
            WriteInt(_boxAddress, 0x01)
        end
    end
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
    return ReadInt(inGummi) > 0
end

local function grant_shared_ability(shared_ability_value)
    local current_shared_abilities_qty = #get_shared_abilities()
    if current_shared_abilities_qty < 8 then
        WriteByte(sharedAbilities + current_shared_abilities_qty, shared_ability_value + 128)
    end
end

local function grant_sora_ability(ability_value)
    local current_sora_abilities_qty = #get_sora_abilities()
    if current_sora_abilities_qty < 48 then
        WriteByte(soraCurAbilities + current_sora_abilities_qty, ability_value + 128)
    end
end

local function spawn_prize(item_id)
    --[[Spawns a physical item pickup near Sora via kh1_native.call_function.
    Unlike the rest of this library this calls real game code rather than
    just reading/writing memory, so it needs fnc_spawn_prize (see
    SteamGlobal_1_0_0_2.lua / EGSGlobal_1_0_0_10.lua) and kh1_native.dll to be
    present.

    fnc_spawn_prize's second argument is a pointer to a packed {x,y,z} world
    position float vector (confirmed from the real EVDL caller,
    fnc_22A_scatter_map_gimmick_prizes, which builds one on its own stack) --
    NOT an entity/parent pointer. kh1_native.write_floats builds that vector
    on the fly since Lua can't take the address of a local value itself.

    Deliberately does NOT run the claim/display sequence (fnc_update_widget_queue,
    EVDL opcodes 0x170/0x16F) real prize-scatter events chain after this: that
    pair's "value" argument is actually the pop-in animation's total duration
    (matched against a countdown elsewhere), not an item id, so feeding it
    item_id there corrupts the icon's scale animation -- confirmed live: the
    spawned icon visibly grows without bound, faster for lower item ids. The
    physical pickup from fnc_spawn_prize alone is real and collectible without
    it; only the (broken) HUD notification icon is skipped.

    Returns true if the spawn call completed without crashing; the physical
    pickup may still fail to appear for reasons unrelated to the call itself
    (e.g. no valid spawn point nearby).]]
    local pos = get_sora_pos()
    local pos_ptr = kh1_native.write_floats(pos["X"], pos["Y"], pos["Z"])
    local spawned = kh1_native.call_function(fnc_spawn_prize, item_id, pos_ptr, 0)
    return spawned
end

local function show_custom_item_popup(text)
    --[[Shows the map-prize pickup popup (the small text window that
    normally names the item you got) with arbitrary custom text instead of
    a real item name -- no spawn_prize or actual pickup needed.

    Installs two kh1_native in-process hooks on first use (both idempotent):
    one on fnc_draw_item_popup_entry that redirects the displayed text to a
    custom buffer while a flag is set, and one on fnc_item_popup_tick (the
    always-ticking queue consumer) that watches the popup's lifecycle state
    and clears that flag the moment the popup actually finishes displaying
    (not on the first frame -- the box renders across many frames, so
    clearing any earlier would revert to the real item name mid-display).
    Then writes `text` as the KHSCII buffer and triggers the popup via
    fnc_show_item_message(1, 1) (item id 1 just picks which icon/graphic
    shows; the text is what's overridden).

    Self-cleaning: nothing is left "stuck" for a later, unrelated real
    popup to pick up by accident, and no separate clear call is needed.

    Returns true if the enqueue call completed without crashing.]]
    kh1_native.install_popup_text_hook(fnc_item_popup_text_hook, fnc_item_popup_text_resume, fnc_item_popup_text_call_target)
    kh1_native.install_popup_completion_hook(fnc_item_popup_tick, fnc_item_popup_tick_resume, g_item_popup_state)
    kh1_native.set_custom_popup_text(GetKHSCII(text))
    return kh1_native.call_function(fnc_show_item_message, 1, 1)
end

local function play_se2(se_id, param_2)
    --[[Plays a sound effect via kh1_native.call_function on fnc_play_se2 (see
    SteamGlobal_1_0_0_2.lua / EGSGlobal_1_0_0_10.lua). Wraps the same native
    call the game itself uses for EVDL opcode 0x161 (play_se2), the
    cutscene-skip-prompt chime, and one other hardcoded call site -- all
    confirmed live call sites pass a real SE id plus a second integer whose
    exact meaning (priority/channel?) isn't fully understood; real call sites
    observed use param_2=5, but param_2=0 is also confirmed fine (see below).

    CAUTION: se_id is NOT a free-form integer. Live-tested 2026-07-12 --
    calling with an arbitrary/unregistered se_id (1) crashed the game
    outright (likely a null/garbage sound-bank lookup downstream). Known-good
    pairs confirmed live and audible in the field: (31, 0) -- carried over
    from an older code-cave-injection implementation in the sibling
    KH1-LUA-LIBRARY-DEV repo (scripts/1fmASMDriver.lua +
    play_sound_effect/assemblyPlaySE2), reproduced here through this
    function's kh1_native.call_function path. Per that same prior work, the
    valid se_id range for this bank is roughly decimal 1-76 with param_2=0.
    (0x3eb0, 5) also ran the full call chain without error/crash but
    produced no audible sound in the field -- that id is the
    cutscene-skip-prompt chime and is likely only resident during that
    specific context. Only pass se_id values already known to be valid,
    real KH1 SE ids.

    Returns true if the call completed without crashing.]]
    return kh1_native.call_function(fnc_play_se2, se_id, param_2)
end

local function apply_status_effect_to_sora(type_index, persistent)
    --[[Investigative/debug tool, NOT confirmed safe for normal gameplay use.

    Calls fnc_apply_actor_status_effect(sora_actor_ptr, type_flags) directly
    against Sora's own actor (GetPointer(soraPointer)) via
    kh1_native.call_function. This is the exact native call the .bd AI
    scripting language's verb t0 0x4b (AttachStatusEffect?) goes through --
    confirmed by decompiling the chain in Ghidra (fnc_bd_verb_attach_status_effect
    @ 0x1402ba410 -> fnc_apply_actor_status_effect @ 0x1402ae160 ->
    fnc_status_effect_activate_by_type @ 0x1401e0100). type_index is looked
    up in the ACTOR's own status-effect table (actor_ptr+0x68) - it is NOT
    confirmed to be a universal/game-wide id, so a given type_index may mean
    something completely different (or nothing at all) depending on which
    actor it's applied to.

    Added 2026-07-20 while investigating Sephiroth's Heartless Angel attack:
    his ex_3000_02.bd behavior block calls this chain 11 times in a row
    against its target with type_index 34 through 44 (raw values 2082-2092,
    all with the persistent bit set), immediately before a MakeAttack call.
    This function exists to test type_index values 0-51ish live against
    Sora and observe what actually happens (HP/MP/other) - see the debug
    overlay's "Apply Status Effect (Sora)" action.

    persistent (bool, default true) sets bit 0x800 (infinite duration),
    matching every real .bd call site surveyed so far (all had it set).

    Returns true if the call completed without crashing -- NOT a confirmation
    that the effect index is meaningful or safe. Untested type indices could
    plausibly corrupt actor state; this is diagnostic tooling, not a stable
    API.]]
    local flags = type_index & 0x7FF
    if persistent == nil or persistent then flags = flags | 0x800 end
    local sora_actor = GetPointer(soraPointer)
    return kh1_native.call_function(fnc_apply_actor_status_effect, sora_actor, flags)
end

-- Keyed by window_id; each entry is {deadline=os.clock() value or nil,
-- open_world=byte, open_room=byte}. Always populated on a successful open
-- (regardless of duration_seconds) so update_text_boxes can auto-close on a
-- room/world transition even for boxes with no timer.
local open_text_boxes = {}

local function set_text_box_style(window_id, style)
    --[[Sets the visual style of a text box window's TEMPLATE (Set_window_type,
    EVDL opcode 5) via kh1_native.call_evdl_syscall -- no window needs to be
    open yet, this configures what a later open_text_box() call for the same
    window_id will look like (matching how real scripts always call
    Set_window_type/Set_window_size/etc. before Open_window). Normally called
    for you by open_text_box's own style parameter -- call this directly only
    if you want to configure a window_id's template without opening it yet.

    style is a raw integer 0-8 (9 total; confirmed live in-game, 2026-07-12 --
    values outside this range read past the end of the engine's own lookup
    table and produce undefined/glitchy rendering, confirmed by testing
    style 9). Visual catalogue, same test text at each:
      0 - no box at all; plain white outlined text floating over the scene
          (subtitle-style).
      1 - black semi-transparent box, purple/pink border, boxy corners.
      2 - the classic peach/orange rounded box (KH1's default look).
      3 - peach/orange, same family as 2 but a different corner/side treatment.
      4 - peach/orange, again a different corner/side treatment from 2 and 3.
      5 - peach/orange with jagged/spiky "comic burst" corners.
      6 - peach/orange with beveled (angle-cut) corners.
      7 - visually identical to 6.
      8 - visually identical to 6/7.
    This deliberately doesn't expose named constants since 6/7/8 don't look
    distinguishable despite being different values -- pass the raw integer.
    window_id defaults to 1, matching open_text_box's default.

    Returns true if the syscall completed without crashing.]]
    window_id = window_id or 1
    return kh1_native.call_evdl_syscall(fnc_005_set_window_type, {window_id, style})
end

local function set_text_box_position(window_id, x, y)
    --[[Sets the position of a text box window's TEMPLATE (Set_window_position,
    EVDL opcode 3) via kh1_native.call_evdl_syscall. Normally called for you
    by open_text_box's own x/y parameters -- call this directly only if you
    want to configure a window_id's template without opening it yet (this
    does NOT move an already-open window).

    x/y are raw integers cast to float by the engine (not a bit-reinterpret --
    the EVDL stack only holds int32s). Not screen pixels -- an internal
    layout unit not yet characterized live. window_id defaults to 1.

    Returns true if the syscall completed without crashing.]]
    window_id = window_id or 1
    return kh1_native.call_evdl_syscall(fnc_003_set_window_position, {window_id, x, y})
end

local function set_text_box_size(window_id, width, height)
    --[[Sets the size of a text box window's TEMPLATE (Set_window_size, EVDL
    opcode 4) via kh1_native.call_evdl_syscall. Normally called for you by
    open_text_box's own width/height parameters -- call this directly only
    if you want to configure a window_id's template without opening it yet.

    width/height are raw integers cast to float by the engine. Real scripts
    were observed (grepped across the KH1-RANDOMIZER asm corpus) using small
    values like width=10, height=2-3 -- an internal layout unit, not screen
    pixels, not yet characterized live. window_id defaults to 1.

    Returns true if the syscall completed without crashing.]]
    window_id = window_id or 1
    return kh1_native.call_evdl_syscall(fnc_004_set_window_size, {window_id, width, height})
end

local function open_text_box(text, window_id, duration_seconds, style, x, y, width, height)
    --[[Opens a real EVDL dialogue/text window (the same kind used for NPC
    talk prompts and chest item-get messages) showing arbitrary Lua-supplied
    text, without needing an actual EVDL script to trigger it.

    window_id selects both which of the 4 concurrent window "slots" gets used
    and which previously-configured template (position/size/type) the box
    inherits. Defaults to 1, which is the id most on-foot dialogue/prompt
    windows use.

    style/x/y/width/height are all optional and, if given, are applied to
    window_id's template via set_text_box_style/set_text_box_position/
    set_text_box_size BEFORE opening (matching how real EVDL scripts always
    call Set_window_type/Set_window_position/Set_window_size before
    Open_window). x and y must be given together (or not at all); same for
    width and height -- each pair maps to a single underlying syscall that
    can't set just one half. Omitting all of these leaves window_id's
    template exactly as it was last configured (by a real script, or a
    previous call here).

    Calls the real fnc_0B1_open_window_no_close and fnc_001_display_message
    EVDL syscall handlers via kh1_native.call_evdl_syscall, which builds a
    throwaway scriptCtx and feeds them arguments the same way the real EVDL
    interpreter would (these handlers read off a script-VM stack, not
    registers -- see SteamGlobal_1_0_0_2.lua's fnc_0B1/001/002 comments).
    Open_window_no_close (opcode 0xB1) is used instead of the plain
    Open_window (opcode 0): fnc_000_open_window's own close-when-queue-empty
    callback chain auto-closes the window the instant its one queued message
    finishes displaying, with no wait for a real player confirm press --
    confirmed live via Cheat Engine breakpoint tracing, the window was
    closing within the same frame it finished typing out, regardless of
    input. fnc_0B1_open_window_no_close's callback chain instead leaves the
    window open (idle, ready) once its queue empties, so it stays up
    indefinitely until close_text_box() is called.

    Message id 0 is always pushed for Display_message; the real per-script
    string table lookup that id would normally trigger is discarded because
    two hooks (install_textbox_hook + install_textbox_anim_hook) redirect the
    resolved text pointer to a custom KHSCII buffer instead. Two hooks are
    needed because a freshly-opened window is never immediately "idle/ready"
    in the same frame as fnc_0B1_open_window_no_close -- fnc_001_display_message
    just queues the message and returns early, and the actual display happens
    a few frames later via fnc_display_message_on_window_opened_no_close's own
    independent lookup (confirmed live: hooking only the first site left the
    window showing its real, unrelated leftover text every time). Both hooks
    self-clear the moment either one actually fires, so this deliberately
    does NOT call clear_textbox_text() itself. Message-count bookkeeping
    still happens for real inside fnc_001_display_message.

    duration_seconds is optional. There's no native engine field for
    "auto-close this window after N seconds" on this window system (unlike
    e.g. show_prompt's Level-Up-style boxes, which do have a real duration
    field written straight into the box struct) -- fnc_0B1_open_window_no_close
    leaves the box open indefinitely once shown, full stop. If given, this is
    purely a Lua-side timer (os.clock()-based, the same pattern used
    elsewhere in this ecosystem for connect timeouts). Regardless of
    duration_seconds, the box also auto-closes the moment get_world()/
    get_room() changes from what they were at open time (a room/world
    transition almost certainly means whatever this box was about no longer
    applies, and a stale window left open across a map load looks broken).
    update_text_boxes() must be called every frame from your own script's
    _OnFrame to actually drive either of these -- kh1_lua_library has no
    persistent per-frame hook of its own. Omit duration_seconds (or pass
    nil/0) to only auto-close on room/world transition, not on a timer.

    Returns true if both syscalls completed without crashing.]]
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
    --[[Closes a text box opened via open_text_box (or a real in-game one)
    using the same window_id. Calls the real fnc_002_close_window EVDL
    syscall handler via kh1_native.call_evdl_syscall -- see open_text_box's
    comment for how that works. Not required if the player is expected to
    dismiss the box themselves; provided for programmatic control.

    Cancels any pending duration_seconds/room-transition tracking for this
    window_id, so update_text_boxes() won't also try to close it again later.

    Returns true if the syscall completed without crashing.]]
    window_id = window_id or 1
    open_text_boxes[window_id] = nil
    kh1_native.clear_pending_text_box()
    return kh1_native.call_evdl_syscall(fnc_002_close_window, {window_id})
end

-- Runs once, every time this module is loaded/required -- including after
-- an F1 script reload, which tears down and re-requires every Lua module
-- (losing any Lua-side state like pending_text_box_closes above) without
-- ever giving a text box opened before the reload a chance to close itself.
-- kh1_native.dll's own pending-window-id/close-RVA tracking survives the
-- reload (see its "PENDING TEXT BOX TRACKING" comment -- the DLL pins
-- itself in memory across reloads), so this recovers and closes whatever
-- open_text_box last opened instead of leaving it stranded on screen
-- forever. Deliberately calls kh1_native.close_pending_text_box() directly
-- (not close_text_box(), which needs fnc_002_close_window) -- on a fast F1
-- reload this module-load code can run before the consuming script's
-- require("VersionCheck") has re-populated that global this cycle,
-- confirmed live by an earlier version of this crashing the syscall with
-- rva=0x0; the native side already has its own copy of that RVA from when
-- the box was opened.
kh1_native.close_pending_text_box()

local function update_text_boxes()
    --[[Closes any text boxes opened via open_text_box() once their
    duration_seconds timer elapses (if one was given) or the player has
    changed world/room since it was opened (always checked, regardless of
    duration_seconds). Call this every frame from your own script's
    _OnFrame (harmless/no-op if no text boxes are currently tracked) -- see
    open_text_box's comment for why this can't happen automatically.]]
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

local function sora_koed()
    return ReadByte(maxHP - 0x1) == 0
end

local function ko_sora()
    if not sora_koed() then
        WriteByte(soraHP, 0)
        WriteByte(maxHP - 0x1, 0)
        WriteByte(stateFlag, 1)
        WriteShort(deathCheck, 0x9090)
        revertCode = true
    end
end

local function heartless_angel_sora()
    if not sora_koed() then
        WriteByte(soraHP, 1)
        WriteByte(maxHP - 0x1, 1)
        WriteByte(soraHP + 0x8, 0)
        WriteByte(maxHP - 0x1 + 2, 0)
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
    show_custom_item_popup = show_custom_item_popup,
    play_se2 = play_se2,
    apply_status_effect_to_sora = apply_status_effect_to_sora,
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