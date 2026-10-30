#pragma once

#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace wowee::rendering {

/// The textures a traced hit is coloured by (water_rt.glsli, rt_trace_detail.glsli):
/// the scene's triangles otherwise carry one colour each - their texture's
/// mean times its tint - which in the ray traced water's reflection read as
/// untextured walls, trees and ground.
///
/// Each texture the scene's meshes use is kept as a small pyramid, 64x64 down
/// to 1x1, as RGB565 two texels a word (5461 texels, 2736 words, about 11 KB),
/// in one storage buffer the trace shaders read and filter themselves - no
/// image descriptors, so the scene's set and every pipeline that traces keep
/// their layout.
namespace rt_detail {

inline constexpr int kSide = 64;
inline constexpr int kLevels = 7;  // 64, 32, 16, 8, 4, 2, 1
inline constexpr uint32_t kTexels = 64 * 64 + 32 * 32 + 16 * 16 + 8 * 8 + 4 * 4 + 2 * 2 + 1;
inline constexpr uint32_t kSlotWords = 2736;  // kTexels / 2 rounded up to a multiple of 16
static_assert(kSlotWords * 2 >= kTexels);

/// Where `level` starts in a pyramid, in texels.
[[nodiscard]] constexpr uint32_t levelOffset(int level) {
    uint32_t at = 0;
    for (int l = 0, side = kSide; l < level; ++l, side /= 2) at += uint32_t(side) * uint32_t(side);
    return at;
}

[[nodiscard]] uint16_t packRgb565(float r, float g, float b);
[[nodiscard]] std::array<float, 3> unpackRgb565(uint16_t c);

/// The pyramid of a 64x64 RGBA8 texture (row-major, R in the low byte),
/// kSlotWords words: each level the mean of four texels of the one above,
/// alpha aside, and the 1x1 level `mean` - the texture's visible mean
/// (VkTexture::averageColor), which the triangle's packed colour carries
/// and the shader divides it out by.
[[nodiscard]] std::vector<uint32_t> buildPyramid(const std::array<uint32_t, kSide * kSide>& rgba,
                                                 const std::array<float, 3>& mean);

/// One texel of a pyramid, as the shader reads it.
[[nodiscard]] std::array<float, 3> texel(const std::vector<uint32_t>& pyramid, int level, int x, int y);

}  // namespace rt_detail

/// Fixed-size slots of 32-bit words, shared by content and counted by the
/// meshes that use them: the detail textures' pyramids, the terrain chunks'
/// layer records. A slot is freed with its last reference, so the store
/// follows the models and tiles loaded rather than growing over a session.
class RtSlotStore {
public:
    static constexpr uint32_t kNone = 0xFFFFFFFFu;

    explicit RtSlotStore(uint32_t slotWords) : slotWords_(slotWords) {}

    /// The slot holding `words` (slotWords of them): one with the same
    /// content if there is one, else a new one. Not referenced until
    /// acquire(); collect() frees one never acquired.
    uint32_t add(const uint32_t* words);
    void acquire(uint32_t slot);
    void release(uint32_t slot);
    /// Free the slots no reference holds: added and never acquired.
    void collect();

    [[nodiscard]] uint32_t slotWords() const { return slotWords_; }
    /// Slots allocated, free ones included: what the GPU copy must hold.
    [[nodiscard]] uint32_t capacity() const { return static_cast<uint32_t>(refs_.size()); }
    [[nodiscard]] uint32_t live() const { return live_; }
    [[nodiscard]] uint32_t refs(uint32_t slot) const { return slot < refs_.size() ? refs_[slot] : 0; }
    [[nodiscard]] uint64_t bytes() const { return uint64_t(words_.size()) * sizeof(uint32_t); }
    [[nodiscard]] const uint32_t* slot(uint32_t s) const { return words_.data() + size_t(s) * slotWords_; }
    [[nodiscard]] const std::vector<uint32_t>& words() const { return words_; }

    /// The slots whose words changed since the last call, each once.
    std::vector<uint32_t> takeDirty();
    void markAllDirty();

private:
    uint64_t hashOf(const uint32_t* words) const;
    void freeSlot(uint32_t s);

    uint32_t slotWords_;
    std::vector<uint32_t> words_;
    std::vector<uint32_t> refs_;
    std::vector<uint64_t> hashes_;
    std::vector<uint8_t> used_;  // holds content, referenced or waiting for collect()
    std::vector<uint32_t> free_;
    std::vector<uint32_t> unreferenced_;  // added since the last collect()
    std::unordered_multimap<uint64_t, uint32_t> byHash_;
    std::vector<uint32_t> dirty_;
    std::vector<uint8_t> isDirty_;
    uint32_t live_ = 0;
};

}  // namespace wowee::rendering
