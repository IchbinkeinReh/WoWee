#pragma once

#include <cstddef>
#include <cstdint>
#include <list>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace wowee::pipeline {

/// The asset manager's cache of file contents: the least recently read go
/// first once the budget is reached.
///
/// Not thread-safe on its own except for lookup(): the owner holds its lock
/// shared for lookups and exclusively for everything else. A lookup moves
/// the entry to the front of the recency list, so it takes the cache's own
/// small mutex for that splice - concurrent lookups under the shared lock
/// then cannot corrupt the list. A splice invalidates no iterator, so the
/// map's stored positions stay good.
class FileLruCache {
public:
    /// Copies the file's bytes into out and marks it most recently used.
    /// False when it is not cached.
    bool lookup(const std::string& key, std::vector<uint8_t>& out) const {
        auto it = map_.find(key);
        if (it == map_.end()) return false;
        out = it->second.data;
        std::lock_guard<std::mutex> lock(lruMutex_);
        lru_.splice(lru_.begin(), lru_, it->second.pos);
        return true;
    }

    /// Adds a file, first evicting the least recently used until it fits in
    /// budget. A file of half the budget or more is not cached (it would
    /// empty the cache for one entry); nor is one already there.
    void insert(const std::string& key, const std::vector<uint8_t>& data, size_t budget) {
        const size_t size = data.size();
        if (size == 0 || size >= budget / 2) return;
        if (map_.find(key) != map_.end()) return;
        while (bytes_ + size > budget && !lru_.empty()) {
            auto victim = map_.find(lru_.back());
            if (victim != map_.end()) {
                bytes_ -= victim->second.data.size();
                map_.erase(victim);
            }
            lru_.pop_back();
        }
        lru_.push_front(key);
        Entry e;
        e.data = data;
        e.pos = lru_.begin();
        map_.emplace(key, std::move(e));
        bytes_ += size;
    }

    void clear() {
        map_.clear();
        lru_.clear();
        bytes_ = 0;
    }

    [[nodiscard]] size_t bytes() const { return bytes_; }
    [[nodiscard]] size_t size() const { return map_.size(); }
    [[nodiscard]] bool contains(const std::string& key) const { return map_.count(key) != 0; }

private:
    struct Entry {
        std::vector<uint8_t> data;
        std::list<std::string>::iterator pos;  // its place in lru_
    };
    std::unordered_map<std::string, Entry> map_;
    mutable std::list<std::string> lru_;  // most recently used first
    mutable std::mutex lruMutex_;
    size_t bytes_ = 0;
};

}  // namespace wowee::pipeline
