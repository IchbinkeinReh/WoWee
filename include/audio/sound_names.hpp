#pragma once

/// The sound files read, by their bytes (audio/sound_key.hpp). A play is handed
/// bytes and no name; whoever wants to say which sound it was - the MCP
/// server's list of the ones playing - asks here.

#include "audio/sound_key.hpp"

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace wowee::audio::sound_names {

/// More than the client's tables name.
inline constexpr size_t kMost = 20000;

namespace detail {
inline std::mutex& guard() {
    static std::mutex m;
    return m;
}
inline std::unordered_map<uint64_t, std::string>& names() {
    static std::unordered_map<uint64_t, std::string> n;
    return n;
}
}  // namespace detail

/// A file read, from any thread.
inline void note(const std::vector<uint8_t>& file, const std::string& path) {
    if (file.empty() || path.empty()) return;
    const uint64_t key = sound_key::of(file);
    const std::lock_guard<std::mutex> lock(detail::guard());
    auto& names = detail::names();
    if (names.size() < kMost) names.try_emplace(key, path);
}

/// The file of these bytes, "" for none read.
inline std::string of(uint64_t key) {
    const std::lock_guard<std::mutex> lock(detail::guard());
    const auto it = detail::names().find(key);
    return it != detail::names().end() ? it->second : std::string();
}

}  // namespace wowee::audio::sound_names
