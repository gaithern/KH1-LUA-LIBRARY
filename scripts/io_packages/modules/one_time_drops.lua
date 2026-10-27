---@diagnostic disable: undefined-global

local cffi = require("cffi")
local native = require("helpers.native")
local call_redirect = require("helpers.call_redirect")

cffi.cdef[[
    typedef uint64_t (*OneTimeDropsSpawnEnemyDrops)(uint64_t drop_table, uint64_t position, uint64_t mode);
    typedef uint64_t (*OneTimeDropsSpawnPrize)(int32_t item, uint64_t position, int32_t mode);
]]

local BD_NAME_OFFSET = 4
local DROP_CHANCE_OFFSET = 0x1C
local DROP_ITEM_OFFSET = 0x20
local DROP_SLOT_STRIDE = 8
local DROP_SLOT_COUNT = 3
local GUARANTEED_CHANCE = 0x3F800000
local PRIZE_SLOT_SIZE = 0x50
local PRIZE_SLOT_COUNT = 8
local PRIZE_STATE_OFFSET = 0x00
local PRIZE_ENTITY_OFFSET = 0x08
local PRIZE_SERIAL_OFFSET = 0x38
local PRIZE_STATE_PICKED_UP = 2
local TAGS_KEY = "kh1_one_time_drops_tags_v1"
local TAG_SIZE = 0x18
local TAG_FLAG_ADDRESS_OFFSET = 0x00
local TAG_FLAG_BIT_OFFSET = 0x08
local TAG_SERIAL_OFFSET = 0x0C
local TAG_ACTIVE_OFFSET = 0x10

local locations = {}
local tags_block = nil
local pending_location = nil
local installed = false
local original_spawn_enemy_drops = nil
local original_spawn_prize = nil

local function log(message)
    native.log_debug("[one_time_drops] " .. message)
end

local function name_matches(address, name)
    local bytes = ReadArray(address, #name + 1, true)
    for index = 1, #name do
        if bytes[index] ~= string.byte(name, index) then
            return false
        end
    end
    return bytes[#name + 1] == 0
end

local function find_location(drop_table)
    for _, location in ipairs(locations) do
        for _, source in ipairs(location.tables) do
            local name_address = drop_table - source.offset + BD_NAME_OFFSET
            if name_matches(name_address, source.bd_name) then
                return location
            end
        end
    end
    return nil
end

local function open_tags()
    if tags_block == nil then
        local block = native.persistent_block(TAGS_KEY, TAG_SIZE * PRIZE_SLOT_COUNT)
        if block ~= 0 then
            tags_block = block
        end
    end
    return tags_block ~= nil
end

local function tag_address(slot_index)
    return tags_block + slot_index * TAG_SIZE
end

local function tag_is_active(slot_index)
    return ReadInt(tag_address(slot_index) + TAG_ACTIVE_OFFSET, true) == 1
end

local function clear_tag(slot_index)
    WriteInt(tag_address(slot_index) + TAG_ACTIVE_OFFSET, 0, true)
end

local function write_tag(slot_index, location, serial)
    local address = tag_address(slot_index)
    WriteLong(address + TAG_FLAG_ADDRESS_OFFSET, location.flag_address, true)
    WriteInt(address + TAG_FLAG_BIT_OFFSET, location.flag_bit, true)
    WriteInt(address + TAG_SERIAL_OFFSET, serial, true)
    WriteInt(address + TAG_ACTIVE_OFFSET, 1, true)
end

local function flag_mask(flag_bit)
    return 1 << (flag_bit - 1)
end

local function is_collected(location)
    local flags = ReadByte(location.flag_address)
    return (flags & flag_mask(location.flag_bit)) ~= 0
end

local function mark_collected(flag_address, flag_bit)
    local flags = ReadByte(flag_address)
    WriteByte(flag_address, flags | flag_mask(flag_bit))
end

local function is_on_ground(location)
    for slot_index = 0, PRIZE_SLOT_COUNT - 1 do
        if tag_is_active(slot_index) then
            local address = tag_address(slot_index)
            local flag_address = ReadLong(address + TAG_FLAG_ADDRESS_OFFSET, true)
            local flag_bit = ReadInt(address + TAG_FLAG_BIT_OFFSET, true)
            if flag_address == location.flag_address and flag_bit == location.flag_bit then
                return true
            end
        end
    end
    return false
end

local function prize_slot_address(slot_index)
    return g_prize_pool + slot_index * PRIZE_SLOT_SIZE
end

local function check_pickups()
    if not open_tags() then
        return
    end
    for slot_index = 0, PRIZE_SLOT_COUNT - 1 do
        if tag_is_active(slot_index) then
            local address = tag_address(slot_index)
            local slot_address = prize_slot_address(slot_index)
            local serial = ReadInt(slot_address + PRIZE_SERIAL_OFFSET)
            local state = ReadInt(slot_address + PRIZE_STATE_OFFSET)
            local entity = ReadLong(slot_address + PRIZE_ENTITY_OFFSET)
            if serial ~= ReadInt(address + TAG_SERIAL_OFFSET, true) then
                clear_tag(slot_index)
            elseif state == PRIZE_STATE_PICKED_UP then
                mark_collected(ReadLong(address + TAG_FLAG_ADDRESS_OFFSET, true), ReadInt(address + TAG_FLAG_BIT_OFFSET, true))
                clear_tag(slot_index)
            elseif entity == 0 then
                clear_tag(slot_index)
            end
        end
    end
end

local function tag_new_prize(location)
    local new_serial = ReadInt(g_prize_serial) - 1
    for slot_index = 0, PRIZE_SLOT_COUNT - 1 do
        local slot_address = prize_slot_address(slot_index)
        local serial = ReadInt(slot_address + PRIZE_SERIAL_OFFSET)
        local entity = ReadLong(slot_address + PRIZE_ENTITY_OFFSET)
        if serial == new_serial and entity ~= 0 then
            write_tag(slot_index, location, new_serial)
            return
        end
    end
end

local function find_empty_drop_slot(drop_table)
    for slot = 0, DROP_SLOT_COUNT - 1 do
        local item = ReadInt(drop_table + DROP_ITEM_OFFSET + slot * DROP_SLOT_STRIDE, true)
        if item == 0 then
            return slot
        end
    end
    return nil
end

local function handle_spawn_enemy_drops(drop_table, position, mode)
    check_pickups()
    local table_address = cffi.tonumber(drop_table)
    local location = find_location(table_address)
    if location == nil or is_collected(location) or is_on_ground(location) then
        return original_spawn_enemy_drops(drop_table, position, mode)
    end
    local slot = find_empty_drop_slot(table_address)
    if slot == nil then
        log(string.format("no empty item slot in the drop table at 0x%X", table_address))
        return original_spawn_enemy_drops(drop_table, position, mode)
    end
    local chance_address = table_address + DROP_CHANCE_OFFSET + slot * DROP_SLOT_STRIDE
    local item_address = table_address + DROP_ITEM_OFFSET + slot * DROP_SLOT_STRIDE
    local previous_chance = ReadInt(chance_address, true)
    WriteInt(chance_address, GUARANTEED_CHANCE, true)
    WriteInt(item_address, location.item, true)
    pending_location = location
    local result = original_spawn_enemy_drops(drop_table, position, mode)
    pending_location = nil
    WriteInt(chance_address, previous_chance, true)
    WriteInt(item_address, 0, true)
    return result
end

local function spawn_enemy_drops(drop_table, position, mode)
    local ok, result = pcall(handle_spawn_enemy_drops, drop_table, position, mode)
    if ok then
        return result
    end
    pending_location = nil
    log("spawn_enemy_drops error: " .. tostring(result))
    return original_spawn_enemy_drops(drop_table, position, mode)
end

local function handle_spawn_prize(item, position, mode)
    check_pickups()
    local location = pending_location
    if location == nil or item ~= location.item then
        return original_spawn_prize(item, position, mode)
    end
    pending_location = nil
    local result = original_spawn_prize(item, position, mode)
    tag_new_prize(location)
    return result
end

local function spawn_prize(item, position, mode)
    local ok, result = pcall(handle_spawn_prize, item, position, mode)
    if ok then
        return result
    end
    log("spawn_prize error: " .. tostring(result))
    return original_spawn_prize(item, position, mode)
end

local function addresses_known()
    return fnc_spawn_enemy_drops ~= nil
        and fnc_spawn_enemy_drops_jump_bd_7a ~= nil
        and fnc_spawn_enemy_drops_jump_bd_d7 ~= nil
        and fnc_spawn_prize ~= nil
        and fnc_spawn_prize_call_enemy_drops ~= nil
        and g_prize_pool ~= nil
        and g_prize_serial ~= nil
end

local function install()
    if installed then
        return true
    end
    if not addresses_known() then
        return false
    end
    if not open_tags() then
        return false
    end
    original_spawn_enemy_drops = cffi.cast("OneTimeDropsSpawnEnemyDrops", BASE_ADDR + fnc_spawn_enemy_drops)
    original_spawn_prize = cffi.cast("OneTimeDropsSpawnPrize", BASE_ADDR + fnc_spawn_prize)
    local redirected_sites = {}
    local sites = {
        { fnc_spawn_enemy_drops_jump_bd_7a, fnc_spawn_enemy_drops, "OneTimeDropsSpawnEnemyDrops", spawn_enemy_drops },
        { fnc_spawn_enemy_drops_jump_bd_d7, fnc_spawn_enemy_drops, "OneTimeDropsSpawnEnemyDrops", spawn_enemy_drops },
        { fnc_spawn_prize_call_enemy_drops, fnc_spawn_prize, "OneTimeDropsSpawnPrize", spawn_prize },
    }
    for _, site in ipairs(sites) do
        if not call_redirect.redirect_call(site[1], site[2], site[3], site[4]) then
            log(string.format("could not redirect the call at 0x%X", site[1]))
            for _, redirected_site in ipairs(redirected_sites) do
                call_redirect.restore(redirected_site)
            end
            return false
        end
        redirected_sites[#redirected_sites + 1] = site[1]
    end
    installed = true
    return true
end

local function set_one_time_drops(new_locations)
    if not install() then
        return false
    end
    locations = new_locations
    return true
end

local function update_one_time_drops()
    if g_prize_pool == nil then
        return
    end
    check_pickups()
end

return {
    set_one_time_drops = set_one_time_drops,
    update_one_time_drops = update_one_time_drops,
}
