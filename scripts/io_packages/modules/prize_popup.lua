---@diagnostic disable: undefined-global

-- Prize pickup popup feature: show_custom_item_popup forces a map-prize pickup box with our text.
-- The popup's item-name lookup is redirected to Lua; update_prize_popup clears the text on close.

local cffi = require("cffi")
local native = require("helpers.native")
local call_redirect = require("helpers.call_redirect")
local khscii = require("helpers.khscii")

local GetKHSCII = khscii.GetKHSCII

cffi.cdef[[
    typedef uint64_t (*PrizePopupResolveName)(uint64_t first, uint64_t second, uint64_t third, uint64_t fourth);
]]

local STATE_KEY  = "kh1_popup_state_v2"
local STATE_SIZE = 0x220
local OFF_FLAG = 0x00
local OFF_PREVIOUS_POPUP_STATE = 0x04
local OFF_BUF  = 0x20
local BUF_MAX  = 511

local state = nil
local installed = false
local original_resolve_name = nil

local function open_state()
    if state == nil then
        local block = native.persistent_block(STATE_KEY, STATE_SIZE)
        if block ~= 0 then
            state = block
        end
    end
    return state ~= nil
end

local function custom_name(first, second, third, fourth)
    local name = original_resolve_name(first, second, third, fourth)
    if ReadByte(state + OFF_FLAG, true) == 1 then
        return state + OFF_BUF
    end
    return name
end

local function install()
    if installed then
        return true
    end
    if fnc_item_popup_name_call == nil or fnc_resolve_item_name_ptr == nil or g_item_popup_state == nil then
        return false
    end
    if not open_state() then
        return false
    end
    original_resolve_name = cffi.cast("PrizePopupResolveName", BASE_ADDR + fnc_resolve_item_name_ptr)
    installed = call_redirect.redirect_call(fnc_item_popup_name_call, fnc_resolve_item_name_ptr,
        "PrizePopupResolveName", custom_name)
    return installed
end

local function set_custom_popup_text(text)
    local khs = GetKHSCII(text)
    local n = math.min(#khs, BUF_MAX)
    local bytes = {}
    for i = 1, n do bytes[i] = khs[i] end
    bytes[n + 1] = 0
    WriteArray(state + OFF_BUF, bytes, true)
    WriteByte(state + OFF_FLAG, 1, true)
end

-- ######################## --
-- # Public                # --
-- ######################## --

local function show_custom_item_popup(text)
    --[[ Uses the in game function to force a map prize pickup box, with
    the item name lookup swapped for our text.]]
    if not install() then return false end
    set_custom_popup_text(text)
    return native.call_function(fnc_show_item_message, 1, 1)
end

local function update_prize_popup()
    --[[ Clears the custom text once the popup closes. kh1_text_box_driver.lua calls this every frame.]]
    if g_item_popup_state == nil then return end
    if not open_state() then return end
    local popup_state = ReadInt(g_item_popup_state)
    local previous_popup_state = ReadInt(state + OFF_PREVIOUS_POPUP_STATE, true)
    if previous_popup_state ~= 0 and popup_state == 0 then
        WriteByte(state + OFF_FLAG, 0, true)
    end
    WriteInt(state + OFF_PREVIOUS_POPUP_STATE, popup_state, true)
end

return {
    show_custom_item_popup = show_custom_item_popup,
    update_prize_popup = update_prize_popup,
}
