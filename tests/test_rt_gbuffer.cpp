// The ray traced lighting's G-buffer (rt_gbuffer_pack.hpp, the mirror of
// rt_gbuffer.glsli and rt_apply.glsli): what an opaque surface writes beside
// the colour it drew with the traced terms neutral, applied afterwards, has to
// give the colour the forward shaders give with those terms in them - the
// formulas of m2.frag, wmo.frag, character.frag and terrain.frag, clamps,
// texture stages and fog included - to within what the bytes hold.
#include <catch_amalgamated.hpp>

#include "rendering/rt_gbuffer_pack.hpp"

#include <algorithm>
#include <random>

using namespace wowee::rendering::rt_gbuffer;

namespace {

/// rt_lighting.glsli's RtLight, as the forward shaders take it.
struct RtLight {
    float weight;
    float sun;
    float ao;
    glm::vec3 point;
    glm::vec3 pointOwn;
    float bakedShade;
    float interiorShade = 1.0f;
};

constexpr RtLight kNeutral{1.0f, 1.0f, 1.0f, glm::vec3(0.0f), glm::vec3(0.0f), 1.0f, 1.0f};

struct Strengths {
    float k = 0.6f;        // rtTune.x
    float sun = 1.0f;      // lightTune.y
    float occlusion = 1.0f;  // lightTune.w
    int mode = 3;
};

float rtShadow(const RtLight& r, float map, const Strengths& t) {
    return std::min(map, 1.0f - r.weight * t.sun * (1.0f - r.sun));
}
float rtShadowScale(const RtLight& r, float shadow, const Strengths& t) {
    return (1.0f - r.weight) * (0.7f + 0.3f * shadow) + r.weight * ((1.0f - t.k) + t.k * shadow);
}
glm::vec3 rtAmbient(const RtLight& r, const glm::vec3& flat, const Strengths& t) {
    if (t.mode < 2) return flat;
    return glm::mix(flat, flat * r.ao, r.weight * t.occlusion);
}
glm::vec3 clamp01(const glm::vec3& v) { return glm::clamp(v, glm::vec3(0.0f), glm::vec3(1.0f)); }

struct Rng {
    std::mt19937 gen{12345u};
    float operator()(float lo, float hi) { return std::uniform_real_distribution<float>(lo, hi)(gen); }
    glm::vec3 v(float lo, float hi) { return {(*this)(lo, hi), (*this)(lo, hi), (*this)(lo, hi)}; }
};

RtLight randomTraced(Rng& rng) {
    RtLight r;
    r.weight = rng(0.0f, 1.0f) < 0.2f ? rng(0.0f, 1.0f) : 1.0f;
    r.sun = rng(0.0f, 1.0f) < 0.3f ? 0.0f : rng(0.0f, 1.0f);
    r.ao = rng(0.2f, 1.0f);
    r.point = rng.v(0.0f, 0.6f) * r.weight;
    r.pointOwn = r.point * rng(0.0f, 1.0f);
    r.bakedShade = 1.0f - 0.6f * r.weight * rng(0.0f, 1.0f);
    r.interiorShade = interiorShade(r.point, r.weight, rng(0.0f, 1.0f), rng(0.05f, 1.0f));
    return r;
}

Traced toTraced(const RtLight& r) {
    Traced t;
    t.weight = r.weight;
    t.sun = r.sun;
    t.ao = r.ao;
    t.point = r.point;
    t.pointOwn = r.pointOwn;
    t.bakedShade = r.bakedShade;
    t.interiorShade = r.interiorShade;
    return t;
}

/// What the apply pass makes of a surface written beside c0.
glm::vec3 applied(const Surface& s, const glm::vec3& c0, const RtLight& r, const Strengths& st,
                  const glm::vec3& toSun) {
    const Texel t = unpack(pack(s, c0));
    Tune tune;
    tune.shadowStrength = st.k;
    tune.sunShadow = st.sun;
    tune.occlusion = st.occlusion;
    tune.mode = st.mode;
    const Factors f = apply(t, toTraced(r), tune, toSun);
    return c0 * f.mul + f.add;
}

/// The bytes hold a channel to 1/255 of the colour, a knee to 1/31, and the
/// point lights' factor to 2/255 of itself - under half a step of an 8-bit
/// display where a colour is dark and a torch bright beside it.
void requireClose(const glm::vec3& got, const glm::vec3& want, const glm::vec3& c0) {
    for (int c = 0; c < 3; ++c) {
        const float tolerance = 0.02f * std::max(c0[c], want[c]) + 0.003f;
        INFO("channel " << c << ": got " << got[c] << ", want " << want[c] << ", c0 " << c0[c]);
        REQUIRE(std::abs(got[c] - want[c]) <= tolerance);
    }
}

}  // namespace

TEST_CASE("G-buffer words hold what a surface writes", "[rt_gbuffer]") {
    Rng rng;
    for (int i = 0; i < 200; ++i) {
        Surface s;
        s.normal = glm::normalize(rng.v(-1.0f, 1.0f) + glm::vec3(0.0f, 0.0f, 0.01f));
        s.map = rng(0.0f, 1.0f);
        s.flags = static_cast<uint32_t>(rng(0.0f, 15.99f));
        const glm::vec3 c0 = rng.v(0.05f, 1.5f);
        s.g = c0 * rng.v(0.0f, 1.0f);
        // The most the occlusion takes is part of the colour.
        s.occY = c0 * rng.v(0.0f, 1.0f);
        s.occE = s.occY * rng.v(0.0f, 1.0f);
        s.kp = rng.v(0.0f, 1.9f);
        const Texel t = unpack(pack(s, c0));
        REQUIRE(glm::dot(t.normal, s.normal) > 0.9999f);
        REQUIRE(std::abs(t.map - s.map) <= 0.5f / 255.0f + 1e-6f);
        REQUIRE(t.flags == s.flags);
        for (int c = 0; c < 3; ++c) {
            REQUIRE(std::abs(t.g[c] - s.g[c] / c0[c]) <= 0.5f / 255.0f + 1e-5f);
            REQUIRE(std::abs(t.occ[c] - (s.occY[c] - s.occE[c]) / c0[c]) <= 0.5f / 255.0f + 1e-5f);
            REQUIRE(std::abs(t.kp[c] - s.kp[c]) <= 1.0f / 255.0f + 1e-5f);
            REQUIRE(std::abs(t.knee[c] - s.occE[c] / s.occY[c]) <= 0.5f / 31.0f + 1e-5f);
        }
    }
}

TEST_CASE("A cleared G-buffer leaves the colour as it is", "[rt_gbuffer]") {
    const Texel t = unpack(Words{});
    REQUIRE((t.flags & kValid) == 0u);
    Traced r;
    r.weight = 1.0f;
    r.sun = 0.0f;
    r.point = glm::vec3(1.0f);
    const Factors f = apply(t, r, Tune{}, glm::vec3(0.0f, 0.0f, 1.0f));
    REQUIRE(f.mul == glm::vec3(1.0f));
    REQUIRE(f.add == glm::vec3(0.0f));
}

TEST_CASE("Neutral traced terms leave the colour the surface drew", "[rt_gbuffer]") {
    Rng rng;
    for (int i = 0; i < 100; ++i) {
        Surface s;
        s.map = rng(0.0f, 1.0f);
        s.flags = kValid | kSun | (i % 2 ? kBaked : 0u) | (i % 3 == 0 ? kLiftAfter : 0u);
        const glm::vec3 c0 = rng.v(0.05f, 1.2f);
        s.g = c0 * rng.v(0.3f, 1.0f);
        s.occY = rng.v(0.0f, 0.5f);
        s.occE = s.occY * rng.v(0.0f, 1.0f);
        s.kp = rng.v(0.0f, 1.0f);
        requireClose(applied(s, c0, kNeutral, Strengths{}, glm::vec3(0.3f, 0.2f, 0.93f)), c0, c0);
    }
}

// m2.frag's exterior batch: clamp(ambient x occlusion x probes + N.L x direct)
// x the shadow scale, plus the point lights, through the texture stages
// (affine in the diffuse: U + M x d) and the fog (phi c + gamma).
TEST_CASE("A model's exterior light applies as m2.frag lights it", "[rt_gbuffer]") {
    Rng rng;
    for (int i = 0; i < 400; ++i) {
        const glm::vec3 flat = rng.v(0.1f, 0.7f), ratio = rng.v(0.5f, 1.6f), direct = rng.v(0.0f, 0.9f);
        const float map = rng(0.0f, 1.0f) < 0.4f ? 1.0f : rng(0.0f, 1.0f);
        const glm::vec3 u = rng.v(0.0f, 0.2f), m = rng.v(0.1f, 2.0f), tint = rng.v(0.3f, 1.0f);
        const float phi = rng(0.3f, 1.0f);
        const glm::vec3 gamma = rng.v(0.0f, 0.3f);
        Strengths st;
        st.k = rng(0.3f, 0.9f);
        st.occlusion = rng(0.0f, 1.0f) < 0.5f ? 1.0f : rng(0.0f, 1.0f);
        const auto colour = [&](const RtLight& r) {
            const glm::vec3 light = clamp01(rtAmbient(r, flat, st) * ratio + direct) *
                                        rtShadowScale(r, rtShadow(r, map, st), st) +
                                    r.point;
            return phi * (u + m * (light * tint)) + gamma;
        };
        const glm::vec3 c0 = colour(kNeutral);
        Surface s;
        s.flags = kValid | kSun;
        s.map = map;
        const float sigma0 = rtShadowScale(kNeutral, map, st);
        s.g = clamp01(flat * ratio + direct) * sigma0;
        s.occY = flat * ratio * sigma0;
        s.occE = glm::max(flat * ratio + direct - glm::vec3(1.0f), glm::vec3(0.0f)) * sigma0;
        s.kp = glm::vec3(1.0f);
        const glm::vec3 scale = m * tint * phi;
        s.g *= scale;
        s.occY *= scale;
        s.occE *= scale;
        s.kp *= scale;
        const RtLight r = randomTraced(rng);
        requireClose(applied(s, c0, r, st, glm::vec3(0.0f, 0.0f, 1.0f)), colour(r), c0);
    }
}

// wmo.frag's unified pass over baked vertex colour: the light halved and
// added to it, clamped - two clamps in a row - by the shadow scale where
// shadowed, then the lamps' shade and the other lights, doubled by the texture,
// an env reflection beside it, and the fog.
TEST_CASE("A building's unified baked pass applies as wmo.frag draws it", "[rt_gbuffer]") {
    Rng rng;
    for (int i = 0; i < 400; ++i) {
        const glm::vec3 flat = rng.v(0.1f, 0.8f), ndlDirect = rng.v(0.0f, 0.8f);
        const glm::vec3 v = rng.v(0.0f, 0.7f), c29 = rng.v(0.0f, 0.1f);
        const glm::vec3 tex = rng.v(0.1f, 1.0f), env = rng.v(0.0f, 0.3f);
        const float bakedLight = rng(0.5f, 1.5f);
        const bool shadowed = i % 3 != 0;
        const float map = rng(0.0f, 1.0f);
        const float phi = rng(0.4f, 1.0f);
        const glm::vec3 gamma = rng.v(0.0f, 0.2f);
        Strengths st;
        const auto colour = [&](const RtLight& r) {
            const glm::vec3 light = clamp01(rtAmbient(r, flat, st) + ndlDirect);
            glm::vec3 c = clamp01(light * 0.5f + v + c29);
            if (shadowed) c *= rtShadowScale(r, rtShadow(r, map, st), st);
            c = c * (bakedLight * r.bakedShade) + 0.5f * r.pointOwn;
            return phi * (2.0f * c * tex + env) + gamma;
        };
        const glm::vec3 c0 = colour(kNeutral);
        Surface s;
        s.flags = kValid | kBaked | (shadowed ? kSun : 0u);
        s.map = map;
        const float scale = (shadowed ? rtShadowScale(kNeutral, map, st) : 1.0f) * bakedLight;
        s.g = clamp01(clamp01(flat + ndlDirect) * 0.5f + v + c29) * scale;
        // min(1, 0.5 min(1, a ao + d) + t) = min(min(1, 0.5 + t), 0.5 a ao + 0.5 d + t)
        const glm::vec3 y = 0.5f * flat, z = 0.5f * ndlDirect + v + c29;
        const glm::vec3 cap = glm::min(glm::vec3(1.0f), 0.5f + v + c29);
        s.occY = y * scale;
        s.occE = glm::max(y + z - cap, glm::vec3(0.0f)) * scale;
        s.kp = glm::vec3(0.5f);
        const glm::vec3 m = 2.0f * tex * phi;
        s.g *= m;
        s.occY *= m;
        s.occE *= m;
        s.kp *= m;
        const RtLight r = randomTraced(rng);
        requireClose(applied(s, c0, r, st, glm::vec3(0.0f, 0.0f, 1.0f)), colour(r), c0);
    }
}

// character.frag's exterior light: the lesser of the map's and the traced
// shadow, then the edge-on lift over that, and in a building's light the
// lamps' shade over all of it and only the other lights added.
TEST_CASE("A unit's light applies as character.frag lights it", "[rt_gbuffer]") {
    Rng rng;
    for (int i = 0; i < 400; ++i) {
        const glm::vec3 n = glm::normalize(rng.v(-1.0f, 1.0f) + glm::vec3(0.0f, 0.0f, 0.01f));
        const glm::vec3 toSun = glm::normalize(glm::vec3(0.4f, -0.3f, 0.6f));
        const glm::vec3 flat = rng.v(0.1f, 0.7f), direct = rng.v(0.0f, 1.0f) * std::max(glm::dot(n, toSun), 0.0f);
        const float map = rng(0.0f, 1.0f) < 0.4f ? 1.0f : rng(0.0f, 1.0f);
        const bool baked = i % 2 == 0;
        const glm::vec3 m = rng.v(0.2f, 1.5f);
        Strengths st;
        st.sun = rng(0.0f, 1.0f) < 0.5f ? 1.0f : rng(0.0f, 1.0f);
        // The normal goes through ten bits; the lift is taken from the one
        // that comes back, as the apply pass takes it.
        const glm::vec3 nStored = unpack(pack(Surface{n}, glm::vec3(1.0f))).normal;
        const float lift = edgeOnLift(nStored, toSun);
        const auto colour = [&](const RtLight& r) {
            float shadow = rtShadow(r, map, st);
            shadow += (1.0f - shadow) * lift;
            glm::vec3 light = clamp01(rtAmbient(r, flat, st) + direct) * rtShadowScale(r, shadow, st);
            light = baked ? light * r.bakedShade + r.pointOwn : light + r.point;
            return m * light;
        };
        const glm::vec3 c0 = colour(kNeutral);
        Surface s;
        s.normal = n;
        s.flags = kValid | kSun | kLiftAfter | (baked ? kBaked : 0u);
        s.map = map;
        const float sigma0 = rtShadowScale(kNeutral, map + (1.0f - map) * lift, st);
        s.g = clamp01(flat + direct) * sigma0 * m;
        s.occY = flat * sigma0 * m;
        s.occE = glm::max(flat + direct - glm::vec3(1.0f), glm::vec3(0.0f)) * sigma0 * m;
        s.kp = m;
        const RtLight r = randomTraced(rng);
        requireClose(applied(s, c0, r, st, toSun), colour(r), c0);
    }
}

// Inside (RT_GB_INTERIOR), m2.frag's interior doodad and character.frag's
// unit on an interior floor: the baked colour times the lesser of the lamps'
// shade and the room's own light (rtInteriorBakedShade), the other lights
// added. A baked surface that is not an interior's takes the lamps' shade
// alone.
TEST_CASE("An interior's baked colour applies as the room's light shades it", "[rt_gbuffer]") {
    Rng rng;
    for (int i = 0; i < 400; ++i) {
        const glm::vec3 baked = rng.v(0.05f, 1.0f);
        const glm::vec3 m = rng.v(0.2f, 1.5f);
        const bool interior = i % 4 != 0;
        const auto colour = [&](const RtLight& r) {
            const float shade = interior ? std::min(r.bakedShade, r.interiorShade) : r.bakedShade;
            return m * (baked * shade + r.pointOwn);
        };
        const glm::vec3 c0 = colour(kNeutral);
        Surface s;
        s.flags = kValid | kBaked | (interior ? kInterior : 0u);
        s.g = baked * m;
        s.kp = m;
        const RtLight r = randomTraced(rng);
        requireClose(applied(s, c0, r, Strengths{}, glm::vec3(0.0f, 0.0f, 1.0f)), colour(r), c0);
    }
}

// A WMO transition batch (wmo.frag, rtSurfaceMix): the outside pass, under
// the sun's shadow, times alpha, plus the inside pass, the room's baked
// colour, times one less alpha. Each pass takes its own shade: the sun the
// trace finds blocked at a door darkens only the outside pass's share, the
// room's light only the inside's. One shade over both made the floor at the
// doors black.
TEST_CASE("A transition batch's passes each take their own shade", "[rt_gbuffer]") {
    Rng rng;
    for (int i = 0; i < 400; ++i) {
        const glm::vec3 m = rng.v(0.2f, 1.0f);
        const float outsideLight = rng(0.1f, 1.0f), baked = rng(0.05f, 1.0f);
        const float alpha = i % 5 == 0 ? 0.0f : (i % 5 == 1 ? 1.0f : rng(0.0f, 1.0f));
        const float map = rng(0.0f, 1.0f) < 0.5f ? 1.0f : rng(0.0f, 1.0f);
        Strengths st;
        st.mode = 1;  // no occlusion: the passes here clamp nothing
        // C0: the colour with the traced terms neutral.
        const glm::vec3 c0 = m * (alpha * outsideLight * rtShadowScale(kNeutral, map, st) + (1.0f - alpha) * baked);
        const float gOut = alpha * outsideLight * rtShadowScale(kNeutral, map, st);
        const float gIn = (1.0f - alpha) * baked;
        Surface s;
        s.flags = kValid | kTransition | kSun | kBaked | kInterior;
        s.map = map;
        s.g = m * (gOut + gIn);
        s.outside = gOut + gIn > 1e-6f ? gOut / (gOut + gIn) : alpha;
        // The point lights by the same shares (the passes' Kp in proportion
        // to their G, as the WMO's texel makes them).
        s.kp = m;
        const RtLight r = randomTraced(rng);
        const glm::vec3 want = alpha * (m * (outsideLight * rtShadowScale(r, rtShadow(r, map, st), st))) +
                               (1.0f - alpha) * (m * (baked * std::min(r.bakedShade, r.interiorShade))) +
                               m * (s.outside * r.point + (1.0f - s.outside) * r.pointOwn);
        requireClose(applied(s, c0, r, st, glm::vec3(0.0f, 0.0f, 1.0f)), want, c0);
    }
    // The share survives the words, to a 63rd.
    Surface s;
    s.flags = kValid | kTransition;
    for (float o : {0.0f, 0.3f, 0.77f, 1.0f}) {
        s.outside = o;
        CHECK(std::abs(unpack(pack(s, glm::vec3(0.5f))).outside - o) <= 0.5f / 63.0f + 1e-5f);
    }
}

// rtInteriorShade: half where no lamp reaches (the default), all of it at the
// light named full and above, linear between; as painted where the pixel
// found no traced result of its own, and at a base of one.
TEST_CASE("The room's own light runs from the base to the whole colour", "[rt_gbuffer]") {
    CHECK(interiorShade(glm::vec3(0.0f), 1.0f, 0.5f, 0.5f) == Catch::Approx(0.5f));
    CHECK(interiorShade(glm::vec3(0.25f, 0.1f, 0.0f), 1.0f, 0.5f, 0.5f) == Catch::Approx(0.75f));
    CHECK(interiorShade(glm::vec3(0.5f), 1.0f, 0.5f, 0.5f) == Catch::Approx(1.0f));
    CHECK(interiorShade(glm::vec3(3.0f), 1.0f, 0.5f, 0.5f) == Catch::Approx(1.0f));
    CHECK(interiorShade(glm::vec3(0.0f), 0.0f, 0.5f, 0.5f) == Catch::Approx(1.0f));
    CHECK(interiorShade(glm::vec3(0.0f), 0.5f, 0.5f, 0.5f) == Catch::Approx(0.75f));
    CHECK(interiorShade(glm::vec3(0.0f), 1.0f, 1.0f, 0.5f) == Catch::Approx(1.0f));
    CHECK(interiorShade(glm::vec3(0.0f), 1.0f, 0.0f, 0.5f) == Catch::Approx(0.0f));
    // Brighter light never darkens.
    float last = 0.0f;
    for (float l = 0.0f; l <= 1.0f; l += 0.05f) {
        const float v = interiorShade(glm::vec3(l), 1.0f, 0.3f, 0.7f);
        CHECK(v >= last);
        last = v;
    }
    // The flag survives the words beside the others.
    Surface s;
    s.flags = kValid | kBaked | kLiftAfter | kInterior | kUnit;
    CHECK(unpack(pack(s, glm::vec3(0.5f))).flags == s.flags);
}

// terrain.frag: lit and unlit layers alike scaled by the shadow, the light
// clamped before the vertex shading scales it, the point lights on the lit
// layers only, and nothing of the occlusion below Shadows and occlusion.
TEST_CASE("The terrain's layers apply as terrain.frag lights them", "[rt_gbuffer]") {
    Rng rng;
    for (int mode = 1; mode <= 3; ++mode) {
        for (int i = 0; i < 200; ++i) {
            const glm::vec3 flat = rng.v(0.1f, 0.7f), direct = rng.v(0.0f, 0.9f), shading = rng.v(0.5f, 1.0f);
            const glm::vec3 lit = rng.v(0.0f, 0.8f), unlit = rng.v(0.0f, 0.3f);
            const float map = rng(0.0f, 1.0f);
            const float phi = rng(0.3f, 1.0f);
            Strengths st;
            st.mode = mode;
            const auto colour = [&](const RtLight& r) {
                const glm::vec3 light = clamp01(rtAmbient(r, flat, st) + direct) * shading;
                return phi * ((lit * light + unlit) * rtShadowScale(r, rtShadow(r, map, st), st) + lit * r.point);
            };
            const glm::vec3 c0 = colour(kNeutral);
            Surface s;
            s.flags = kValid | kSun;
            s.map = map;
            const float sigma0 = rtShadowScale(kNeutral, map, st);
            s.g = (lit * clamp01(flat + direct) * shading + unlit) * sigma0 * phi;
            s.occY = flat * lit * shading * sigma0 * phi;
            s.occE = glm::max(flat + direct - glm::vec3(1.0f), glm::vec3(0.0f)) * lit * shading * sigma0 * phi;
            s.kp = lit * phi;
            const RtLight r = randomTraced(rng);
            requireClose(applied(s, c0, r, st, glm::vec3(0.0f, 0.0f, 1.0f)), colour(r), c0);
        }
    }
}
