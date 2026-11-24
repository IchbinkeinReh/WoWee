#include "mcp/script.hpp"

namespace wowee::mcp {

Script::Fn Script::frames(int n) {
    return [left = n](Script&) mutable { return left-- > 0 ? Step::Again : Step::Next; };
}

Script::Fn Script::seconds(double s) {
    return [s, until = -1.0](Script& script) mutable {
        if (until < 0.0) until = script.now() + s;
        return script.now() < until ? Step::Again : Step::Next;
    };
}

void Script::end() {
    if (ended_) return;
    ended_ = true;
    steps_.clear();
    if (onEnd) onEnd();
}

bool Script::run(double now) {
    if (ended_) return true;
    now_ = now;
    if (began_ < 0.0) began_ = now;
    // A step is taken off before it is asked, so that one that puts others in
    // front of the queue finds it as it expects.
    constexpr int kMostStepsAFrame = 64;
    for (int i = 0; i < kMostStepsAFrame && !steps_.empty(); ++i) {
        Fn step = std::move(steps_.front());
        steps_.pop_front();
        const Step what = step(*this);
        if (what == Step::Done) {
            end();
            return true;
        }
        if (what == Step::Again) {
            steps_.push_front(std::move(step));
            return false;
        }
    }
    if (steps_.empty()) {
        end();
        return true;
    }
    return false;
}

void Script::cancel() {
    if (ended_) return;
    failed = true;
    result = "cancelled";
    end();
}

Script::Step Script::fail(std::string why) {
    failed = true;
    result = std::move(why);
    return Step::Done;
}

Script::Step Script::done(json value) {
    result = std::move(value);
    return Step::Done;
}

}  // namespace wowee::mcp
