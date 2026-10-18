---@diagnostic disable: undefined-global

-- Points one `call` instruction in the game at a Lua function instead of its usual target.
-- The called function is left untouched, so the Lua function can still call the original.

local cffi = require("cffi")
local native = require("helpers.native")

local CALL_OPCODE = 0xE8
local CALL_LENGTH = 5
local JUMP_STUB_LENGTH = 14
local JUMP_STUB_DESTINATION = 6

local redirects = {}

local function to_address(pointer)
    return cffi.tonumber(cffi.cast("uintptr_t", pointer))
end

local function to_signed_32(value)
    if value >= 0x80000000 then
        return value - 0x100000000
    end
    return value
end

local function to_bytes_32(value)
    if value < 0 then
        value = value + 0x100000000
    end
    local bytes = {}
    for i = 0, 3 do
        bytes[i + 1] = (value >> (8 * i)) & 0xFF
    end
    return bytes
end

local function call_target(call_site)
    local relative = to_signed_32(ReadInt(call_site + 1))
    return BASE_ADDR + call_site + CALL_LENGTH + relative
end

local function point_call_at(call_site, destination)
    local relative = destination - (BASE_ADDR + call_site + CALL_LENGTH)
    WriteArray(call_site + 1, to_bytes_32(relative))
end

local function stub_for(call_site)
    local slot = native.persistent_slot(string.format("call_redirect_stub_%X", call_site))
    if slot == 0 then
        return 0
    end
    if native.read_slot(slot) == 0 then
        local stub_address = native.allocate_near(BASE_ADDR + call_site, JUMP_STUB_LENGTH)
        if stub_address == 0 then
            return 0
        end
        local jump_to_address_below = { 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00 }
        WriteArray(stub_address, jump_to_address_below, true)
        native.write_slot(slot, stub_address)
    end
    return native.read_slot(slot)
end

local function restore(call_site)
    local redirect = redirects[call_site]
    if redirect == nil then
        return
    end
    redirects[call_site] = nil
    local stub_destination = ReadLong(redirect.stub_address + JUMP_STUB_DESTINATION, true)
    if stub_destination ~= to_address(redirect.callback) then
        return
    end
    point_call_at(call_site, BASE_ADDR + redirect.original_target)
end

local function redirect_call(call_site, expected_target, function_type, lua_function)
    if redirects[call_site] ~= nil then
        return true
    end
    if ReadByte(call_site) ~= CALL_OPCODE then
        return false
    end
    local stub_address = stub_for(call_site)
    if stub_address == 0 then
        return false
    end
    local current_target = call_target(call_site)
    if current_target ~= BASE_ADDR + expected_target and current_target ~= stub_address then
        return false
    end
    local callback = cffi.cast(function_type, lua_function)
    WriteLong(stub_address + JUMP_STUB_DESTINATION, to_address(callback), true)
    point_call_at(call_site, stub_address)
    redirects[call_site] = {
        original_target = expected_target,
        stub_address = stub_address,
        callback = callback,
        restore_when_lua_closes = setmetatable({}, { __gc = function() restore(call_site) end }),
    }
    return true
end

return {
    redirect_call = redirect_call,
    restore = restore,
}
