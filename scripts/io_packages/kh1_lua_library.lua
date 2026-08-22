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

local kh1_native = require("kh1_native")
local kh1_spawn_enemy = require("modules.spawn_enemy")
local kh1_khscii = require("helpers.khscii")
local kh1_text_boxes = require("modules.text_boxes")
local kh1_prize_popup = require("modules.prize_popup")
local kh1_level_up_prompt = require("modules.level_up_prompt")

local GetKHSCII = kh1_khscii.GetKHSCII

-- ########### --
-- # Helpers # --
-- ########### --

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
    --[[ Sets a spell's cost. Valid values: 15 = 1/2 CP, 30 = 1 CP,
    100/200/300 = 1/2/3 MP. Anything else behaves unexpectedly.]]
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
    -- Credits to KSX, ASM target that allows summons outside of combat.
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
    Currently affects mid air spells too.]]
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

local function is_pressed(button_array, only)
    --[[Returns true if the buttons in button_array are pressed. If only is
    true, returns true only when no other buttons are pressed.]]
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
    -- Needs a rename: returns true for the whole Gummi ship, not just the garage
    return ReadInt(inGummi) > 0
end

local function is_in_gummi()
    return ReadInt(saveOpenAddress) == 7
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
    show_prompt = kh1_level_up_prompt.show_prompt,
    GetKHSCII = GetKHSCII,
    is_pressed = is_pressed,
    is_in_gummi_garage = is_in_gummi_garage,
    is_in_gummi = is_in_gummi,
    give_shared_ability = grant_shared_ability,
    give_sora_ability = grant_sora_ability,
    spawn_prize = spawn_prize,
    spawn_enemy = kh1_spawn_enemy.spawn_enemy,
    show_custom_item_popup = kh1_prize_popup.show_custom_item_popup,
    play_se2 = play_se2,
    open_text_box = kh1_text_boxes.open_text_box,
    close_text_box = kh1_text_boxes.close_text_box,
    update_text_boxes = kh1_text_boxes.update_text_boxes,
    set_text_box_style = kh1_text_boxes.set_text_box_style,
    set_text_box_position = kh1_text_boxes.set_text_box_position,
    set_text_box_size = kh1_text_boxes.set_text_box_size,
    sora_koed = sora_koed,
    ko_sora = ko_sora,
    heartless_angel_sora = heartless_angel_sora
}