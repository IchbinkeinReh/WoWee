// SpellChainEffects beams: the row as the client reads it, the bolts of a
// chain (0x007fa4d0), and the CLightning strip (Common\Lightning.cpp).
#include <catch_amalgamated.hpp>

#include "rendering/spell_chain.hpp"

#include <glm/glm.hpp>

#include <cstring>
#include <string>
#include <vector>

namespace sc = wowee::rendering::spell_chain;

namespace {

void put32(std::vector<uint8_t>& rec, size_t at, uint32_t v) { std::memcpy(rec.data() + at, &v, 4); }
void putF(std::vector<uint8_t>& rec, size_t at, float v) { std::memcpy(rec.data() + at, &v, 4); }

sc::ChainEffect plainEffect() {
    sc::ChainEffect e;
    e.id = 1;
    e.avgSegLen = 1.0f;
    e.width = 0.5f;
    e.segDuration = 500;
    e.alpha = 255;
    e.red = 255;
    e.green = 128;
    e.blue = 0;
    e.blendMode = 3;
    return e;
}

}  // namespace

TEST_CASE("a SpellChainEffects record is read as 0x008b73b0 lays it out", "[spell_chain]") {
    auto stringAt = [](uint32_t offset) { return offset == 7 ? std::string("tex.blp") : std::string(); };
    for (uint32_t size : {177u, 180u, 192u}) {
        std::vector<uint8_t> rec(size, 0);
        put32(rec, 0x00, 42);
        putF(rec, 0x04, 0.75f);
        put32(rec, 0x14, 300);
        put32(rec, 0x1c, 7);
        put32(rec, 0x20, 0x21);
        putF(rec, 0x7c, 120.0f);
        putF(rec, 0x98, 2.0f);
        const uint32_t stride = size >= 192 ? 4 : 1;
        rec[0x9c] = 200;
        rec[0x9c + 1 * stride] = 10;
        rec[0x9c + 2 * stride] = 20;
        rec[0x9c + 3 * stride] = 30;
        rec[0x9c + 4 * stride] = 2;
        put32(rec, size - 12, 3);
        putF(rec, size - 8, 4.0f);
        putF(rec, size - 4, 1.5f);
        const auto e = sc::parseChainEffect(rec.data(), size, stringAt);
        REQUIRE(e);
        CHECK(e->id == 42);
        CHECK(e->avgSegLen == 0.75f);
        CHECK(e->segDuration == 300);
        CHECK(e->texture == "tex.blp");
        CHECK(e->flags == 0x21);
        CHECK(e->delayBetweenEffects == 120.0f);
        CHECK(e->pulseFadeLength == 2.0f);
        CHECK(e->alpha == 200);
        CHECK(e->red == 10);
        CHECK(e->green == 20);
        CHECK(e->blue == 30);
        CHECK(e->blendMode == 2);
        CHECK(e->renderLayer == 3);
        CHECK(e->textureLength == 4.0f);
        CHECK(e->wavePhase == 1.5f);
    }
    std::vector<uint8_t> shortRec(100, 0);
    CHECK_FALSE(sc::parseChainEffect(shortRec.data(), 100, stringAt));
}

TEST_CASE("a chain hops target to target, or fans out from the unit (0x007fa4d0)", "[spell_chain]") {
    sc::ChainEffect e = plainEffect();
    e.segDelay = 100;
    e.segDuration = 400;
    e.delayBetweenEffects = 150.9f;  // chopped to 150
    uint32_t end = 0;
    const auto hops = sc::planBolts(e, 3, false, 1000, end);
    REQUIRE(hops.size() == 3);
    CHECK(hops[0].from == 0);
    CHECK(hops[0].to == 1);
    CHECK(hops[1].from == 1);
    CHECK(hops[1].to == 2);
    CHECK(hops[2].from == 2);
    CHECK(hops[2].to == 3);
    CHECK(hops[0].startMs == 1100);
    CHECK(hops[1].startMs == 1250);
    CHECK(hops[2].startMs == 1400);
    CHECK(hops[2].endMs == 1800);
    CHECK(end == 1800);

    const auto fan = sc::planBolts(e, 2, true, 0, end);
    REQUIRE(fan.size() == 2);
    CHECK(fan[0].from == 0);
    CHECK(fan[1].from == 0);
    CHECK(fan[1].to == 2);

    CHECK(sc::boltShows(hops[0], false, 1100));
    CHECK_FALSE(sc::boltShows(hops[0], false, 1099));
    CHECK_FALSE(sc::boltShows(hops[0], false, 1500));
    CHECK(sc::boltShows(hops[0], true, 99999));
}

TEST_CASE("a kit's chain goes where 0x007265c0 asks first", "[spell_chain]") {
    using sc::ChainTargets;
    CHECK(sc::chainTargets(true, true, true, 3) == ChainTargets::FromOther);
    CHECK(sc::chainTargets(false, true, true, 3) == ChainTargets::Place);
    CHECK(sc::chainTargets(false, false, true, 1) == ChainTargets::Channel);
    CHECK(sc::chainTargets(false, false, true, 0) == ChainTargets::Channel);
    CHECK(sc::chainTargets(false, false, true, 2) == ChainTargets::Hits);
    CHECK(sc::chainTargets(false, false, false, 2) == ChainTargets::Hits);
    CHECK(sc::chainTargets(false, false, false, 0) == ChainTargets::None);
}

TEST_CASE("the joints a strip takes (0x009ab2e0)", "[spell_chain]") {
    sc::ChainEffect e = plainEffect();
    e.avgSegLen = 2.0f;
    CHECK(sc::jointCountFor(e, 10.0f) == 8);   // 5 + 2 + 1
    CHECK(sc::jointCountFor(e, 11.9f) == 8);   // chopped
    CHECK(sc::jointCountFor(e, 0.0f) == 3);
    CHECK(sc::jointCountFor(e, 1e7f) == 1000);
    e.flags = sc::kFlagFixedJointCount;
    e.jointCount = 0;
    CHECK(sc::jointCountFor(e, 50.0f) == 2);
    e.jointCount = 12;
    CHECK(sc::jointCountFor(e, 50.0f) == 13);
    e.flags = 0;
    e.avgSegLen = 0.0f;
    CHECK(sc::jointCountFor(e, 5.0f) == 3);  // what does not fit chops to 0
}

TEST_CASE("the client's cubic cosine and sine (0x005fff80, 0x006f7a10)", "[spell_chain]") {
    for (float x : {0.0f, 0.5f, 1.0f, 2.0f, 3.14159265f, -1.0f, -2.5f, 5.0f}) {
        CHECK(sc::clientCos(x) == Catch::Approx(std::cos(x)).margin(0.03));
        CHECK(sc::clientSin(x) == Catch::Approx(std::sin(x)).margin(0.03));
    }
}

TEST_CASE("a pulse shows a window along the strip (0x009a9490)", "[spell_chain]") {
    sc::ChainEffect e = plainEffect();
    std::vector<uint8_t> alphas;
    auto span = sc::pulseAlphas(e, 0.0f, 1.0f, 10, alphas);
    CHECK(span.first == 0);
    CHECK(span.count == 10);
    CHECK(alphas[9] == 255);

    e.flags = sc::kFlagPulse;
    e.pulseOnLength = 2.0f;
    e.pulseFadeLength = 1.0f;
    // Pairs a yard apart; the head at 4.5: fade in from 0.5, on from 1.5,
    // fading out from 3.5, gone at 4.5.
    span = sc::pulseAlphas(e, 4.5f, 1.0f, 20, alphas);
    CHECK(alphas[0] == 0);                      // 0 yards
    CHECK(alphas[2] == 127);                    // 1 yard: half way in
    CHECK(alphas[4] == 255);                    // 2 yards
    CHECK(alphas[6] == 255);                    // 3 yards
    CHECK(alphas[8] == 127);                    // 4 yards: half way out
    CHECK(alphas[10] == 0);                     // 5 yards: past the head
    CHECK(alphas[3] == alphas[2]);
    CHECK(span.first == 0);
    CHECK(span.count == 11);
}

TEST_CASE("the texture's u runs along the strip (0x009aa210)", "[spell_chain]") {
    sc::ChainEffect e = plainEffect();
    auto t = sc::textureU(e, 20.0f, 0.25f, 0.5f);
    CHECK(t.scale == 1.0f);
    CHECK(t.offset == 0.75f);
    e.flags = sc::kFlagTextureLength;
    e.textureLength = 4.0f;
    t = sc::textureU(e, 20.0f, 0.0f, 0.0f);
    CHECK(t.scale == 5.0f);
    CHECK(t.offset == 0.0f);
    e.flags |= sc::kFlagTextureFromEnd;
    t = sc::textureU(e, 20.0f, 0.0f, 0.0f);
    CHECK(t.offset == 5.0f);
}

TEST_CASE("a beam's Gx blend and state (0x009ab070, 0x009aa210)", "[spell_chain]") {
    sc::ChainEffect e = plainEffect();
    e.blendMode = 3;  // Gx add
    auto m = sc::materialFor(e);
    CHECK(m.blend == 4);
    CHECK_FALSE(m.depthWrite);
    CHECK_FALSE(m.fogged);
    CHECK_FALSE(m.lit);
    CHECK_FALSE(m.cull);
    e.blendMode = 2;  // Gx alpha
    m = sc::materialFor(e);
    CHECK(m.blend == 2);
    CHECK(m.depthWrite);
    e.blendMode = 10;  // Gx no-alpha add
    CHECK(sc::materialFor(e).blend == 3);
}

TEST_CASE("a lightning strip lies between its ends, pinched at them", "[spell_chain]") {
    sc::ChainEffect e = plainEffect();
    e.avgSegLen = 1.0f;
    sc::Rng rng(1234);
    sc::Lightning bolt;
    bolt.init(&e, rng);
    const glm::vec3 a(0.0f, 0.0f, 0.0f);
    const glm::vec3 b(10.0f, 0.0f, 0.0f);
    bolt.setEnds(a, b);
    std::vector<wowee::rendering::client_ribbon::Vertex> strip;
    // Hidden, nothing.
    bolt.update(0.016f, rng);
    CHECK_FALSE(bolt.build(glm::vec3(5.0f, -20.0f, 0.0f), strip));
    bolt.setVisible(true);
    bolt.update(0.016f, rng);
    CHECK(bolt.jointCount() == 13);  // 10 + 2 + 1
    REQUIRE(bolt.build(glm::vec3(5.0f, -20.0f, 0.0f), strip));
    REQUIRE(strip.size() == 26);
    CHECK(strip.front().position == a);
    CHECK(strip[1].position == a);
    CHECK(strip.back().position == b);
    CHECK(strip.front().uv.y == 0.5f);
    CHECK(strip.back().uv.x - strip.front().uv.x == Catch::Approx(1.0f).margin(1e-5));
    // No noise: every joint on the line, the sides a width apart, facing
    // the camera (across Z, as the camera looks along Y).
    for (size_t k = 2; k + 2 < strip.size(); k += 2) {
        const glm::vec3 mid = (strip[k].position + strip[k + 1].position) * 0.5f;
        CHECK(mid.y == Catch::Approx(0.0f).margin(1e-4));
        CHECK(mid.z == Catch::Approx(0.0f).margin(1e-4));
        CHECK(glm::length(strip[k].position - strip[k + 1].position) == Catch::Approx(1.0f).margin(1e-3));
        CHECK(std::fabs(strip[k].position.z - strip[k + 1].position.z) == Catch::Approx(1.0f).margin(1e-3));
        CHECK(strip[k].color == 0xFFFF8000u);
    }
}

TEST_CASE("a flickering strip goes off for its off time", "[spell_chain]") {
    sc::ChainEffect e = plainEffect();
    e.flags = sc::kFlagFlicker;
    e.minFlickerOnDuration = e.maxFlickerOnDuration = 0.1f;
    e.minFlickerOffDuration = e.maxFlickerOffDuration = 0.2f;
    sc::Rng rng(7);
    sc::Lightning bolt;
    bolt.init(&e, rng);
    bolt.setEnds(glm::vec3(0.0f), glm::vec3(5.0f, 0.0f, 0.0f));
    bolt.setVisible(true);
    std::vector<wowee::rendering::client_ribbon::Vertex> strip;
    bolt.update(0.05f, rng);
    CHECK(bolt.build(glm::vec3(0.0f, -10.0f, 0.0f), strip));
    bolt.update(0.06f, rng);  // on time spent
    CHECK(bolt.flickeredOff());
    CHECK_FALSE(bolt.build(glm::vec3(0.0f, -10.0f, 0.0f), strip));
    bolt.update(0.25f, rng);  // off time spent: on again
    CHECK_FALSE(bolt.flickeredOff());
    CHECK(bolt.build(glm::vec3(0.0f, -10.0f, 0.0f), strip));
}

TEST_CASE("the Combo string's words name rows as 0x009a8ce0 gathers them", "[spell_chain]") {
    CHECK(sc::comboRow(0) == 0);
    CHECK(sc::comboRow(0x00000004u) == 4);
    // Each byte's low bit is dropped.
    CHECK(sc::comboRow(0x00000105u) == 4);
    CHECK(sc::comboRow(0x00003231u) == 0x3230);
    // The high bits gathered down.
    CHECK(sc::comboRow(0x20000000u) == 0x100);
    CHECK(sc::comboRow(0x02000000u) == 0x80);
    CHECK(sc::comboRow(0x40000000u) == 0x10000);
    CHECK(sc::comboRow(0x04000000u) == 0x8000);
    CHECK(sc::comboRow(0xffffffffu) == 0xffffff);
}

TEST_CASE("the Combo words are read four bytes at a time, zeros past the block", "[spell_chain]") {
    const uint8_t bytes[6] = {0x04, 0x00, 0x00, 0x00, 0x08, 0x01};
    const auto words = sc::comboWordsAt(bytes, sizeof(bytes));
    CHECK(words[0] == 4);
    CHECK(words[1] == 0x0108u);
    CHECK(words[2] == 0);
    CHECK(words[11] == 0);
    // An empty Combo, the next string's first letters after its terminator.
    const uint8_t empty[5] = {0x00, 'S', 'p', 'e', 'l'};
    CHECK(sc::comboRow(sc::comboWordsAt(empty, sizeof(empty))[0]) == 0x71d300u);
}

TEST_CASE("the first bolt carries the unit's chain counter", "[spell_chain]") {
    sc::ChainEffect e = plainEffect();
    uint32_t end = 0;
    const auto bolts = sc::planBolts(e, 3, false, 0, end, 6);
    REQUIRE(bolts.size() == 3);
    CHECK(bolts[0].counter == 6);
    CHECK(bolts[1].counter == -1);
    CHECK(bolts[2].counter == -1);
    CHECK(sc::planBolts(e, 1, false, 0, end)[0].counter == -1);
}

TEST_CASE("a bolt's pulse lets the kits at its ends play", "[spell_chain]") {
    // At the source: the source's, counter -1.
    auto r = sc::pulseRelease(true, false, true);
    CHECK(r.source);
    CHECK_FALSE(r.target);
    CHECK_FALSE(r.sourceCounter);
    // At a unit's end: the target's.
    r = sc::pulseRelease(false, true, true);
    CHECK_FALSE(r.source);
    CHECK(r.target);
    CHECK_FALSE(r.sourceCounter);
    // At a place: the source's kits waiting on the bolt's counter.
    r = sc::pulseRelease(false, true, false);
    CHECK_FALSE(r.target);
    CHECK(r.sourceCounter);
}

TEST_CASE("a strip without a pulse shows both ends; a pulse only where it is", "[spell_chain]") {
    sc::ChainEffect e = plainEffect();
    sc::Rng rng(3);
    sc::Lightning bolt;
    bolt.init(&e, rng);
    bolt.setEnds(glm::vec3(0.0f), glm::vec3(10.0f, 0.0f, 0.0f));
    bolt.setVisible(true);
    std::vector<wowee::rendering::client_ribbon::Vertex> strip;
    bolt.update(0.01f, rng);
    REQUIRE(bolt.build(glm::vec3(0.0f, -10.0f, 0.0f), strip));
    CHECK(bolt.pulseAtSource());
    CHECK(bolt.pulseAtEnd());

    sc::ChainEffect p = plainEffect();
    p.flags = sc::kFlagPulse;
    p.pulseSpeed = 2.0f;
    p.pulseOnLength = 1.0f;
    p.pulseFadeLength = 0.5f;
    sc::Lightning pulsed;
    pulsed.init(&p, rng);
    pulsed.setEnds(glm::vec3(0.0f), glm::vec3(10.0f, 0.0f, 0.0f));
    pulsed.setVisible(true);
    pulsed.update(0.5f, rng);  // its head a yard out
    pulsed.build(glm::vec3(0.0f, -10.0f, 0.0f), strip);
    CHECK(pulsed.pulseAtSource());
    CHECK_FALSE(pulsed.pulseAtEnd());
    pulsed.update(5.5f, rng);  // its head at the far end
    pulsed.build(glm::vec3(0.0f, -10.0f, 0.0f), strip);
    CHECK_FALSE(pulsed.pulseAtSource());
    CHECK(pulsed.pulseAtEnd());
    // Not shown: neither.
    pulsed.setVisible(false);
    pulsed.build(glm::vec3(0.0f, -10.0f, 0.0f), strip);
    CHECK_FALSE(pulsed.pulseAtSource());
    CHECK_FALSE(pulsed.pulseAtEnd());
}
