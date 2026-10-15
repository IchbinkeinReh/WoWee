-- A macro button as an addon makes one (framexml_taint_play): created by
-- insecure code from the interface's secure template and configured out of
-- combat, which is how every action bar addon of the day was built. Its click
-- is the template's, and must reach RunMacroText secure.
forceinsecure()
local b = CreateFrame("Button", "TaintPlayMacroButton", UIParent, "SecureActionButtonTemplate")
b:SetWidth(40)
b:SetHeight(40)
b:SetPoint("CENTER", UIParent, "CENTER", 0, 200)
b:RegisterForClicks("AnyUp")
b:SetAttribute("type", "macro")
b:SetAttribute("macrotext", "/cast Fireball")
b:Show()
