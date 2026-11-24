#pragma once

// The last of a run of things, each with a number of its own, so that a caller
// who comes back can be given what it has not had yet.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <utility>
#include <vector>

namespace wowee::mcp {

template <typename T>
class RingLog {
public:
    explicit RingLog(size_t capacity) : capacity_(capacity) {}

    struct Entry {
        uint64_t seq = 0;
        T value;
    };

    void push(T value) {
        const std::lock_guard<std::mutex> lock(mutex_);
        entries_.push_back({.seq = ++last_, .value = std::move(value)});
        if (entries_.size() > capacity_) entries_.pop_front();
    }

    /// What came after `since` and passes `keep`: the newest `limit` of it,
    /// oldest first.
    template <typename Keep>
    std::vector<Entry> after(uint64_t since, size_t limit, Keep keep) const {
        const std::lock_guard<std::mutex> lock(mutex_);
        std::vector<Entry> out;
        for (auto it = entries_.rbegin(); it != entries_.rend() && out.size() < limit; ++it) {
            if (it->seq <= since) break;
            if (keep(it->value)) out.push_back(*it);
        }
        std::reverse(out.begin(), out.end());
        return out;
    }

    /// The number of the newest, 0 when there has been none.
    [[nodiscard]] uint64_t last() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return last_;
    }
    /// The number of the oldest still kept: what lies between a caller's
    /// `since` and this is gone.
    [[nodiscard]] uint64_t oldest() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return entries_.empty() ? last_ + 1 : entries_.front().seq;
    }

private:
    mutable std::mutex mutex_;
    std::deque<Entry> entries_;
    size_t capacity_;
    uint64_t last_ = 0;
};

}  // namespace wowee::mcp
