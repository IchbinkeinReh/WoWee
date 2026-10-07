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
