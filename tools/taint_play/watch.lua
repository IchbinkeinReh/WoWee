-- Watches ordinary play for the taint system refusing it (framexml_taint_play).
--
-- Run as the interface's own secure code, before the play: every refusal event
-- is kept, and each protected function the play should reach records whether
-- the code reaching it was secure. hooksecurefunc's hook runs after the
-- original under the taint the call ran under, which is what issecure() reads.
TaintPlay = { events = {}, insecure = {}, calls = {} }

local watcher = CreateFrame("Frame")
for _, event in ipairs({ "ADDON_ACTION_BLOCKED", "ADDON_ACTION_FORBIDDEN",
                         "MACRO_ACTION_BLOCKED", "MACRO_ACTION_FORBIDDEN" }) do
    watcher:RegisterEvent(event)
end
watcher:SetScript("OnEvent", function(_, event, who, what)
    table.insert(TaintPlay.events, event .. " " .. tostring(who) .. " " .. tostring(what))
end)

for _, name in ipairs({ "UseAction", "TargetUnit", "UseContainerItem",
                        "PickupContainerItem", "CastSpellByName", "RunMacroText",
                        "SpellStopCasting", "ClearTarget" }) do
    if type(_G[name]) == "function" then
        hooksecurefunc(name, function()
            TaintPlay.calls[name] = (TaintPlay.calls[name] or 0) + 1
            if not issecure() then
                table.insert(TaintPlay.insecure, name)
            end
        end)
    end
end

-- Whether a list of calls was reached, every one of them secure, and nothing
-- refused - with what went wrong raised, so the run shows it.
function TaintPlay_Clean(...)
    local bad = {}
    for _, e in ipairs(TaintPlay.events) do bad[#bad + 1] = e end
    for _, n in ipairs(TaintPlay.insecure) do bad[#bad + 1] = "insecure at " .. n end
    for i = 1, select("#", ...) do
        local n = select(i, ...)
        if not TaintPlay.calls[n] then bad[#bad + 1] = n .. " never reached" end
    end
    if #bad > 0 then error(table.concat(bad, "; "), 0) end
    return true
end
