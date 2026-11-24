#pragma once

// A tool call as the game's loop sees it: steps, looked at once a frame, until
// one of them ends it.
//
// Reading the world is one step. Pressing a key is several frames long - down,
// held, up - as a key on a keyboard is, and walking somewhere is a step that is
// asked again every frame until it is there. None of them ever waits: the game
// goes on between the frames.

#include <deque>
#include <functional>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

namespace wowee::mcp {

using json = nlohmann::json;

class Script {
public:
    enum class Step {
        Again,  ///< ask this step again next frame
        Next,   ///< on to the next step, in this frame
        Done    ///< the call is over
    };
    using Fn = std::function<Step(Script&)>;

    /// Steps to come, in order.
    Script& then(Fn fn) {
        steps_.push_back(std::move(fn));
        return *this;
    }
    /// Steps to come before whatever is already waiting - what a step that
    /// has just found out what is to be done puts there.
    Script& first(std::deque<Fn> fns) {
        steps_.insert(steps_.begin(), std::make_move_iterator(fns.begin()), std::make_move_iterator(fns.end()));
        return *this;
    }

    /// A step that lets n frames go by.
    static Fn frames(int n);
    /// A step that lets time go by.
    static Fn seconds(double s);

    /// One frame's worth. True when the call is over.
    bool run(double now);
    /// The call is given up - the client went away, or time ran out: whatever
    /// it holds down is let go of.
    void cancel();

    /// Set by a step that ends the call, or left null for the last one to.
    json result;
    bool failed = false;
    /// A picture in the answer, base64 of a PNG or JPEG (with result as the
    /// text beside it). Made off the game's thread by `finish`.
    std::string imageBase64;
    std::string imageMime;
    /// The slow part of the answer, run on the caller's thread.
    std::function<void(Script&)> finish;
    /// Run whenever the call ends, however it does.
    std::function<void()> onEnd;

    /// Ends the call with an error.
    Step fail(std::string why);
    /// Ends the call with a result.
    Step done(json value);

    /// The frame's time, for the steps.
    [[nodiscard]] double now() const { return now_; }
    /// Seconds since the call began.
    [[nodiscard]] double elapsed() const { return now_ - began_; }

private:
    void end();

    std::deque<Fn> steps_;
    double now_ = 0.0;
    double began_ = -1.0;
    bool ended_ = false;
};

}  // namespace wowee::mcp
