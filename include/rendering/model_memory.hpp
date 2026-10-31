#pragma once

// What a model's CPU-side data takes, for /mem: the animation tracks, bones,
// sequences and the other per-model arrays the renderers keep after upload.
// Estimates from the containers' capacities - enough to rank the owners, not
// to account for every allocation's header.

#include <cstdint>
#include <vector>

#include "pipeline/adt_loader.hpp"
#include "pipeline/m2_loader.hpp"
#include "pipeline/terrain_mesh.hpp"
#include "rendering/memory_report.hpp"

namespace wowee::rendering {

/// One animation track: its per-sequence key arrays, and the sequence records
/// themselves, which cost their own size even when every array in them is
/// empty.
inline uint64_t trackBytes(const pipeline::M2AnimationTrack& t) {
    uint64_t b = vectorBytes(t.sequences);
    for (const auto& s : t.sequences) {
        b += vectorBytes(s.timestamps) + vectorBytes(s.vec3Values) + vectorBytes(s.quatValues) +
             vectorBytes(s.floatValues);
    }
    return b;
}

/// What a track's per-sequence records cost where they hold no keys at all:
/// four empty vectors each.
inline uint64_t emptyKeysBytes(const pipeline::M2AnimationTrack& t) {
    uint64_t b = 0;
    for (const auto& s : t.sequences) {
        if (s.timestamps.empty() && s.vec3Values.empty() && s.quatValues.empty() && s.floatValues.empty()) {
            b += sizeof(s);
        }
    }
    return b;
}

/// A bone list and every track in it.
inline uint64_t bonesBytes(const std::vector<pipeline::M2Bone>& bones) {
    uint64_t b = vectorBytes(bones);
    for (const auto& bone : bones) {
        b += trackBytes(bone.translation) + trackBytes(bone.rotation) + trackBytes(bone.scale);
    }
    return b;
}

/// A list of tracks.
inline uint64_t tracksBytes(const std::vector<pipeline::M2AnimationTrack>& tracks) {
    uint64_t b = vectorBytes(tracks);
    for (const auto& t : tracks) b += trackBytes(t);
    return b;
}

/// Particle emitters, which carry a dozen tracks each.
inline uint64_t emittersBytes(const std::vector<pipeline::M2ParticleEmitter>& emitters) {
    uint64_t b = vectorBytes(emitters);
    for (const auto& e : emitters) {
        b += trackBytes(e.emissionSpeed) + trackBytes(e.speedVariation) + trackBytes(e.verticalRange) +
             trackBytes(e.horizontalRange) + trackBytes(e.gravity) + trackBytes(e.lifespan) +
             trackBytes(e.emissionRate) + trackBytes(e.emissionAreaLength) +
             trackBytes(e.emissionAreaWidth) + trackBytes(e.zSource) + trackBytes(e.enabledTrack) +
             vectorBytes(e.splinePoints);
    }
    return b;
}

/// A whole loaded M2 file as the loader hands it over.
inline uint64_t m2ModelBytes(const pipeline::M2Model& m) {
    uint64_t b = vectorBytes(m.vertices) + vectorBytes(m.indices) + bonesBytes(m.bones) +
                 vectorBytes(m.sequences) + vectorBytes(m.globalSequenceDurations) +
                 vectorBytes(m.events) + vectorBytes(m.boneLookupTable) + vectorBytes(m.batches) +
                 vectorBytes(m.textures) + vectorBytes(m.textureLookup) +
                 vectorBytes(m.textureCoordCombos) + vectorBytes(m.textureCombinerCombos) +
                 vectorBytes(m.materials) + vectorBytes(m.textureTransforms) +
                 vectorBytes(m.textureTransformLookup) + vectorBytes(m.textureWeights) +
                 tracksBytes(m.textureWeightTracks) + vectorBytes(m.textureWeightLookup) +
                 vectorBytes(m.colorAlphas) + vectorBytes(m.colorRGB) + tracksBytes(m.colorAlphaTracks) +
                 tracksBytes(m.colorRGBTracks) + vectorBytes(m.attachments) + vectorBytes(m.cameras) +
                 vectorBytes(m.attachmentLookup) + emittersBytes(m.particleEmitters) +
                 vectorBytes(m.ribbonEmitters) + vectorBytes(m.lights) +
                 vectorBytes(m.collisionVertices) + vectorBytes(m.collisionIndices) +
                 vectorBytes(m.collisionNormals) + vectorBytes(m.footstepEventTimes);
    for (const auto& tt : m.textureTransforms) {
        b += trackBytes(tt.translation) + trackBytes(tt.rotation) + trackBytes(tt.scale);
    }
    for (const auto& r : m.ribbonEmitters) {
        b += trackBytes(r.colorTrack) + trackBytes(r.alphaTrack) + trackBytes(r.heightAboveTrack) +
             trackBytes(r.heightBelowTrack) + trackBytes(r.visibilityTrack) + trackBytes(r.texSlotTrack);
    }
    for (const auto& f : m.footstepEventTimes) b += vectorBytes(f);
    return b;
}

/// A parsed ADT tile: its chunks' fixed arrays and their alpha and shadow maps.
inline uint64_t adtBytes(const pipeline::ADTTerrain& t) {
    uint64_t b = sizeof(t) + vectorBytes(t.textures) + vectorBytes(t.doodadNames) +
                 vectorBytes(t.doodadIds) + vectorBytes(t.wmoNames) + vectorBytes(t.wmoIds) +
                 vectorBytes(t.doodadPlacements) + vectorBytes(t.wmoPlacements);
    for (const auto& c : t.chunks) {
        b += vectorBytes(c.layers) + vectorBytes(c.alphaMap) + vectorBytes(c.shadowMap);
    }
    return b;
}

/// A tile's built mesh: vertices, indices and the per-layer alpha copies.
inline uint64_t terrainMeshBytes(const pipeline::TerrainMesh& m) {
    uint64_t b = sizeof(m) + vectorBytes(m.textures);
    for (const auto& c : m.chunks) {
        b += vectorBytes(c.vertices) + vectorBytes(c.indices) + vectorBytes(c.layers) +
             vectorBytes(c.shadowMap);
        for (const auto& l : c.layers) b += vectorBytes(l.alphaData);
    }
    return b;
}

}  // namespace wowee::rendering
