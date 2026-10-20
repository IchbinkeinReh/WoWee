#pragma once

/// What a frame's shadow pass does for this frame slot's map.
///
/// With dynamic shadows off every shader that samples the map - the world's,
/// the volumetric fog's - reads shadowParams.x first and takes the sun as
/// unshadowed, so the map's contents are never looked at. What still matters
/// is its layout: it stays bound for sampling, so it has to be in
/// SHADER_READ_ONLY_OPTIMAL, which a slot that has never been drawn (shadows
/// off from the start) or has just been rebuilt is not. Such a slot gets one
/// empty pass - cleared and transitioned, no casters - and from then on the
/// pass is skipped whole, the way the shadow ablation skips it.
///
/// No includes, so a test can read it without Vulkan.

namespace wowee::rendering {

enum class ShadowPassWork {
    /// Nothing recorded: no casters, no clear, no GPU marks.
    None,
    /// The map cleared and left readable; no cascades, no casters.
    ClearOnly,
    /// Every cascade drawn with its casters.
    DrawCasters,
};

/// mapReady: the slot's image exists and the frame may record into it (the
/// player has a position; not ablated or switched off for measuring).
/// shadowsOn: the Dynamic shadows setting.
/// slotReadable: the slot's map is already in the layout its readers expect.
constexpr ShadowPassWork shadowPassWork(bool mapReady, bool shadowsOn, bool slotReadable) {
    if (!mapReady) return ShadowPassWork::None;
    if (shadowsOn) return ShadowPassWork::DrawCasters;
    return slotReadable ? ShadowPassWork::None : ShadowPassWork::ClearOnly;
}

}  // namespace wowee::rendering
