---@diagnostic disable: undefined-global

-- Shortens the summon opener: skips the camera swing, fade out and fade in calls and the first three step durations.
-- The summon entrance itself is skipped by edited xs_*.dat event scripts, which the mod using this ships.

local native = require("helpers.native")

local SAVED_KEY = "kh1_skip_summon_animations_v1"
local SAVED_SIZE = 0x20
local SAVED_FLAG = 0x00
local SAVED_CALLS = 0x08
local CALL_LENGTH = 5
local CALL_OPCODE = 0xE8
local NOP5 = { 0x0F, 0x1F, 0x44, 0x00, 0x00 }
local STEP_DURATIONS = { 60.0, 30.0, 60.0 }
local STEP_STRIDE = 16
local SKIPPED_STEP_DURATION = 1.0

local function call_sites()
    return { fnc_summon_camera_swing_call, fnc_summon_fade_out_call, fnc_summon_fade_in_call }
end

local function same_bytes(first, second)
    for i = 1, CALL_LENGTH do
        if first[i] ~= second[i] then
            return false
        end
    end
    return true
end

local function save_original_calls(saved, sites)
    for i = 1, #sites do
        local bytes = ReadArray(sites[i], CALL_LENGTH)
        if bytes[1] ~= CALL_OPCODE then
            return false
        end
        WriteArray(saved + SAVED_CALLS + (i - 1) * CALL_LENGTH, bytes, true)
    end
    WriteByte(saved + SAVED_FLAG, 1, true)
    return true
end

local function original_calls()
    local sites = call_sites()
    local saved = native.persistent_block(SAVED_KEY, SAVED_SIZE)
    if saved == 0 then
        return nil
    end
    if ReadByte(saved + SAVED_FLAG, true) ~= 1 then
        if not save_original_calls(saved, sites) then
            return nil
        end
    end
    local calls = {}
    for i = 1, #sites do
        local original = ReadArray(saved + SAVED_CALLS + (i - 1) * CALL_LENGTH, CALL_LENGTH, true)
        local current = ReadArray(sites[i], CALL_LENGTH)
        if not same_bytes(current, original) and not same_bytes(current, NOP5) then
            return nil
        end
        calls[i] = { address = sites[i], bytes = original }
    end
    return calls
end

local function set_skip_summon_animations(turn_on)
    if g_SummonStepTable == nil or fnc_summon_camera_swing_call == nil
        or fnc_summon_fade_out_call == nil or fnc_summon_fade_in_call == nil then
        return false
    end
    local calls = original_calls()
    if calls == nil then
        return false
    end
    for i = 1, #STEP_DURATIONS do
        local duration = STEP_DURATIONS[i]
        if turn_on then
            duration = SKIPPED_STEP_DURATION
        end
        WriteFloat(g_SummonStepTable + (i - 1) * STEP_STRIDE, duration)
    end
    for i = 1, #calls do
        if turn_on then
            WriteArray(calls[i].address, NOP5)
        else
            WriteArray(calls[i].address, calls[i].bytes)
        end
    end
    return true
end

return {
    set_skip_summon_animations = set_skip_summon_animations,
}
