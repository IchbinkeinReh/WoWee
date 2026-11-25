#pragma once

namespace wowee::addons {

/// The methods of each type of the interface's objects (0x008167e0), beside its parent's.
struct WidgetType {
    const char* name;
    const char* parent;
    const char* methods;
};
inline constexpr WidgetType kWidgetTypes[] = {
    {.name = "Object", .parent = "", .methods =   // 0x00ac1b68
     "GetObjectType IsObjectType GetName GetParent"},
    {.name = "Region", .parent = "Object", .methods =   // 0x00ac1480
     "IsProtected CanChangeProtectedState SetParent GetRect GetCenter GetLeft GetRight GetTop "
     "GetBottom GetWidth SetWidth GetHeight SetHeight SetSize GetSize GetNumPoints GetPoint "
     "SetPoint SetAllPoints ClearAllPoints CreateAnimationGroup GetAnimationGroups StopAnimating "
     "IsDragging IsMouseOver"},
    {.name = "Texture", .parent = "Region", .methods =   // 0x00ac1028
     "IsObjectType GetObjectType GetDrawLayer SetDrawLayer GetBlendMode SetBlendMode "
     "GetVertexColor SetVertexColor SetGradient SetGradientAlpha SetAlpha GetAlpha Show Hide "
     "IsVisible IsShown GetTexture SetTexture GetTexCoord SetTexCoord SetRotation SetDesaturated "
     "IsDesaturated SetNonBlocking GetNonBlocking SetHorizTile GetHorizTile SetVertTile "
     "GetVertTile"},
    {.name = "FontString", .parent = "Region", .methods =   // 0x00ac1118
     "IsObjectType GetObjectType GetDrawLayer SetDrawLayer SetVertexColor GetAlpha SetAlpha "
     "SetAlphaGradient Show Hide IsVisible IsShown GetFontObject SetFontObject GetFont SetFont "
     "GetText GetFieldSize SetText SetFormattedText GetTextColor SetTextColor GetShadowColor "
     "SetShadowColor GetShadowOffset SetShadowOffset GetSpacing SetSpacing SetTextHeight "
     "GetStringWidth GetStringHeight GetJustifyH SetJustifyH GetJustifyV SetJustifyV "
     "CanNonSpaceWrap SetNonSpaceWrap CanWordWrap SetWordWrap GetIndentedWordWrap "
     "SetIndentedWordWrap"},
    {.name = "Frame", .parent = "Region", .methods =   // 0x00ac1550
     "GetTitleRegion CreateTitleRegion CreateTexture CreateFontString GetBoundsRect GetNumRegions "
     "GetRegions GetNumChildren GetChildren GetFrameStrata SetFrameStrata GetFrameLevel "
     "SetFrameLevel HasScript GetScript SetScript HookScript RegisterEvent UnregisterEvent "
     "RegisterAllEvents UnregisterAllEvents IsEventRegistered AllowAttributeChanges "
     "CanChangeAttribute GetAttribute SetAttribute GetEffectiveScale GetScale SetScale "
     "GetEffectiveAlpha GetAlpha SetAlpha GetID SetID SetToplevel IsToplevel EnableDrawLayer "
     "DisableDrawLayer Show Hide IsVisible IsShown Raise Lower GetHitRectInsets SetHitRectInsets "
     "GetClampRectInsets SetClampRectInsets GetMinResize SetMinResize GetMaxResize SetMaxResize "
     "SetMovable IsMovable SetDontSavePosition GetDontSavePosition SetResizable IsResizable "
     "StartMoving StartSizing StopMovingOrSizing SetUserPlaced IsUserPlaced SetClampedToScreen "
     "IsClampedToScreen RegisterForDrag EnableKeyboard IsKeyboardEnabled EnableMouse "
     "IsMouseEnabled EnableMouseWheel IsMouseWheelEnabled EnableJoystick IsJoystickEnabled "
     "GetBackdrop SetBackdrop GetBackdropColor SetBackdropColor GetBackdropBorderColor "
     "SetBackdropBorderColor SetDepth GetDepth GetEffectiveDepth IgnoreDepth IsIgnoringDepth"},
    {.name = "Button", .parent = "Frame", .methods =   // 0x00b2d418
     "Enable Disable IsEnabled GetButtonState SetButtonState SetNormalFontObject "
     "GetNormalFontObject SetDisabledFontObject GetDisabledFontObject SetHighlightFontObject "
     "GetHighlightFontObject SetFontString GetFontString SetText SetFormattedText GetText "
     "SetNormalTexture GetNormalTexture SetPushedTexture GetPushedTexture SetDisabledTexture "
     "GetDisabledTexture SetHighlightTexture GetHighlightTexture SetPushedTextOffset "
     "GetPushedTextOffset GetTextWidth GetTextHeight RegisterForClicks Click LockHighlight "
     "UnlockHighlight GetMotionScriptsWhileDisabled SetMotionScriptsWhileDisabled"},
    {.name = "CheckButton", .parent = "Button", .methods =   // 0x00b2d3e4
     "SetChecked GetChecked GetCheckedTexture SetCheckedTexture GetDisabledCheckedTexture "
     "SetDisabledCheckedTexture"},
    {.name = "StatusBar", .parent = "Frame", .methods =   // 0x00b2cdb8
     "GetOrientation SetOrientation GetMinMaxValues SetMinMaxValues GetValue SetValue "
     "GetStatusBarTexture SetStatusBarTexture GetStatusBarColor SetStatusBarColor "
     "GetRotatesTexture SetRotatesTexture"},
    {.name = "Slider", .parent = "Frame", .methods =   // 0x00b2ce20
     "GetThumbTexture SetThumbTexture GetOrientation SetOrientation GetMinMaxValues "
     "SetMinMaxValues GetValue SetValue GetValueStep SetValueStep Enable Disable IsEnabled"},
    {.name = "ScrollFrame", .parent = "Frame", .methods =   // 0x00b2ce90
     "SetScrollChild GetScrollChild SetHorizontalScroll SetVerticalScroll GetHorizontalScroll "
     "GetVerticalScroll GetHorizontalScrollRange GetVerticalScrollRange UpdateScrollChildRect"},
    {.name = "ScrollingMessageFrame", .parent = "Frame", .methods =   // 0x00b2cee0
     "SetFontObject GetFontObject SetFont GetFont SetTextColor GetTextColor SetShadowColor "
     "GetShadowColor SetShadowOffset GetShadowOffset SetSpacing GetSpacing SetJustifyH GetJustifyH "
     "SetJustifyV GetJustifyV AddMessage GetMessageInfo RemoveMessagesByAccessID ScrollUp "
     "ScrollDown PageUp PageDown ScrollToTop ScrollToBottom SetScrollOffset AtTop AtBottom "
     "UpdateColorByID GetNumMessages GetNumLinesDisplayed GetCurrentScroll GetCurrentLine "
     "GetMaxLines SetMaxLines SetFading GetFading SetTimeVisible GetTimeVisible SetFadeDuration "
     "GetFadeDuration Clear SetInsertMode GetInsertMode SetIndentedWordWrap GetIndentedWordWrap "
     "SetHyperlinksEnabled GetHyperlinksEnabled"},
    {.name = "MessageFrame", .parent = "Frame", .methods =   // 0x00b2d068
     "SetFontObject GetFontObject SetFont GetFont SetTextColor GetTextColor SetShadowColor "
     "GetShadowColor SetShadowOffset GetShadowOffset SetSpacing GetSpacing SetJustifyH GetJustifyH "
     "SetJustifyV GetJustifyV SetInsertMode GetInsertMode SetIndentedWordWrap GetIndentedWordWrap "
     "SetFading GetFading SetTimeVisible GetTimeVisible SetFadeDuration GetFadeDuration AddMessage "
     "Clear"},
    {.name = "SimpleHTML", .parent = "Frame", .methods =   // 0x00b2d150
     "SetFontObject GetFontObject SetFont GetFont SetTextColor GetTextColor SetShadowColor "
     "GetShadowColor SetShadowOffset GetShadowOffset SetSpacing GetSpacing SetJustifyH GetJustifyH "
     "SetJustifyV GetJustifyV SetIndentedWordWrap GetIndentedWordWrap SetText SetHyperlinkFormat "
     "GetHyperlinkFormat SetHyperlinksEnabled GetHyperlinksEnabled"},
    {.name = "EditBox", .parent = "Frame", .methods =   // 0x00b2d210
     "SetFontObject GetFontObject SetFont GetFont SetTextColor GetTextColor SetShadowColor "
     "GetShadowColor SetShadowOffset GetShadowOffset SetSpacing GetSpacing SetJustifyH GetJustifyH "
     "SetJustifyV GetJustifyV SetIndentedWordWrap GetIndentedWordWrap SetAutoFocus IsAutoFocus "
     "SetCountInvisibleLetters IsCountInvisibleLetters SetMultiLine IsMultiLine SetNumeric "
     "IsNumeric SetPassword IsPassword SetBlinkSpeed GetBlinkSpeed Insert SetText GetText "
     "SetNumber GetNumber HighlightText AddHistoryLine ClearHistory SetTextInsets GetTextInsets "
     "SetFocus ClearFocus HasFocus SetMaxBytes GetMaxBytes SetMaxLetters GetMaxLetters "
     "GetNumLetters GetHistoryLines SetHistoryLines GetInputLanguage ToggleInputLanguage "
     "SetAltArrowKeyMode GetAltArrowKeyMode IsInIMECompositionMode SetCursorPosition "
     "GetCursorPosition GetUTF8CursorPosition"},
    {.name = "ColorSelect", .parent = "Frame", .methods =   // 0x00b2cd50
     "GetColorWheelTexture SetColorWheelTexture GetColorWheelThumbTexture "
     "SetColorWheelThumbTexture GetColorValueTexture SetColorValueTexture "
     "GetColorValueThumbTexture SetColorValueThumbTexture SetColorHSV GetColorHSV SetColorRGB "
     "GetColorRGB"},
    {.name = "MovieFrame", .parent = "Frame", .methods =   // 0x00b2cd30
     "StartMovie StopMovie EnableSubtitles"},
    {.name = "Model", .parent = "Frame", .methods =   // 0x00b2cb10
     "SetModel GetModel ClearModel SetPosition SetFacing SetModelScale SetSequence SetSequenceTime "
     "SetCamera SetLight GetLight GetPosition GetFacing GetModelScale AdvanceTime "
     "ReplaceIconTexture SetFogColor GetFogColor SetFogNear GetFogNear SetFogFar GetFogFar "
     "ClearFog SetGlow"},
    {.name = "PlayerModel", .parent = "Model", .methods =   // 0x00acf514
     "SetUnit SetCreature RefreshUnit SetRotation"},
    {.name = "DressUpModel", .parent = "PlayerModel", .methods =   // 0x00acf550
     "Undress Dress TryOn"},
    {.name = "TabardModel", .parent = "PlayerModel", .methods =   // 0x00acf588
     "InitializeTabardColors Save CycleVariation GetUpperBackgroundFileName "
     "GetLowerBackgroundFileName GetUpperEmblemFileName GetLowerEmblemFileName "
     "GetUpperEmblemTexture GetLowerEmblemTexture CanSaveTabardNow"},
    {.name = "Cooldown", .parent = "Frame", .methods =   // 0x00ad13a4
     "SetCooldown SetReverse GetReverse SetDrawEdge GetDrawEdge"},
    {.name = "GameTooltip", .parent = "Frame", .methods =   // 0x00ad2ae0
     "AddFontStrings SetMinimumWidth GetMinimumWidth SetPadding GetPadding IsOwned GetOwner "
     "SetOwner GetAnchorType SetAnchorType ClearLines AddLine AddDoubleLine AddTexture SetText "
     "AppendText FadeOut SetHyperlink SetAction SetPetAction SetShapeshift SetPossession "
     "SetTracking SetSpell SetSpellByID SetGlyph SetInventoryItem SetLootItem SetQuestItem "
     "SetQuestLogItem SetTrainerService SetTradeSkillItem SetMerchantItem SetMerchantCostItem "
     "SetTradePlayerItem SetTradeTargetItem SetBagItem SetUnit SetUnitBuff SetUnitDebuff "
     "SetUnitAura SetTalent SetSendMailItem SetInboxItem SetAuctionSellItem SetAuctionItem "
     "NumLines SetQuestRewardSpell SetQuestLogRewardSpell SetHyperlinkCompareItem SetBuybackItem "
     "SetLootRollItem SetSocketedItem SetSocketGem SetExistingSocketGem SetGuildBankItem IsUnit "
     "GetUnit GetItem GetSpell SetTotem SetCurrencyToken SetBackpackToken IsEquippedItem "
     "SetQuestLogSpecialItem SetEquipmentSet SetFrameStack SetLFGDungeonReward "
     "SetLFGCompletionReward"},
    {.name = "Minimap", .parent = "Frame", .methods =   // 0x00acebb8
     "SetMaskTexture SetIconTexture SetBlipTexture SetClassBlipTexture SetPOIArrowTexture "
     "SetStaticPOIArrowTexture SetCorpsePOIArrowTexture SetPlayerTexture SetPlayerTextureHeight "
     "SetPlayerTextureWidth GetZoomLevels GetZoom SetZoom PingLocation GetPingPosition"},
    {.name = "QuestPOIFrame", .parent = "Frame", .methods =   // 0x00acf180
     "SetFillTexture SetFillAlpha SetBorderTexture SetBorderAlpha SetBorderScalar DrawQuestBlob "
     "EnableSmoothing EnableMerging SetMergeThreshold SetNumSplinePoints UpdateQuestPOI "
     "UpdateMouseOverTooltip GetTooltipIndex GetNumTooltips"},
};

/// Parts the frames' one table of methods, and the two of textures and font strings, by these
/// types: a table for each, as the client has one for each, with what is the type's and nothing
/// of another's - CreateFrame("Frame").GetChecked is nil, and a hook in the table of buttons is
/// none on edit boxes. What is no type's is this client's own, and found by all of them.
///
/// Handed the types, the frames' table and a function that answers a texture's and a font
/// string's; answers the metatables of the frames' types by their names.
inline constexpr const char* kWidgetTypesLua = R"LUA(
local types, frames, regionTable = ...
local all, own = {}, {}
for _, t in ipairs(types) do
    local mine = {}
    for name in pairs(own[t.parent] or {}) do mine[name] = true end
    for name in t.methods:gmatch('%S+') do
        mine[name] = true
        all[name] = true
    end
    own[t.name] = mine
end

-- What is not in a type's table.
local function rest(mine, from)
    return function(_, key)
        if all[key] and not mine[key] then return nil end
        return from[key]
    end
end

for _, kind in ipairs({'Texture', 'FontString'}) do
    local methods = regionTable(kind)
    local mine = own[kind]
    for name in pairs(methods) do
        if all[name] and not mine[name] then methods[name] = nil end
    end
    local known = getmetatable(methods)
    local before = known and known.__index
    setmetatable(methods, {__index = function(t, key)
        if all[key] and not mine[key] then return nil end
        return before and before(t, key)
    end})
end

local metas = {}
for _, t in ipairs(types) do
    local mine = own[t.name]
    local methods = {}
    for name in pairs(mine) do methods[name] = rawget(frames, name) end
    setmetatable(methods, {__index = rest(mine, frames)})
    local meta = {}
    for key, value in pairs(frames) do
        if type(key) == 'string' and key:sub(1, 2) == '__' then meta[key] = value end
    end
    meta.__index = methods
    metas[t.name] = meta
end
metas.WorldFrame = metas.Frame
return metas
)LUA";

}  // namespace wowee::addons
