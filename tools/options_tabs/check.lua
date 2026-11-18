-- The options windows' category lists, clicked the way a player clicks them.
--
-- Run by the framexml_options_tabs test (CMakeLists.txt) through
-- framexml_run: each of the three frames the game menu opens is shown, and
-- every category button visible in its list is clicked. A click has to put
-- that button's panel up and mark the button selected - which is what failed
-- when OptionsList_OnLoad's CreateFrame("BUTTON", ...) left the buttons
-- without the OnClick their template gives them: each lit up under the
-- pointer and opened nothing, on every page of all three windows.

-- Which panels the frame's list has, and the buttons that show them.
local function visibleCategoryButtons(list)
    local out = {}
    if not list or not list.buttons then return out end
    for _, button in ipairs(list.buttons) do
        if button:IsVisible() and button.element then table.insert(out, button) end
    end
    return out
end

OptionsTabs_Failures = {}

local function fail(text)
    table.insert(OptionsTabs_Failures, text)
    print("OPTIONS TAB FAILED: " .. text)
end

-- Every visible category in one frame's list, clicked through Click(), which
-- runs the button's own OnClick with "LeftButton" as the mouse does. Opens
-- and closes the frame itself. The list is the frame's categoryFrame for
-- Video and Sound, InterfaceOptionsFrameCategories for Interface. True when
-- every one showed its panel and became the list's selection.
function OptionsTabs_ClickEach(frameName, listName)
    local frame = _G[frameName]
    if not frame then fail(frameName .. " does not exist") return false end
    frame:Show()
    local list = listName and _G[listName] or frame.categoryFrame
    local buttons = visibleCategoryButtons(list)
    if #buttons == 0 then fail(frameName .. " lists no categories") frame:Hide() return false end
    local ok = true
    for i, button in ipairs(buttons) do
        if not button:GetScript("OnClick") then
            fail(frameName .. " button " .. i .. " (" .. tostring(button:GetText()) .. ") has no OnClick")
            ok = false
        else
            button:Click()
            local panel = button.element
            if not panel:IsShown() then
                fail(frameName .. ": " .. tostring(button:GetText()) .. " did not show its panel")
                ok = false
            end
            if list.selection ~= panel then
                fail(frameName .. ": " .. tostring(button:GetText()) .. " is not the selection")
                ok = false
            end
        end
    end
    frame:Hide()
    return ok
end

-- The panel the button at index i of a frame's list shows, after a click.
function OptionsTabs_Shows(frameName, index)
    local button = _G[frameName .. "CategoryFrameButton" .. index]
        or _G[frameName .. "CategoriesButton" .. index]
    return button ~= nil and button.element ~= nil and button.element:IsShown() ~= nil
end
