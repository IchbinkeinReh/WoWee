/// Frame timing at high refresh rates.
///
/// At 60Hz a frame is 16.7ms and a millisecond of error anywhere is noise.
/// At 144Hz the budget is 6.94ms and the same millisecond is fourteen per
/// cent of it, which is what these check for.

#include <catch_amalgamated.hpp>

#include <algorithm>
#include <cstdint>

#include "core/frame_pacer.hpp"

using wowee::core::FramePacer;
using wowee::core::PacerClock;

namespace {

/// A clock the test moves: a sleep returns `late` after what it was asked
/// for, a yield costs `spin`, and nothing else moves it. What the pacer does
/// is then the same on every machine and under any load - the checks used to
/// sleep for real and measure, and failed where a shared runner's scheduler
/// was coarser than the margins measured.
struct TestClock final : PacerClock {
    std::int64_t now = 1'000'000'000;
    std::int64_t late = 0;
    std::int64_t spin = 1'000;
    int sleeps = 0;
    int yields = 0;
    std::int64_t nowNs() override { return now; }
    void sleepNs(std::int64_t ns) override {
        ++sleeps;
        now += ns + late;
    }
    void yield() override {
        ++yields;
        now += spin;
    }
};

constexpr std::int64_t kFrame144 = 1'000'000'000LL / 144;   // 6'944'444

}  // namespace

TEST_CASE("the steady clock never goes back", "[pacing]") {
    const std::int64_t a = FramePacer::nowNs();
    const std::int64_t b = FramePacer::nowNs();
    CHECK(b >= a);
}

TEST_CASE("the first tick has no previous frame to measure from", "[pacing]") {
    TestClock clock;
    FramePacer pacer(clock);
    CHECK(pacer.tickNs() == 0);
}

TEST_CASE("a tick measures the gap since the last one", "[pacing]") {
    TestClock clock;
    FramePacer pacer(clock);
    (void)pacer.tickNs();   // the baseline
    clock.now += 5'000'000;
    CHECK(pacer.tickNs() == 5'000'000);
    // A clock that steps back by a little on virtualised hardware is no
    // negative frame.
    clock.now -= 10;
    CHECK(pacer.tickNs() == 0);
}

TEST_CASE("the frame start is the reading the delta was taken against", "[pacing]") {
    TestClock clock;
    FramePacer pacer(clock);
    (void)pacer.tickNs();
    const std::int64_t first = pacer.lastTickNs();
    CHECK(first == clock.now);
    clock.now += 2'000'000;
    (void)pacer.tickNs();
    // Not a fresh clock reading: it is the one the delta was measured from,
    // so the cap and the delta cannot disagree about where the frame began.
    CHECK(pacer.lastTickNs() == first + 2'000'000);
}

TEST_CASE("seconds come out of nanoseconds, and a stall is capped", "[pacing]") {
    // 144Hz, to the nanosecond.
    CHECK(FramePacer::toSeconds(6'944'444) == Catch::Approx(0.006944444f).epsilon(0.0001));
    CHECK(FramePacer::toSeconds(16'666'667) == Catch::Approx(0.016666667f).epsilon(0.0001));
    CHECK(FramePacer::toSeconds(0) == Catch::Approx(0.0f));

    // A machine back from sleep hands over an enormous delta, and everything
    // that integrates over it would teleport.
    CHECK(FramePacer::toSeconds(5'000'000'000) == Catch::Approx(0.1f));
    CHECK(FramePacer::toSeconds(5'000'000'000, 0.25f) == Catch::Approx(0.25f));
}

TEST_CASE("no cap means no wait", "[pacing]") {
    TestClock clock;
    FramePacer pacer(clock);
    (void)pacer.tickNs();
    const std::int64_t start = clock.now;
    pacer.waitForCap(0);
    pacer.waitForCap(-1);
    CHECK(clock.now == start);
    CHECK(clock.sleeps + clock.yields == 0);
}

TEST_CASE("a pacer that has never ticked has no frame to pace", "[pacing]") {
    TestClock clock;
    FramePacer pacer(clock);
    pacer.waitForCap(144);   // no baseline yet: must not wait on lastNs_ of 0
    CHECK(clock.sleeps + clock.yields == 0);
}

TEST_CASE("a frame already over budget is not delayed further", "[pacing]") {
    // A frame that took 50ms has blown a 144Hz budget seven times over.
    // Waiting any longer would turn one slow frame into two.
    // The loop's order: tick ends a frame, the next frame's work runs, then
    // the cap is asked to hold. No second tick - that is what would move the
    // frame start forward and hide the overrun.
    TestClock clock;
    FramePacer pacer(clock);
    (void)pacer.tickNs();
    clock.now += 50'000'000;   // slow frame
    pacer.waitForCap(144);
    CHECK(clock.sleeps + clock.yields == 0);
}

TEST_CASE("the cap holds a 144Hz frame to its budget", "[pacing]") {
    // A sleep that returns a millisecond late, as a desktop's does.
    TestClock clock;
    clock.late = 1'000'000;
    FramePacer pacer(clock);
    (void)pacer.tickNs();
    // The first frames learn how late a sleep comes back.
    for (int i = 0; i < 3; ++i) {
        pacer.waitForCap(144);
        (void)pacer.tickNs();
    }
    for (int i = 0; i < 5; ++i) {
        const std::int64_t start = pacer.lastTickNs();
        clock.now += 1'000'000;   // the frame's work
        pacer.waitForCap(144);
        (void)pacer.tickNs();
        const std::int64_t took = pacer.lastTickNs() - start;
        // Never short: returning early is the cap failing to cap, and it is
        // what dropping the spin at the end of the wait would do.
        CHECK(took >= kFrame144);
        // And not long: the spin ends the wait within a yield of the
        // deadline, where sleeping the whole budget would take the sleep's
        // millisecond on top.
        CHECK(took - kFrame144 <= clock.spin);
    }
}

TEST_CASE("a sleep's lateness is learned, and a hitch is forgotten slowly", "[pacing]") {
    TestClock clock;
    FramePacer pacer(clock);
    (void)pacer.tickNs();
    // One hitch: the sleep came back three milliseconds late. That frame is
    // late; the next is not, the margin having grown to cover it.
    clock.late = 3'000'000;
    pacer.waitForCap(144);
    (void)pacer.tickNs();
    clock.late = 0;
    std::int64_t start = pacer.lastTickNs();
    pacer.waitForCap(144);
    (void)pacer.tickNs();
    CHECK(pacer.lastTickNs() - start >= kFrame144);
    CHECK(pacer.lastTickNs() - start - kFrame144 <= clock.spin);
    // The spin it costs shrinks again over the frames that follow.
    int yieldsAt[2] = {0, 0};
    for (int n = 0; n < 2; ++n) {
        const int before = clock.yields;
        pacer.waitForCap(144);
        (void)pacer.tickNs();
        yieldsAt[n] = clock.yields - before;
        for (int i = 0; i < 200; ++i) {
            pacer.waitForCap(144);
            (void)pacer.tickNs();
        }
    }
    CHECK(yieldsAt[1] < yieldsAt[0]);
}

TEST_CASE("which of the two caps a frame is held to", "[frame_pacer]") {
    using wowee::core::capInEffect;
    // The pair, both set: a frame keeps the foreground limit while the window
    // is the active one, and the background one once it is not.
    CHECK(capInEffect(60, 30, true) == 60);
    CHECK(capInEffect(60, 30, false) == 30);
    // A background limit looser than the foreground one changes nothing,
    // focused or not: the client asks whether it is at least as strict.
    CHECK(capInEffect(60, 90, false) == 60);
    CHECK(capInEffect(60, 60, false) == 60);
    // Nothing set asks for no wait at all, on either side - and an unset
    // background never wins the comparison against a limit that is set.
    CHECK(capInEffect(0, 0, true) == 0);
    CHECK(capInEffect(0, 0, false) == 0);
    CHECK(capInEffect(0, 30, false) == 30);
    CHECK(capInEffect(0, 30, true) == 0);
    CHECK(capInEffect(45, 0, false) == 45);
    CHECK(capInEffect(120, 0, false) == 120);
    // A frame held for a quarter of a second or longer is held for an eighth
    // instead, which the client insists on twice: writing either cvar lifts
    // 1..7 to 8, and the wait works it out again (0x00769830, 0x006836d0).
    CHECK(wowee::core::kLowestFrameCap == 8);
    CHECK(capInEffect(3, 0, true) == 8);
    CHECK(capInEffect(0, 1, false) == 8);
    CHECK(capInEffect(8, 0, true) == 8);
    CHECK(capInEffect(9, 0, true) == 9);
}
