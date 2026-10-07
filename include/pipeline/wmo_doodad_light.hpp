#pragma once

/// How the 3.3.5a client (Wow.exe build 12340) lights an M2 standing inside a
/// WMO, without the sun.
///
/// A WMO's doodads are made group by group from each group's MODR list
/// (0x007bf740). A doodad of a group that is neither exterior nor
/// exterior-lit (MOGP flags 0x8, 0x40) is lit by its own MODD colour: an
/// ambient no brighter than 96/255 and one directional light no darker than
/// 112/255, both that colour's hue, from a fixed direction, and no sun
/// (0x007bef40 splits the colour with 0x007c1ad0, 0x007c1150 applies it). A
/// doodad of an exterior group is lit by the zone's light like any other.
///
/// A unit standing on an interior group's floor is lit the same way, by the
/// group's vertex colour under its feet as the client keeps it once loaded
/// (loadedVertexColor) - doubled, plus the WMO's ambient when MOHD flag 0x2 is
/// set - with the direct light no darker than 168/255 (0x007a0d60,
/// 0x007c7fe0). So is a game object: every world object is lit this way. On a
/// transition face (MOPY 0x1) the colour's alpha carries both lights, and the
/// direction, toward the zone's outdoor light (floorLightInZone), and a unit's
/// ambient eases from one floor's to the next (easeAmbient).
///
/// mapObjLightLOD, the cvar that sounds like it would add the MOLT lights, is
/// written by its handler (0x0078ded0) and read nowhere; no M2 takes a WMO's
/// MOLT lights.

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <vector>

#include "pipeline/wmo_loader.hpp"

namespace wowee::pipeline::wmo_doodad_light {

/// MOGP flags the client counts as outside (0x007bdd70's caller tests 0x48).
inline constexpr uint32_t kOutsideGroupFlags = 0x8u | 0x40u;

/// The direction the interior light travels, in world space (0xaeedf0):
/// mostly down, out of the +x +y quarter. x and y are equal, so it is the
/// same vector in render space.
inline constexpr glm::vec3 kInteriorLightDir{-0.30822f, -0.30822f, -0.9f};

/// The brightest of a colour's three channels, 0..255, as 0x007c1ad0 finds
/// it; 1 for black, so nothing divides by zero.
inline int brightestChannel(const glm::ivec3& rgb) {
    const int m = glm::max(rgb.r, glm::max(rgb.g, rgb.b));
    return m == 0 ? 1 : m;
}

/// The directional colour: the colour as it is, or raised to `minimum` in
/// HSV value - all three channels by the same factor - when its brightest
/// channel is below it (0x007c1ad0's first output).
inline glm::vec3 directFromColor(const glm::ivec3& rgb, int minimum) {
    const int m = brightestChannel(rgb);
    glm::vec3 c = glm::vec3(rgb);
    if (m < minimum) c *= static_cast<float>(minimum) / static_cast<float>(m);
    return glm::min(c, glm::vec3(255.0f)) / 255.0f;
}

/// The ambient: the colour as it is, or scaled down so its brightest channel
/// is `maximum` when it is above it, in the byte arithmetic the client uses
/// (0x007c1ad0's second output: (c * k + 255) >> 8, k being maximum * 255 / m
/// rounded after taking off a half - its floor).
inline glm::vec3 ambientFromColor(const glm::ivec3& rgb, int maximum) {
    const int m = brightestChannel(rgb);
    if (m <= maximum) return glm::vec3(rgb) / 255.0f;
    const int k = (maximum * 255) / m;
    const glm::ivec3 scaled = (rgb * k + 255) >> 8;
    return glm::vec3(scaled) / 255.0f;
}

/// What an interior doodad is lit by.
struct InteriorLight {
    glm::vec3 ambient{0.0f};
    glm::vec3 direct{0.0f};
};

/// A doodad's light from its MODD colour (WMODoodad::color, rgb 0..1): the
/// ambient at most 96, the direct at least 112 (0x007bef40 passes 0x70, 0x60).
inline InteriorLight doodadLight(const glm::vec4& moddColor) {
    const glm::ivec3 rgb = glm::ivec3(glm::round(glm::vec3(moddColor) * 255.0f));
    return {ambientFromColor(rgb, 0x60), directFromColor(rgb, 0x70)};
}

/// A unit's light from the colour under its feet (0x007a0d60 passes 0xa8,
/// 0x60): the ambient at most 96, the direct at least 168.
inline InteriorLight unitLight(const glm::ivec3& floorColor) {
    return {ambientFromColor(floorColor, 0x60), directFromColor(floorColor, 0xA8)};
}

/// The colour a unit takes from an interior floor (0x007c7fe0): the
/// triangle's vertex colours at its feet, doubled, plus the WMO's ambient when
/// MOHD flag 0x2 is set, each channel at most 255. `mocv` is the interpolated
/// vertex colour, 0..255.
inline glm::ivec3 unitFloorColor(const glm::vec3& mocv, uint32_t mohdFlags, const glm::vec4& mohdAmbient) {
    glm::ivec3 c = glm::ivec3(mocv) * 2;
    if (mohdFlags & 0x2u) c += glm::ivec3(glm::round(glm::vec3(mohdAmbient) * 255.0f));
    return glm::min(c, glm::ivec3(255));
}

/// A group's vertex colour (rgba, 0..255) as the client keeps it once the
/// group is loaded (0x007d7380, which 0x007d7c30 runs unless MOHD flag 0x8 is
/// set). The vertices of the transition batches, those up to the last of them
/// (MOGP's first batch count), are halved and keep their alpha, which says how
/// far toward the outside their light is. The rest are brightened by their
/// alpha and halved, and their alpha made 255. 0x007c7fe0 doubles what this
/// leaves when it reads the colour under a unit.
inline glm::ivec4 loadedVertexColor(const glm::ivec4& rgba, bool transitionVertex) {
    if (transitionVertex) return {rgba.r >> 1, rgba.g >> 1, rgba.b >> 1, rgba.a};
    const auto lift = [&](int c) { return glm::min(((c * rgba.a >> 6) + c) >> 1, 255); };
    return {lift(rgba.r), lift(rgba.g), lift(rgba.b), 255};
}

/// One colour carried toward another by `alpha` of 256, as the client lerps a
/// colour's bytes (0x006acc50): all the way at 255, not at all at 0.
inline glm::vec3 towardColor(const glm::vec3& from, const glm::vec3& to, int alpha) {
    if (alpha >= 255) return to;
    if (alpha <= 0) return from;
    return from + (to - from) * (static_cast<float>(alpha) / 256.0f);
}

/// The light of a world object - a unit, a player, a game object - on an
/// interior floor. 0x007c7fe0 also says whether the triangle under it is a
/// transition face (MOPY flag 0x1); on one, the vertex colour's alpha there
/// carries both colours toward the zone's outdoor light (0x007a0d60: the
/// direct toward the zone's direct, the ambient toward its ambient), and the
/// light's direction from the fixed interior one toward the sun's
/// (0x007c1730). `towardOutside` is that alpha, 0 off a transition face.
struct FloorLight {
    InteriorLight light;
    int towardOutside = 0;
};

/// What a world object is drawn with: ambient, direct colour, and how far
/// (0..1) the direct light's direction has turned from the interior one
/// (kInteriorLightDir) toward the sun's.
struct ObjectLight {
    glm::vec3 ambient{0.0f};
    glm::vec3 direct{0.0f};
    float towardSun = 0.0f;
};

/// A floor's light with the zone's outdoor light blended in by its alpha
/// (0x007a0d60, 0x007c1730).
inline ObjectLight floorLightInZone(const FloorLight& floor, const glm::vec3& zoneAmbient,
                                    const glm::vec3& zoneDirect) {
    ObjectLight out;
    out.ambient = towardColor(floor.light.ambient, zoneAmbient, floor.towardOutside);
    out.direct = towardColor(floor.light.direct, zoneDirect, floor.towardOutside);
    out.towardSun = static_cast<float>(glm::clamp(floor.towardOutside, 0, 255)) / 255.0f;
    return out;
}

/// A world object's ambient does not jump to a new floor's: every frame each
/// channel moves toward the one it should be by `seconds` x 510, at least
/// 1, of 255, and stops on it (0x007a1e90). Walking through a door the
/// ambient fades over at most half a second. Colours are 0..255.
inline glm::ivec3 easeAmbient(const glm::ivec3& current, const glm::ivec3& target, float seconds) {
    const int step = glm::max(1, static_cast<int>(seconds * 2.0f * 255.0f));
    glm::ivec3 out = current;
    for (int i = 0; i < 3; ++i) {
        if (target[i] > current[i]) out[i] = glm::min(current[i] + step, target[i]);
        else if (target[i] < current[i]) out[i] = glm::max(current[i] - step, target[i]);
    }
    return out;
}

/// The transition batches' vertices are those below this: one past the
/// highest vertex of the last transition batch (0x007d7380 reads MOBA entry
/// transBatchCount - 1's last vertex). 0 for a group with none.
inline uint32_t transitionVertexEnd(const WMOGroup& group) {
    if (group.transBatchCount == 0 || group.transBatchCount > group.batches.size()) return 0;
    return static_cast<uint32_t>(group.batches[group.transBatchCount - 1].lastVertex) + 1;
}

/// A group's vertex colours, rgba 0..1, as the client draws them: what
/// loadedVertexColor leaves, unless MOHD flag 0x8 is set (0x007d7c30 then
/// skips 0x007d7380). A group without MOCV (MOGP flag 0x4) has none to fix.
inline std::vector<glm::vec4> loadedVertexColors(const WMOGroup& group, uint32_t mohdFlags) {
    std::vector<glm::vec4> out;
    out.reserve(group.vertices.size());
    const bool fixUp = (group.flags & 0x4u) && !(mohdFlags & 0x8u);
    const uint32_t transitionEnd = transitionVertexEnd(group);
    for (size_t i = 0; i < group.vertices.size(); ++i) {
        glm::ivec4 c(glm::round(glm::clamp(group.vertices[i].color, 0.0f, 1.0f) * 255.0f));
        if (fixUp) c = loadedVertexColor(c, i < transitionEnd);
        out.push_back(glm::vec4(c) / 255.0f);
    }
    return out;
}

/// Which fog a batch of a WMO group is drawn with (0x007a9380 by way of
/// 0x007a8440): none, the zone's colour (0xd38b8c), or the camera's with its
/// interior fog blended in (0xd38ba0).
enum class BatchFog : uint8_t { None, Zone, Camera };

/// A transition batch (one of MOGP's first transBatchCount) is drawn twice:
/// lit by the outside light (0x007a8b10 mode 1, 2 or 0 unlit) with blend 9,
/// colour times alpha, then by the interior light (mode 3) with blend 7,
/// colour times one minus alpha, added. The alpha is the vertex colour's,
/// which 0x007d7380 left on those vertices. Each pass has its own fog; the
/// material's F_UNFOGGED (0x2) takes it off both, and it is the only place
/// the client reads that flag. Every other batch is fogged by its group: the
/// zone's colour for an exterior or exterior-lit group (0x48), and for an
/// interior one the camera's when the group is drawn in the camera's
/// interior pass (interiorPassGroups; 0x007a9380's local_14), the zone's
/// otherwise.
struct BatchFogs {
    BatchFog outsidePass = BatchFog::Zone;  // the transition batch's first pass
    BatchFog insidePass = BatchFog::Zone;   // its second, or the only pass
};
inline BatchFogs batchFogs(uint32_t groupFlags, bool transitionBatch, uint32_t materialFlags,
                           bool interiorPass) {
    const BatchFog inside = interiorPass ? BatchFog::Camera : BatchFog::Zone;
    BatchFogs out;
    if (transitionBatch) {
        const bool unfogged = (materialFlags & 0x2u) != 0;
        out.outsidePass = unfogged ? BatchFog::None : BatchFog::Zone;
        out.insidePass = unfogged ? BatchFog::None : inside;
        return out;
    }
    out.insidePass = (groupFlags & kOutsideGroupFlags) ? BatchFog::Zone : inside;
    out.outsidePass = out.insidePass;
    return out;
}

/// The light a pass of a WMO batch is drawn in: 0x007a8b10's mode, which
/// 0x007a9380 picks from the group and the material. Unlit (0) none; Outside
/// (1) the zone's direct and ambient light; Averaged (2) the two averaged
/// (averagedOutsideLight); Interior (3) the MOHD ambient and no direct light.
enum class BatchLight : uint8_t { Unlit = 0, Outside = 1, Averaged = 2, Interior = 3 };
struct BatchLights {
    BatchLight outsidePass = BatchLight::Outside;  // the transition batch's first pass
    BatchLight insidePass = BatchLight::Outside;   // its second, or the only pass
};
/// A transition batch's first pass is unlit with F_UNLIT (0x1), averaged with
/// material flag 0x20, outside otherwise; its second is the interior light.
/// Any other batch of an exterior or exterior-lit group (0x48) is unlit with
/// F_UNLIT and outside otherwise, 0x20 not read; of an interior group it is
/// averaged with 0x20 and the interior light otherwise, F_UNLIT not read.
inline BatchLights batchLights(uint32_t groupFlags, bool transitionBatch, uint32_t materialFlags) {
    const bool unlit = (materialFlags & 0x1u) != 0;
    const bool averaged = (materialFlags & 0x20u) != 0;
    BatchLights out;
    if (transitionBatch) {
        out.outsidePass = unlit ? BatchLight::Unlit : averaged ? BatchLight::Averaged : BatchLight::Outside;
        out.insidePass = BatchLight::Interior;
        return out;
    }
    if (groupFlags & kOutsideGroupFlags) {
        out.insidePass = unlit ? BatchLight::Unlit : BatchLight::Outside;
    } else {
        out.insidePass = averaged ? BatchLight::Averaged : BatchLight::Interior;
    }
    out.outsidePass = out.insidePass;
    return out;
}

/// Light mode 2's colours (0x007ee750 keeps them at 0xd38cb0/cb4): a direct
/// light half way from the zone's direct light toward its ambient, and an
/// ambient half way from the ambient toward the direct light and 16/255
/// brighter, clamped. Bytes (0..255 a channel), each step rounded down as the
/// client's packed arithmetic does. Worked out before the liquid's darkening
/// (0x007f3230 tail) and the glare's (0x007816f0), which touch only
/// 0xd38ca8/cac.
struct AveragedLight {
    glm::ivec3 direct{0};
    glm::ivec3 ambient{0};
};
inline AveragedLight averagedOutsideLight(glm::ivec3 direct, glm::ivec3 ambient) {
    AveragedLight out;
    for (int c = 0; c < 3; ++c) {
        const int d = direct[c];
        const int a = ambient[c];
        out.direct[c] = d + ((a - d) >> 1);
        out.ambient[c] = glm::min(a + ((d - a) >> 1) + 0x10, 0xff);
    }
    return out;
}

/// The groups of one WMO drawn in the camera's interior pass. 0x007ad1f0
/// starts a portal walk (0x007ac060) from each group the camera is in with
/// the pass flag set; a group with 0x8 or 0x40 clears it for itself and all
/// it leads to, a group with 0x10000 is not walked. A group so reached sets
/// DAT_00cfbeb8, which 0x007a9380 fogs in the camera's colour, and the
/// objects in it 0x8000 (0x00799310, 0x0079a260), which 0x007c1730 does the
/// same for. `neighbours[g]`: the groups g's portals lead to that the walk
/// may pass. `cameraGroups`: the groups the camera is in.
inline std::vector<uint8_t> interiorPassGroups(const std::vector<uint32_t>& groupFlags,
                                               const std::vector<std::vector<uint32_t>>& neighbours,
                                               const std::vector<uint32_t>& cameraGroups) {
    std::vector<uint8_t> pass(groupFlags.size(), 0);
    std::vector<uint32_t> open;
    const auto visit = [&](uint32_t g) {
        if (g >= groupFlags.size() || pass[g]) return;
        if (groupFlags[g] & (kOutsideGroupFlags | 0x10000u)) return;
        pass[g] = 1;
        open.push_back(g);
    };
    for (uint32_t g : cameraGroups) visit(g);
    while (!open.empty()) {
        const uint32_t g = open.back();
        open.pop_back();
        if (g >= neighbours.size()) continue;
        for (uint32_t n : neighbours[g]) visit(n);
    }
    return pass;
}

/// The floor search under a world object (0x007c2f80): a unit's from a tenth
/// of a yard above its feet (0x007c2a70), a game object's from four yards
/// above or a tenth above its top if that is lower (0x007c2e70), both down
/// a thousand yards.
inline constexpr float kUnitFloorAbove = 0.1f;
inline constexpr float kFloorReach = 1000.0f;
inline float gameObjectFloorStart(float feetZ, float topZ) { return glm::min(feetZ + 4.0f, topZ + 0.1f); }

/// Whether the terrain takes the place of a WMO floor found by that search
/// (0x007c28f0): when the ground under the start is nearer than the floor -
/// both as a fraction of the thousand yards - the object stands on the
/// ground and takes the zone's light. Ground above the start is not counted.
inline bool terrainNearer(float startZ, const std::optional<float>& terrainZ, float floorZ) {
    if (!terrainZ) return false;
    const float terrain = (startZ - *terrainZ) * 0.001f;
    if (terrain < 0.0f) return false;
    return terrain < (startZ - floorZ) / kFloorReach;
}

/// What a world object's floor search found under it: the interior floor's
/// light when it stands on one, whether it stands on any WMO floor (0x007c2a70
/// sets +0xc 0x200; a game object's search never does), and whether its feet
/// are in the terrain's baked shadow where that counts (directScaleTarget).
struct ObjectFloorState {
    std::optional<FloorLight> light;
    bool onWmo = false;
    bool inBakedShadow = false;
};

/// The scale on a world object's direct light it eases toward (0x007a1bc0,
/// at +0xc4). On an interior floor 1, or 1 + alpha x 1.5 on a transition
/// face; outside 2.5 - so 1 once clamped - or 0.5 when its feet are in the
/// terrain's baked shadow (0x007a06a0), which counts only with
/// extShadowQuality below 2 and not for a unit on a WMO floor.
inline float directScaleTarget(bool onInteriorFloor, int towardOutside, bool inBakedShadow) {
    if (onInteriorFloor) {
        return towardOutside > 0 ? static_cast<float>(towardOutside) / 255.0f * 1.5f + 1.0f : 1.0f;
    }
    return inBakedShadow ? 0.5f : 2.5f;
}

/// That scale as drawn (+0x8c): it moves toward the target by 3.33 a second,
/// stops on it, and is never above 1 (0x007a1e90). 0x00781a10 starts it at 1.
inline float easeDirectScale(float current, float target, float seconds) {
    const float step = seconds * 3.3333333f;
    float out = current;
    if (current > target) out = glm::max(current - step, target);
    else if (current < target) out = glm::min(current + step, target);
    return glm::min(out, 1.0f);
}

/// Which of a model's MODD doodads are lit as interior ones: those that only
/// interior groups list in MODR. A group that is exterior or exterior-lit
/// makes its doodads take the zone's light, and one that does keeps it
/// whatever else lists the doodad (0x007bf740 sets the exterior bit and never
/// clears it, and tests it first). A doodad no group lists the client never
/// makes; it is left to the zone's light here.
inline std::vector<uint8_t> interiorDoodads(const WMOModel& model) {
    std::vector<uint8_t> state(model.doodads.size(), 0);  // 0 unlisted, 1 interior, 2 exterior
    for (const auto& group : model.groups) {
        const bool outside = (group.flags & kOutsideGroupFlags) != 0;
        for (uint16_t ref : group.doodadRefs) {
            if (ref >= state.size()) continue;
            if (outside) state[ref] = 2;
            else if (state[ref] == 0) state[ref] = 1;
        }
    }
    for (auto& s : state) s = (s == 1) ? 1 : 0;
    return state;
}

}  // namespace wowee::pipeline::wmo_doodad_light
