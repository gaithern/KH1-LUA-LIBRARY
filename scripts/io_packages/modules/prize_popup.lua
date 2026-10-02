---@diagnostic disable: undefined-global

-- Prize pickup popup feature: show_custom_item_popup forces a map-prize pickup box with our text.
-- The text hooks are C in hooks/prize_popup.c; this side fills the shared state block.

local kh1_native = require("kh1_native")
local khscii = require("helpers.khscii")

local GetKHSCII = khscii.GetKHSCII

-- Persistent state shared with hooks/prize_popup.c; layout must match it.
local STATE_KEY  = "kh1_popup_state_v1"
local STATE_SIZE = 0x220
local OFF_FLAG = 0x00   -- active flag byte (the tick hook clears it when the popup closes)
local OFF_BUF  = 0x20   -- 512-byte text buffer
local BUF_MAX  = 511

local function state_block()
    local blk = kh1_native.persistent_block(STATE_KEY, STATE_SIZE)
    if blk == 0 or blk == nil then return nil end
    return blk
end

local function set_custom_popup_text(text)
    local blk = state_block()
    if not blk then return end
    local khs = GetKHSCII(text)
    local n = math.min(#khs, BUF_MAX)
    local bytes = {}
    for i = 1, n do bytes[i] = khs[i] end
    bytes[n + 1] = 0
    WriteArray(blk + OFF_BUF, bytes, true)
    WriteByte(blk + OFF_FLAG, 1, true)
end

-- ######################## --
-- # Public                # --
-- ######################## --

local function show_custom_item_popup(text)
    --[[ Uses the in game function to force a map prize pickup box, with
    the native text hooks swapping in our text.]]
    if not kh1_native.install_c("hooks/prize_popup.c") then return false end
    set_custom_popup_text(text)
    return kh1_native.call_function(fnc_show_item_message, 1, 1)
end

return {
    show_custom_item_popup = show_custom_item_popup,
}
