---@diagnostic disable: undefined-global

-- Level-up prompt feature: shows custom text in the game's level-up box, using its own box queue
-- so more than one can be on screen at once. Credits to Topaz for the original technique.

local kh1_native = require("kh1_native")
local khscii = require("helpers.khscii")

local GetKHSCII = khscii.GetKHSCII

local function partyActorRVA(z)
    -- Gets the RVA of the Sora, Donald, or Goofy entity/actor pointer
    if z == 1 then return soraPointer end
    if z == 2 then return donaldPointer end
    return goofyPointer
end

local promptBodyRingPos = {0, 0, 0}

local function ClampKHSCII(khscii_bytes, maxBytes)
    -- Clamps KHSCII data so it isn't too long for level up prompts
    if #khscii_bytes <= maxBytes then
        return khscii_bytes
    end
    local clamped = {}
    for i = 1, maxBytes - 1 do
        clamped[i] = khscii_bytes[i]
    end
    clamped[maxBytes] = 0x00
    return clamped
end

local function show_prompt(input_title, input_party, duration, colour)
    if colour == nil then
        colour = 0
    end

    local allOk = true

    for z = 1, 3 do
        local _boxArray = input_party[z]
        if _boxArray then
            local actor_ptr = GetPointer(partyActorRVA(z))
            if actor_ptr == 0 or ReadInt(actor_ptr + 0x130, true) == 0 then
                allOk = false
            else
                local _titleAddress = textMemory + 0x20 * (z - 1)
                local ringPos = promptBodyRingPos[z]
                promptBodyRingPos[z] = (ringPos + 1) % 5
                local _textAddress = (textMemory + 0x70) + (0x140 * (z - 1)) + (0x40 * ringPos)

                if input_title[z] then
                    WriteArray(_titleAddress, ClampKHSCII(GetKHSCII(input_title[z]), 0x20))
                end
                WriteArray(_textAddress, ClampKHSCII(GetKHSCII(_boxArray[1]), 0x20))
                local line1_ptr = BASE_ADDR + _textAddress
                local line2_ptr = 0
                if _boxArray[2] then
                    WriteArray(_textAddress + 0x20, ClampKHSCII(GetKHSCII(_boxArray[2]), 0x20))
                    line2_ptr = BASE_ADDR + _textAddress + 0x20
                end

                local ok = kh1_native.call_function(fnc_enqueue_levelup_prompt, actor_ptr, line1_ptr, line2_ptr)
                if not ok then
                    allOk = false
                else
                    local found = nil
                    for slot = 0, 2 do
                        for q = 0, 4 do
                            local boxAddr = boxMemory + slot * 0x3A20 + q * 0xBA0
                            if ReadLong(boxAddr + 0x20) == line1_ptr then
                                found = boxAddr
                                break
                            end
                        end
                        if found then break end
                    end

                    if found then
                        WriteLong(found + 0x30, BASE_ADDR + _titleAddress)
                        WriteLong(found + 0xB88, BASE_ADDR + colorBox + colour)
                        WriteLong(found + 0xB90, BASE_ADDR + colorText + colour)
                    else
                        allOk = false
                    end
                end
            end
        end
    end

    return allOk
end

return {
    show_prompt = show_prompt,
}
