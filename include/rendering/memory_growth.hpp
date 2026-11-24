#pragma once

// What the memory's owners hold from one map change to the next, and which of
// them keep growing. Only bytes are counted.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>

namespace wowee::rendering {

/// One owner's share of the memory, for /mem: what it holds on the GPU (in
/// Vulkan allocations) and in the process's own heap. On a phone both come
/// out of the same RAM.
struct MemoryEntry {
    std::string name;
    uint64_t gpuBytes = 0;
    uint64_t cpuBytes = 0;
};

/// An owner's name without the counts in it: what is in brackets.
inline std::string memoryOwnerKey(const std::string& name) {
    std::string key;
    int depth = 0;
    for (const char c : name) {
        if (c == '(') ++depth;
        else if (c == ')') depth = std::max(0, depth - 1);
        else if (depth == 0 && !(c == ' ' && (key.empty() || key.back() == ' '))) key += c;
    }
    while (!key.empty() && key.back() == ' ') key.pop_back();
    return key;
}

class MemoryGrowth {
public:
    /// The owners at one point of a map change ("before", "cleared"), each a
    /// line where it is `floorBytes` or more or changed by as much: what it
    /// holds, and how much that is more than the first time at this point and
    /// the last. The greatest growth since the first comes first.
    std::vector<std::string> note(const std::string& stage, const std::vector<MemoryEntry>& owners,
                                  uint64_t floorBytes = 4ull << 20) {
        struct Row {
            const MemoryEntry* entry;
            int64_t sinceFirst, sinceLast;
        };
        std::vector<Row> rows;
        auto& kept = stages_[stage];
        const auto floor = static_cast<int64_t>(floorBytes);
        for (const MemoryEntry& e : owners) {
            const std::string key = memoryOwnerKey(e.name);
            if (key.empty()) continue;
            const auto bytes = static_cast<int64_t>(e.gpuBytes + e.cpuBytes);
            Kept& k = kept.try_emplace(key, Kept{bytes, bytes}).first->second;
            const Row row{&e, bytes - k.first, bytes - k.last};
            k.last = bytes;
            if (bytes >= floor || std::abs(row.sinceFirst) >= floor || std::abs(row.sinceLast) >= floor)
                rows.push_back(row);
        }
        std::stable_sort(rows.begin(), rows.end(),
                         [](const Row& a, const Row& b) { return a.sinceFirst > b.sinceFirst; });
        std::vector<std::string> out;
        constexpr double kMb = 1024.0 * 1024.0;
        for (const Row& r : rows) {
            char buf[320];
            std::snprintf(buf, sizeof(buf), "%+.1f MB since the first, %+.1f since the last: %s: GPU %.1f, CPU %.1f",
                          static_cast<double>(r.sinceFirst) / kMb, static_cast<double>(r.sinceLast) / kMb,
                          r.entry->name.c_str(), static_cast<double>(r.entry->gpuBytes) / kMb,
                          static_cast<double>(r.entry->cpuBytes) / kMb);
            out.emplace_back(buf);
        }
        return out;
    }

private:
    struct Kept {
        int64_t first, last;
    };
    std::unordered_map<std::string, std::unordered_map<std::string, Kept>> stages_;
};

}  // namespace wowee::rendering
