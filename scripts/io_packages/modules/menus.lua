---@diagnostic disable: undefined-global

local native = require("helpers.native")
local game = require("modules.menus.game")
local windows = require("modules.menus.windows")
local keyboard = require("modules.menus.keyboard")
local list_view = require("modules.menus.list_view")
local item_menu = require("modules.menus.item_menu")
local pause_menu = require("modules.menus.pause_menu")
local title_menu = require("modules.menus.title_menu")

local OWNER_SLOT_NAME = "menus_pause_menu_owner"
local MAXIMUM_ROWS = 8
local REQUIRED_FUNCTIONS = {
    text = { "get", "set" },
    toggle = { "get", "set" },
    button = { "on_confirm" },
    list = {},
}

local menus = {}

local added = false
local owner_slot = 0
local owner_mark = 0
local release_owner_when_lua_closes = nil

local function mark_for(id)
    local mark = 0
    for i = 1, #id do
        mark = (mark * 31 + string.byte(id, i)) % 0x7FFFFFFF
    end
    return mark + 1
end

local function row_problem(row, index)
    local required = REQUIRED_FUNCTIONS[row.kind]
    if required == nil then
        return "row " .. index .. " has unknown kind " .. tostring(row.kind)
    end
    if type(row.label) ~= "string" then
        return "row " .. index .. " needs a label"
    end
    for _, name in ipairs(required) do
        if type(row[name]) ~= "function" then
            return "row " .. index .. " needs a " .. name .. " function"
        end
    end
    if row.kind == "text" and type(row.max_length) ~= "number" then
        return "row " .. index .. " needs a max_length"
    end
    if row.kind == "list" and row.rows == nil then
        return "row " .. index .. " needs rows"
    end
    return nil
end

local function definition_problem(definition)
    for _, name in ipairs({ "id", "label", "help", "title" }) do
        if type(definition[name]) ~= "string" then
            return "the definition needs a " .. name
        end
    end
    if type(definition.rows) ~= "table" or #definition.rows == 0 or #definition.rows > MAXIMUM_ROWS then
        return "the definition needs 1 to " .. MAXIMUM_ROWS .. " rows"
    end
    for index, row in ipairs(definition.rows) do
        local problem = row_problem(row, index)
        if problem ~= nil then
            return problem
        end
    end
    return nil
end

local function release_owner()
    if owner_slot ~= 0 and native.read_slot(owner_slot) == owner_mark then
        native.write_slot(owner_slot, 0)
    end
end

local function claim_owner(id)
    owner_slot = native.persistent_slot(OWNER_SLOT_NAME)
    if owner_slot == 0 then
        return false
    end
    owner_mark = mark_for(id)
    local current_owner = native.read_slot(owner_slot)
    if current_owner ~= 0 and current_owner ~= owner_mark then
        return false
    end
    native.write_slot(owner_slot, owner_mark)
    release_owner_when_lua_closes = setmetatable({}, { __gc = release_owner })
    return true
end

function menus.add_pause_menu_item(definition)
    if added then
        return false, "a pause menu item was already added by this script"
    end
    local problem = definition_problem(definition)
    if problem ~= nil then
        return false, problem
    end
    if not claim_owner(definition.id) then
        return false, "another script already added a pause menu item"
    end
    if not game.install() or not windows.install() or not keyboard.install() then
        return false, "this game version is missing menu addresses"
    end
    if not list_view.install(definition.id) or not item_menu.install(definition) then
        return false, "could not create the menu windows"
    end
    if not pause_menu.install(definition.label, definition.help, item_menu.header_descriptor(), item_menu.opener_address()) then
        return false, "could not hook the pause menu"
    end
    if definition.title_screen ~= nil then
        title_menu.install(definition.title_screen.key)
    end
    added = true
    return true, nil
end

function menus.frame()
    if added then
        game.run_safely("menus", title_menu.frame)
    end
end

return menus
