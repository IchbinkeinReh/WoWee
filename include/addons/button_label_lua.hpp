#pragma once

// A button's label and the font it is drawn in, kept the way CSimpleButton
// keeps them.
//
// wow.exe reads a button's <NormalFont>, <HighlightFont> and <DisabledFont>
// into three fields while it walks the button's children (0x0096fed0) and
// only then hands the <ButtonText> to SetFontString (0x0096f7b0). That puts
// the label on the button and runs the state update (0x0096f4a0), which gives
// it the font of the state the button is in. So the font reaches the label
// whatever order the XML names them in, and a label set later - from Lua, or
// from an instance whose template named the font - gets it too.
//
// SetFontString also places a label that has no anchors of its own, at the
// left, the right or the middle of the button by the normal font's
// justification: UIMenuButtonTemplate names GameFontNormalLeft and no anchors,
// and its "Say" sits against the left edge.
//
// SetNormalFontObject is the C binding (lua_Frame_SetNormalFontObject). It
// keeps the font in __normalFontObject and gives it to the label the button
// already has; this is the other half. Run with __WoweeFrameMT defined, after
// the block that installs the rest of the button methods.
//
// GetFontString makes a label on demand when a button is asked for one it has
// not been given. Every button has a font string in the real client, and
// FrameXML assumes it: FCF_SetTabColor does
// minFrame:GetFontString():SetTextColor(...) without checking.

namespace wowee::addons {

inline constexpr const char* kButtonLabelLua = R"LUA(
local mt = __WoweeFrameMT

local function fontObjectOf(font)
    if type(font) == 'string' then return rawget(_G, font) end
    return font
end

-- Where a label with no anchors goes: the point its button's normal font
-- justifies to, the middle when it says nothing.
local function labelPoint(font)
    local object = fontObjectOf(font)
    local justify = type(object) == 'table' and rawget(object, 'justifyH')
    if justify == 'LEFT' or justify == 'RIGHT' then return justify end
    return 'CENTER'
end

function mt:GetNormalFontObject()
    return fontObjectOf(self.__normalFontObject)
end

function mt:SetFontString(fs)
    if fs == self.__fontString then return end
    self.__fontString = fs
    if not fs then return end
    local font = self.__normalFontObject
    if fs.GetNumPoints and fs.SetPoint and fs:GetNumPoints() == 0 then
        local point = labelPoint(font)
        fs:SetPoint(point, self, point)
    end
    if font ~= nil and self.SetNormalFontObject then
        self:SetNormalFontObject(font)
    end
end

function mt:GetFontString()
    if not self.__fontString and self.CreateFontString then
        self:SetFontString(self:CreateFontString(nil, 'OVERLAY'))
    end
    return self.__fontString
end
)LUA";

} // namespace wowee::addons
