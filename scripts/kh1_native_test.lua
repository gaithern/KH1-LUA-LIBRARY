---@diagnostic disable: undefined-global
LUAGUI_NAME = "kh1_native_test"
LUAGUI_AUTH = "test"
LUAGUI_DESC = "Debug overlay driver for kh1_native.dll -- press F6 in-game to toggle the window"

local kh1_lib = nil
local kh1_native = nil

function _OnInit()
	if GAME_ID == 0xAF71841E and ENGINE_TYPE == "BACKEND" then
		require("VersionCheck")
		kh1_lib = require("kh1_lua_library")
		kh1_native = require("kh1_native")
		ConsolePrint("kh1_native debug overlay loaded - press F6 to toggle")
	else
		ConsolePrint("KH1 not detected, not running script")
	end
end

-- The overlay window runs on its own thread and can't safely call game
-- functions itself (see kh1_native's dllmain.cpp) -- it only queues a named
-- request, which we dispatch here through the real Lua functions every frame,
-- since this runs on the same thread the rest of this library's calls do.
function _OnFrame()
	if kh1_native == nil then return end

	-- Also drives the F6 show/hide toggle -- must be polled every frame
	-- regardless of whether an action is pending.
	local action = kh1_native.poll_debug_action()
	if action == nil then return end

	if action.action == "spawn_prize" then
		local ok = kh1_lib.spawn_prize(action.param1)
		kh1_native.set_debug_result("spawn_prize(" .. action.param1 .. ") = " .. tostring(ok))
	elseif action.action == "show_item_popup" then
		local ok = kh1_lib.show_item_popup(action.param1)
		kh1_native.set_debug_result("show_item_popup(" .. action.param1 .. ") = " .. tostring(ok))
	elseif action.action == "set_custom_popup_text" then
		kh1_lib.set_custom_item_popup_text(action.param_text)
		kh1_native.set_debug_result("custom popup text set to \"" .. action.param_text .. "\"")
	elseif action.action == "clear_custom_popup_text" then
		kh1_lib.clear_custom_item_popup_text()
		kh1_native.set_debug_result("custom popup text cleared")
	end
end
