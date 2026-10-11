// The characters as the ray traced lighting sees them (RtScene): every unit,
// mount and creature in range, and what they carry, cast its shadow and
// occludes the probes' light.
//
// A character is skinned and animated, and the tracers take only rigid
// meshes. Two stand-ins, both built once per appearance and only moved after:
//
//  - The whole model in its Stand pose (rtStandPose), one dynamic instance a
//    character placed by its model matrix. What every character in range
//    gets: a shadow of the right size and place, in a standing pose.
//  - The model cut into bone pieces (rt_rigid_parts.hpp), one instance a
//    piece placed by its bone's matrix this frame. What the nearest
//    characters get - kPosedCount within kPosedRadius - so the shadows the
//    eye is close to walk, swing and cast as the bodies on screen do.
//
// Skinning on the GPU into per-frame meshes and refitting a BLAS per
// character would follow the surface exactly. It would also cost a compute
// pass, a BLAS refit per character per frame, and, for the software tracer,
// a hierarchy refit on the CPU per character per frame; the pieces cost a
// matrix each. In a city of four hundred units the dynamic top level then
// holds some two thousand instances, rebuilt every frame in a few tenths of
// a millisecond.
//
// Weapons, helms, shoulders and enchant models are instances of their own
// here, placed by the matrix their parent's bone gives them, and join their
// parent's group: a body and what it carries never shadow each other.

#include "rendering/character_renderer.hpp"

#include "core/logger.hpp"
#include "rendering/m2_texture_combiner.hpp"
#include "rendering/rt_bvh.hpp"
#include "rendering/rt_lighting.hpp"
#include "rendering/rt_point_lights.hpp"
#include "rendering/rt_rigid_parts.hpp"
#include "rendering/rt_scene.hpp"
#include "rendering/vk_texture.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace wowee {
namespace rendering {

// In character_renderer.cpp: whether a batch is drawn as alpha-cut hair.
bool batchIsHairCard(const pipeline::M2Model& model, const pipeline::M2Batch& batch,
                     uint16_t blendMode, bool isSceneModel);

namespace {

// Units farther than this from the camera do not cast: their shadows would
// fall where the shadow map and the fog already hide them.
constexpr float kCastRadius = 100.0f;
// The most that cast, nearest first, whatever the crowd.
constexpr size_t kMaxCasting = 600;
// The nearest bodies posed piece by piece.
constexpr size_t kPosedCount = 16;
constexpr float kPosedRadius = 40.0f;
// A bone piece under this many triangles goes to its parent's (a finger to
// its hand).
constexpr uint32_t kMinPieceTriangles = 24;
// How far a group's box reaches past its geometry, and how far its floor is
// lifted: the ground the feet stand on must stay outside, or the unit would
// stop shadowing the ground right at its feet.
constexpr float kGroupMargin = 0.1f;
constexpr float kGroupFloorLift = 0.15f;
// How many of the nearest bodies' boxes go to the lighting as units
// (rtUnitBoxes); the rest of RtLighting::kUnitBoxes is for riders.
constexpr size_t kUnitBoxBodies = RtLighting::kUnitBoxes - 4;

RtAabb transformBox(const glm::vec3& bmin, const glm::vec3& bmax, const glm::mat4& m) {
    RtAabb out;
    for (int i = 0; i < 8; ++i) {
        const glm::vec3 c((i & 1) ? bmax.x : bmin.x, (i & 2) ? bmax.y : bmin.y, (i & 4) ? bmax.z : bmin.z);
        out.grow(glm::vec3(m * glm::vec4(c, 1.0f)));
    }
    return out;
}

uint64_t mixHash(uint64_t h, uint64_t v) {
    // FNV-1a over the words; the key only has to tell appearances apart.
    for (int i = 0; i < 8; ++i) {
        h ^= (v >> (i * 8)) & 0xFF;
        h *= 1099511628211ull;
    }
    return h;
}

}  // namespace

const std::vector<glm::mat4>& CharacterRenderer::rtStandPose(M2ModelGPU& gm) {
    if (gm.rtStandPoseDone) return gm.rtStandPose;
    gm.rtStandPoseDone = true;
    const auto& model = gm.data;
    gm.rtStandPose.assign(model.bones.size(), glm::mat4(1.0f));
    // Stand (animation 0), its first variation; with none, the bind pose.
    int stand = -1;
    for (size_t i = 0; i < model.sequences.size(); ++i) {
        if (model.sequences[i].id == 0 && model.sequences[i].variationIndex == 0) {
            stand = static_cast<int>(i);
            break;
        }
    }
    if (stand < 0) return gm.rtStandPose;
    for (size_t i = 0; i < model.bones.size(); ++i) {
        const auto& bone = model.bones[i];
        const glm::mat4 local =
            getBoneTransform(bone, 0.0f, 0.0f, stand, model.globalSequenceDurations, m2_track::BlendSample{});
        if (bone.parentBone >= 0 && static_cast<size_t>(bone.parentBone) < i) {
            gm.rtStandPose[i] = gm.rtStandPose[bone.parentBone] * local;
        } else {
            gm.rtStandPose[i] = local;
        }
    }
    return gm.rtStandPose;
}

uint64_t CharacterRenderer::rtShapeKey(const CharacterInstance& inst, const M2ModelGPU& gm,
                                       std::vector<std::pair<uint32_t, uint32_t>>& batches) {
    // The batches the shadow pass casts (renderShadow), less the per-frame
    // fades: the same geoset filter and its fallback, nothing blended or
    // without a shader, and hair and alpha keys cut by their texture.
    batches.clear();
    const auto& data = gm.data;
    bool applyGeosetFilter = !inst.activeGeosets.empty();
    if (applyGeosetFilter) {
        applyGeosetFilter = std::any_of(data.batches.begin(), data.batches.end(), [&](const pipeline::M2Batch& b) {
            return inst.activeGeosets.count(b.submeshId) != 0;
        });
    }
    uint64_t key = mixHash(1469598103934665603ull, inst.modelId);
    for (uint32_t bi = 0; bi < data.batches.size(); ++bi) {
        const auto& batch = data.batches[bi];
        const uint16_t blendMode =
            batch.materialIndex < data.materials.size() ? data.materials[batch.materialIndex].blendMode : 0;
        if (blendMode >= 2) continue;
        if (applyGeosetFilter && inst.activeGeosets.find(batch.submeshId) == inst.activeGeosets.end()) continue;
        if (!applyGeosetFilter) {
            const uint16_t grp = batch.submeshId / 100;
            if (grp == 17 || grp == 18 || grp == 15) continue;
        }
        const uint16_t shaderId = m2BatchShaderId(batch.shader, blendMode, batch.textureCount, batch.textureUnit,
                                                  data.globalFlags, data.textureCoordCombos,
                                                  data.textureCombinerCombos);
        const uint16_t firstCoord =
            batch.textureUnit < data.textureCoordCombos.size() ? data.textureCoordCombos[batch.textureUnit] : 0;
        const M2BatchCombiner combiner = m2ResolveCombiner(shaderId, batch.textureCount, firstCoord);
        if (!combiner.drawn) continue;

        uint32_t mask = RtScene::kInvalid;
        if (blendMode == 1 || batchIsHairCard(data, batch, blendMode, inst.isSceneModel)) {
            VkTexture* tex = nullptr;
            auto groupTexIt = inst.groupTextureOverrides.find(static_cast<uint16_t>(batch.submeshId / 100));
            if (groupTexIt != inst.groupTextureOverrides.end() && groupTexIt->second != nullptr) {
                tex = groupTexIt->second;
            } else if (combiner.stages > 1) {
                tex = resolveStageTexture(inst, gm, batch, 0);
            } else {
                tex = resolveBatchTexture(inst, gm, batch);
            }
            if (tex && tex != whiteTexture_.get() && tex->alphaMask() && tex->alphaCoverage() < 1.0f) {
                mask = rtScene_->addAlphaMask(*tex->alphaMask());
            }
        }
        batches.emplace_back(bi, mask);
        key = mixHash(key, (uint64_t(bi) << 32) | mask);
    }
    return key;
}

void CharacterRenderer::rtAcquireShape(uint64_t key, const CharacterInstance& inst, const M2ModelGPU& gm,
                                       const std::vector<std::pair<uint32_t, uint32_t>>& batches) {
    RtShape& shape = rtShapes_[key];
    ++shape.refs;
    if (shape.refs > 1) return;
    shape.modelId = inst.modelId;
    const auto& data = gm.data;

    // The triangles and what each is made of, kept for the pieces.
    for (const auto& [bi, mask] : batches) {
        const auto& batch = data.batches[bi];
        VkTexture* tex = resolveBatchTexture(inst, gm, batch);
        glm::vec3 albedo(0.5f);
        if (tex && tex != whiteTexture_.get()) {
            const glm::vec3 avg = tex->averageColor();
            // A composited skin is not a BLP and reports white; that is no
            // colour to bounce.
            if (avg != glm::vec3(1.0f)) albedo = avg;
        }
        const float opacity = mask != RtScene::kInvalid ? 0.5f : 1.0f;
        const float surface = packRtSurface(albedo, opacity);
        const uint16_t firstCoord =
            batch.textureUnit < data.textureCoordCombos.size() ? data.textureCoordCombos[batch.textureUnit] : 0;
        const size_t end = std::min<size_t>(size_t(batch.indexStart) + batch.indexCount, data.indices.size());
        for (size_t i = batch.indexStart; i + 2 < end; i += 3) {
            const uint32_t a = data.indices[i], b = data.indices[i + 1], c = data.indices[i + 2];
            if (a >= data.vertices.size() || b >= data.vertices.size() || c >= data.vertices.size()) continue;
            shape.triangles.insert(shape.triangles.end(), {a, b, c});
            shape.surfaces.push_back(surface);
            shape.masks.push_back(mask);
            shape.uvSet.push_back(firstCoord == 1 ? 1 : 0);
        }
    }
    if (shape.triangles.empty()) return;

    // The whole model in its Stand pose. No normals: a character has no
    // inside for the probes to find themselves in.
    M2ModelGPU& gmMutable = models[inst.modelId];
    const auto& pose = rtStandPose(gmMutable);
    RtScene::MeshSource src;
    src.positions.reserve(data.vertices.size());
    src.uvs.reserve(data.vertices.size());
    for (const auto& v : data.vertices) {
        glm::vec4 p(0.0f);
        float wsum = 0.0f;
        for (int k = 0; k < 4; ++k) {
            const float w = v.boneWeights[k] / 255.0f;
            if (w <= 0.0f || v.boneIndices[k] >= pose.size()) continue;
            p += (pose[v.boneIndices[k]] * glm::vec4(v.position, 1.0f)) * w;
            wsum += w;
        }
        src.positions.push_back(wsum > 0.0f ? glm::vec3(p) / wsum : v.position);
        src.uvs.push_back(v.texCoords[0]);
    }
    src.indices = shape.triangles;
    src.surfaces = shape.surfaces;
    src.masks = shape.masks;
    RtAabb box;
    for (uint32_t idx : src.indices) box.grow(src.positions[idx]);
    shape.wholeMin = box.bmin;
    shape.wholeMax = box.bmax;
    shape.wholeMesh = rtScene_->addMesh(std::move(src));
}

void CharacterRenderer::rtBuildPieces(RtShape& shape, const M2ModelGPU& gm) {
    shape.partsTried = true;
    const auto& data = gm.data;
    if (data.bones.size() < 2 || shape.triangles.empty()) return;
    std::vector<std::array<uint8_t, 4>> idx(data.vertices.size()), wgt(data.vertices.size());
    for (size_t i = 0; i < data.vertices.size(); ++i) {
        for (int k = 0; k < 4; ++k) {
            idx[i][k] = data.vertices[i].boneIndices[k];
            wgt[i][k] = data.vertices[i].boneWeights[k];
        }
    }
    std::vector<int16_t> parents(data.bones.size());
    for (size_t b = 0; b < data.bones.size(); ++b) parents[b] = data.bones[b].parentBone;
    const auto parts = rt_parts::splitByBone(idx, wgt, shape.triangles, parents, kMinPieceTriangles);

    // Each piece in the bind pose, its vertices its own.
    std::vector<uint32_t> remap(data.vertices.size(), RtScene::kInvalid);
    for (const auto& part : parts) {
        RtScene::MeshSource src;
        RtAabb box;
        for (uint32_t t : part.triangles) {
            for (int c = 0; c < 3; ++c) {
                const uint32_t v = shape.triangles[t * 3 + c];
                if (remap[v] == RtScene::kInvalid) {
                    remap[v] = static_cast<uint32_t>(src.positions.size());
                    src.positions.push_back(data.vertices[v].position);
                    src.uvs.push_back(data.vertices[v].texCoords[shape.uvSet[t]]);
                    box.grow(data.vertices[v].position);
                }
                src.indices.push_back(remap[v]);
            }
            src.surfaces.push_back(shape.surfaces[t]);
            src.masks.push_back(shape.masks[t]);
        }
        for (uint32_t t : part.triangles) {
            for (int c = 0; c < 3; ++c) remap[shape.triangles[t * 3 + c]] = RtScene::kInvalid;
        }
        RtShape::Piece piece;
        piece.bone = part.bone;
        piece.bmin = box.bmin;
        piece.bmax = box.bmax;
        piece.mesh = rtScene_->addMesh(std::move(src));
        if (piece.mesh != RtScene::kInvalid) shape.pieces.push_back(piece);
    }
    // The sources are copied into the scene; the shape keeps no more of them.
    shape.triangles.clear();
    shape.triangles.shrink_to_fit();
    shape.surfaces.clear();
    shape.surfaces.shrink_to_fit();
    shape.masks.clear();
    shape.masks.shrink_to_fit();
    shape.uvSet.clear();
    shape.uvSet.shrink_to_fit();
}

void CharacterRenderer::rtReleaseShape(uint64_t key) {
    auto it = rtShapes_.find(key);
    if (it == rtShapes_.end()) return;
    RtShape& shape = it->second;
    if (shape.refs > 1) {
        --shape.refs;
        return;
    }
    // Its instances are gone already (rtDropInstances runs first).
    if (shape.wholeMesh != RtScene::kInvalid) rtScene_->removeMesh(shape.wholeMesh);
    for (const auto& piece : shape.pieces) rtScene_->removeMesh(piece.mesh);
    rtShapes_.erase(it);
}

void CharacterRenderer::rtDropInstances(RtCharacter& rc) {
    if (rc.whole != RtScene::kInvalid) rtScene_->removeInstance(rc.whole);
    rc.whole = RtScene::kInvalid;
    for (uint32_t id : rc.pieces) {
        if (id != RtScene::kInvalid) rtScene_->removeInstance(id);
    }
    rc.pieces.clear();
}

void CharacterRenderer::rtForget(RtCharacter& rc) {
    rtDropInstances(rc);
    if (rc.hasShape) rtReleaseShape(rc.shapeKey);
    rc.hasShape = false;
    if (rc.group != 0) rtScene_->removeGroup(rc.group);
    rc.group = 0;
}

glm::vec3 CharacterRenderer::rtPlacedAt(const CharacterInstance& inst) {
    return inst.hasOverrideModelMatrix ? glm::vec3(inst.overrideModelMatrix[3]) : inst.position + inst.renderOffset;
}

// The surfaces read the lighting traced a frame earlier, found by projecting
// their position with that frame's camera. That follows the camera alone: a
// unit that walked on since stands somewhere else than it did in the result,
// finds the ground behind it there or nothing at all, and is drawn in other
// light - the player most of all, whom the camera follows, so that every
// step lands the body away from where the result had it. Moved back by how
// far it went, it finds itself. Its pose moves too - a limb, a turn - but by
// far less.
void CharacterRenderer::rtMotionPush(const CharacterInstance& inst, CharPushConstants& push) const {
    // Only from the latest trace: the opaque surfaces draw before this
    // frame's (RtLighting::record), so it is the one they read. What draws
    // after it in the frame reads the same result but is placed as now.
    if (inst.rtTracedGeneration == 0 || inst.rtTracedGeneration != rtSyncGeneration_) return;
    const glm::vec3 d = rtPlacedAt(inst) - inst.rtTracedAt;
    // Packed as rtUnitMotion unpacks it; farther than two yards in a frame
    // is a teleport, or a frame long enough that nothing would match anyway.
    constexpr float kStep = 1024.0f;
    const glm::vec2 q = glm::round(glm::vec2(d) * kStep) + 2048.0f;
    if (q.x < 0.0f || q.y < 0.0f || q.x > 4095.0f || q.y > 4095.0f || std::abs(d.z) > 2.0f) return;
    if (glm::dot(d, d) < 1e-8f) return;
    const uint32_t packed = static_cast<uint32_t>(q.x) | (static_cast<uint32_t>(q.y) << 12);
    push.lightFlags.w = static_cast<float>(packed + 1u);
    push.diffuseColour.w = d.z;
}

void CharacterRenderer::syncRtScene(const glm::vec3& cameraPos) {
    rtUnitBoxes_.clear();
    if (!rtScene_ || !rtScene_->isActive()) return;
    const uint64_t gen = ++rtSyncGeneration_;
    rtStats_ = RtStats{};
    // Where everything stands as this frame's lighting is traced (rtMotionPush).
    for (auto& [id, inst] : instances) {
        inst.rtTracedAt = rtPlacedAt(inst);
        inst.rtTracedGeneration = gen;
    }
    // The nearest bodies' boxes for rtUnitBoxes, by their group.
    std::unordered_map<uint32_t, size_t> unitBoxOf;

    // Who casts: what the shadow pass would draw, in range, nearest first.
    struct Candidate {
        float distSq;
        uint32_t id;
        CharacterInstance* inst;
    };
    std::vector<Candidate> bodies, carried;
    std::unordered_map<uint32_t, uint32_t> parentOf;
    for (auto& [id, inst] : instances) {
        if (!inst.visible || inst.isSceneModel || inst.isEffectModel) continue;
        if (!inst.posedOnce && !inst.hasOverrideModelMatrix) continue;
        if (inst.opacity * inst.kitAlpha <= 0.01f) continue;
        if (!inst.cachedModel || !inst.cachedModel->vertexBuffer) continue;
        const glm::vec3 at = inst.hasOverrideModelMatrix ? glm::vec3(inst.overrideModelMatrix[3]) : inst.position;
        const glm::vec3 d = at - cameraPos;
        const float distSq = glm::dot(d, d);
        if (distSq > kCastRadius * kCastRadius) continue;
        (inst.hasOverrideModelMatrix ? carried : bodies).push_back({distSq, id, &inst});
        for (const auto& wa : inst.weaponAttachments) {
            parentOf[wa.weaponInstanceId] = id;
            for (const auto& fx : wa.effects) parentOf[fx.effectInstanceId] = id;
        }
    }
    auto nearer = [](const Candidate& a, const Candidate& b) {
        return a.distSq != b.distSq ? a.distSq < b.distSq : a.id < b.id;
    };
    if (bodies.size() > kMaxCasting) {
        std::nth_element(bodies.begin(), bodies.begin() + kMaxCasting, bodies.end(), nearer);
        bodies.resize(kMaxCasting);
    }
    std::sort(bodies.begin(), bodies.end(), nearer);

    std::vector<std::pair<uint32_t, uint32_t>> batches;
    auto place = [&](const Candidate& c, bool body, size_t rank) {
        CharacterInstance& inst = *c.inst;
        RtCharacter& rc = rtCharacters_[c.id];
        rc.seen = gen;
        const M2ModelGPU& gm = *inst.cachedModel;

        if (!rc.hasShape || rc.appearanceSerial != inst.rtAppearanceSerial) {
            const uint64_t key = rtShapeKey(inst, gm, batches);
            rc.appearanceSerial = inst.rtAppearanceSerial;
            if (!rc.hasShape || key != rc.shapeKey) {
                rtDropInstances(rc);
                if (rc.hasShape) rtReleaseShape(rc.shapeKey);
                rtAcquireShape(key, inst, gm, batches);
                rc.shapeKey = key;
                rc.hasShape = true;
            }
        }
        RtShape& shape = rtShapes_[rc.shapeKey];
        if (shape.wholeMesh == RtScene::kInvalid) return;

        const bool wantPosed = body && rank < kPosedCount && c.distSq < kPosedRadius * kPosedRadius &&
                               !inst.boneMatrices.empty();
        if (wantPosed && !shape.partsTried) rtBuildPieces(shape, gm);
        // Posed only once every piece is traced; until then the whole model.
        bool posed = wantPosed && !shape.pieces.empty();
        if (posed) {
            for (const auto& piece : shape.pieces) {
                if (!rtScene_->meshReady(piece.mesh)) {
                    posed = false;
                    break;
                }
            }
        }
        if (posed != rc.posed) {
            rtDropInstances(rc);
            rc.posed = posed;
        }

        // The group: a body's own, or the one its carrier has.
        uint32_t group = 0;
        if (body) {
            if (rc.group == 0) rc.group = rtScene_->addGroup();
            group = rc.group;
        } else if (auto p = parentOf.find(c.id); p != parentOf.end()) {
            if (auto pc = rtCharacters_.find(p->second); pc != rtCharacters_.end()) group = pc->second.group;
        }

        const glm::mat4 modelMat = inst.hasOverrideModelMatrix ? inst.overrideModelMatrix : getModelMatrix(inst);
        RtAabb box;
        if (!posed) {
            if (rc.whole == RtScene::kInvalid) {
                rc.whole = rtScene_->addDynamicInstance(shape.wholeMesh, modelMat, group);
            } else {
                rtScene_->setInstanceTransform(rc.whole, modelMat);
            }
            box = transformBox(shape.wholeMin, shape.wholeMax, modelMat);
            ++rtStats_.instances;
        } else {
            rc.pieces.resize(shape.pieces.size(), RtScene::kInvalid);
            for (size_t i = 0; i < shape.pieces.size(); ++i) {
                const auto& piece = shape.pieces[i];
                const glm::mat4 m = piece.bone < inst.boneMatrices.size() ? modelMat * inst.boneMatrices[piece.bone]
                                                                          : modelMat;
                if (rc.pieces[i] == RtScene::kInvalid) {
                    rc.pieces[i] = rtScene_->addDynamicInstance(piece.mesh, m, group);
                } else {
                    rtScene_->setInstanceTransform(rc.pieces[i], m);
                }
                box.grow(transformBox(piece.bmin, piece.bmax, m));
            }
            rtStats_.instances += static_cast<uint32_t>(shape.pieces.size());
            ++rtStats_.posed;
        }
        if (body && rc.group != 0 && box.valid()) {
            glm::vec3 lo = box.bmin - glm::vec3(kGroupMargin);
            const glm::vec3 hi = box.bmax + glm::vec3(kGroupMargin);
            lo.z = std::min(box.bmin.z + kGroupFloorLift, hi.z);
            rtScene_->setGroupBounds(rc.group, lo, hi);
            if (rank < kUnitBoxBodies) {
                unitBoxOf[rc.group] = rtUnitBoxes_.size();
                rtUnitBoxes_.emplace_back(lo, hi);
            }
        } else if (!body && box.valid()) {
            auto ub = group != 0 ? unitBoxOf.find(group) : unitBoxOf.end();
            if (ub != unitBoxOf.end()) {
                // A staff, a shield, a weapon on the back: part of the unit.
                // Not its floor, which stays clear of the ground it stands on.
                auto& [lo, hi] = rtUnitBoxes_[ub->second];
                lo = glm::vec3(glm::min(glm::vec2(lo), glm::vec2(box.bmin) - kGroupMargin), lo.z);
                hi = glm::max(hi, box.bmax + glm::vec3(kGroupMargin));
            } else if (group == 0 && rtUnitBoxes_.size() < RtLighting::kUnitBoxes &&
                       c.distSq < kPosedRadius * kPosedRadius) {
                // One of its own that is hung on another - a rider in its seat.
                glm::vec3 lo = box.bmin - glm::vec3(kGroupMargin);
                const glm::vec3 hi = box.bmax + glm::vec3(kGroupMargin);
                lo.z = std::min(box.bmin.z + kGroupFloorLift, hi.z);
                rtUnitBoxes_.emplace_back(lo, hi);
            }
        }
        ++rtStats_.characters;
    };
    for (size_t i = 0; i < bodies.size(); ++i) place(bodies[i], true, i);
    for (const Candidate& c : carried) {
        // What a unit carries casts with it, or not at all.
        auto p = parentOf.find(c.id);
        if (p != parentOf.end() && rtCharacters_.count(p->second) &&
            rtCharacters_[p->second].seen != gen) {
            continue;
        }
        place(c, false, 0);
    }

    for (auto it = rtCharacters_.begin(); it != rtCharacters_.end();) {
        if (it->second.seen != gen) {
            rtForget(it->second);
            it = rtCharacters_.erase(it);
        } else {
            ++it;
        }
    }
    rtStats_.shapes = static_cast<uint32_t>(rtShapes_.size());
}

void CharacterRenderer::collectRtLights(const glm::vec3& eye, float range,
                                        std::vector<rt_lights::PointLight>& out) const {
    for (const auto& [id, inst] : instances) {
        if (!inst.cachedModel || inst.cachedModel->data.lights.empty()) continue;
        if (!inst.visible || inst.isSceneModel) continue;
        if (!inst.posedOnce && !inst.hasOverrideModelMatrix) continue;
        const glm::mat4 modelMat = inst.hasOverrideModelMatrix ? inst.overrideModelMatrix : getModelMatrix(inst);
        const glm::vec3 at(modelMat[3]);
        const float reach = range + inst.cachedModel->visualBoundRadius * std::max(inst.scale, 1.0f) + 30.0f;
        if (glm::dot(at - eye, at - eye) > reach * reach) continue;
        const auto& data = inst.cachedModel->data;
        const float fade = std::clamp(inst.opacity * inst.kitAlpha, 0.0f, 1.0f);
        for (const auto& light : data.lights) {
            auto l = rt_lights::evaluateM2Light(light, inst.currentSequenceIndex, inst.animationTime,
                                                inst.globalSequenceTime, data.globalSequenceDurations, modelMat,
                                                inst.boneMatrices);
            if (!l) continue;
            l->color *= fade;
            l->source = rt_lights::Source::Unit;
            out.push_back(*l);
        }
    }
}

}  // namespace rendering
}  // namespace wowee
