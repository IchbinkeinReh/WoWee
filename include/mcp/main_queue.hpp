#pragma once

// From the threads that speak HTTP to the one that plays the game.
//
// A call is put here by a connection's thread, which waits for it; the game's
// loop runs what is here once a frame, for so long and no longer. Calls that
// only look run side by side; those that press something run one after the
// other, as one pair of hands would.

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "mcp/script.hpp"

namespace wowee::mcp {

class MainQueue {
public:
    struct Outcome {
        enum class Kind {
            Finished,  ///< script has the answer
            TimedOut,  ///< the game's loop did not get through it in time
            Busy,      ///< too many calls are waiting
            Closed     ///< the client is ending
        };
        Kind kind = Kind::Closed;
        std::shared_ptr<Script> script;
    };

    /// `make` is run on the game's thread, for what it reads is the game's.
    /// Waits up to `patience` for the call to end; past that the call is
    /// cancelled.
    Outcome submit(std::function<std::shared_ptr<Script>()> make, bool hands, std::chrono::milliseconds patience);

    /// The game's loop, once a frame: for `budget` at most, though always one
    /// step of one call. `now` in seconds. True where a call someone waits
    /// for was worked on: the player's hands at work, which keeps the client
    /// from going idle (Input::noteHardwareEvent) as a key press does.
    bool pump(double now, std::chrono::microseconds budget);

    /// No more calls; the ones waiting are told so. From the game's thread.
    void close();

    /// Whether a call that presses something is under way.
    [[nodiscard]] bool handsBusy() const;
    /// When pump last ran, by the steady clock; a long while ago, the game is
    /// loading.
    [[nodiscard]] std::chrono::steady_clock::time_point lastPump() const;

    static constexpr size_t kMostWaiting = 32;

private:
    struct Call {
        std::function<std::shared_ptr<Script>()> make;
        std::shared_ptr<Script> script;
        bool hands = false;
        bool abandoned = false;  ///< nobody waits for it any more
        bool over = false;
    };

    mutable std::mutex mutex_;
    std::condition_variable over_;
    std::deque<std::shared_ptr<Call>> calls_;
    bool closed_ = false;
    std::chrono::steady_clock::time_point lastPump_{};
};

}  // namespace wowee::mcp
