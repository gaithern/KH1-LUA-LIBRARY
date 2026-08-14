---@diagnostic disable: undefined-global
-- ################################################################################################### --
-- #  ___________                              _________                                             # --
-- #  \_   _____/ ____   ____   _____ ___.__. /   _____/_____   ____  _  ______     ______           # --
-- #   |    __)_ /    \_/ __ \ /     <   |  | \_____  \\____ \ /  _ \ \/ \/ /    \   /  ___/          # --
-- #   |        \   |  \  ___/|  Y Y  \___  | /        \  |_> >  <_> )     /   |  \  \___ \           # --
-- #  /_______  /___|  /\___  >__|_|  / ____|/_______  /   __/ \____/ \/\_/|___|  / /____  >          # --
-- #          \/     \/     \/      \/\/             \/|__|                     \/       \/           # --
-- ################################################################################################### --

--[[
    Enemy spawning for KH1, split out of kh1_lua_library.lua so the feature stands on its own.

    Use it either way round:

        local spawns = require("kh1_lua_library.enemy_spawns")
        spawns.spawn_enemy(model_path, motion_path, x, y, z, callback)

    ...or through the main library, which re-exports both public functions
    unchanged (`kh1.spawn_enemy` / `kh1.update_spawn_enemy`).

    Whichever you use, update_spawn_enemy() must be called every frame from your
    own script's _OnFrame -- spawn_enemy() only queues the request.
--]]

-- Native module (native/KH1Native) that lets Lua call real game functions.
local kh1_native = require("kh1_native")

-- Per-creature char-id/weight/template data extracted offline from room .ard
-- files, so spawn_enemy works without visiting the creature's native room.
local kh1_creature_data = require("kh1_lua_library.creature_data")

--[[kh1_lua_library is resolved on first use rather than at load time: the
library requires this module so it can re-export us, so requiring it back up
here would be a load-time cycle (it hasn't finished loading, so it isn't in
package.loaded yet). By the time any of these functions actually run, the
library is fully loaded and require() just returns the cached table.]]
local kh1 = nil
local function lib()
    kh1 = kh1 or require("kh1_lua_library")
    return kh1
end

--[[Human text for each short refusal code kh1_native.spawn_enemy returns.
Written here in Lua because long native-pushed strings often arrive empty
or truncated -- only the short code survives the boundary reliably.]]
local SPAWN_FAILURE_MESSAGES = {
    bad_args          = "spawn_enemy: model_path/motion_path are required",
    unconfigured      = "spawn_enemy: this game build is missing an address this feature needs",
    handles_full      = "spawn_enemy: too many distinct resources loaded this session -- restart the game to clear them",
    table_invalid     = "spawn_enemy: the room isn't in a spawnable state right now -- try again in a moment",
    no_creature_data  = "spawn_enemy: no offline data for this creature (missing from kh1_lua_library/creature_data.lua)",
    hook_install_fail = "spawn_enemy: the safety hooks failed to install -- refusing rather than spawn unprotected",
    slot_collision    = "spawn_enemy: that creature's slot is already used by a different creature in this room",
    -- Temporary, unlike handles_full above: slots come back on defeats/room changes.
    slots_full        = "spawn_enemy: no creature slots free right now -- each creature type claims several. They come back as spawned enemies are defeated, or when you change rooms.",
    alloc_fail        = "spawn_enemy: out of memory building the placement table",
    run_slot_taken    = "spawn_enemy: this creature needs several consecutive slots and one of them is already in use",
    load_threw        = "spawn_enemy: the asset load threw an exception",
    blob_invalid      = "spawn_enemy: this creature's resource data isn't valid in this room right now -- try again after re-entering the room",
    ctor_threw        = "spawn_enemy: the game's own constructor threw an exception",
    ctor_refused      = "spawn_enemy: the room's concurrent-enemy budget is full right now -- try again after some are defeated",
    -- Deliberate per-room block (g_spawnRoomBlacklist in dllmain.cpp), not a bug.
    room_blacklisted  = "spawn_enemy: enemy spawning is turned off in this room",
}

local function describe_spawn_failure(code, native_message)
    --[[Turns a native refusal code into displayable text, falling back
    progressively since `code` itself may not survive the boundary.]]
    -- slots_full carries its numbers in the code: "slots_full/<needed>/<longest free run>".
    if code then
        local needed, longest = code:match("^slots_full/(%d+)/(%d+)$")
        if needed then
            return string.format(
                "spawn_enemy: no creature slots free right now -- this creature needs %s "
                .. "consecutive free slots, and we currently only have %s", needed, longest)
        end
    end
    local text = code and SPAWN_FAILURE_MESSAGES[code]
    if text then return text end
    if code and code ~= "" then
        return "spawn_enemy: refused (" .. code .. ") -- see kh1_native.log for the full reason"
    end
    if native_message and native_message ~= "" then return native_message end
    return "spawn_enemy: refused, reason did not survive the native/Lua boundary -- see kh1_native.log"
end

local function spawn_enemy_attempt(model_path, motion_path, x, y, z)
    --[[Single-attempt primitive behind spawn_enemy; x/y/z default to Sora's
    position. Can return stillLoading -- use spawn_enemy instead of this.]]

    -- Don't spawn mid-cutscene
    if ReadInt(inCutscene) ~= 0 then
        return false, "in cutscene", "cutscene"
    end

    --[[Don't spawn in the Gummi ship -- a creature surviving the transition out
    reads a wiped resource blob and crashes (reproduced live).]]
    if lib().is_in_gummi_garage() then
        return false, "in gummi ship", "gummi"
    end

    if x == nil or y == nil or z == nil then
        local pos = lib().get_sora_pos()
        x = x or pos["X"]
        y = y or pos["Y"]
        z = z or pos["Z"]
    end

    local charId, weight, template = 0, 0, nil
    local known = kh1_creature_data[model_path]
    if known then
        charId = known.charId
        weight = known.weight
        template = known.template
    end
    -- 30 positional args -- must stay in step with l_spawn_enemy's parameter reads.
    return kh1_native.spawn_enemy(fnc_spawn_world_gimmick_entity, placementTablePtr, placementTableCount,
        fnc_load_gimmick_assets, loadedSpeciesPtrTable, fnc_mint_resource_handle, fnc_resolve_resource_handle,
        speciesResourceTable, model_path, motion_path, x, y, z,
        g_WorldNumber, g_AreaNumber, g_SetNumber,
        fnc_iterate_live_entities, charId, weight, template,
        entityPoolBase, soraPointer, g_SoraObjPtr, g_PartyMember1ObjectPtr, g_PartyMember2ObjPtr,
        resource_handle_bucket_table,
        g_GameSpeed, g_BossDefeatEffect, g_BossDefeatEffectStart,
        text_slot_table_base,
        fnc_claim_species_slot_run,
        fnc_release_species_slot_run)
end

-- Would spawn_enemy refuse right now? Returns "ready"/"retry"/"unavailable", code, message
-- without spawning. No model_path = cheap gates-only check. See the README.
local function can_spawn_enemy(model_path)
    -- Cheap Lua-side gates first, matching spawn_enemy's own front door.
    if ReadInt(inCutscene) ~= 0 then
        return "retry", "cutscene", "a cutscene is playing"
    end
    if lib().is_in_gummi_garage() then
        return "retry", "gummi", "the player is in the Gummi ship"
    end

    local charId, template = 0, nil
    local known = model_path and kh1_creature_data[model_path]
    if known then
        charId = known.charId
        template = known.template
    end

    -- 20 positional args -- must stay in step with l_spawn_enemy_precheck's parameter reads.
    return kh1_native.spawn_enemy_precheck(
        model_path, charId, template,
        g_GameSpeed, g_BossDefeatEffect, g_BossDefeatEffectStart,
        g_WorldNumber, g_AreaNumber, g_SetNumber,
        resource_handle_bucket_table,
        placementTablePtr, placementTableCount,
        loadedSpeciesPtrTable, speciesResourceTable,
        fnc_iterate_live_entities, fnc_resolve_resource_handle,
        text_slot_table_base,
        fnc_mint_resource_handle, fnc_load_gimmick_assets,
        model_path == nil and 1 or 0)
end

-- Pending spawn_enemy requests
local pending_spawn_requests = {}
local next_spawn_request_id = 1

-- Queues a spawn; drive it with update_spawn_enemy() every frame. Ignored in cutscenes/Gummi.
-- callback(ok, message, raw_native_message, code) -- code is the short refusal code, nil on success.
local function spawn_enemy(model_path, motion_path, x, y, z, callback)
    if ReadInt(inCutscene) ~= 0 then
        if callback then
            callback(false, "spawn_enemy: ignored, player is in a cutscene", nil, "cutscene")
        end
        return
    end

    -- See spawn_enemy_attempt's own Gummi comment for why this is refused
    -- rather than queued.
    if lib().is_in_gummi_garage() then
        if callback then
            callback(false, "spawn_enemy: ignored, player is in the Gummi ship", nil, "gummi")
        end
        return
    end

    local id = next_spawn_request_id
    next_spawn_request_id = next_spawn_request_id + 1
    pending_spawn_requests[id] = {
        model_path = model_path,
        motion_path = motion_path,
        x = x, y = y, z = z,
        callback = callback,
        deadline = os.clock() + 10.0,
    }
end

--[[Live entity census (kh1_native.census_live_entities, still exported) was run
once and removed: it proved the engine does NOT retire old-room creatures before
evicting species slots, but polling it on a timer froze the game.]]

local function update_spawn_enemy()
    --[[Drives every pending spawn_enemy request one step forward. Call this
    every frame from your own script's _OnFrame.]]

    for id, req in pairs(pending_spawn_requests) do
        -- 4th value is the short refusal code (see SPAWN_FAILURE_MESSAGES).
        local ok, result, stillLoading, code = spawn_enemy_attempt(req.model_path, req.motion_path, req.x, req.y, req.z)
        if stillLoading then
            if stillLoading == "cutscene" then
                -- Cutscene started after queueing -- drop rather than wait it out.
                pending_spawn_requests[id] = nil
                if req.callback then
                    req.callback(false, "spawn_enemy: ignored, player entered a cutscene while this request was pending",
                                 nil, "cutscene")
                end
            elseif stillLoading == "gummi" then
                -- Boarded the Gummi ship after queueing -- same reasoning as above.
                pending_spawn_requests[id] = nil
                if req.callback then
                    req.callback(false, "spawn_enemy: ignored, player boarded the Gummi ship while this request was pending",
                                 nil, "gummi")
                end
            elseif os.clock() >= req.deadline then
                pending_spawn_requests[id] = nil
                if req.callback then
                    req.callback(false, "spawn_enemy: timed out waiting for asset load", nil, "timeout")
                end
            end
            -- else: still within budget, leave it queued for next frame.
        else
            pending_spawn_requests[id] = nil
            if req.callback then
                if ok then
                    -- result is the spawned entity's pointer.
                    req.callback(true, result)
                else
                    -- Lua-authored text (see SPAWN_FAILURE_MESSAGES); the raw native
                    -- message is still passed third, the short code fourth.
                    req.callback(false, describe_spawn_failure(code, result), result, code)
                end
            end
        end
    end
end

return {
    spawn_enemy = spawn_enemy,
    update_spawn_enemy = update_spawn_enemy,
    can_spawn_enemy = can_spawn_enemy,
}
