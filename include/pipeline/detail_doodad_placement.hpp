#pragma once

/// Where the 3.3.5a client puts the ground clutter ("detail doodads") of one
/// terrain chunk: 0x007d3390, run once a chunk comes within groundEffectDist
/// (0x007d3fe0). Everything here is the client's own arithmetic, so a chunk
/// grows the same tufts in the same places as it does in wow.exe.

#include "pipeline/adt_loader.hpp"
#include "pipeline/detail_doodad_shade.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace wowee::pipeline {

/// The client's table-driven random generator (seeded by 0x004c1510, stepped
/// by 0x00464580). Four lagged indices into a 64-entry table at 0x009f1700.
class ClientRandom {
public:
    explicit ClientRandom(uint32_t seed = 0) { reseed(seed); }

    /// 0x004c1510.
    void reseed(uint32_t s) {
        state_ = s;
        lags_ = ((s % 59u) << 10) | ((s % 61u) << 2) | ((s % 53u) << 18) |
                (((s / 47u) * 17u + s) << 26);
    }

    /// 0x00464580.
    uint32_t next() {
        auto at = [](uint32_t byteOffset) { return kTable[(byteOffset >> 2) & 63u]; };
        auto rol = [](uint32_t v, int n) { return (v << n) | (v >> (32 - n)); };
        int i3 = static_cast<int>(lags_ >> 24) - 0x04;
        int i2 = static_cast<int>((lags_ >> 16) & 0xFF) - 0x0C;
        int i1 = static_cast<int>((lags_ >> 8) & 0xFF) - 0x18;
        int i0 = static_cast<int>(lags_ & 0xFF) - 0x1C;
        if (i3 < 0) i3 += 0xBC;
        if (i2 < 0) i2 += 0xD4;
        if (i1 < 0) i1 += 0xEC;
        if (i0 < 0) i0 += 0xF4;
        const uint32_t mix = rol(at(static_cast<uint32_t>(i1)), 3) ^ rol(at(static_cast<uint32_t>(i2)), 2) ^
                             at(static_cast<uint32_t>(i0)) ^ rol(at(static_cast<uint32_t>(i3)), 1);
        lags_ = (static_cast<uint32_t>(i3) << 24) | (static_cast<uint32_t>(i2) << 16) |
                (static_cast<uint32_t>(i1) << 8) | static_cast<uint32_t>(i0);
        state_ += mix;
        return state_;
    }

    /// The signed unit float 0x007d3390 builds from one draw: the mantissa as
    /// a float in [1, 2), then 2 - f for a negative draw and f - 2 otherwise.
    float nextSigned() {
        const uint32_t r = next();
        const uint32_t bits = (r & 0x7FFFFFu) | 0x3F800000u;
        float f;
        static_assert(sizeof(f) == sizeof(bits));
        std::memcpy(&f, &bits, sizeof(f));
        return (r & 0x80000000u) ? 2.0f - f : f - 2.0f;
    }

private:
    static constexpr std::array<uint32_t, 64> kTable = {
        0x9927148Eu, 0x08C7AAFDu, 0x1F3EE6D5u, 0xDA55BBF6u, 0x6A4AA075u, 0xFF97BDE8u,
        0x9FBC9BDEu, 0x46A18A81u, 0x63E30B6Eu, 0x5D6C7A76u, 0xCA69D388u, 0x25B947C3u,
        0x3FA2AB83u, 0xBA7C41A6u, 0x0195ACE5u, 0xC109CF7Eu, 0x717062D9u, 0x0205DB8Du,
        0x54EF8724u, 0x3037D4C6u, 0x7BCB1BD0u, 0xECD8E4B8u, 0xDCADCE49u, 0xC494A913u,
        0x0DAE398Fu, 0x0EDD5218u, 0x85F5FA78u, 0x6DAFD258u, 0x3B53B2A4u, 0xBE50A551u,
        0x11F42DFCu, 0xF1169848u, 0x663DDF86u, 0x2F2E445Eu, 0x176B0736u, 0xB64C298Bu,
        0xE75F89E2u, 0xE121A7CDu, 0xED65C94Du, 0x239CEEFEu, 0x04B77D33u, 0x402A9A9Eu,
        0xF35B10B3u, 0x921C7782u, 0x571E4E20u, 0x8C067222u, 0xFB732C67u, 0xBF0AC259u,
        0x0CF95C79u, 0x68121A28u, 0x42193474u, 0xF884C0B1u, 0x9D15F038u, 0x6F3AF260u,
        0x91EB90B4u, 0x61357F1Du, 0x5603325Au, 0x932BC5A3u, 0x434B0F80u, 0x3CE0A8F7u,
        0x2664D196u, 0x4FCC45D7u, 0xB5E9B0C8u, 0xEA31D600u,
    };
    uint32_t state_ = 0;
    uint32_t lags_ = 0;
};

/// One GroundEffectTexture.dbc row, as 0x007d3390 reads it.
struct GroundEffectRecord {
    std::array<uint32_t, 4> doodadIds{};  ///< +0x04, GroundEffectDoodad ids
    std::array<uint32_t, 4> weights{};    ///< +0x14
    uint32_t density = 0;                 ///< +0x24, tries a cell; 0 reads as 8
};

/// The sixteen-slot doodad wheel 0x007d3390 builds from a record: each doodad
/// written weight times, thirteen slots apart, and any slots the weights leave
/// filled from the doodads in turn.
inline std::array<uint32_t, 16> groundEffectWheel(const GroundEffectRecord& r) {
    std::array<uint32_t, 16> wheel{};
    uint32_t slot = 0;
    uint32_t total = 0;
    for (int i = 0; i < 4; ++i) {
        for (uint32_t w = 0; w < r.weights[static_cast<size_t>(i)]; ++w) {
            wheel[slot & 15u] = r.doodadIds[static_cast<size_t>(i)];
            slot += 13;
        }
        total += r.weights[static_cast<size_t>(i)];
    }
    for (; total < 16; ++total) {
        wheel[slot & 15u] = r.doodadIds[total & 3u];
        slot += 13;
    }
    return wheel;
}

/// One clutter doodad as the client places it.
struct DetailDoodadPlacement {
    uint32_t doodadId = 0;  ///< GroundEffectDoodad id
    float fracX = 0.0f;     ///< quads across the chunk's columns, 0-8
    float fracY = 0.0f;     ///< quads across its rows, 0-8
    float rotation = 0.0f;  ///< radians about the up axis, 0 to 2 pi
    float scale = 1.0f;     ///< 1 give or take a third
};

/// The client's cell picks for groundEffectDensity (16-256, 0x0078dab0):
/// wowee's ground clutter setting is a percentage of which 150 is the 64 that
/// the client's Ground Density slider tops out at (lua_system_api's binding).
inline uint32_t detailCellPicks(float densityScale) {
    if (!(densityScale > 0.0f)) return 0;
    const float picks = densityScale * 100.0f * 64.0f / 150.0f;
    return static_cast<uint32_t>(std::clamp(picks + 0.5f, 16.0f, 256.0f));
}

/// 0x007d3390's placement for one chunk. `globalCol`/`globalRow` are the
/// chunk's index across the whole map (ADT x * 16 + column, ADT y * 16 + row,
/// the chunk's +0x34/+0x38 from 0x007d6b30); `cellPicks` the client's
/// groundEffectDensity; `effectFor(effectId)` the GroundEffectTexture row or
/// null. The doodads come out in the client's order, before the per-texture
/// buffer limits of 0x007b31e0 (applyDetailBatchLimits) are applied.
template <class EffectLookup>
std::vector<DetailDoodadPlacement> placeDetailDoodads(const MapChunk& chunk, uint32_t globalCol,
                                                      uint32_t globalRow, uint32_t cellPicks,
                                                      const EffectLookup& effectFor, float unitSize) {
    std::vector<DetailDoodadPlacement> out;
    if (cellPicks == 0 || !chunk.hasHeightMap() || chunk.layers.empty()) return out;

    ClientRandom rng(0);
    rng.reseed((globalRow << 16) | globalCol);

    // First pass: the cells, two draws each, picked with repeats.
    std::vector<std::array<int, 2>> cells(cellPicks);
    for (uint32_t i = 0; i < cellPicks; ++i) {
        const int cx = static_cast<int>(rng.next() & 7u);
        const int cy = static_cast<int>(rng.next() & 7u);
        cells[i] = {cx, cy};
    }

    for (uint32_t i = 0; i < cellPicks; ++i) {
        const int cx = cells[i][0];
        const int cy = cells[i][1];
        // MCNK's no-effect-doodad bit, its low-resolution hole bit, and the
        // layer its 2-bit map gives the cell.
        if (chunk.isEffectDisabled(cy, cx)) continue;
        if (chunk.isHole(cy, cx)) continue;
        const uint32_t layer = chunk.effectLayerFor(cy, cx);
        if (layer >= chunk.layers.size()) continue;
        const GroundEffectRecord* rec = effectFor(chunk.layers[layer].effectId);
        if (!rec) continue;

        const auto wheel = groundEffectWheel(*rec);
        const uint32_t tries = rec->density ? rec->density : 8u;
        for (uint32_t j = 0; j < tries; ++j) {
            const float rx = rng.nextSigned();
            const float ry = rng.nextSigned();
            const uint32_t doodad = wheel[(j + i) & 15u];
            if (doodad == 0) continue;
            DetailDoodadPlacement p;
            p.doodadId = doodad;
            p.fracX = static_cast<float>(cx) + (rx + 1.0f) * 0.5f;
            p.fracY = static_cast<float>(cy) + (ry + 1.0f) * 0.5f;
            // The cell's triangle under it has to face up by 0.4 or more.
            const DetailDoodadShade s = detailDoodadShade(chunk, p.fracX, p.fracY, unitSize, false);
            if (s.normal.z < 0.4f) continue;
            p.rotation = (rng.nextSigned() + 1.0f) * 3.14159265f;
            p.scale = rng.nextSigned() * 0.33f + 1.0f;
            out.push_back(p);
        }
    }
    return out;
}

/// What 0x007b31e0 needs of a doodad's model to file it in a chunk's buffers.
struct DetailModelSize {
    std::string texture;  ///< the batch key: one texture a buffer
    uint32_t vertices = 0;
    uint32_t indices = 0;
};

/// 0x007b31e0's limits: a chunk has four buffers, one texture each, each
/// holding fewer than cellPicks x 64 vertices (at most 4096, 0x007b2a80) and
/// as many indices. A doodad goes in the first buffer of its texture it fits,
/// else a free one, else nowhere. Answers which placements survive.
template <class SizeLookup>
std::vector<bool> applyDetailBatchLimits(const std::vector<DetailDoodadPlacement>& placements,
                                         uint32_t cellPicks, const SizeLookup& sizeOf) {
    std::vector<bool> keep(placements.size(), false);
    const uint32_t cap = std::min<uint32_t>(cellPicks * 64u, 0x1000u);
    struct Buffer {
        bool used = false;
        std::string texture;
        uint32_t vertices = 0;
        uint32_t indices = 0;
    };
    std::array<Buffer, 4> buffers{};
    for (size_t k = 0; k < placements.size(); ++k) {
        const DetailModelSize* m = sizeOf(placements[k].doodadId);
        if (!m) continue;
        Buffer* into = nullptr;
        for (auto& b : buffers) {
            if (b.used && b.texture == m->texture && b.vertices + m->vertices < cap &&
                b.indices + m->indices < cap) {
                into = &b;
                break;
            }
        }
        if (!into) {
            for (auto& b : buffers) {
                if (!b.used) {
                    b.used = true;
                    b.texture = m->texture;
                    into = &b;
                    break;
                }
            }
        }
        if (!into) continue;
        into->vertices += m->vertices;
        into->indices += m->indices;
        keep[k] = true;
    }
    return keep;
}

}  // namespace wowee::pipeline
