#pragma once

namespace wowee::addons {

/// The chat bubbles' frames (0x0056cad0, 0x0056c7f0, 0x0056c340): children of the
/// WorldFrame without a name, which addons find among them, made as they are needed
/// and used again. The chunk answers a function that is handed the bubbles there are,
/// each frame: how many units there are to the client's one (DDC), then of each its id,
/// text, type, alpha and place (ChatBubbleView).
///
/// The text in NAMEPLATE_FONT at 0.01 - 12.8 units on a 4:3 screen, 15.7 on a 16:9 one -, as
/// wide as it runs between 32 units and 0.2; the backdrop 0.01 larger on each side, its tail
/// below its middle. A place that is less than a unit off is kept.
inline constexpr const char* kChatBubblesLua = R"LUA(
local byId, free = {}, {}
local PAD, MOST = 12.8, 256

local function make()
    local f = CreateFrame('Frame', nil, WorldFrame)
    f:SetFrameStrata('WORLD')
    f:SetBackdrop({bgFile = 'Interface\\Tooltips\\ChatBubble-Background',
                   edgeFile = 'Interface\\Tooltips\\ChatBubble-Backdrop',
                   tile = true, tileSize = 16, edgeSize = 16,
                   insets = {left = 16, right = 16, top = 16, bottom = 16}})
    local tail = f:CreateTexture(nil, 'ARTWORK')
    tail:SetTexture('Interface\\Tooltips\\ChatBubble-Tail')
    tail:SetWidth(16)
    tail:SetHeight(16)
    tail:SetPoint('TOPRIGHT', f, 'BOTTOM', 0, 4)
    local text = f:CreateFontString(nil, 'ARTWORK')
    return {frame = f, text = text}
end

local function fill(b, words, kind)
    b.text:SetFont(NAMEPLATE_FONT or 'Fonts\\FRIZQT__.TTF', PAD)
    b.text:SetPoint('BOTTOM', b.frame, 'BOTTOM', 0, PAD)
    local info = ChatTypeInfo and ChatTypeInfo[kind]
    if info then b.text:SetTextColor(info.r, info.g, info.b) else b.text:SetTextColor(1, 1, 1) end
    b.text:SetWidth(0)
    b.text:SetText(words)
    local w = b.text:GetStringWidth()
    if w > MOST then w = MOST elseif w < 32 then w = 32 end
    b.text:SetWidth(w)
    b.frame:SetWidth(w + 2 * PAD)
    b.frame:SetHeight(b.text:GetHeight() + 2 * PAD)
end

return function(units, ...)
    local seen = {}
    if units > 0 and PAD ~= units * .01 then
        PAD, MOST = units * .01, units * .2
        for id, b in pairs(byId) do fill(b, b.words, b.kind) end
    end
    for i = 1, select('#', ...), 6 do
        local id, words, kind, alpha, x, y = select(i, ...)
        seen[id] = true
        local b = byId[id]
        if not b then
            b = table.remove(free) or make()
            byId[id] = b
            b.words, b.kind = words, kind
            fill(b, words, kind)
            b.x, b.y = nil, nil
            b.frame:Show()
        end
        if not b.x or x - b.x >= 1 or b.x - x >= 1 or y - b.y >= 1 or b.y - y >= 1 then
            b.x, b.y = x, y
            b.frame:ClearAllPoints()
            b.frame:SetPoint('BOTTOM', WorldFrame, 'BOTTOMLEFT', x, y - PAD)
        end
        b.frame:SetAlpha(alpha)
        b.frame:SetFrameLevel((i - 1) / 6 + 2)
    end
    for id, b in pairs(byId) do
        if not seen[id] then
            b.frame:Hide()
            byId[id] = nil
            free[#free + 1] = b
        end
    end
end
)LUA";

}  // namespace wowee::addons
