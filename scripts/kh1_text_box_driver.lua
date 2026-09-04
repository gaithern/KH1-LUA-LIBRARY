---@diagnostic disable: undefined-global

LUAGUI_NAME = "KH1TextBoxDriver"
LUAGUI_AUTH = "Gicu"
LUAGUI_DESC = "Drives the shared text box queue (kh1_lua_library.queue_text_box)"

local kh1_lua_library = require("kh1_lua_library")

local ERROR_LOG_INTERVAL_FRAMES = 600
local error_count = 0

function _OnInit()
    if GAME_ID == 0xAF71841E and ENGINE_TYPE == "BACKEND" then
        require("VersionCheck")
    else
        ConsolePrint("KH1 not detected, not running text box driver")
    end
end

function _OnFrame()
    if not canExecute then return end
    local ok, err = pcall(kh1_lua_library.text_box_queue_frame)
    if ok then
        error_count = 0
        return
    end
    error_count = error_count + 1
    if error_count <= 3 or error_count % ERROR_LOG_INTERVAL_FRAMES == 0 then
        ConsolePrint("Text box driver error (x" .. error_count .. "): " .. tostring(err))
    end
end
