// The point lights the ray traced lighting shades with (rt_point_lights.hpp):
// an M2's lights as the file has them (parseM2Lights) and as a frame places
// them, a building's, the falloff the shader shares, and which of a crowded
// view's lights are kept. None of it fails loudly - a light read a few bytes
// off, a falloff that disagrees with the shader's, a ranking that shuffles -
// it only lights the wrong place, or flickers.
#include <catch_amalgamated.hpp>

#include "pipeline/m2_loader.hpp"
#include "rendering/rt_point_lights.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cstring>
#include <vector>

using namespace wowee;
using namespace wowee::rendering::rt_lights;

namespace {

/// A file in the making: values put at offsets, and room taken at the end
/// for the tracks' arrays.
struct Blob {
    std::vector<uint8_t> bytes;
    explicit Blob(size_t size) : bytes(size, 0) {}

    template <class T>
    void put(size_t at, const T& v) {
        REQUIRE(at + sizeof(T) <= bytes.size());
        std::memcpy(bytes.data() + at, &v, sizeof(T));
    }
    size_t take(size_t n) {
        const size_t at = bytes.size();
        bytes.resize(at + ((n + 3) & ~size_t(3)), 0);
        return at;
    }

    /// One timeline of keys for the 20-byte track header at `header`.
    template <class Key>
    void track(size_t header, const std::vector<uint32_t>& times, const std::vector<Key>& keys,
               int16_t globalSequence = -1) {
        const size_t tsArray = take(8);
        const size_t keyArray = take(8);
        const size_t tsData = take(times.size() * 4);
        const size_t keyData = take(keys.size() * sizeof(Key));
        put<uint16_t>(header, 1);  // linear
        put<int16_t>(header + 2, globalSequence);
        put<uint32_t>(header + 4, 1);
        put<uint32_t>(header + 8, static_cast<uint32_t>(tsArray));
        put<uint32_t>(header + 12, 1);
        put<uint32_t>(header + 16, static_cast<uint32_t>(keyArray));
        put<uint32_t>(tsArray, static_cast<uint32_t>(times.size()));
        put<uint32_t>(tsArray + 4, static_cast<uint32_t>(tsData));
        put<uint32_t>(keyArray, static_cast<uint32_t>(keys.size()));
        put<uint32_t>(keyArray + 4, static_cast<uint32_t>(keyData));
        for (size_t i = 0; i < times.size(); ++i) put<uint32_t>(tsData + i * 4, times[i]);
        for (size_t i = 0; i < keys.size(); ++i) put<Key>(keyData + i * sizeof(Key), keys[i]);
    }
};

struct Vec3 {
    float x, y, z;
};

// Offsets inside a light record (FUN_00839270's).
constexpr size_t kRecord = 0x9c;
constexpr size_t kAmbientColor = 0x10, kDiffuseColor = 0x38, kDiffuseIntensity = 0x4c;
constexpr size_t kAttStart = 0x60, kAttEnd = 0x74, kVisibility = 0x88;

/// Two light records at 0x40: a point light on bone 2 whose colour fades
/// from orange to red over a second, and a directional one with nothing.
Blob twoLights() {
    Blob b(0x40 + 2 * kRecord);
    const size_t first = 0x40;
    b.put<uint16_t>(first, 1);
    b.put<int16_t>(first + 2, 2);
    b.put<Vec3>(first + 4, {0.5f, -1.0f, 2.25f});
    b.track<Vec3>(first + kAmbientColor, {0}, {{0.1f, 0.1f, 0.1f}});
    b.track<Vec3>(first + kDiffuseColor, {0, 1000}, {{1.0f, 0.5f, 0.0f}, {1.0f, 0.0f, 0.0f}});
    b.track<float>(first + kDiffuseIntensity, {0}, {2.0f});
    b.track<float>(first + kAttStart, {0}, {1.5f});
    b.track<float>(first + kAttEnd, {0}, {6.0f});
    b.track<uint8_t>(first + kVisibility, {0, 500}, {uint8_t(1), uint8_t(0)});
    const size_t second = first + kRecord;
    b.put<uint16_t>(second, 0);
    b.put<int16_t>(second + 2, -1);
    return b;
}

}  // namespace

TEST_CASE("an M2's lights are read from where the client fixes them up", "[rt][lights]") {
    const Blob b = twoLights();
    const auto lights = pipeline::parseM2Lights(b.bytes, 2, 0x40, {0x20});
    REQUIRE(lights.size() == 2);

    const auto& l = lights[0];
    CHECK(l.type == 1);
    CHECK(l.bone == 2);
    CHECK(l.position == glm::vec3(0.5f, -1.0f, 2.25f));
    REQUIRE(l.diffuseColor.sequences.size() == 1);
    CHECK(l.diffuseColor.sequences[0].timestamps == std::vector<uint32_t>{0, 1000});
    REQUIRE(l.diffuseColor.sequences[0].vec3Values.size() == 2);
    CHECK(l.diffuseColor.sequences[0].vec3Values[1] == glm::vec3(1.0f, 0.0f, 0.0f));
    CHECK(l.diffuseIntensity.sequences[0].floatValues == std::vector<float>{2.0f});
    CHECK(l.attenuationStart.sequences[0].floatValues == std::vector<float>{1.5f});
    CHECK(l.attenuationEnd.sequences[0].floatValues == std::vector<float>{6.0f});
    // A byte a key, 1 read as one - not as a denormal float.
    CHECK(l.visibility.sequences[0].floatValues == std::vector<float>{1.0f, 0.0f});

    CHECK(lights[1].type == 0);
    CHECK(lights[1].bone == -1);
    CHECK_FALSE(lights[1].diffuseColor.hasData());
}

TEST_CASE("a light record that runs past the file is dropped", "[rt][lights]") {
    Blob b = twoLights();
    // As many claimed as could never fit: only whole records are read.
    const size_t whole = (b.bytes.size() - 0x40) / kRecord;
    CHECK(pipeline::parseM2Lights(b.bytes, 100000, 0x40, {0x20}).size() == std::min<size_t>(whole, 256));
    CHECK(pipeline::parseM2Lights(b.bytes, 1, static_cast<uint32_t>(b.bytes.size() - 8), {0x20}).empty());
    CHECK(pipeline::parseM2Lights(b.bytes, 0, 0x40, {0x20}).empty());
}

TEST_CASE("a sequence whose keys live in an .anim file is left empty", "[rt][lights]") {
    const Blob b = twoLights();
    const auto lights = pipeline::parseM2Lights(b.bytes, 1, 0x40, {0x00});
    REQUIRE(lights.size() == 1);
    REQUIRE(lights[0].diffuseColor.sequences.size() == 1);
    CHECK(lights[0].diffuseColor.sequences[0].vec3Values.empty());
}

TEST_CASE("an M2 light is placed by its bone and lit by its tracks", "[rt][lights]") {
    const Blob b = twoLights();
    const auto lights = pipeline::parseM2Lights(b.bytes, 2, 0x40, {0x20});
    REQUIRE(lights.size() == 2);
    const glm::mat4 model = glm::scale(glm::translate(glm::mat4(1.0f), glm::vec3(100.0f, 200.0f, 30.0f)),
                                       glm::vec3(2.0f));
    std::vector<glm::mat4> bones(3, glm::mat4(1.0f));
    bones[2] = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, 1.0f));
    const std::vector<uint32_t> noGlobals;

    const auto at = evaluateM2Light(lights[0], 0, 250.0f, 0.0f, noGlobals, model, bones);
    REQUIRE(at.has_value());
    // (0.5, -1, 2.25) up the bone's yard, doubled, moved.
    CHECK(at->position.x == Catch::Approx(101.0f));
    CHECK(at->position.y == Catch::Approx(198.0f));
    CHECK(at->position.z == Catch::Approx(36.5f));
    // A quarter of the way from orange to red, times two.
    CHECK(at->color.r == Catch::Approx(2.0f));
    CHECK(at->color.g == Catch::Approx(0.75f));
    CHECK(at->color.b == Catch::Approx(0.0f));
    // The window grows with the model, and reaches farther in the world.
    CHECK(at->attenuationStart == Catch::Approx(3.0f * kWorldReachScale));
    CHECK(at->attenuationEnd == Catch::Approx(12.0f * kWorldReachScale));

    // Past half a second its visibility track has it off.
    CHECK_FALSE(evaluateM2Light(lights[0], 0, 750.0f, 0.0f, noGlobals, model, bones).has_value());
    // A directional light is the model's own business.
    CHECK_FALSE(evaluateM2Light(lights[1], 0, 0.0f, 0.0f, noGlobals, model, bones).has_value());
    // No bones posed: the model's own space.
    const auto unposed = evaluateM2Light(lights[0], 0, 0.0f, 0.0f, noGlobals, model, {});
    REQUIRE(unposed.has_value());
    CHECK(unposed->position.z == Catch::Approx(34.5f));
}

TEST_CASE("a building's omni lights are placed and the rest left to its baking", "[rt][lights]") {
    const glm::mat4 model = glm::translate(glm::mat4(1.0f), glm::vec3(10.0f, 0.0f, 0.0f));
    const auto omni = placeWmoLight(glm::vec3(1.0f, 2.0f, 3.0f), glm::vec3(1.0f, 0.8f, 0.5f), 1.5f, 2.0f,
                                    8.0f, 0, model);
    REQUIRE(omni.has_value());
    CHECK(omni->position == glm::vec3(11.0f, 2.0f, 3.0f));
    CHECK(omni->color.g == Catch::Approx(1.2f));
    CHECK(omni->attenuationEnd == Catch::Approx(8.0f * kWorldReachScale));
    // Its interior is lit by it already, in its vertex colours.
    CHECK(omni->baked);
    CHECK_FALSE(placeWmoLight(glm::vec3(0.0f), glm::vec3(1.0f), 1.0f, 1.0f, 8.0f, 1, model).has_value());
    CHECK_FALSE(placeWmoLight(glm::vec3(0.0f), glm::vec3(1.0f), 1.0f, 1.0f, 0.0f, 0, model).has_value());
    CHECK_FALSE(placeWmoLight(glm::vec3(0.0f), glm::vec3(1.0f), 0.0f, 1.0f, 8.0f, 0, model).has_value());
}

TEST_CASE("the falloff is whole inside the start, gone at the end and smooth between", "[rt][lights]") {
    CHECK(attenuation(0.0f, 2.0f, 6.0f) == 1.0f);
    CHECK(attenuation(2.0f, 2.0f, 6.0f) == 1.0f);
    CHECK(attenuation(4.0f, 2.0f, 6.0f) == Catch::Approx(0.5f));
    CHECK(attenuation(6.0f, 2.0f, 6.0f) == 0.0f);
    CHECK(attenuation(9.0f, 2.0f, 6.0f) == 0.0f);
    float last = 1.0f;
    for (float d = 2.0f; d <= 6.0f; d += 0.25f) {
        const float a = attenuation(d, 2.0f, 6.0f);
        CHECK(a <= last);
        last = a;
    }
    // A start past the end is the end; no end is no light.
    CHECK(attenuation(5.0f, 9.0f, 6.0f) == 1.0f);
    CHECK(attenuation(0.0f, 0.0f, 0.0f) == 0.0f);
}

TEST_CASE("the lights kept are those that reach the view, the strongest first", "[rt][lights]") {
    const glm::vec3 eye(0.0f);
    auto light = [](glm::vec3 p, float strength, float end) {
        PointLight l;
        l.position = p;
        l.color = glm::vec3(strength);
        l.attenuationEnd = end;
        return l;
    };
    std::vector<PointLight> lights = {
        light({5, 0, 0}, 1.0f, 4.0f),     // 0: close by
        light({500, 0, 0}, 5.0f, 10.0f),  // 1: out of range
        light({30, 0, 0}, 1.0f, 4.0f),    // 2: farther, same strength
        light({8, 0, 0}, 0.001f, 4.0f),   // 3: too faint to show
        light({-20, 0, 0}, 3.0f, 6.0f),   // 4: behind the view
        light({12, 0, 0}, 2.0f, 0.1f),    // 5: reaches nothing
        light({15, 0, 0}, 4.0f, 6.0f),    // 6: strong
    };
    const auto inView = [](const glm::vec3& p, float r) { return p.x + r > 0.0f; };
    std::vector<PointLight> kept = lights;
    REQUIRE(selectLights(kept, eye, 100.0f, 8, inView) == 3);
    CHECK(kept[0].position.x == 15.0f);
    CHECK(kept[1].position.x == 5.0f);
    CHECK(kept[2].position.x == 30.0f);

    // Cut to two: the third goes.
    kept = lights;
    REQUIRE(selectLights(kept, eye, 100.0f, 2, inView) == 2);
    CHECK(kept[0].position.x == 15.0f);
    CHECK(kept[1].position.x == 5.0f);

    // Equal lights keep the order they came in, so the frame after keeps
    // them in the same slots.
    std::vector<PointLight> twins = {light({3, 0, 0}, 1.0f, 4.0f), light({3, 0, 0}, 1.0f, 4.0f)};
    twins[1].attenuationStart = 1.0f;
    REQUIRE(selectLights(twins, eye, 100.0f, 2, inView) == 2);
    CHECK(twins[0].attenuationStart == 0.0f);
    CHECK(twins[1].attenuationStart == 1.0f);
}

TEST_CASE("the light count limit is held to what the buffer takes", "[rt][lights]") {
    CHECK(lightLimit(kDefaultLightLimit) == 512u);
    CHECK(lightLimit(0) == static_cast<size_t>(kMinLightLimit));
    CHECK(lightLimit(-5) == static_cast<size_t>(kMinLightLimit));
    CHECK(lightLimit(8) == 8u);
    CHECK(lightLimit(300) == 300u);
    CHECK(lightLimit(512) == 512u);
    CHECK(lightLimit(2048) == 2048u);
    CHECK(lightLimit(100000) == static_cast<size_t>(kMaxLightLimit));
    STATIC_CHECK(kMinLightLimit <= kDefaultLightLimit);
    STATIC_CHECK(kDefaultLightLimit <= kMaxLightLimit);

    // A city's worth and more: 2100 lights in a row along the view, the
    // nearer the stronger. A limit keeps that many, the most important - the
    // nearest - first; the same lights whatever limit, as far as the lower
    // one goes.
    std::vector<PointLight> city;
    for (int i = 0; i < 2100; ++i) {
        PointLight l;
        l.position = {2.0f + 0.05f * float(i), 0.0f, 0.0f};
        l.color = glm::vec3(1.0f);
        l.attenuationEnd = 4.0f;
        city.push_back(l);
    }
    const glm::vec3 eye(0.0f);
    const auto all = [](const glm::vec3&, float) { return true; };
    for (int asked : {0, 8, 64, 128, 300, 512, 2000, 5000}) {
        std::vector<PointLight> kept = city;
        const size_t limit = lightLimit(asked);
        REQUIRE(selectLights(kept, eye, 200.0f, limit, all) == limit);
        for (size_t i = 0; i < kept.size(); ++i) CHECK(kept[i].position.x == Catch::Approx(2.0f + 0.05f * float(i)));
    }
}

TEST_CASE("the GPU record carries the window beside position and colour", "[rt][lights]") {
    PointLight l;
    l.position = {1, 2, 3};
    l.color = {0.5f, 0.25f, 0.125f};
    l.attenuationStart = 1.0f;
    l.attenuationEnd = 7.0f;
    const PointLightGPU g = toGpu(l);
    CHECK(g.positionEnd == glm::vec4(1, 2, 3, 7));
    CHECK(g.colorStart == glm::vec4(0.5f, 0.25f, 0.125f, 1.0f));
    // A building's light says so by the sign of its end, which the shader
    // takes the magnitude of.
    l.baked = true;
    CHECK(toGpu(l).positionEnd == glm::vec4(1, 2, 3, -7));
}

TEST_CASE("a candle's sub-yard window still reaches the ground around it", "[rt][lights]") {
    pipeline::M2Light candle;
    candle.type = 1;
    auto key = [](float v) {
        pipeline::M2AnimationTrack t;
        t.sequences.resize(1);
        t.sequences[0].timestamps = {0};
        t.sequences[0].floatValues = {v};
        return t;
    };
    candle.diffuseIntensity = key(1.0f);
    candle.diffuseColor.sequences.resize(1);
    candle.diffuseColor.sequences[0].timestamps = {0};
    candle.diffuseColor.sequences[0].vec3Values = {glm::vec3(1.0f, 0.6f, 0.3f)};
    candle.attenuationStart = key(0.33f);
    candle.attenuationEnd = key(0.97f);
    const auto l = evaluateM2Light(candle, 0, 0.0f, 0.0f, {}, glm::mat4(1.0f), {});
    REQUIRE(l.has_value());
    CHECK(l->attenuationEnd == Catch::Approx(kMinM2LightReach));
    CHECK(l->attenuationStart == Catch::Approx(0.33f * kWorldReachScale));
}

TEST_CASE("a fire without a light of its own is lit from the top of its box", "[rt][lights]") {
    const glm::mat4 model = glm::translate(glm::mat4(1.0f), glm::vec3(10.0f, 20.0f, 5.0f));
    const PointLight brazier = fireLight(glm::vec3(-1.0f, -1.0f, 0.0f), glm::vec3(1.0f, 1.0f, 2.0f), model,
                                         false, 0.0f, 0.0f);
    CHECK(brazier.position.x == Catch::Approx(10.0f));
    CHECK(brazier.position.y == Catch::Approx(20.0f));
    CHECK(brazier.position.z == Catch::Approx(6.6f));
    // Three times its size, held between its floor and ceiling.
    CHECK(brazier.attenuationEnd == Catch::Approx(6.0f * kWorldReachScale));
    CHECK(brazier.attenuationStart < brazier.attenuationEnd);
    CHECK(brazier.color.r > brazier.color.g);
    CHECK(brazier.color.g > brazier.color.b);
    // A huge bonfire stops at the ceiling.
    const PointLight bonfire = fireLight(glm::vec3(-10.0f), glm::vec3(10.0f), model, false, 0.0f, 0.0f);
    CHECK(bonfire.attenuationEnd == Catch::Approx(14.0f * kWorldReachScale));
    // It flickers, but never out.
    for (float t = 0.0f; t < 5000.0f; t += 37.0f) {
        const PointLight f = fireLight(glm::vec3(-1.0f), glm::vec3(1.0f), model, true, t, 3.0f);
        CHECK(f.color.r > 0.7f);
        CHECK(f.color.r <= 1.0f);
    }
}

TEST_CASE("a fire's light comes from its flames, not from the top of its box", "[rt][lights]") {
    // Orgrimmar's bonfire: the flames a yard up, smoke over them, a box to
    // 23 yards.
    auto emitter = [](glm::vec3 position, uint16_t bone, uint8_t blending) {
        pipeline::M2ParticleEmitter e{};
        e.position = position;
        e.bone = bone;
        e.blendingType = blending;
        return e;
    };
    glm::mat4 model(1.0f);
    model[3] = glm::vec4(100.0f, 200.0f, 10.0f, 1.0f);
    const auto carried = [&](uint32_t bone) {
        glm::mat4 m = model;
        if (bone == 1) m[3].z += 0.5f;  // a bone that lifts its flame half a yard
        return m;
    };
    const std::vector<pipeline::M2ParticleEmitter> bonfire = {
        emitter({0.0f, 0.0f, 1.0f}, 0, 4), emitter({2.0f, 0.0f, 1.5f}, 1, 4),
        emitter({0.0f, 0.0f, 9.0f}, 0, 2)};  // the smoke
    const auto flame = flameCentre(bonfire, carried);
    REQUIRE(flame);
    CHECK(flame->x == Catch::Approx(101.0f));
    CHECK(flame->y == Catch::Approx(200.0f));
    CHECK(flame->z == Catch::Approx(11.5f));
    // Where none is additive, all of them.
    const std::vector<pipeline::M2ParticleEmitter> smoky = {emitter({0.0f, 0.0f, 2.0f}, 0, 2),
                                                            emitter({0.0f, 0.0f, 4.0f}, 0, 1)};
    const auto smoke = flameCentre(smoky, carried);
    REQUIRE(smoke);
    CHECK(smoke->z == Catch::Approx(13.0f));
    // None without emitters: the box's top, as fireLight puts it.
    CHECK_FALSE(flameCentre({}, carried));
}

TEST_CASE("a lamp without a light of its own is lit at its glass, at night", "[rt][lights]") {
    // A street lamp: a post up the middle, the lantern out on an arm. Only
    // the lantern's glass glows.
    const std::vector<glm::vec3> positions = {
        {0.0f, 0.0f, 0.0f}, {0.2f, 0.0f, 0.0f}, {0.0f, 0.0f, 4.0f},     // the post
        {1.4f, 0.0f, 3.0f}, {1.6f, 0.0f, 3.0f}, {1.5f, 0.0f, 3.6f}};    // the glass
    const std::vector<uint16_t> indices = {0, 1, 2, 3, 4, 5};
    const glm::vec3 glass = lampGlowCentre(
        positions, indices, [](size_t i) { return i >= 3; }, glm::vec3(0.0f), glm::vec3(1.6f, 0.2f, 4.0f));
    CHECK(glass.x == Catch::Approx(1.5f));
    CHECK(glass.z == Catch::Approx(3.2f));
    // Nothing glows: the top of the box, as a fire's.
    const glm::vec3 top = lampGlowCentre(
        positions, indices, [](size_t) { return false; }, glm::vec3(0.0f), glm::vec3(1.6f, 0.2f, 4.0f));
    CHECK(top.x == Catch::Approx(0.8f));
    CHECK(top.z == Catch::Approx(3.2f));

    const glm::mat4 model = glm::translate(glm::mat4(1.0f), glm::vec3(10.0f, 20.0f, 30.0f));
    const PointLight night = lampLight(glass, model);
    CHECK(night.position.x == Catch::Approx(11.5f));
    CHECK(night.position.z == Catch::Approx(33.2f));
    CHECK(night.attenuationEnd == Catch::Approx(kLampReach * kWorldReachScale));
    CHECK(night.attenuationStart < night.attenuationEnd);
    CHECK(night.color.r > night.color.b);
    CHECK_FALSE(night.baked);
    CHECK(night.color.r == Catch::Approx(1.0f));
    // A lantern scaled down still reaches the ground.
    const PointLight small = lampLight(glass, glm::scale(model, glm::vec3(0.2f)));
    CHECK(small.attenuationEnd == Catch::Approx(3.0f * kWorldReachScale));
}

TEST_CASE("A rug is a flat sheet; a table, a post and a plate on its edge are not", "[rt_point_lights]") {
    auto box = [](glm::vec3 lo, glm::vec3 hi) {
        std::vector<pipeline::M2Vertex> v(2);
        v[0].position = lo;
        v[1].position = hi;
        return v;
    };
    CHECK(isFlatSheet(box({0, 0, 0}, {3.0f, 2.0f, 0.02f})));   // a rug
    CHECK(isFlatSheet(box({0, 0, 0}, {0.8f, 0.6f, 0.05f})));   // a doormat
    CHECK_FALSE(isFlatSheet(box({0, 0, 0}, {2.0f, 1.0f, 0.9f})));   // a table
    CHECK_FALSE(isFlatSheet(box({0, 0, 0}, {0.2f, 0.2f, 3.0f})));   // a post
    CHECK_FALSE(isFlatSheet(box({0, 0, 0}, {0.3f, 0.3f, 0.01f})));  // a coin: too small to matter
    CHECK_FALSE(isFlatSheet(box({0, 0, 0}, {2.0f, 2.0f, 0.4f})));   // a low platform
    CHECK_FALSE(isFlatSheet({}));
}

TEST_CASE("A lamp's light follows the bone that holds its glass", "[rt_point_lights]") {
    std::vector<pipeline::M2Vertex> v(6);
    for (auto& x : v) {
        x.boneWeights[0] = 255;
        x.boneWeights[1] = x.boneWeights[2] = x.boneWeights[3] = 0;
        x.boneIndices[1] = x.boneIndices[2] = x.boneIndices[3] = 0;
    }
    // The chain on bone 0, the glass on bone 2.
    v[0].boneIndices[0] = v[1].boneIndices[0] = v[2].boneIndices[0] = 0;
    v[3].boneIndices[0] = v[4].boneIndices[0] = v[5].boneIndices[0] = 2;
    const std::vector<uint16_t> idx = {0, 1, 2, 3, 4, 5};
    CHECK(dominantBone(v, idx, [](size_t i) { return i >= 3; }) == 2u);
    CHECK(dominantBone(v, idx, [](size_t) { return true; }) == 0u);  // a tie: the lower bone
    CHECK(dominantBone(v, idx, [](size_t) { return false; }) == ~0u);

    // Carried by its bone's pose, or by the model alone without one.
    const glm::mat4 model = glm::translate(glm::mat4(1.0f), glm::vec3(10.0f, 0.0f, 0.0f));
    std::vector<glm::mat4> bones(3, glm::mat4(1.0f));
    bones[2] = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.5f, 0.0f));  // swung half a yard
    const glm::vec3 at = glm::vec3(boneCarried(model, bones, 2) * glm::vec4(0.0f, 0.0f, 3.0f, 1.0f));
    CHECK(at.x == Catch::Approx(10.0f));
    CHECK(at.y == Catch::Approx(0.5f));
    const glm::vec3 still = glm::vec3(boneCarried(model, {}, 2) * glm::vec4(0.0f, 0.0f, 3.0f, 1.0f));
    CHECK(still.y == Catch::Approx(0.0f));
}

TEST_CASE("A light the ground or a building stops gives nothing at any shadow strength", "[rt_point_lights]") {
    // The Goldshire inn's cellar under the hearth: every ray to its flames
    // meets the hall's floor, and at 70% shadows it took 30% of them.
    for (float s : {0.05f, 0.3f, 0.7f, 1.0f}) {
        const float hard = hardShadowVisibility(s);
        CHECK(shadowedShare(hard, s) == Catch::Approx(0.0f).margin(1e-6f));
        // As a shadow sample of the baked lamps (clamped), a whole shadow.
        CHECK(std::clamp(hard, 0.0f, 1.0f) == 0.0f);
        // Something in the room - a table - still lets 1 - s through, and an
        // open line all of it.
        CHECK(shadowedShare(0.0f, s) == Catch::Approx(1.0f - s));
        CHECK(shadowedShare(1.0f, s) == Catch::Approx(1.0f));
    }
    // A pixel's samples averaged by the filters: half its rays stopped by the
    // floor, half open, is half the light - not half plus 15%.
    const float s = 0.7f;
    const float mean = 0.5f * hardShadowVisibility(s) + 0.5f * 1.0f;
    CHECK(shadowedShare(mean, s) == Catch::Approx(0.5f));
    // No point shadows at all: nothing to make whole.
    CHECK(hardShadowVisibility(0.0f) == 0.0f);
    CHECK(shadowedShare(hardShadowVisibility(0.0f), 0.0f) == Catch::Approx(1.0f));
}

TEST_CASE("A light on a billboard bone stays put however the camera looks", "[rt_point_lights]") {
    // Bone 1 is a billboard under the root, bone 2 hangs from it, bone 3 is
    // on its own; a parent loop (4 <-> 5) ends the walk.
    const std::vector<int16_t> parents = {-1, 0, 1, 0, 5, 4};
    const std::vector<bool> billboard = {false, true, false, false, false, false};
    const auto owners = billboardOwners(parents, billboard);
    REQUIRE(owners.size() == 6);
    CHECK(owners[0] == ~0u);
    CHECK(owners[1] == 1u);
    CHECK(owners[2] == 1u);
    CHECK(owners[3] == ~0u);
    CHECK(owners[4] == ~0u);
    CHECK(owners[5] == ~0u);

    // The billboard turned two ways, as from two cameras, about its pivot,
    // after its animation moved it half a yard up.
    const glm::vec3 pivot(1.0f, 0.0f, 2.0f);
    const glm::vec3 local(1.3f, 0.0f, 2.0f);  // the glow, 0.3 yards in front of the pivot
    const glm::mat4 model = glm::translate(glm::mat4(1.0f), glm::vec3(100.0f, 50.0f, 0.0f));
    auto posed = [&](float turn) {
        std::vector<glm::mat4> bones(3, glm::mat4(1.0f));
        glm::mat4 b = glm::translate(glm::mat4(1.0f), pivot + glm::vec3(0.0f, 0.0f, 0.5f));
        b = glm::rotate(b, turn, glm::vec3(0.0f, 0.0f, 1.0f));
        bones[1] = glm::translate(b, -pivot);
        bones[2] = bones[1];
        return bones;
    };
    const auto a = posed(0.0f), b = posed(2.0f);
    // Carried as drawn, the light goes round with the card.
    const glm::vec3 drawnA = glm::vec3(boneCarried(model, a, 2) * glm::vec4(local, 1.0f));
    const glm::vec3 drawnB = glm::vec3(boneCarried(model, b, 2) * glm::vec4(local, 1.0f));
    CHECK(glm::length(drawnA - drawnB) > 0.3f);
    // Carried steadily, it is where the animation took it, from either view.
    const glm::vec3 steadyA = glm::vec3(steadyBoneCarried(model, a, 2, owners[2], pivot) * glm::vec4(local, 1.0f));
    const glm::vec3 steadyB = glm::vec3(steadyBoneCarried(model, b, 2, owners[2], pivot) * glm::vec4(local, 1.0f));
    CHECK(glm::length(steadyA - steadyB) < 1e-5f);
    CHECK(steadyA.x == Catch::Approx(101.3f));
    CHECK(steadyA.y == Catch::Approx(50.0f));
    CHECK(steadyA.z == Catch::Approx(2.5f));
    // Without a billboard above it, as boneCarried.
    const glm::vec3 plain = glm::vec3(steadyBoneCarried(model, a, 2, ~0u, pivot) * glm::vec4(local, 1.0f));
    CHECK(glm::length(plain - drawnA) < 1e-5f);
}
