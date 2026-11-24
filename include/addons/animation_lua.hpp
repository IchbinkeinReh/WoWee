#pragma once

namespace wowee::addons {

/// What the animation groups playing on a region make of it, and how it is
/// given back. Run before the rest of the animations' Lua, which keeps in each
/// group what it makes of the region: dx, dy, rad (with rotAnim, whose origin it
/// turns about), sx, sy (with scaleAnim) and alphaChange.
///
/// The region's own alpha is kept as the first Alpha animation on it begins
/// (0x00488000), and given back - with its place, its turn and its size - as a
/// group on it ends or stops, after the scripts have run (0x0049b470,
/// 0x0049b0f0 -> 0x00488980 -> 0x00488060), where no other is playing on it.
/// There is no keeping the final alpha. A Scale is by two factors about its
/// origin, and the region's own scale and anchors are as they were.
inline constexpr const char* kAnimationShowLua = R"LUA(
__WoweePlayingAnimations = {}
local playing = __WoweePlayingAnimations

function __WoweeShowAnimation(p)
    local dx, dy, rad, sx, sy = 0, 0, 0, 1, 1
    local change, rotAnim, scaleAnim
    for g in pairs(playing) do
        if g.parent == p and g.dx then
            dx, dy, rad = dx + g.dx, dy + g.dy, rad + g.rad
            sx, sy = sx * g.sx, sy * g.sy
            rotAnim, scaleAnim = g.rotAnim or rotAnim, g.scaleAnim or scaleAnim
            if g.alphaChange then change = (change or 0) + g.alphaChange end
        end
    end
    if change then
        local base = rawget(p, '__animBaseAlpha')
        if not base then
            base = p:GetAlpha()
            rawset(p, '__animBaseAlpha', base)
        end
        local alpha = base + change
        if alpha < 0 then alpha = 0 elseif alpha > 1 then alpha = 1 end
        p:SetAlpha(alpha)
    end
    __WoweeSetAnimOffset(p, dx, dy)
    local r, z = rotAnim or {}, scaleAnim or {}
    __WoweeSetAnimRotation(p, rad, r.originPoint or 'CENTER', r.originX or 0, r.originY or 0)
    __WoweeSetAnimScale(p, sx, sy, z.originPoint or 'CENTER', z.originX or 0, z.originY or 0)
end

function __WoweeAnimGiveBack(g)
    local p = g.parent
    if not p or g.isPlaying then return end
    g.dx, g.dy, g.rad, g.rotAnim = nil, nil, nil, nil
    g.sx, g.sy, g.scaleAnim, g.alphaChange = nil, nil, nil, nil
    local others = false
    for o in pairs(playing) do
        if o ~= g and o.parent == p then others = true end
    end
    local base = rawget(p, '__animBaseAlpha')
    if base and not others then
        rawset(p, '__animBaseAlpha', nil)
        p:SetAlpha(base)
    end
    __WoweeShowAnimation(p)
end
)LUA";

}  // namespace wowee::addons
