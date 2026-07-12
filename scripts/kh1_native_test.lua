---@diagnostic disable: undefined-global
LUAGUI_NAME = "kh1_native_test"
LUAGUI_AUTH = "test"
LUAGUI_DESC = "Manual test trigger for kh1_native.dll / spawn_prize -- hold L1+R1+R2 to spawn a Potion"

local kh1_lib = nil
local wasPressed = false

function _OnInit()
	if GAME_ID == 0xAF71841E and ENGINE_TYPE == "BACKEND" then
		require("VersionCheck")
		kh1_lib = require("kh1_lua_library")
		ConsolePrint("kh1_native_test loaded - hold L1+R1+R2 to spawn_prize(1)")
	else
		ConsolePrint("KH1 not detected, not running script")
	end
end

function _OnFrame()
	if kh1_lib == nil then return end

	local pressed = kh1_lib.is_pressed({"L1", "R1", "R2"}, true)
	if pressed and not wasPressed then
		local ok = kh1_lib.spawn_prize(1)
		ConsolePrint("spawn_prize(1) returned: " .. tostring(ok))
	end
	wasPressed = pressed
end
