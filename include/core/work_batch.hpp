#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <exception>
#include <functional>
#include <mutex>
#include <utility>

namespace wowee {
namespace core {

// A fixed number of independent jobs, numbered 0..size()-1, run by whichever
// threads call runAvailable(): the thread that made the batch and any idle
// thread that joins in. Each index runs exactly once, on one thread; a job
// writes only its own result slot, so the results come out in index order
// whoever ran them. wait() returns once every job has finished.
//
// The thread that made the batch should call runAvailable() itself before
// wait(): then the batch finishes even if nobody else ever joins in.
class WorkBatch {
public:
    WorkBatch(size_t count, std::function<void(size_t)> job)
        : count_(count), job_(std::move(job)) {}

    WorkBatch(const WorkBatch&) = delete;
    WorkBatch& operator=(const WorkBatch&) = delete;

    // Claims and runs jobs until none are left to claim. Returns how many
    // this call ran. A job that throws counts as finished; the first
    // exception is rethrown by wait().
    size_t runAvailable() {
        size_t ran = 0;
        for (;;) {
            const size_t i = next_.fetch_add(1, std::memory_order_relaxed);
            if (i >= count_) break;
            try {
                job_(i);
            } catch (...) {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!error_) error_ = std::current_exception();
            }
            ++ran;
            if (done_.fetch_add(1, std::memory_order_acq_rel) + 1 == count_) {
                std::lock_guard<std::mutex> lock(mutex_);
                cv_.notify_all();
            }
        }
        return ran;
    }

    // Every job has been claimed (some may still be running).
    [[nodiscard]] bool exhausted() const {
        return next_.load(std::memory_order_relaxed) >= count_;
    }

    // Blocks until every job has finished.
    void wait() {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this]() { return done_.load(std::memory_order_acquire) >= count_; });
        if (error_) std::rethrow_exception(error_);
    }

    [[nodiscard]] size_t size() const { return count_; }

private:
    const size_t count_;
    std::function<void(size_t)> job_;
    std::atomic<size_t> next_{0};
    std::atomic<size_t> done_{0};
    std::mutex mutex_;
    std::condition_variable cv_;
    std::exception_ptr error_;
};

} // namespace core
} // namespace wowee
