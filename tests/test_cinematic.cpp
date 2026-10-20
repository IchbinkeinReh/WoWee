// A cinematic's camera tracks (0x0082b460, 0x008284d0, 0x00984450,
// 0x005ff440) and its course (0x00528af0 .. 0x0051d7c0, the fades of
// 0x005ed120).
#include <catch_amalgamated.hpp>

#include "game/cinematic.hpp"
#include "rendering/m2_camera_track.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace cam = wowee::rendering::m2_camera;
namespace cin = wowee::game::cinematic;
using wowee::pipeline::M2SplineTrack;

namespace {

M2SplineTrack vec3Track(uint16_t type, std::vector<uint32_t> times, std::vector<float> keys) {
    M2SplineTrack t;
    t.interpolationType = type;
    t.components = 3;
    t.sequences.resize(1);
    t.sequences[0].timestamps = std::move(times);
    t.sequences[0].keys = std::move(keys);
    return t;
}

}  // namespace

TEST_CASE("camera track keys around a time", "[cinematic]") {
    const std::vector<uint32_t> times = {0, 1000, 3000};
    auto s = cam::findKeys(times, 500);
    CHECK(s.lower == 0);
    CHECK(s.upper == 1);
    CHECK(s.fraction == Catch::Approx(0.5f));
    s = cam::findKeys(times, 1000);
    CHECK(s.lower == 1);
    CHECK(s.upper == 2);
    CHECK(s.fraction == Catch::Approx(0.0f));
    s = cam::findKeys(times, 2500);
    CHECK(s.lower == 1);
    CHECK(s.fraction == Catch::Approx(0.75f));
    // Past the last key: the last key.
    s = cam::findKeys(times, 9000);
    CHECK(s.lower == 2);
    CHECK(s.upper == 2);
    CHECK(s.fraction == 0.0f);
    // One key: that key.
    s = cam::findKeys({7}, 100);
    CHECK(s.lower == 0);
    CHECK(s.upper == 0);
}

TEST_CASE("camera track interpolations", "[cinematic]") {
    // Two keys, (value, in, out) each: 0 -> 10 on x, with tangents.
    const std::vector<float> keys = {
        0, 0, 0,   1, 1, 1,   2, 2, 2,   // key 0: value 0, in 1, out 2
        10, 10, 10, 3, 3, 3,  4, 4, 4,   // key 1: value 10, in 3, out 4
    };
    float out[3] = {-1, -1, -1};

    REQUIRE(cam::sample(vec3Track(0, {0, 1000}, keys), 0, 500, out));
    CHECK(out[0] == 0.0f);  // the earlier key

    REQUIRE(cam::sample(vec3Track(1, {0, 1000}, keys), 0, 250, out));
    CHECK(out[0] == Catch::Approx(2.5f));

    // Bezier at t = 0.5: v0/8 + out0*3/8 + in1*3/8 + v1/8.
    REQUIRE(cam::sample(vec3Track(2, {0, 1000}, keys), 0, 500, out));
    CHECK(out[0] == Catch::Approx(0.0f / 8 + 2.0f * 3 / 8 + 3.0f * 3 / 8 + 10.0f / 8));

    // Hermite at t = 0.5: h00 = 0.5, h10 = 0.125, h01 = 0.5, h11 = -0.125;
    // out0 with h10, in1 with h11.
    REQUIRE(cam::sample(vec3Track(3, {0, 1000}, keys), 0, 500, out));
    CHECK(out[1] == Catch::Approx(0.0f * 0.5f + 2.0f * 0.125f + 10.0f * 0.5f - 3.0f * 0.125f));

    // A type the client does not know leaves the cache as it was.
    out[0] = 42.0f;
    CHECK_FALSE(cam::sample(vec3Track(7, {0, 1000}, keys), 0, 500, out));
    CHECK(out[0] == 42.0f);

    // A sequence past the track's own reads its first; one with no keys, zero.
    REQUIRE(cam::sample(vec3Track(1, {0, 1000}, keys), 5, 1000, out));
    CHECK(out[0] == Catch::Approx(10.0f));
    REQUIRE(cam::sample(vec3Track(1, {}, {}), 0, 500, out));
    CHECK(out[0] == 0.0f);
}

TEST_CASE("a cinematic camera in the world", "[cinematic]") {
    wowee::pipeline::M2Camera camera;
    camera.positionBase = {1.0f, 0.0f, 0.0f};
    camera.targetBase = {2.0f, 0.0f, 0.0f};
    camera.positionTrack = vec3Track(1, {0, 1000}, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 5, 0, 0, 0, 0, 0, 0});
    camera.rollTrack.components = 1;
    cam::Pose cache;
    const cam::Pose p = cam::evaluate(camera, 0, 1000, cache);
    CHECK(p.position.z == Catch::Approx(5.0f));
    CHECK(p.position.x == Catch::Approx(1.0f));
    CHECK(p.target.x == Catch::Approx(2.0f));
    CHECK(p.roll == 0.0f);

    // Facing a quarter turn: the model's +x is the world's +y.
    const glm::vec3 w = cam::placeInWorld({1.0f, 0.0f, 3.0f}, {100.0f, 200.0f, 10.0f}, 1.5707964f);
    CHECK(w.x == Catch::Approx(100.0f).margin(1e-4));
    CHECK(w.y == Catch::Approx(201.0f));
    CHECK(w.z == Catch::Approx(13.0f));

    // Looking along +x, no roll: up is up.
    cam::View v = cam::viewOf({0, 0, 0}, {5, 0, 0}, 0.0f);
    CHECK(v.forward.x == Catch::Approx(1.0f));
    CHECK(v.up.z == Catch::Approx(1.0f));
    // A roll tilts the reference toward the world's +y (0x005ff440).
    v = cam::viewOf({0, 0, 0}, {5, 0, 0}, 0.5f);
    CHECK(v.up.y == Catch::Approx(std::sin(0.5f)));
    CHECK(v.up.z == Catch::Approx(std::cos(0.5f)));
    // Straight down: the left is +x.
    v = cam::viewOf({0, 0, 10}, {0, 0, 0}, 0.0f);
    CHECK(v.forward.z == Catch::Approx(-1.0f));
    CHECK(glm::length(v.up) == Catch::Approx(1.0f));
}

TEST_CASE("the screen fades as the client's", "[cinematic]") {
    using Then = cin::ScreenFade::Then;
    cin::ScreenFade f;
    CHECK(f.frame(0) == Then::Nothing);
    CHECK(f.alpha() == 0.0f);
    // From black only where there is black.
    CHECK(f.fromBlack(0.25f, Then::End) == Then::Nothing);
    CHECK_FALSE(f.active());

    f.toBlack(0.25f, Then::StartCamera);
    CHECK(f.frame(1000) == Then::Nothing);  // its clock starts here
    CHECK(f.alpha() == 0.0f);
    CHECK(f.frame(1125) == Then::Nothing);
    CHECK(f.alpha() == Catch::Approx(0.5f));
    CHECK(f.frame(1250) == Then::Nothing);  // there
    CHECK(f.alpha() == 1.0f);
    CHECK(f.frame(1260) == Then::StartCamera);  // the frame after
    // Black stays.
    CHECK(f.frame(5000) == Then::Nothing);
    CHECK(f.alpha() == 1.0f);
    CHECK(f.active());

    f.fromBlack(0.25f, Then::Nothing);
    f.frame(6000);
    CHECK(f.alpha() == 1.0f);
    f.frame(6100);
    CHECK(f.alpha() == Catch::Approx(0.6f));
    f.frame(6300);
    CHECK(f.alpha() == 0.0f);
    f.frame(6310);
    CHECK_FALSE(f.active());

    // Over at once.
    f.toBlack(0.25f, Then::Nothing);
    CHECK(f.fromBlack(0.0f, Then::NextCamera) == Then::NextCamera);
    CHECK_FALSE(f.active());
}

namespace {

struct FakeHost : cin::Host {
    std::vector<std::string> log;
    bool player = true;
    bool cameraArea = true;
    bool playerArea = true;
    uint32_t duration = 2000;
    bool inCinematic = false;
    std::vector<uint32_t> posed;

    std::optional<cin::SequenceRow> sequenceRow(uint32_t id) override {
        if (id != 1) return std::nullopt;
        cin::SequenceRow r;
        r.soundId = 0;
        r.cameras = {10, 20, 0, 0, 0, 0, 0, 0};
        return r;
    }
    std::optional<cin::CameraRow> cameraRow(uint32_t id) override {
        if (id != 10 && id != 20) return std::nullopt;
        cin::CameraRow r;
        r.id = id;
        r.soundId = id + 1000;
        return r;
    }
    bool hasActivePlayer() override { return player; }
    void stopWorldSounds() override {}
    void setInCinematic(bool in) override { inCinematic = in; }
    void fireStart() override { log.emplace_back("START"); }
    void fireStop() override { log.emplace_back("STOP"); }
    void playSequenceSound(uint32_t) override {}
    void stopSequenceSound() override { log.emplace_back("stopSeq"); }
    void playCameraSound(uint32_t id) override { log.push_back("sound " + std::to_string(id)); }
    void stopCameraSound() override { log.emplace_back("stopCamSound"); }
    std::optional<uint32_t> loadCamera(const cin::CameraRow& row) override {
        log.push_back("load " + std::to_string(row.id));
        return duration;
    }
    void poseCamera(uint32_t t) override { posed.push_back(t); }
    bool cameraAreaReady() override { return cameraArea; }
    bool playerAreaReady() override { return playerArea; }
    void releaseCamera() override { log.emplace_back("release"); }
    void sendNextCamera() override { log.emplace_back("NEXT"); }
    void sendComplete() override { log.emplace_back("COMPLETE"); }
};

/// Frames every 10 ms from `from` to `to`.
void run(cin::Director& d, uint32_t from, uint32_t to) {
    for (uint32_t t = from; t <= to; t += 10) d.update(t);
}

}  // namespace

TEST_CASE("a cinematic's course", "[cinematic]") {
    FakeHost host;
    cin::Director d(host);
    d.start(1, 0);
    CHECK(host.inCinematic);
    CHECK(d.state() == cin::Director::State::FadingOut);
    CHECK(host.log.empty());

    // A quarter second to black, then the first camera.
    run(d, 0, 240);
    CHECK(host.log.empty());
    run(d, 250, 300);
    REQUIRE(host.log.size() >= 4);
    CHECK(host.log[0] == "START");
    CHECK(host.log[1] == "load 10");
    CHECK(host.log[2] == "NEXT");
    CHECK(host.log[3] == "sound 1010");
    CHECK(d.state() == cin::Director::State::Playing);
    CHECK(d.cameraActive());

    // Its two seconds, its last frame held, to black, the second camera.
    host.log.clear();
    run(d, 310, 2400);
    CHECK(d.cameraTimeMs() == 1999);
    run(d, 2410, 2900);
    REQUIRE(host.log.size() >= 4);
    CHECK(host.log[0] == "stopCamSound");
    CHECK(host.log[1] == "load 20");
    CHECK(host.log[2] == "NEXT");
    CHECK(host.log[3] == "sound 1020");

    // Its end: no third camera, so the cinematic ends.
    host.log.clear();
    run(d, 2910, 6000);
    const std::vector<std::string> ending = {"stopCamSound", "STOP", "release", "stopSeq", "COMPLETE"};
    CHECK(host.log == ending);
    CHECK_FALSE(host.inCinematic);
    CHECK(d.state() == cin::Director::State::Idle);
    CHECK_FALSE(d.cameraActive());
    run(d, 6010, 6400);
    CHECK(d.fadeAlpha() == 0.0f);
}

TEST_CASE("a cinematic stopped, and waiting for the world", "[cinematic]") {
    FakeHost host;
    host.cameraArea = false;
    cin::Director d(host);
    d.start(1, 0);
    run(d, 0, 300);
    // Black while the world loads around the camera; nothing sent yet.
    CHECK(d.state() == cin::Director::State::LoadingCamera);
    CHECK(d.fadeAlpha() == 1.0f);
    CHECK(std::find(host.log.begin(), host.log.end(), "NEXT") == host.log.end());
    host.cameraArea = true;
    run(d, 310, 400);
    CHECK(d.state() == cin::Director::State::Playing);

    // Escape: the camera's sound stops, to black, then the end - which
    // waits for the world around the player before it completes.
    host.log.clear();
    host.playerArea = false;
    d.stop(500);
    CHECK(host.log == std::vector<std::string>{"stopCamSound"});
    run(d, 500, 800);
    CHECK(d.state() == cin::Director::State::LoadingPlayer);
    CHECK(host.inCinematic);
    CHECK(d.fadeAlpha() == 1.0f);
    host.playerArea = true;
    d.update(810);
    CHECK_FALSE(host.inCinematic);
    CHECK(host.log.back() == "COMPLETE");
}

TEST_CASE("a cinematic with no camera, or no player", "[cinematic]") {
    FakeHost host;
    cin::Director d(host);
    // An unknown sequence: black, START, then straight to the end.
    d.start(99, 0);
    run(d, 0, 300);
    const std::vector<std::string> expect = {"START", "STOP", "release", "stopSeq", "COMPLETE"};
    CHECK(host.log == expect);
    CHECK_FALSE(host.inCinematic);

    // No active player: in the cinematic, and nothing more (0x00528af0).
    FakeHost alone;
    alone.player = false;
    cin::Director d2(alone);
    d2.start(1, 0);
    run(d2, 0, 1000);
    CHECK(alone.inCinematic);
    CHECK(alone.log.empty());
    CHECK(d2.fadeAlpha() == 0.0f);
}
