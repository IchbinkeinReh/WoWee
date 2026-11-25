#pragma once

namespace wowee::addons {

/// The nameplates' frames (CGNamePlateFrame, 0x0098f790): children of the WorldFrame without a
/// name, which addons find among them and know by their regions, made as they are needed and
/// used again for other units. In the order the client makes them: the threat flash, the border,
/// the cast bar's border and shield, the spell's icon, the glow, the name, the level, the skull,
/// the raid icon and the elite's dragon; and two status bars, the health's and the cast's.
///
/// The chunk answers two functions: one that is handed a plate, each frame - how many units
/// there are to one of the client's (DDC, the screen's diagonal), the unit, the place of the
/// plate's top's middle, and what is to be seen on it (ui::NamePlateView) -, and one that is
/// called after the last, which puts away the others.
inline constexpr const char* kNamePlatesLua = R"LUA(
local byGuid, free, pass = {}, {}, 0
local W, H = .1, .025                       -- 0x00b2da74, 0x00b2da70
local BORDER = 'Interface\\Tooltips\\Nameplate-Border'
local FILL = 'Interface\\TargetingFrame\\UI-TargetingFrame-BarFill'

local function texture(f, layer, file)
    local t = f:CreateTexture(nil, layer)
    if file then t:SetTexture(file) end
    return t
end

local function bar(f)
    local b = CreateFrame('StatusBar', nil, f)
    b:SetStatusBarTexture(FILL)
    return b
end

local function make()
    local f = CreateFrame('Frame', nil, WorldFrame)
    f:Hide()
    f:SetFrameStrata('WORLD')
    local p = {frame = f}
    p.threat = texture(f, 'BACKGROUND', 'Interface\\TargetingFrame\\UI-TargetingFrame-Flash')
    p.threat:SetTexCoord(0, .555, .53, .6)
    p.border = texture(f, 'ARTWORK', BORDER)
    p.border:SetAllPoints(f)
    p.health = bar(f)
    p.castBorder = texture(f, 'ARTWORK', BORDER)
    p.castBorder:SetTexCoord(1, 0, 0, 1)
    p.shield = texture(f, 'ARTWORK', 'Interface\\Tooltips\\Nameplate-CastBar-Shield')
    p.cast = bar(f)
    p.icon = texture(f, 'OVERLAY')
    p.glow = texture(f, 'HIGHLIGHT', 'Interface\\Tooltips\\Nameplate-Glow')
    p.glow:SetBlendMode('ADD')
    p.glow:SetAllPoints(f)
    p.name = f:CreateFontString(nil, 'OVERLAY')
    p.level = f:CreateFontString(nil, 'OVERLAY')
    p.skull = texture(f, 'OVERLAY', 'Interface\\TargetingFrame\\UI-TargetingFrame-Skull')
    p.raid = texture(f, 'ARTWORK', 'Interface\\TargetingFrame\\UI-RaidTargetingIcons')
    p.elite = texture(f, 'BORDER', 'Interface\\Tooltips\\EliteNameplateIcon')
    p.elite:SetTexCoord(0, .578125, 0, .84375)
    for _, key in ipairs({'threat', 'castBorder', 'shield', 'cast', 'icon', 'glow', 'level', 'skull', 'raid', 'elite'}) do
        p[key]:Hide()
    end
    return p
end

local function size(region, w, h)
    region:SetWidth(w)
    region:SetHeight(h)
end

-- 0x0098f390 and 0x0098f790's sizes, in the client's units.
local function layout(p, k)
    local f, w, h = p.frame, W * k, H * k
    size(f, w, h)
    size(p.threat, .11 * k, .029 * k)
    p.threat:SetPoint('TOP', f, 'TOP', -.001 * k, -.0065 * k)
    size(p.health, w * .804, h * .281)
    p.health:SetPoint('BOTTOMLEFT', f, 'BOTTOMLEFT', w * .031, h * .125)
    size(p.castBorder, w, h)
    p.castBorder:SetPoint('CENTER', f, 'CENTER', 0, -h * .5)
    size(p.shield, w, h)
    p.shield:SetPoint('CENTER', p.castBorder, 'CENTER', 0, -h * .5)
    size(p.cast, w * .804, h * .281)
    size(p.icon, .01 * k, .01 * k)
    local font = NAMEPLATE_FONT or 'Fonts\\FRIZQT__.TTF'
    p.name:SetFont(font, .01 * k)
    p.name:SetPoint('BOTTOM', f, 'CENTER', 0, 0)
    p.level:SetFont(font, .009 * k)
    p.level:SetPoint('CENTER', f, 'BOTTOMRIGHT', -w * .092, h * .284)
    for _, text in ipairs({p.name, p.level}) do
        text:SetShadowColor(0, 0, 0)
        text:SetShadowOffset(.001 * k, -.001 * k)
    end
    size(p.skull, .01 * k, .01 * k)
    p.skull:SetPoint('CENTER', p.level, 'CENTER', 0, 0)
    size(p.raid, .02 * k, .02 * k)
    p.raid:SetPoint('RIGHT', p.border, 'LEFT', 0, 0)
    size(p.elite, .0294 * k, .0215 * k)
    p.elite:SetPoint('CENTER', p.level, 'CENTER', .003 * k, -.001 * k)
end

local function shown(region, on)
    if on then region:Show() else region:Hide() end
end

-- 0x0098f040: hung from the border's corner, over it or, with the shield in its place, under it.
local function layoutCast(p, k, shield)
    shown(p.castBorder, not shield)
    shown(p.shield, shield)
    p.cast:SetPoint('BOTTOMRIGHT', p.castBorder, 'BOTTOMRIGHT', -.003125 * k, (shield and -.003125 or .003125) * k)
    p.icon:SetPoint('CENTER', p.castBorder, 'BOTTOMLEFT', .0092 * k, shield and 0 or .0071 * k)
end

local function plate(k, guid, x, y, target, alpha, name, nr, ng, nb, level, lr, lg, lb, elite, raid,
                     health, most, hr, hg, hb, tr, tg, tb, glow, spell, low, high, value, cr, cg, cb, ca, shield)
    local p = byGuid[guid]
    if not p then
        p = table.remove(free) or make()
        p.was = {}
        byGuid[guid] = p
    end
    p.pass = pass
    local f, was = p.frame, p.was
    if was.k ~= k then
        was.k, was.x, was.shield = k, nil, nil
        layout(p, k)
    end
    if was.x ~= x or was.y ~= y then
        was.x, was.y = x, y
        f:SetPoint('TOP', WorldFrame, 'BOTTOMLEFT', x, y)
    end
    if was.target ~= target then
        was.target = target
        local at = target and 20 or 10
        f:SetFrameLevel(at)
        p.health:SetFrameLevel(at - 1)
        p.cast:SetFrameLevel(at - 1)
    end
    if was.alpha ~= alpha then
        was.alpha = alpha
        f:SetAlpha(alpha)
    end
    if was.name ~= name then
        was.name = name
        p.name:SetText(name)
    end
    if was.nr ~= nr or was.ng ~= ng or was.nb ~= nb then
        was.nr, was.ng, was.nb = nr, ng, nb
        p.name:SetTextColor(nr, ng, nb)
    end
    if was.level ~= level then
        was.level = level
        if level then p.level:SetText(level) end
        shown(p.level, level)
        shown(p.skull, not level)
    end
    if level and (was.lr ~= lr or was.lg ~= lg or was.lb ~= lb) then
        was.lr, was.lg, was.lb = lr, lg, lb
        p.level:SetTextColor(lr, lg, lb)
    end
    if was.elite ~= elite then
        was.elite = elite
        shown(p.elite, elite)
    end
    if was.raid ~= raid then
        was.raid = raid
        if raid then
            local left, top = raid % 4 * .25, math.floor(raid / 4) * .25
            p.raid:SetTexCoord(left, left + .25, top, top + .25)
        end
        shown(p.raid, raid)
    end
    if was.most ~= most then
        was.most = most
        p.health:SetMinMaxValues(0, most)
    end
    if was.health ~= health then
        was.health = health
        p.health:SetValue(health)
    end
    if was.hr ~= hr or was.hg ~= hg or was.hb ~= hb then
        was.hr, was.hg, was.hb = hr, hg, hb
        p.health:SetStatusBarColor(hr, hg, hb)
    end
    if was.tr ~= tr or was.tg ~= tg or was.tb ~= tb then
        was.tr, was.tg, was.tb = tr, tg, tb
        if tr then p.threat:SetVertexColor(tr, tg, tb) end
        shown(p.threat, tr)
    end
    if was.glow ~= glow then
        was.glow = glow
        shown(p.glow, glow)
        if glow then f:EnableDrawLayer('HIGHLIGHT') else f:DisableDrawLayer('HIGHLIGHT') end
    end
    if spell then
        if was.spell ~= spell then
            p.icon:SetTexture((select(3, GetSpellInfo(spell))))
            p.icon:Show()
            p.cast:Show()
        end
        if was.spell ~= spell or was.shield ~= shield then
            was.shield = shield
            layoutCast(p, k, shield)
        end
        if was.low ~= low or was.high ~= high then
            was.low, was.high = low, high
            p.cast:SetMinMaxValues(low, high)
        end
        p.cast:SetValue(value)
        if was.cr ~= cr or was.cg ~= cg or was.cb ~= cb then
            was.cr, was.cg, was.cb = cr, cg, cb
            p.cast:SetStatusBarColor(cr, cg, cb)
        end
        if was.ca ~= ca then
            was.ca = ca
            for _, region in ipairs({p.cast, p.castBorder, p.shield, p.icon}) do region:SetAlpha(ca) end
        end
    elseif was.spell then
        for _, region in ipairs({p.cast, p.castBorder, p.shield, p.icon}) do region:Hide() end
    end
    was.spell = spell
    if not was.up then
        was.up = true
        f:Show()
    end
end

return plate, function()
    for guid, p in pairs(byGuid) do
        if p.pass ~= pass then
            if p.was.spell then
                for _, region in ipairs({p.cast, p.castBorder, p.shield, p.icon}) do region:Hide() end
            end
            p.frame:Hide()
            byGuid[guid] = nil
            free[#free + 1] = p
        end
    end
    pass = pass + 1
end
)LUA";

}  // namespace wowee::addons
