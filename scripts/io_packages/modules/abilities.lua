---@diagnostic disable: undefined-global

local kh1_native = require("kh1_native")
local khscii = require("helpers.khscii")

local GetKHSCII = khscii.GetKHSCII

local FIRST_ID  = 0x42
local LAST_ID   = 0x7F
local ROW_SIZE  = 12
local TEXT_SIZE = 0x100

local function mint(addr)
    local ok, handle = kh1_native.call_function(fnc_mint_resource_handle, addr)
    if not ok then return 0 end
    return handle & 0xFFFFFFFF
end

local function register_ability(id, ap, sort, name, help)
    if type(id) ~= "number" or id < FIRST_ID or id > LAST_ID then return false, "id must be 0x42-0x7F" end
    local name_bytes, help_bytes = GetKHSCII(name or ""), GetKHSCII(help or "")
    if #name_bytes + #help_bytes > TEXT_SIZE then return false, "name and help too long" end
    local tbl = ReadLong(g_pAbilityTable)
    if tbl == 0 or ReadInt(tbl + (0x41 - 1) * ROW_SIZE + 4, true) == 0 then
        return false, "ability table not loaded"
    end
    local text = kh1_native.persistent_block(string.format("kh1_ability_text_%02X", id), TEXT_SIZE)
    if text == nil or text == 0 then return false, "no text block" end
    WriteArray(text, name_bytes, true)
    WriteArray(text + #name_bytes, help_bytes, true)
    local name_h, help_h = mint(text), mint(text + #name_bytes)
    if name_h == 0 or help_h == 0 then return false, "could not mint text handles" end
    local row = tbl + (id - 1) * ROW_SIZE
    WriteShort(row, ap or 0, true)
    WriteShort(row + 2, sort or 0, true)
    WriteInt(row + 4, name_h, true)
    WriteInt(row + 8, help_h, true)
    return true
end

return {
    register_ability = register_ability,
}
