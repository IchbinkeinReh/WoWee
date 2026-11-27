#pragma once

// Streaming data mode: files read once on a thread of their own so that a
// later read of them on the main thread finds their bytes on the disk.
//
// A table the game first opens in play (a DBC behind a window, a spell's
// visual, a tooltip's line) is read on the main thread where it is asked
// for, and it has to be there whole: two hundred places expect it at once.
// On a local install that is a disk read; streaming it is a wait for the
// server in the middle of a frame (pipeline/stream_wait.hpp). Every chunk a
// read fetches stays in the chunk cache, so reading those files once in the
// background, early, turns the later read on the main thread into the disk
// read it is on a local install. Nothing is kept in memory here - the bytes
// read are dropped; the chunk cache is what holds them.

#include <atomic>
#include <cstddef>
#include <functional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace wowee::pipeline {

class BackgroundWarm {
public:
    BackgroundWarm() = default;
    ~BackgroundWarm() { stop(); }
    BackgroundWarm(const BackgroundWarm&) = delete;
    BackgroundWarm& operator=(const BackgroundWarm&) = delete;

    /// On a thread of its own: takes the list from `list` (which may itself
    /// read, a listfile say), then reads each path on it with `read`, in
    /// order, until all are read or stop() is called. A warm already running
    /// is cut short first.
    void start(std::function<std::vector<std::string>()> list,
               std::function<void(const std::string&)> read) {
        stop();
        stop_.store(false);
        done_.store(false);
        warmed_.store(0);
        thread_ = std::thread([this, list = std::move(list), read = std::move(read)]() {
            const std::vector<std::string> paths = list ? list() : std::vector<std::string>{};
            for (const std::string& path : paths) {
                if (stop_.load(std::memory_order_relaxed)) break;
                read(path);
                warmed_.fetch_add(1, std::memory_order_relaxed);
            }
            done_.store(true);
        });
    }

    /// Cuts the reading short and waits for the thread (shutdown).
    void stop() {
        stop_.store(true);
        if (thread_.joinable()) thread_.join();
    }

    [[nodiscard]] bool done() const { return done_.load(); }
    [[nodiscard]] size_t warmed() const { return warmed_.load(std::memory_order_relaxed); }

private:
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> done_{false};
    std::atomic<size_t> warmed_{0};
};

}  // namespace wowee::pipeline
