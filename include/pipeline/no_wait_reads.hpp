#pragma once

// Files the main thread needs together - a sound kit's clips, say - read
// without waiting for the streaming server (pipeline/stream_wait.hpp).
//
// ready() reads each named file it does not hold yet under a NoWait and keeps
// what it got; a file still being fetched in the background is left for the
// next call. Once every file is held (read, or found missing: empty) it
// answers true and take() hands the bytes out one by one. A caller that is
// not ready yet plays or shows nothing this time and asks again later -
// nothing is remembered as missing because it was still on its way, and what
// already came is not read a second time.
//
// Outside streaming no read is deferred: ready() is true on the first call
// and this is a plain read of each file, as before.

#include "pipeline/stream_wait.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace wowee::pipeline {

class NoWaitReads {
public:
    /// `read(path)` gives a file's bytes (empty: missing or deferred), e.g.
    /// AssetManager::readFile. True when every path is held.
    template <class Read>
    bool ready(const std::vector<std::string>& paths, Read&& read) {
        bool all = true;
        for (const std::string& path : paths) {
            if (held_.find(path) != held_.end()) continue;
            stream_wait::NoWait noWait;
            std::vector<uint8_t> data = read(path);
            if (data.empty() && noWait.deferred()) {
                all = false;   // on its way: asked for again next time
                continue;
            }
            held_.emplace(path, std::move(data));
        }
        return all;
    }

    /// The bytes ready() held for `path` (empty when missing), given up.
    std::vector<uint8_t> take(const std::string& path) {
        auto it = held_.find(path);
        if (it == held_.end()) return {};
        std::vector<uint8_t> data = std::move(it->second);
        held_.erase(it);
        return data;
    }

    /// Files held for a set that is not complete yet (tests, diagnostics).
    [[nodiscard]] size_t held() const { return held_.size(); }

    void clear() { held_.clear(); }

private:
    std::unordered_map<std::string, std::vector<uint8_t>> held_;
};

}  // namespace wowee::pipeline
