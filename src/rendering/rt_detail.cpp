#include "rendering/rt_detail.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace wowee::rendering {
namespace rt_detail {

uint16_t packRgb565(float r, float g, float b) {
    auto q = [](float v, int maxv) {
        return static_cast<uint32_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * float(maxv)));
    };
    return static_cast<uint16_t>(q(r, 31) | (q(g, 63) << 5) | (q(b, 31) << 11));
}

std::array<float, 3> unpackRgb565(uint16_t c) {
    return {float(c & 31u) / 31.0f, float((c >> 5) & 63u) / 63.0f, float((c >> 11) & 31u) / 31.0f};
}

std::vector<uint32_t> buildPyramid(const std::array<uint32_t, kSide * kSide>& rgba,
                                   const std::array<float, 3>& mean) {
    // The colour as it is, alpha aside: a terrain tileset's alpha is its
    // highlight's mask and a model's may be anything; a cutout's invisible
    // texels are cut away by its mask before its texture is read.
    std::vector<std::array<float, 3>> level(size_t(kSide) * kSide);
    for (size_t i = 0; i < level.size(); ++i) {
        const uint32_t c = rgba[i];
        level[i] = {float(c & 255u) / 255.0f, float((c >> 8) & 255u) / 255.0f, float((c >> 16) & 255u) / 255.0f};
    }
    std::vector<uint16_t> texels(kSlotWords * 2, 0);
    int side = kSide;
    for (int l = 0; l < kLevels; ++l) {
        const uint32_t base = levelOffset(l);
        for (int i = 0; i < side * side; ++i) {
            const auto& c = side == 1 ? mean : level[size_t(i)];
            texels[base + uint32_t(i)] = packRgb565(c[0], c[1], c[2]);
        }
        if (side == 1) break;
        const int half = side / 2;
        std::vector<std::array<float, 3>> next(size_t(half) * half);
        for (int y = 0; y < half; ++y) {
            for (int x = 0; x < half; ++x) {
                std::array<float, 3> sum{0.0f, 0.0f, 0.0f};
                for (int dy = 0; dy < 2; ++dy) {
                    for (int dx = 0; dx < 2; ++dx) {
                        const auto& p = level[size_t(y * 2 + dy) * side + size_t(x * 2 + dx)];
                        for (int k = 0; k < 3; ++k) sum[size_t(k)] += p[size_t(k)];
                    }
                }
                next[size_t(y) * half + size_t(x)] = {sum[0] * 0.25f, sum[1] * 0.25f, sum[2] * 0.25f};
            }
        }
        level = std::move(next);
        side = half;
    }
    std::vector<uint32_t> words(kSlotWords);
    for (uint32_t w = 0; w < kSlotWords; ++w) {
        words[w] = uint32_t(texels[w * 2]) | (uint32_t(texels[w * 2 + 1]) << 16);
    }
    return words;
}

std::array<float, 3> texel(const std::vector<uint32_t>& pyramid, int level, int x, int y) {
    const int side = kSide >> level;
    const uint32_t i = levelOffset(level) + uint32_t(y * side + x);
    const uint32_t w = pyramid[i / 2];
    return unpackRgb565(static_cast<uint16_t>((i & 1u) ? (w >> 16) : (w & 0xFFFFu)));
}

}  // namespace rt_detail

uint64_t RtSlotStore::hashOf(const uint32_t* words) const {
    uint64_t h = 1469598103934665603ull;
    for (uint32_t i = 0; i < slotWords_; ++i) {
        h ^= words[i];
        h *= 1099511628211ull;
    }
    return h;
}

uint32_t RtSlotStore::add(const uint32_t* words) {
    const uint64_t h = hashOf(words);
    auto [lo, hi] = byHash_.equal_range(h);
    for (auto it = lo; it != hi; ++it) {
        // Its words while they are here; once uploaded, the 64-bit hash.
        const auto p = pending_.find(it->second);
        if (p == pending_.end() ||
            std::memcmp(p->second.data(), words, size_t(slotWords_) * sizeof(uint32_t)) == 0) {
            return it->second;
        }
    }
    uint32_t s;
    if (!free_.empty()) {
        s = free_.back();
        free_.pop_back();
    } else {
        s = static_cast<uint32_t>(refs_.size());
        refs_.push_back(0);
        hashes_.push_back(0);
        used_.push_back(0);
    }
    refs_[s] = 0;
    hashes_[s] = h;
    used_[s] = 1;
    byHash_.emplace(h, s);
    unreferenced_.push_back(s);
    ++live_;
    auto& w = pending_[s];
    if (w.empty()) pendingWords_ += slotWords_;
    w.assign(words, words + slotWords_);
    return s;
}

void RtSlotStore::acquire(uint32_t s) {
    if (s < refs_.size() && used_[s]) ++refs_[s];
}

void RtSlotStore::freeSlot(uint32_t s) {
    auto [lo, hi] = byHash_.equal_range(hashes_[s]);
    for (auto it = lo; it != hi; ++it) {
        if (it->second == s) {
            byHash_.erase(it);
            break;
        }
    }
    if (auto p = pending_.find(s); p != pending_.end()) {
        pendingWords_ -= slotWords_;
        pending_.erase(p);
    }
    used_[s] = 0;
    free_.push_back(s);
    --live_;
    if (live_ == 0) {
        // Nothing left: the bookkeeping goes too, so a store emptied by the
        // tracing being switched off holds nothing.
        refs_.clear();
        refs_.shrink_to_fit();
        hashes_.clear();
        hashes_.shrink_to_fit();
        used_.clear();
        used_.shrink_to_fit();
        free_.clear();
        free_.shrink_to_fit();
        unreferenced_.clear();
        byHash_.clear();
        pending_.clear();
        pendingWords_ = 0;
    }
}

void RtSlotStore::release(uint32_t s) {
    if (s >= refs_.size() || !used_[s] || refs_[s] == 0) return;
    if (--refs_[s] == 0) freeSlot(s);
}

void RtSlotStore::collect() {
    std::vector<uint32_t> slots;
    slots.swap(unreferenced_);
    for (uint32_t s : slots) {
        if (s < refs_.size() && used_[s] && refs_[s] == 0) freeSlot(s);
    }
}

std::vector<RtSlotStore::Pending> RtSlotStore::takePending() {
    std::vector<Pending> out;
    out.reserve(pending_.size());
    for (auto& [slot, words] : pending_) out.push_back({slot, std::move(words)});
    pending_.clear();
    pendingWords_ = 0;
    return out;
}

}  // namespace wowee::rendering
