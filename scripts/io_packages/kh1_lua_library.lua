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
    end

    table.insert(_returnArray, 0x00)
    return _returnArray
end

local function GetTextFromKHSCII(INPUT)
    local _charTable = {
        [0x01] =  ' ',
        [0x02] =  '\n',
        [0x6E] =  '-',
        [0x5F] =  '!',
        [0x60] =  '?',
        [0x62] =  '%',
        [0x66] =  '/',
        [0x68] =  '.',
        [0x69] =  ',',
        [0x6C] =  ';',
        [0x6B] =  ':',
        [0x71] =  '\'',
        [0x74] =  '(',
        [0x75] =  ')',
        [0x76] =  '[',
        [0x77] =  ']',
        [0xCA] =  '¡',
        [0xCB] =  '¿',
        [0xCC] =  'À',
        [0xCD] =  'Á',
        [0xCE] =  'Â',
        [0xCF] =  'Ä',
        [0xD0] =  'Ç',
        [0xD1] =  'È',
        [0xD2] =  'É',
        [0xD3] =  'Ê',
        [0xD4] =  'Ë',
        [0xD5] =  'Ì',
        [0xD6] =  'Í',
        [0xD7] =  'Î',
        [0xD8] =  'Ï',
        [0xD9] =  'Ñ',
        [0xDA] =  'Ò',
        [0xDB] =  'Ó',
        [0xDC] =  'Ô',
        [0xDD] =  'Ö',
        [0xDE] =  'Ù',
        [0xDF] =  'Ú',
        [0xE0] =  'Û',
        [0xE1] =  'Ü',
        [0xE2] =  'ß',
        [0xE3] =  'à',
        [0xE4] =  'á',
        [0xE5] =  'â',
        [0xE6] =  'ä',
        [0xE7] =  'ç',
        [0xE8] =  'è',
        [0xE9] =  'é',
        [0xEA] =  'ê',
        [0xEB] =  'ë',
        [0xEC] =  'ì',
        [0xED] =  'í',
        [0xEE] =  'î',
        [0xEF] =  'ï',
        [0xF0] =  'ñ',
        [0xF1] =  'ò',
        [0xF2] =  'ó',
        [0xF3] =  'ô',
        [0xF4] =  'ö',
        [0xF5] =  'ù',
        [0xF6] =  'ú',
        [0xF7] =  'û',
        [0xF8] =  'ü'
    }

    local _returnString = ''

    for i = 1, #INPUT do
        local _code = INPUT[i]

        if _code == 0x00 then
            break
        elseif _code >= 0x45 and _code <= 0x5E then
            _returnString = _returnString .. string.char(_code + 0x1C)
        elseif _code >= 0x2B and _code <= 0x44 then
            _returnString = _returnString .. string.char(_code + 0x16)
        elseif _code >= 0x21 and _code <= 0x2A then
            _returnString = _returnString .. string.char(_code + 0x0F)
        elseif _charTable[_code] ~= nil then
            _returnString = _returnString .. _charTable[_code]
        else
            _returnString = _returnString .. ' '
        end
    end

    return _returnString
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

local function get_absolute_item_descriptions_memory_address()
    local offsets = {0x0, 0xD8, 0x8, 0x60}
    local absolute_item_description_memory_address = GetPointer(itemDescriptions)
    ConsolePrint(absolute_item_description_memory_address)
    for _, offset in pairs(offsets) do
        absolute_item_description_memory_address = GetPointerA(absolute_item_description_memory_address + offset)
        ConsolePrint(absolute_item_description_memory_address)
    end
    return absolute_item_description_memory_address
end

local function get_absolute_item_description_at_idx_memory_address(idx)
    local absolute_item_description_memory_address = get_absolute_item_descriptions_memory_address()
    local delimiter_cnt = 0
    local i = 0
    while delimiter_cnt < idx do
        if ReadByte(absolute_item_description_memory_address + i, true) == 0xCD then return end
        if ReadByte(absolute_item_description_memory_address + i, true) == 0x00 then delimiter_cnt = delimiter_cnt + 1 end
        i = i + 1
    end
    local item_description_start_addr = absolute_item_description_memory_address + i
    local y = 1
    while ReadByte(item_description_start_addr + y, true) ~= 0x00 and ReadByte(item_description_start_addr + y, true) ~= 0xCD do
        y = y + 1
    end
    local bytes = ReadArray(item_description_start_addr, y, true)
    local description = GetTextFromKHSCII(bytes)
    return {["start"] = item_description_start_addr, ["length"] = y, ["bytes"] = bytes, ["description"] = description}
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
            WriteByte(memory_location, possible_magic_costs[value])
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

local function set_absolute_item_description_at_idx_memory_address(idx, overwrite_str)
    local tgt_string = get_absolute_item_description_at_idx_memory_address(idx)
    ConsolePrint("Start: " .. tostring(tgt_string["start"]))
    ConsolePrint("Length: " .. tostring(tgt_string["length"]))
    ConsolePrint("Description: " .. tostring(tgt_string["description"]))
    if tgt_string == nil then ConsolePrint("Nothing found for that idx " .. tostring(idx) .. ".") return end
    local overwrite_bytes = GetKHSCII(overwrite_str)
    table.remove(overwrite_bytes)
    if #overwrite_bytes > tgt_string["length"] then ConsolePrint("Input string " .. overwrite_str .. " too long!") return end
    while #overwrite_bytes < tgt_string["length"] do table.insert(overwrite_bytes, 0x01) end
    WriteArray(tgt_string["start"], overwrite_bytes, true)
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
        WriteByte(shared_abilities_address + current_shared_abilities_qty, shared_ability_value + 128)
    end
end

local function grant_sora_ability(ability_value)
    local current_sora_abilities_qty = #get_sora_abilities()
    if current_sora_abilities_qty < 48 then
        WriteByte(soraCurAbilities + current_sora_abilities_qty, ability_value + 128)
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
    get_absolute_item_description_at_idx_memory_address = get_absolute_item_description_at_idx_memory_address,
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
    set_absolute_item_description_at_idx_memory_address = set_absolute_item_description_at_idx_memory_address,
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
    GetTextFromKHSCII = GetTextFromKHSCII,
    is_pressed = is_pressed,
    is_in_gummi_garage = is_in_gummi_garage,
    give_shared_ability = grant_shared_ability,
    give_sora_ability = grant_sora_ability
}