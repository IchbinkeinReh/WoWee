#version 460
#ifdef RT_HARDWARE
#extension GL_EXT_ray_query : require
#endif
// One sample per trace pixel of each enabled term:
//   sun visibility - a ray toward a point on the sun's disc
//   ambient occlusion - a short cosine-weighted ray
//   point light visibility - a ray toward one of the frame's point lights
// Noisy by design; rt_temporal and rt_filter turn it into something usable.
// The diffuse light that bounces is the probe volume's (rt_gi_trace), which
// the surfaces read for themselves.
//
// The point lights (rt_point_lights.hpp) are taken whole and shadowed by one
// of them. Their unshadowed light at the pixel - every light's colour by its
// falloff and N.L - is exact and goes out as it is (uPointOut). Their shadow
// is one ray, to a light picked in proportion to what it gives here, whose
// visibility is an unbiased estimate of the fraction of that light which
// arrives: the sum of each light's share times its own visibility. That
// fraction is what the temporal and spatial filters smooth, in the third
// channel beside the sun's and the occlusion's, and the surfaces multiply the
// two back together (rt_lighting.glsli). A flickering torch then flickers at
// once, without waiting for its shadow to converge, and a dozen lights cost
// one ray a pixel, not a dozen.
//
// A building's own lights (MOLT) are baked into its interior's vertex
// colours already. Their part of the unshadowed light goes out beside it, in
// uPointOut's alpha (the brightest channel's worth), so those surfaces can
// take the rest of the light and only the shadows of these
// (rt_lighting.glsli).
//
// Only on their own storey, though (rt_light_storey.hpp): a building's light
// leaves out a building's baked colour above its ceiling or below its floor
// altogether - neither its light nor its shadow there. The Goldshire inn's
// hall lights sit a yard or so under its upper floor; every ray to them from
// the rooms upstairs meets the floorboards, and the walls there, which face
// them sideways, went dark with their shadow over colours they never lit.

#include "rt_common.glsli"
#ifdef RT_HARDWARE
#include "rt_trace_hw.glsli"
#else
#include "rt_trace_sw.glsli"
#endif

layout(set = 0, binding = 3) uniform sampler2D uGbufCur;
layout(set = 0, binding = 4, rgba16f) uniform writeonly image2D uRaw;
// In: the normal the surface is lit by, and in z whether it is a building's
// baked colour (rt_prepare). Out: the point lights' light; a = the baked
// lights' part.
layout(set = 0, binding = 11, rgba16f) uniform image2D uPointOut;
// The probe atlas (rt_gi.glsli), for which of the probes round a point it can
// see (rtProbesSeen).
layout(set = 0, binding = 15) uniform sampler2D uGiAtlas;

// Must match rt_lights::PointLightGPU.
struct RtPointLight {
    vec4 positionEnd;  // xyz, w = where its light ends (yards), negative for a baked one
    vec4 colorStart;   // rgb, w = where its falloff starts
    vec4 storey;       // a baked one's floor and ceiling (world z), z = 1 when it has them;
                       // w what gave it (rt_lights::Source)
};

// The Debug Graphics view "Kind of each light" (rt_light_kinds.hpp): what
// each light gives, drawn as its brightness in the colour of what gave it.
const int RT_LIGHT_KIND_VIEW = 14;
// By rt_lights::Source. Must match rt_light_kinds::kKinds, index for index
// (tests/test_rt_light_kinds.cpp reads this list).
vec3 rtLightKindColour(int kind) {
    const vec3 kKinds[7] = vec3[7](
        vec3(1.0, 1.0, 1.0),    // model light: white
        vec3(1.0, 0.12, 0.05),  // fire: red
        vec3(1.0, 0.85, 0.1),   // lamp: yellow
        vec3(0.15, 0.35, 1.0),  // building light: blue
        vec3(0.1, 1.0, 0.2),    // unit light: green
        vec3(0.1, 0.95, 1.0),   // glow: cyan
        vec3(1.0, 0.15, 0.85)   // particles: magenta
    );
    return kKinds[clamp(kind, 0, 6)];
}
layout(std430, set = 0, binding = 12) readonly buffer RtPointLights { RtPointLight rtPointLights[]; };

// rt_lights::attenuation: all within the start, none past the end, a
// smoothed ramp between.
float rtLightFalloff(float d, float start, float end) {
    if (d >= end) return 0.0;
    start = clamp(start, 0.0, end);
    if (d <= start) return 1.0;
    float t = (end - d) / (end - start);
    return t * t * (3.0 - 2.0 * t);
}

// A light is a sphere, so its shadows have a soft edge: the larger the
// light's reach the larger its source - a candle's flame, a lantern's glass,
// a hearth - from 0.15 to half a yard. At a fixed 0.15 every lamp's shadow of
// a post or a door frame was knife-sharp to its end, a hard wedge and arc on
// the floor (the Goldshire inn upstairs).
float rtLightRadius(float reach) {
    return clamp(0.04 * reach, 0.15, 0.5);
}
// The Ray Tracing page's "Realistic light falloff" (pass.tune5.z): the inverse
// square of the distance from a core of radius r0, windowed to reach nothing
// at the same end (rt_lights::realisticAttenuation, which must agree):
//     I = 2 / (1 + (d / r0)^2) * (1 - (d / end)^4)^2
// r0 is the light's full-strength window (its start), or its source's size
// (rtLightRadius) where that is larger. The 2 makes it as strong as the
// window at r0, where the two curves cross; nearer it is up to twice as
// bright, farther a long soft tail instead of a ramp and an edge.
const float RT_REALISTIC_GAIN = 2.0;
float rtLightFalloffRealistic(float d, float start, float end) {
    if (d >= end) return 0.0;
    float r0 = max(clamp(start, 0.0, end), rtLightRadius(end));
    float q = d / r0;
    float x = d / end;
    float w = 1.0 - x * x * x * x;
    return RT_REALISTIC_GAIN * w * w / (1.0 + q * q);
}
// A light sits inside its lamp - a lantern's glass, a brazier's bowl - so a
// shadow ray stops this short of it rather than finding the lamp itself.
const float RT_LIGHT_STANDOFF = 0.45;
// The most a lamp's source is wide, and the ray stopped short of it, when
// the lights shadow themselves: well inside the walls round a flame.
const float RT_SELF_SHADOW_SOURCE_RADIUS = 0.08;
// How far a unit's point-light shadow reaches from its body: no farther than
// pass.tune4.z yards (the Ray Tracing page's "Unit shadow reach", 5 by
// default), nor than three quarters of the way from the
// surface to the light, and fading out over the last half of it
// (rtUnitShadowReach).
const float RT_UNIT_SHADOW_SHARE = 0.75;
const float RT_UNIT_SHADOW_FADE = 0.5;

// How far along a point light's shadow ray, of length d from the surface, a
// unit may stop it (rtUnitReach). A unit's traced body is its own triangles
// cut into rigid pieces, each held by one bone and placed by its bone
// (character_renderer_rt.cpp): right in size, place and pose, but not the
// skin at the joints. Close to a body that is the shadow one expects - feet,
// legs, the floor beside it, a wall it stands against. A lamp just above and
// behind the head, though, threw the body's shadow across the room: the
// farther the surface is from the body against the body's distance to the
// light, the more the pieces' every error is magnified, to a hard,
// room-sized wedge (the Goldshire inn's upper floor). So the body occludes
// only the part of the ray near the surface - within pass.tune4.z
// yards and within RT_UNIT_SHADOW_SHARE of the way to the light, so its
// shadow is, at the 5 yards it defaults to, never more than four times its
// size - and that end is drawn per
// ray, down to RT_UNIT_SHADOW_FADE of it: the shadow is whole up to about
// 1.6 times the body's size and fades out beyond, rather than stops at an
// edge. A unit's own surface never meets its own pieces (its group's box,
// rtInGroup), which do not hold its skin closely enough to shadow it.
//
// Past its default the slider governs this as well: the share of the way to
// the light grows to RT_UNIT_SHADOW_SHARE_MAX by RT_UNIT_SHADOW_OPEN_REACH
// yards, and the fade moves from that share to the slider's own end. Near a
// fire the share was the whole limit - a body two yards from the flames cast
// a yard-long stub of a shadow whatever the slider said - while the shadow
// of a body two yards from a light low enough to stand over runs on well
// past it. At the default the reach is what it was.
const float RT_UNIT_SHADOW_DEFAULT_REACH = 5.0;
const float RT_UNIT_SHADOW_OPEN_REACH = 20.0;
const float RT_UNIT_SHADOW_SHARE_MAX = 0.98;
float rtUnitShadowReach(float d, inout uint rng) {
    float slider = pass.tune4.z;
    float open = clamp((slider - RT_UNIT_SHADOW_DEFAULT_REACH) /
                       (RT_UNIT_SHADOW_OPEN_REACH - RT_UNIT_SHADOW_DEFAULT_REACH), 0.0, 1.0);
    float share = mix(RT_UNIT_SHADOW_SHARE, RT_UNIT_SHADOW_SHARE_MAX, open);
    float fade = mix(RT_UNIT_SHADOW_FADE, 1.0, rtRandom(rng));
    return min(slider * fade, share * d * mix(fade, 1.0, open));
}

// How far outside a light's storey a building's baked colour still takes
// its shadow: the floor's own boards, the ceiling's (rt_light_storey::kMargin).
const float RT_STOREY_MARGIN = 0.25;

// Whether a baked light reaches a building's baked colour at height z: only
// from its own storey (rt_light_storey::shades).
bool rtOnStorey(vec4 storey, float z) {
    return storey.z < 0.5 || (z >= storey.x - RT_STOREY_MARGIN && z <= storey.y + RT_STOREY_MARGIN);
}

// The point shadow sample of a light that the ground or a building stops
// (rt_lights::hardShadowVisibility). The surfaces take the point lights as
// their unshadowed light times mix(1, vis, s), s the point shadows' strength
// (Debug Graphics), so below full strength every shadow lets 1 - s of the
// light through - a softening meant for the shadow of a table or a post in
// the room the light is in. Through a floor or a wall it lit what the light
// cannot reach: the Goldshire inn's cellar, seven yards under the hall, took
// 30% of the hearth's flames (particle lights, reach 15) on its walls,
// barrels and stairs, under the floorboards that stopped every ray. A sample
// of (s - 1) / s makes mix(1, vis, s) zero; clamped to 0..1 (the baked
// lamps' shadow, rtBakedDarkness) it is the shadow it is. At s = 0 there are
// no point shadows at all.
float rtHardShadowVis(float s) {
    return s > 1e-3 ? (s - 1.0) / s : 0.0;
}

// How far, in yards per unit of the sine between a face and its smooth
// normal, a shadow ray is lifted off its triangle toward the smooth surface:
// about a quarter of a ground triangle's edge (4.17 yards), the height of a
// rounded surface over the chord of it.
const float RT_TERMINATOR_LIFT = 1.0;
// The kinds of light that are a glow rather than a lamp (rt_lights::Source:
// Unit 4, Emissive 5, Particle 6), and how far over its centre a glow is
// taken to shine from (rtLightAim).
const int RT_LIGHT_KIND_UNIT = 4;
const int RT_LIGHT_KIND_EMISSIVE = 5;
const int RT_LIGHT_KIND_PARTICLE = 6;
const float RT_GLOW_LIFT = 1.0;
// How wide a particle light's source is taken to be for its shadow rays.
const float RT_PARTICLE_SOURCE_RADIUS = 1.5;
// A floor's normal this far off its face is a fold's, not a bump's: from all
// bump below about 10 degrees to all fold past about 18.
const float RT_FLOOR_BUMP_COS = 0.985;
const float RT_FLOOR_LEAN_COS = 0.95;
// How far a ray leaving a triangle turned from its light has to go to be off
// it: a ground triangle's longest edge, about three yards.
const float RT_TERMINATOR_REACH = 3.0;

// How far past the terminator the lamps' light wraps, (N.L + w) / (1 + w),
// is pass.tune3.w: 0.25 (a face turned some 15 degrees from a lamp still
// takes a little of it), 0.5 with "Softer lamp light" (some 30).
// The sine of how far above a face a lamp must be for the surface's own
// normal (its normal map's bumps) to light by: none below 3 degrees, all of
// it from 20.
const float RT_LAMP_DETAIL_LOW = 0.05;
const float RT_LAMP_DETAIL_HIGH = 0.34;

// Where a light shines from, for the faces it reaches and its shadow rays.
// A lamp from its point. A glow - a unit's light, a glowing surface, a
// cloud of particles - is a volume of light, not the point at its centre:
// the Dreadsteed's burning hooves put theirs a hand's width over the
// ground, so the ground took it at a few degrees, every triangle tilted a
// little away was left dark and every bump between cut the rest, and the
// orange round the mount came out in the terrain's triangles. It shines
// from the top of its volume (RT_GLOW_LIFT over the centre): the ground's
// own grain no longer turns from it or stands in its way, while anything
// that stands up between - a crate, a wall, a leg - still does.
vec3 rtLightAim(RtPointLight light) {
    vec3 at = light.positionEnd.xyz;
    int kind = int(light.storey.w + 0.5);
    if (kind == RT_LIGHT_KIND_UNIT || kind == RT_LIGHT_KIND_EMISSIVE || kind == RT_LIGHT_KIND_PARTICLE) {
        at.z += RT_GLOW_LIFT;
    }
    return at;
}

// How a light goes out over a floor as it comes down to the floor's face.
const float RT_FLOOR_FACE_FADE_LOW = -0.02;
const float RT_FLOOR_FACE_FADE_HIGH = 0.10;

// A character's shadow on itself (the Unit self-shadowing option,
// pass.tune5.y), by the surface the drawing shades - its interpolated normal -
// not the traced body's flat triangles. That body is low-poly, and its
// pieces are rigid where the drawn one bends; a ray leaving a pixel along
// its own triangle met the triangles beside it, and the self-shadow followed
// the mesh in hard facets. The ray here starts lifted off the smooth surface
// (RT_SELF_LIFT along the shading normal, onto where the smooth surface
// stands over the triangle) and asks the character alone (rtOwnGroupOnly);
// what it finds is let in by the smooth normal's angle to the light, from
// nothing at the terminator, where the smooth and the flat surface part, to
// all of it where the light falls well onto the face - so the self-shadow's
// edge runs with the smooth shading. 1 where the option is off, or the
// point is not on a character.
const float RT_SELF_LIFT = 0.04;
float rtSelfVis(vec3 surface, vec3 shadingN, vec3 dir, float reach, inout uint rng) {
    if (pass.tune5.y < 0.5) return 1.0;
    float facing = dot(shadingN, dir);
    if (facing <= 0.0) return 1.0;  // turned from the light: the shading has none to take
    bool skipUnits = rtSkipUnits;
    rtSkipUnits = false;
    rtOwnGroupOnly = true;
    bool fromSet = rtGroupFromSet;
    vec3 from = rtGroupFrom;
    rtGroupFrom = surface;
    rtGroupFromSet = true;
    bool hit = rtOccluded(surface + shadingN * RT_SELF_LIFT, dir, reach, rng);
    rtGroupFrom = from;
    rtGroupFromSet = fromSet;
    rtOwnGroupOnly = false;
    rtSkipUnits = skipUnits;
    if (!hit) return 1.0;
    return 1.0 - smoothstep(0.05, 0.35, facing);
}

#include "rt_gi.glsli"

// giSampleCascade (rt_gi.glsli) with every probe it would weigh asked by a
// ray: none that geometry stands between the point and it counts. The probe
// volume's visibility tiles are a few texels a probe, each a wide cone's mean
// depth; past a wall thinner than a probe cell - the Tirisfal zeppelin
// tower's, a yard of stone - a probe outside in the sunlit air passed their
// test for the room's floor at the wall's foot, while the probes inside were
// buried in the stone, and with the weights normalised the outside one lit
// the floor alone: a lilac band of sky light round the room (user162). All
// eight are asked, not the heaviest few: the one that leaks is often a light
// one, and dropping the heavy ones would hand it the whole of the cell.
vec4 rtGiSampleCascadeSeen(int cascade, vec4 vol, vec4 volMod, vec3 p, vec3 n, vec3 v, inout uint rng) {
    float spacing = vol.w;
    vec3 pb = p + (n * 0.2 + v * 0.8) * (0.225 * spacing);
    vec3 g = pb / spacing - vol.xyz;
    ivec3 base = clamp(ivec3(floor(g)), ivec3(0), GI_PROBES - 2);
    vec3 alpha = clamp(g - vec3(base), 0.0, 1.0);
    vec3 sum = vec3(0.0);
    float wsum = 0.0;
    // Asked from just off the surface, as the probes' own bias has it, but
    // from the surface's side: the biased point can stand past a thin wall.
    vec3 from = p + n * max(0.05, 0.02 * spacing);
    for (int i = 0; i < 8; ++i) {
        ivec3 off = ivec3(i & 1, (i >> 1) & 1, i >> 2);
        ivec3 local = base + off;
        ivec3 st = giStorage(volMod, local);
        vec3 probe = giProbeWorld(vol, local) + giProbeOffset(uGiAtlas, cascade, st);
        vec3 toProbe = normalize(probe - p);
        float back = (dot(toProbe, n) + 1.0) * 0.5;
        float w = back * back + 0.2;
        vec3 fromProbe = pb - probe;
        float d = length(fromProbe);
        vec3 vis = textureLod(uGiAtlas, giTileUv(giVisibilityTileOrigin(cascade, st), GI_VIS_TEXELS,
                                                 fromProbe / max(d, 1e-4)), 0.0).rgb;
        if (vis.b > 0.5) continue;
        vec2 m = vis.rg;
        if (d > m.x) {
            float variance = abs(m.x * m.x - m.y);
            float over = d - m.x;
            float cheb = variance / (variance + over * over);
            w *= max(cheb * cheb * cheb, 0.05);
        }
        w = max(w, 1e-6);
        if (w < 0.2) w *= w * w / 0.04;
        vec3 tri = mix(1.0 - alpha, alpha, vec3(off));
        w *= tri.x * tri.y * tri.z;
        if (w < 1e-5) continue;
        vec3 toward = probe - from;
        float reach = length(toward);
        if (reach > 1e-3) {
            rtSkipUnits = true;
            rtCutoutsOpen = true;
            bool hidden = rtOccluded(from, toward / reach, reach, rng);
            rtCutoutsOpen = false;
            rtSkipUnits = false;
            if (hidden) continue;
        }
        vec3 irr = textureLod(uGiAtlas, giTileUv(giIrradianceTileOrigin(cascade, st), GI_IRR_TEXELS, n), 0.0).rgb;
        sum += sqrt(max(irr, vec3(0.0))) * w;
        wsum += w;
    }
    vec3 r = sum / max(wsum, 1e-6);
    return vec4(r * r, clamp(wsum * 20.0, 0.0, 1.0));
}

// giIrradiance with the probes' visibility asked (rtGiSampleCascadeSeen).
vec4 rtGiIrradianceSeen(vec3 p, vec3 n, vec3 v, inout uint rng) {
    float c0 = giCascadeCover(pass.giVolume[0], p);
    vec4 s0 = c0 > 0.0 ? rtGiSampleCascadeSeen(0, pass.giVolume[0], pass.giVolumeMod[0], p, n, v, rng) : vec4(0.0);
    c0 *= s0.a;
    if (c0 >= 1.0) return vec4(s0.rgb, 1.0);
    float c1 = giCascadeCover(pass.giVolume[1], p);
    vec4 s1 = c1 > 0.0 ? rtGiSampleCascadeSeen(1, pass.giVolume[1], pass.giVolumeMod[1], p, n, v, rng) : vec4(0.0);
    c1 *= s1.a;
    float w0 = c0;
    float w1 = c1 * (1.0 - c0);
    float cover = w0 + w1;
    if (cover <= 1e-4) return vec4(0.0);
    return vec4((s0.rgb * w0 + s1.rgb * w1) / cover, cover);
}

// How much of the probes' ratio over the flat ambient (rt_lighting.glsli's
// rtGiRatio, as the surfaces take it) is left with only the probes the point
// can see: their ambient is scaled by it with the occlusion, never brightened.
float rtProbesSeen(vec3 p, vec3 n, inout uint rng) {
    if (pass.giOpenParams.x < 0.5) return 1.0;
    vec3 v = normalize(pass.cameraPos.xyz - p);
    vec4 eAll = giIrradiance(uGiAtlas, pass.giVolume[0], pass.giVolumeMod[0], pass.giVolume[1], pass.giVolumeMod[1],
                            p, n, v);
    vec4 seen = rtGiIrradianceSeen(p, n, v, rng);
    float lift = texelFetch(uGiAtlas, GI_EXPOSURE_TEXEL, 0).r;
    lift = lift > 0.0 ? lift : 1.0;
    vec3 rAll = giAmbientRatio(eAll, pass.giOpenSky.rgb, pass.giOpenGround.rgb, n, pass.giOpenSky.w,
                               pass.giOpenGround.w, pass.giOpenParams.z, lift);
    vec3 rSeen = giAmbientRatio(seen, pass.giOpenSky.rgb, pass.giOpenGround.rgb, n, pass.giOpenSky.w,
                                pass.giOpenGround.w, pass.giOpenParams.z, lift);
    float lAll = dot(rAll, GI_LUMA);
    if (lAll <= 1e-4) return 1.0;
    return clamp(dot(rSeen, GI_LUMA) / lAll, 0.0, 1.0);
}

layout(local_size_x = 8, local_size_y = 8) in;

void main() {
    ivec2 px = ivec2(gl_GlobalInvocationID.xy);
    if (any(greaterThanEqual(px, ivec2(pass.extent.xy)))) return;

    vec4 g = texelFetch(uGbufCur, px, 0);
    float dist = rtGbufDist(g);
    if (dist <= 0.0) {
        imageStore(uRaw, px, vec4(1.0, 1.0, 1.0, 0.0));
        imageStore(uPointOut, px, vec4(0.0));
        return;
    }
    vec3 n = rtGbufNormal(g);
    vec4 normalIn = imageLoad(uPointOut, px);
    vec3 shadingN = rtOctDecode(normalIn.xy);
    // rt_prepare's share bits (RtSurface::storeyBound): a building's baked
    // colour, or the ground.
    uint shareBits = uint(normalIn.z + 0.5);
    bool storeyBound = (shareBits & RT_GB_SHARE_STOREY) != 0u;
    // And a building's floor outside its rooms, by the smooth normal the
    // G-buffer resolved for it (RT_GB_SHARE_LAMP_NORMAL): measured by the
    // faces, a fire low over Orgrimmar's streets faded the triangles turned
    // a degree from it to nothing.
    bool terrain = (shareBits & (RT_GB_SHARE_TERRAIN | RT_GB_SHARE_LAMP_NORMAL)) != 0u;
    // A room's baked colour: its normal already blended toward its face in
    // the G-buffer (rtInteriorLampNormal), so not again here.
    bool normalResolved = normalIn.w > 0.5;
    vec3 p = rtGbufWorld(g, px);
    vec3 origin = p + n * rtSurfaceOffset(dist);
    uint rng = rtSeed(uvec2(px), uint(pass.cameraPos.w));
    uint mode = uint(pass.params.x + 0.5);

    // Whether a unit stopped one of this pixel's rays: a unit moves, and
    // its shadow must not stay behind in the history (rt_temporal).
    bool unitHit = false;

    float sun = 0.0;
    vec3 L = normalize(pass.sunDir.xyz);
    if (pass.sunColor.w > 0.5) {
        // A surface turned away from the sun is traced from its far side.
        // That was taken as shadowed outright, and is for a solid - its ray
        // starts inside and leaves through the solid's own sunlit face, as a
        // hillside's runs under the ground until it comes out of it. But a
        // card - a tuft of grass, a leaf - has nothing behind it: seen from
        // its shaded face, against the sun, it is lit through, and a
        // meadow looked at toward an evening sun went dark tuft by tuft.
        // Its light still comes by its own N.L in the surface shaders, or
        // by the ground's (ground clutter); this is only whether anything
        // stands between it and the sun.
        vec3 sunOrigin = dot(n, L) > 0.0 ? origin : p - n * rtSurfaceOffset(dist);
        // The shadow terminator: a surface shaded by its smooth normal is
        // lit where that faces the sun, but the ray leaves its flat
        // triangle, and near the terminator the next triangles of a
        // rounded hill rise across it - or the triangle itself is turned
        // from the sun - and whole triangles went dark in hard wedges on a
        // hillside under a low sun. The ray starts instead from about
        // where the smooth surface would be (Hanika, "Hacking the Shadow
        // Terminator"): lifted along the smooth normal by the triangle's
        // bend from it, the more the lower the sun stands over the face.
        float smoothToSun = dot(shadingN, L);
        if (smoothToSun > 0.0) {
            float bend = length(cross(n, shadingN));
            float lift = RT_TERMINATOR_LIFT * bend * (1.0 - clamp(dot(n, L), 0.0, 1.0));
            if (dot(n, L) <= 0.0) {
                // A triangle turned from the sun its smooth normal still
                // faces: the ray runs down into its own plane, and must be
                // high enough to have left the triangle before it gets
                // there - its far edge some RT_TERMINATOR_REACH away.
                sunOrigin = p + shadingN * rtSurfaceOffset(dist);
                lift = max(lift, RT_TERMINATOR_REACH * -dot(n, L) / max(dot(shadingN, n), 0.5));
            }
            sunOrigin += shadingN * lift;
        }
        // Within the shadow map's reach the units' sun shadows are the map's:
        // drawn this frame in the pose on screen. Traced, they came a frame
        // late and settled over several more, and a running body trailed a
        // ghost of its staff and legs over the ground and itself.
        vec3 fromMap = p - pass.unitMap.xyz;
        rtSkipUnits = dot(fromMap, fromMap) < pass.unitMap.w * pass.unitMap.w;
        // As many rays as the settings ask, each to its own point on the
        // sun's disc; the share that gets through.
        uint sunRays = rtRayCount(pass.rays.x);
        // A unit's own body shadows itself while the option asks for it
        // (pass.tune5.y), by rtSelfVis: the smooth surface's, not its triangles'.
        for (uint k = 0u; k < sunRays; ++k) {
            vec3 sd = rtConeSample(L, pass.sunDir.w, rng);
            bool byUnit;
            float through = rtOccludedBy(sunOrigin, sd, pass.params.w, rng, byUnit) ? 0.0 : 1.0;
            if (through > 0.0) through *= rtSelfVis(p, shadingN, sd, pass.params.w, rng);
            sun += through;
            unitHit = unitHit || byUnit;
        }
        sun /= float(sunRays);
        rtSkipUnits = false;
    }

    float ao = 1.0;
    if (mode >= RT_MODE_AO) {
        uint aoRays = rtRayCount(pass.rays.y);
        float aoSum = 0.0;
        rtCutoutsOpen = true;
        for (uint k = 0u; k < aoRays; ++k) {
            vec3 ad = rtCosineHemisphere(n, rng);
            float t;
            vec3 hn;
            vec4 hs;
            aoSum += rtClosest(origin, ad, pass.params.y, rng, t, hn, hs) ? t / pass.params.y : 1.0;
        }
        rtCutoutsOpen = false;
        ao = aoSum / float(aoRays);
    }
    // The probes' light only from the probes the point can see: the ambient
    // the occlusion scales is the probes' ratio over the flat one
    // (rt_lighting.glsli), so what is lost of it goes with the occlusion.
    // Debug views 1 and 3 (the probes' ratio and ambient) are drawn before
    // the trace; the apply pass scales them by what the trace leaves of the
    // probes' light, so there the occlusion carries that factor alone.
    if (mode >= RT_MODE_AO) {
        int view = int(pass.tune2.z + 0.5);
        float probes = rtProbesSeen(p, shadingN, rng);
        ao = (view == 1 || view == 3) ? probes : ao * probes;
    }

    // The point lights: their sum, and one of them picked by weighted
    // reservoir sampling in the same pass.
    // The normal the lamps light by: the surface's own where it agrees with
    // the face it is on, the face's where it does not. A building's floor
    // has its vertex normals smoothed with the risers of the steps on it -
    // tilted thirty and forty degrees toward them at the steps' foot - and
    // a candle on a step lights the floor from a few degrees above it, so
    // the triangles touching the steps came out several times brighter than
    // the next ones, cut off along a straight triangle edge across the floor
    // (Silvermoon, the Path of the Ancients). A character's or a pillar's
    // smoothing stays: their normals lie within some fifteen degrees of
    // their faces (rtNormalAgreement). A room's baked colour comes with the
    // same rule applied already, to its vertex normal (normalResolved).
    vec3 lightN = normalize(mix(n, shadingN, (normalResolved || terrain) ? 1.0 : rtNormalAgreement(n, shadingN, pass.tune4.xy)));
    // What "behind the surface" and "a lamp low over it" below are measured
    // against: a floor's face, as they were written for (a plank, a step, a
    // curb); a room's wall its smooth normal (rtInteriorLampNormal leaves
    // it its vertex normal); anything else its face and own normal half and
    // half. A wall's or a cave's rock is curved where its normal leans off
    // its faces, and measured by the faces a light near it lit some
    // triangles and not the next - a low-poly cave's every facet hard-edged
    // in its pool's and mushrooms' particle light, coming and going as the
    // particles drifted (Thunder Bluff's Pools of Vision).
    // A floor's normal that lies near its face (lightN comes to n where it does
    // not: the Undercity's) counts half as well, as on a curve: a folded hide or
    // a cave's uneven floor turned one triangle from a low lamp and lit the next,
    // black to bright along the edge between them.
    // A room's floor (normalResolved) by its lamp normal outright: bump-free
    // where the vertex normal is good (rtInteriorLampNormal), and the face's
    // where it was smoothed with the walls. Measured by the face, a low light
    // lit a cave floor triangle by triangle (the Pools of Vision).
    float floorLean = smoothstep(RT_FLOOR_BUMP_COS, RT_FLOOR_LEAN_COS, dot(n, lightN));
    // The ground by its smooth normal outright: a heightfield's vertex
    // normals are the shape of the hill its triangles cut through, not a
    // building floor's smoothed with its walls. Measured by the faces, a lamp
    // low over the ground - Camp Narache's tauren lamp posts, their flames
    // half a yard up - lit the triangles a degree or two turned toward it and
    // faded those turned away, and the edge of its light ran round it as a
    // ring of hard triangular wedges.
    vec3 faceRef = (normalResolved || terrain) ? lightN
                 : n.z >= RT_NORMAL_FLOOR_Z ? normalize(mix(n, lightN, floorLean)) : normalize(n + lightN);
    vec3 pointSum = vec3(0.0);
    float bakedSum = 0.0;
    float weightSum = 0.0;
    // One light picked a ray, each by its own draw in the same pass: with
    // one ray the pick as it always was.
    uint pointRays = rtRayCount(pass.rays.z);
    int picked[RT_MAX_RAYS] = int[RT_MAX_RAYS](-1, -1, -1, -1, -1);
    uint lightCount = uint(pass.params.z + 0.5);
    for (uint i = 0u; i < lightCount; ++i) {
        RtPointLight light = rtPointLights[i];
        vec3 toLight = rtLightAim(light) - p;
        float d2 = dot(toLight, toLight);
        float end = abs(light.positionEnd.w);
        if (d2 >= end * end) continue;
        // A building's light of another storey than this baked colour's.
        if (storeyBound && light.positionEnd.w < 0.0 && !rtOnStorey(light.storey, p.z)) continue;
        float d = sqrt(d2);
        // Behind the surface itself - below a floor, behind a wall - no
        // light, by its geometric face, whatever its normal map says: a
        // bumped plank's normal leans toward a lamp under the floor (the
        // hearth's light below the inn's upper rooms) and, its shadow ray
        // starting from the far side of the board, took that light in
        // grainy streaks.
        // A floor's only: anywhere else this cut a light off where the wrap
        // below still gave it a fifth of its strength, a step along a curved
        // line that moved with every particle over a cave's wall. There
        // the wrap runs it down to nothing, and a wall between stops it.
        // Faded rather than cut: the wrap below leaves a light a fifth of its
        // strength there. And by whichever of the floor's face and its smoothed
        // normal looks at the light the more: a flat floor whose vertex normals
        // lean some eleven degrees off its face (Thunder Bluff's lift car) turned
        // from a carried torch at half its reach, a ring round the bearer, on
        // boards the light still fell on. A cave's facet turned from a lamp its
        // smooth normal faces keeps it the other way round.
        float faceFade = 1.0;
        float faceRise = d > 1e-4 ? dot(terrain ? faceRef : n, toLight) / d : 1.0;
        if (d > 1e-4 && n.z >= RT_NORMAL_FLOOR_Z) {
            faceFade = smoothstep(RT_FLOOR_FACE_FADE_LOW, RT_FLOOR_FACE_FADE_HIGH,
                                  max(dot(faceRef, toLight) / d, faceRise));
            if (faceFade <= 0.0) continue;
        }
        // Wrapped a little past the terminator: a lamp is a flame, a glass, a
        // glow, not a point, and a face turned just past one still takes
        // some of its light. Unwrapped, a crease turned from a candle went
        // from lit to black on the crease itself - the bevel of the curb
        // round the plaza in Silvermoon, a straight dark edge across the floor.
        // A bump in a normal map turned from a lamp a few degrees above its
        // face went dark, the next one lit: a low candle laid the bumps'
        // shading over the floor in hard patches. The detail is faded out
        // as the light comes down to the face - all of it from some twenty
        // degrees above, none of it under three - so a lamp overhead still
        // shows it, and one at the floor's level lights the floor as a floor.
        vec3 nl = lightN;
        if (d > 1e-4) {
            float rise = dot(faceRef, toLight / d);
            nl = normalize(mix(faceRef, lightN, smoothstep(RT_LAMP_DETAIL_LOW, RT_LAMP_DETAIL_HIGH, rise)));
        }
        float ndl = d > 1e-4 ? (dot(nl, toLight / d) + pass.tune3.w) / (1.0 + pass.tune3.w) : 1.0;
        // A low light on a floor is lit as the face has it at the least, the
        // ground's falloff on any flat floor, however its normals lean.
        if (d > 1e-4 && n.z >= RT_NORMAL_FLOOR_Z) {
            float low = 1.0 - smoothstep(RT_LAMP_DETAIL_LOW, RT_LAMP_DETAIL_HIGH, dot(faceRef, toLight / d));
            ndl = max(ndl, low * (faceRise + pass.tune3.w) / (1.0 + pass.tune3.w));
        }
        if (ndl <= 0.0) continue;
        float falloff = pass.tune5.z > 0.5 ? rtLightFalloffRealistic(d, light.colorStart.w, end)
                                           : rtLightFalloff(d, light.colorStart.w, end);
        vec3 c = light.colorStart.rgb * (falloff * ndl * faceFade);
        float w = max(c.r, max(c.g, c.b));
        if (w <= 0.0) continue;
        // The view of the lights' kinds: the same light, its brightness in
        // its kind's colour (rt_light_kinds::tinted). The shadow ray is
        // picked by the light as it is, so the shadows do not change.
        if (int(pass.tune2.z + 0.5) == RT_LIGHT_KIND_VIEW) {
            c = rtLightKindColour(int(light.storey.w + 0.5)) * dot(c, vec3(0.2126, 0.7152, 0.0722));
        }
        pointSum += c;
        if (light.positionEnd.w < 0.0) bakedSum += w;
        weightSum += w;
        for (uint k = 0u; k < pointRays; ++k) {
            if (rtRandom(rng) * weightSum < w) picked[k] = int(i);
        }
    }
    float pointVisSum = 0.0;
    for (uint k = 0u; k < pointRays; ++k) {
    float pointVis = 1.0;
    if (picked[k] >= 0) {
        RtPointLight light = rtPointLights[picked[k]];
        float radius = rtLightRadius(abs(light.positionEnd.w));
        // A cloud of particles is a volume of light, not a lamp's point: its
        // shadow as soft as the cloud is wide. Taken from points a hand
        // apart, a particle drifting by a cave's low-poly wall cast the
        // wall's every ridge as a hard triangle - and as the particles drift,
        // the triangles came and went from frame to frame (Thunder Bluff's
        // Pools of Vision, 37 particle lights off its pool and mushrooms).
        if (int(light.storey.w + 0.5) == RT_LIGHT_KIND_PARTICLE) radius = max(radius, RT_PARTICLE_SOURCE_RADIUS);
        // With the lights shadowing themselves (pass.tune5.x) a lamp's light
        // is taken from within its lamp: a source up to half a yard wide,
        // and the ray stopped that far short of it, reached through and past
        // the walls round its flame - Camp Narache's tauren lamp post, whose
        // flame burns a third of a yard inside its walls, lit the ground all
        // round it instead of out of its one opening only.
        // The fixtures themselves cast always, whatever the option says:
        // everything casts while tracing (the user's rule) - a lamp's or a
        // brazier's own model too, by its faces turned toward the light
        // (rtFixtureFacesOnly, below). Passed by whole, a fixture cast no
        // shadow of any fixed light: a lantern none of the torch beside it,
        // a fire bowl none of its own fire. The option keeps the source
        // small and inside its lamp.
        bool selfShadow = pass.tune5.x > 0.5;
        if (selfShadow && int(light.storey.w + 0.5) != RT_LIGHT_KIND_PARTICLE) {
            radius = min(radius, RT_SELF_SHADOW_SOURCE_RADIUS);
        }
        vec3 jitter = (vec3(rtRandom(rng), rtRandom(rng), rtRandom(rng)) * 2.0 - 1.0) * radius;
        // Lifted off its triangle toward the smooth surface as the sun's ray
        // is (RT_TERMINATOR_LIFT): a lamp low over a rounded floor or a
        // hillside laid the same hard triangles over its light.
        vec3 lampOrigin = origin;
        {
            vec3 toAim = rtLightAim(light) - p;
            vec3 dir = normalize(toAim);
            if (dot(shadingN, dir) > 0.0) {
                float bend = length(cross(n, shadingN));
                float lift = RT_TERMINATOR_LIFT * bend * (1.0 - clamp(dot(n, dir), 0.0, 1.0));
                // And a triangle turned from the lamp its smooth normal still
                // faces, as the sun's: its ray ran down into its own plane
                // and the next faces, and a low-poly cave lit by mushrooms
                // and its pool (Thunder Bluff's Pools of Vision) showed its
                // triangles as hard facets of lamp shadow - flickering, as
                // the lamp each ray picks changes from frame to frame.
                if (dot(n, dir) <= 0.0) {
                    lampOrigin = p + shadingN * rtSurfaceOffset(dist);
                    lift = max(lift, RT_TERMINATOR_REACH * -dot(n, dir) / max(dot(shadingN, n), 0.5));
                }
                // Never past half way to the lamp, which is near.
                lampOrigin += shadingN * min(lift, 0.5 * length(toAim));
            }
        }
        vec3 toLight = rtLightAim(light) + jitter - lampOrigin;
        float d = length(toLight);
        // Short of the light by the standoff, and by the jitter's reach so a
        // point picked at the source's edge does not land inside the lamp.
        // With the lights shadowing themselves (pass.tune5.x, a WoWee-only
        // option) only the jitter's reach: the lamp's own faces turned
        // toward the light stop the ray (rtFixtureFacesOnly).
        float reach = d - min((selfShadow ? 0.05 : RT_LIGHT_STANDOFF) + radius, 0.5 * d);
        if (d > 1e-3 && reach > 0.0) {
            // The units cast their shadows as far as anything else does: the
            // reach they were held to (rtUnitShadowReach) let a unit's shadow
            // stop short of the wall the lamp threw it on.
            bool byUnit;
            // The unit that carries the light shadows it too: its body may
            // stand between the light and what it lights (the user's rule;
            // rtLampEnd passed the carrier by).
            // The fixtures by their faces turned toward the light, for every
            // fixed light (storey.z -1: PointLight::carried meets them whole).
            rtSkipFixtures = int(light.storey.w + 0.5) != RT_LIGHT_KIND_UNIT && light.storey.z > -0.5;
            rtFixtureFacesOnly = true;
            // The units' boxes asked about the surface's point, not the
            // lifted one (rtGroupFrom).
            rtGroupFrom = origin;
            rtGroupFromSet = true;
            pointVis = rtOccludedBy(lampOrigin, toLight / d, reach, rng, byUnit) ? 0.0 : 1.0;
            // A unit's own body shadows itself while the option asks for it
            // (pass.tune5.y), by rtSelfVis: the smooth surface's, not its triangles'.
            if (pointVis > 0.0) pointVis *= rtSelfVis(origin, shadingN, toLight / d, reach, rng);
            rtGroupFromSet = false;
            rtFixtureFacesOnly = false;
            rtSkipFixtures = false;
            rtUnitReach = 1e30;
            unitHit = unitHit || byUnit;
            // Stopped by the ground or a building - a floor, a wall: the
            // light is in another space and gives nothing here, whatever the
            // point shadows' strength (rtHardShadowVis).
            if (pointVis < 0.5 && pass.tune2.w > 0.5 && rtOccludedByStructure(lampOrigin, toLight / d, reach, rng)) {
                pointVis = rtHardShadowVis(pass.tune2.y);
            }
        }
    }
    pointVisSum += pointVis;
    }
    float pointVis = pointVisSum / float(pointRays);

    imageStore(uRaw, px, vec4(sun, ao, pointVis, unitHit ? 1.0 : 0.0));
    imageStore(uPointOut, px, vec4(pointSum, bakedSum));
}
