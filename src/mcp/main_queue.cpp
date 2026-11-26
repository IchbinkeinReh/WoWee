#include "mcp/main_queue.hpp"

#include <algorithm>
#include <exception>
#include <vector>

namespace wowee::mcp {

MainQueue::Outcome MainQueue::submit(std::function<std::shared_ptr<Script>()> make, bool hands,
                                     std::chrono::milliseconds patience) {
    auto call = std::make_shared<Call>();
    call->make = std::move(make);
    call->hands = hands;
    std::unique_lock<std::mutex> lock(mutex_);
    if (closed_) return {.kind = Outcome::Kind::Closed, .script = nullptr};
    if (calls_.size() >= kMostWaiting) return {.kind = Outcome::Kind::Busy, .script = nullptr};
    calls_.push_back(call);
    const bool done = over_.wait_for(lock, patience, [&] { return call->over; });
    if (!done) {
        // Taken off and its keys let go of by the next pump.
        call->abandoned = true;
        return {.kind = Outcome::Kind::TimedOut, .script = nullptr};
    }
    if (!call->script) return {.kind = Outcome::Kind::Closed, .script = nullptr};
    return {.kind = Outcome::Kind::Finished, .script = call->script};
}

bool MainQueue::pump(double now, std::chrono::microseconds budget) {
    const auto start = std::chrono::steady_clock::now();
    std::vector<std::shared_ptr<Call>> todo;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        lastPump_ = start;
        if (calls_.empty()) return false;
        bool handsTaken = false;
        for (const auto& call : calls_) {
            // The first that presses something has the hands; the others wait
            // their turn. What nobody waits for is gone through to be ended.
            if (call->hands && !call->abandoned) {
                if (handsTaken) continue;
                handsTaken = true;
            }
            todo.push_back(call);
        }
    }

    bool any = false;
    bool worked = false;
    for (const auto& call : todo) {
        if (any && std::chrono::steady_clock::now() - start > budget) break;
        bool abandoned = false;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            abandoned = call->abandoned;
        }
        any = true;
        bool ended = false;
        if (abandoned) {
            if (call->script) call->script->cancel();
            ended = true;
        } else {
            worked = true;
            try {
                if (!call->script) call->script = call->make();
                ended = call->script->run(now);
            } catch (const std::exception& e) {
                // A tool that failed is an answer like any other, and no
                // reason for the game to end.
                if (!call->script) call->script = std::make_shared<Script>();
                call->script->cancel();
                call->script->failed = true;
                call->script->result = std::string("the tool failed: ") + e.what();
                ended = true;
            }
        }
        if (!ended) continue;
        const std::lock_guard<std::mutex> lock(mutex_);
        call->over = true;
        calls_.erase(std::remove(calls_.begin(), calls_.end(), call), calls_.end());
        over_.notify_all();
    }
    return worked;
}

void MainQueue::close() {
    std::deque<std::shared_ptr<Call>> left;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        left.swap(calls_);
    }
    for (const auto& call : left) {
        if (call->script) call->script->cancel();
    }
    const std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& call : left) {
        call->script = nullptr;
        call->over = true;
    }
    over_.notify_all();
}

bool MainQueue::handsBusy() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return std::any_of(calls_.begin(), calls_.end(), [](const auto& c) { return c->hands && c->script; });
}

std::chrono::steady_clock::time_point MainQueue::lastPump() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return lastPump_;
}

}  // namespace wowee::mcp
