---@diagnostic disable: undefined-global

local cffi = require("cffi")
local native = require("helpers.native")
local game = require("modules.menus.game")

local DESCRIPTOR_SIZE = 0x30
local DESCRIPTOR_COLUMN_WIDTH = 0x20
local DESCRIPTOR_ROW_COUNT = 0x26
local DESCRIPTOR_VISIBLE_ROWS = 0x28
local WINDOW_LAYER = 6
local WINDOW_CALLBACK = 0x08
local WINDOW_FLAGS = 0x68
local WINDOW_DESCRIPTOR = 0x80
local WINDOW_IN_USE = 0xB8
local WINDOW_CURSOR = 0xFE
local WINDOW_CLOSING_FLAG = 1
local WINDOW_POOL_COUNT = 24
local WINDOW_SIZE = 0x118
local CURSOR_ROW_STEP = 0x10
local RETURN_STUB_SLOT_NAME = "menus_return_stub"
local RETURN_STUB_SIZE = 16
local RETURN_OPCODE = 0xC3

local windows = {}

local owned_descriptors = {}
local return_stub = 0
local release_when_lua_closes = nil

function windows.copy_descriptor(slot_name, source, callback)
    local descriptor = native.persistent_block(slot_name, DESCRIPTOR_SIZE)
    if descriptor == 0 then
        return 0
    end
    WriteArray(descriptor, ReadArray(source, DESCRIPTOR_SIZE), true)
    WriteLong(descriptor, game.to_integer(callback), true)
    owned_descriptors[descriptor] = true
    return descriptor
end

function windows.set_column_width(descriptor, width)
    WriteShort(descriptor + DESCRIPTOR_COLUMN_WIDTH, width, true)
end

function windows.set_row_count(descriptor, count)
    WriteShort(descriptor + DESCRIPTOR_ROW_COUNT, count, true)
    WriteShort(descriptor + DESCRIPTOR_VISIBLE_ROWS, count, true)
end

function windows.open_child(parent, descriptor)
    local window = cffi.tonumber(game.create_window(descriptor, WINDOW_LAYER))
    if window == 0 then
        return 0
    end
    game.set_child(parent, window)
    game.set_parent(window, parent)
    return window
end

function windows.set_cursor_row(window, row)
    WriteShort(window + WINDOW_CURSOR, row * CURSOR_ROW_STEP, true)
end

local function make_return_stub()
    local slot = native.persistent_slot(RETURN_STUB_SLOT_NAME)
    if slot == 0 then
        return 0
    end
    if native.read_slot(slot) == 0 then
        local stub_address = native.allocate(RETURN_STUB_SIZE, true)
        if stub_address == 0 then
            return 0
        end
        WriteByte(stub_address, RETURN_OPCODE, true)
        native.write_slot(slot, stub_address)
    end
    return native.read_slot(slot)
end

local function release_window(window)
    local descriptor = ReadLong(window + WINDOW_DESCRIPTOR, true)
    if descriptor == BASE_ADDR + g_MainMenuDescriptor then
        WriteLong(window + WINDOW_CALLBACK, BASE_ADDR + fnc_main_menu_update, true)
    elseif descriptor == BASE_ADDR + g_MainMenuHelpDescriptor then
        WriteLong(window + WINDOW_CALLBACK, BASE_ADDR + fnc_main_menu_help_update, true)
    elseif owned_descriptors[descriptor] then
        WriteLong(window + WINDOW_CALLBACK, return_stub, true)
        WriteInt(window + WINDOW_FLAGS, ReadInt(window + WINDOW_FLAGS, true) | WINDOW_CLOSING_FLAG, true)
    end
end

local function release_menu_windows()
    local pool = BASE_ADDR + g_MenuWindowPool
    for index = 0, WINDOW_POOL_COUNT - 1 do
        local window = pool + index * WINDOW_SIZE
        if ReadInt(window + WINDOW_IN_USE, true) ~= 0 then
            release_window(window)
        end
    end
end

function windows.install()
    if g_MenuWindowPool == nil then
        return false
    end
    return_stub = make_return_stub()
    if return_stub == 0 then
        return false
    end
    if release_when_lua_closes == nil then
        release_when_lua_closes = setmetatable({}, { __gc = release_menu_windows })
    end
    return true
end

return windows
