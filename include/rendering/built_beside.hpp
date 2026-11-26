#pragma once

#include <chrono>
#include <future>
#include <memory>
#include <utility>

namespace wowee::rendering {

/// Something made on a thread of its own and taken by the frame once it is
/// there, without waiting for it: a renderer whose pipelines the driver
/// compiles for seconds with its cache empty. 12340 has no such cost on a
/// frame - its shaders are made when the device is (D3D9 takes them
/// compiled) - so a frame that takes input must not carry it either.
template <typename T>
class BuiltBeside {
public:
    BuiltBeside() = default;
    BuiltBeside(const BuiltBeside&) = delete;
    BuiltBeside& operator=(const BuiltBeside&) = delete;
    /// Waits for what is being made: it may hold what its owner frees next.
    ~BuiltBeside() { (void)take(true); }

    /// Begin making it: `make` returns the thing, or nullptr where it could
    /// not be made.
    template <typename Make>
    void begin(Make&& make) {
        future_ = std::async(std::launch::async, std::forward<Make>(make));
    }
    /// Whether it was begun and not taken yet.
    [[nodiscard]] bool started() const { return future_.valid(); }
    /// Whether it was begun and is still being made.
    [[nodiscard]] bool pending() const {
        return future_.valid() && future_.wait_for(std::chrono::seconds(0)) != std::future_status::ready;
    }
    /// The thing once it is made - once; nullptr while it is still being
    /// made (unless `wait`), where it was never begun, or where it failed.
    [[nodiscard]] std::unique_ptr<T> take(bool wait = false) {
        if (!future_.valid() || (!wait && pending())) return nullptr;
        return future_.get();
    }

    /// The thing, waited for: `pump` is called each `step` it is not there
    /// yet - a loading screen's frame, so that a wait of seconds is not a
    /// window that stops answering. nullptr where it was never begun or failed.
    template <typename Pump>
    [[nodiscard]] std::unique_ptr<T> takeWaiting(Pump&& pump,
                                                 std::chrono::milliseconds step = std::chrono::milliseconds(10)) {
        while (future_.valid() && future_.wait_for(step) != std::future_status::ready) pump();
        return take(true);
    }

private:
    std::future<std::unique_ptr<T>> future_;
};

}  // namespace wowee::rendering
