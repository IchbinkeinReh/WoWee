#pragma once

#include "rendering/vk_shader.hpp"
#include "rendering/shadow_params.hpp"
#include "rendering/blob_shadow.hpp"

#include "pipeline/m2_loader.hpp"
#include "rendering/m2_track_sampler.hpp"
#include "pipeline/blp_loader.hpp"
#include "pipeline/wmo_doodad_light.hpp"
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <glm/glm.hpp>
#include <memory>
#include <vector>
#include <optional>
#include <functional>
#include <array>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <utility>
#include <future>
#include <deque>
#include <condition_variable>
#include <mutex>
#include <atomic>

namespace wowee {
namespace pipeline { class AssetManager; }
namespace rendering {

// Forward declarations
class Camera;
class Frustum;
class VkContext;
class VkTexture;

// Enchant visual (glint, glow) attached to a weapon at one of the weapon model's
// own item-visual attachment points.
struct WeaponEffectAttachment {
    uint32_t effectModelId;
    uint32_t effectInstanceId;
    glm::vec3 offset;          // attachment position on the weapon model
};

// Weapon attached to a character instance at a bone attachment point
struct WeaponAttachment {
    uint32_t weaponModelId;
    uint32_t weaponInstanceId;
    uint32_t attachmentId;     // 1=RightHand, 2=LeftHand
    uint16_t boneIndex;
    glm::vec3 offset;
    glm::mat4 localTransform{1.0f}; // sheath/hand orientation after attachment point
    float sheathPush = 0.0f;   // smoothed outward push when the arm swings into the blade
    std::vector<WeaponEffectAttachment> effects;
};

/**
 * Character renderer for M2 models with skeletal animation
 *
 * Features:
 * - Skeletal animation with bone transformations
 * - Keyframe interpolation (linear position/scale, slerp rotation)
 * - Vertex skinning (GPU-accelerated via bone SSBO)
 * - Texture loading from BLP via AssetManager
 */
class CharacterRenderer {
public:
    CharacterRenderer();
    ~CharacterRenderer();

    [[nodiscard]] bool initialize(VkContext* ctx, VkDescriptorSetLayout perFrameLayout, pipeline::AssetManager* am,
                    VkRenderPass renderPassOverride = VK_NULL_HANDLE,
                    VkSampleCountFlagBits msaaSamples = VK_SAMPLE_COUNT_1_BIT);
    void shutdown();
    void clear();  // Remove all models/instances/textures but keep pipelines/pools

    void setAssetManager(pipeline::AssetManager* am) { assetManager = am; }

    bool loadModel(const pipeline::M2Model& model, uint32_t id);

    uint32_t createInstance(uint32_t modelId, const glm::vec3& position,
                           const glm::vec3& rotation = glm::vec3(0.0f),
                           float scale = 1.0f);

    // oneShotReturnAnim: animation to resume (looping) when a non-looping
    // animation finishes; 0 = Stand. Lets NPC one-shot emotes return to their
    // persistent work/state loop instead of idling.
    void playAnimation(uint32_t instanceId, uint32_t animationId, bool loop = true,
                       uint32_t oneShotReturnAnim = 0);
    /// The loop a unit rests in - its stand state's (sitting, sleeping,
    /// kneeling...) or 0 for Stand. A one-shot played with no return anim
    /// goes back to it, so a seated unit that is hit stays seated.
    void setRestAnimation(uint32_t instanceId, uint32_t restAnim);
    /// An arm on an animation of its own, as the client plays one on a key
    /// bone and the bones below it (0x00735820 -> 0x00832ab0): 0 the left,
    /// from ShoulderL (key bone 2), 1 the right, from ShoulderR (3). It
    /// plays once at its own rate from its own start, whatever the body
    /// does, and the arm follows the body again when it ends. `eventId`
    /// names the M2 event ("$SHL", "$SHR") reported when the arm's
    /// animation reaches it. False when the model has no such animation
    /// or no such shoulder.
    bool playArmAnimation(uint32_t instanceId, int arm, uint32_t animationId, uint32_t eventId);
    /// Both arms back to the body's animation (0x00832840 on key bones 3
    /// and 2).
    void stopArmAnimations(uint32_t instanceId);
    /// The animation an arm plays on its own; nothing when it follows the
    /// body.
    [[nodiscard]] std::optional<uint32_t> armAnimation(uint32_t instanceId, int arm) const;
    /// Whether the model has the key bone.
    [[nodiscard]] bool hasKeyBone(uint32_t instanceId, int32_t keyBoneId) const;
    /// What the arms' animations reached since the last call: bit 0 the
    /// left arm's event, 1 the right's; bit 2 the left's end, 3 the right's.
    uint8_t takeArmAnimationEvents(uint32_t instanceId);
    /// The model held at an attachment of the instance - a weapon - with
    /// one of its events: where the event is in the world, and the ambient
    /// light the held model is drawn in. Nothing when nothing is held there
    /// or its model has no such event.
    struct HeldModelEvent {
        glm::vec3 position{0.0f};
        glm::vec3 ambient{0.0f};
    };
    [[nodiscard]] std::optional<HeldModelEvent> heldModelEvent(uint32_t instanceId, uint32_t attachmentId,
                                                               uint32_t eventId) const;
    static constexpr uint8_t kArmEventLeft = 1, kArmEventRight = 2, kArmEndLeft = 4, kArmEndRight = 8;

    void update(float deltaTime, const glm::vec3& cameraPos = glm::vec3(0.0f));

    /** Pre-allocate GPU resources (bone SSBOs, descriptors) on main thread before parallel render. */
    void prepareRender(uint32_t frameIndex);
    /// Which batches a render() call draws. Blended batches leave no depth
    /// behind, so whatever is drawn after them paints over them where it is
    /// further away. The client sorts every model of the scene - doodads and
    /// units alike - by its distance and draws the blended ones far to near,
    /// so the world draws Opaque here, and a unit's Blended batches in turn
    /// among the doodads' (see planBlended and M2Renderer::BlendedInterleave).
    enum class Phase { All, Opaque, Blended };
    /// onlyInstance: draw that instance alone (0 = all of them).
    void render(VkCommandBuffer cmd, VkDescriptorSet perFrameSet, const Camera& camera,
                Phase phase = Phase::All, uint32_t onlyInstance = 0);

    /// An instance with blended batches, and its squared distance from the camera.
    struct BlendedDraw {
        float distSq;
        uint32_t instanceId;
    };
    /// The instances render(Phase::Blended) would draw, furthest first.
    std::vector<BlendedDraw> planBlended(const Camera& camera) const;
    /// The blended batches of one instance.
    void renderBlendedInstance(VkCommandBuffer cmd, VkDescriptorSet perFrameSet,
                               const Camera& camera, uint32_t instanceId) {
        render(cmd, perFrameSet, camera, Phase::Blended, instanceId);
    }
    void recreatePipelines();
    /// The five main-pass pipelines, which initialize() and
    /// recreatePipelines() both need and each used to describe.
    void buildMainPassPipelines(VkDevice device, VkRenderPass mainPass,
                                VkSampleCountFlagBits samples,
                                wowee::rendering::VkShaderModule& charVert,
                                wowee::rendering::VkShaderModule& charFrag);
    [[nodiscard]] bool initializeShadow(VkRenderPass shadowRenderPass);
    /// Once a frame, before the first renderShadow: hands back this frame
    /// slot's texture sets and marks a new frame for the bone copies.
    /// renderShadow is then called once per shadow cascade.
    void beginShadowFrame(uint32_t frameIndex);
    void renderShadow(VkCommandBuffer cmd, const glm::mat4& lightSpaceMatrix,
                      const glm::vec3& shadowCenter = glm::vec3(0), float shadowRadius = 1e9f);

    void setInstancePosition(uint32_t instanceId, const glm::vec3& position);
    void setInstanceRotation(uint32_t instanceId, const glm::vec3& rotation);
    void setInstanceTorsoYaw(uint32_t instanceId, float deltaYawRad);
    void moveInstanceTo(uint32_t instanceId, const glm::vec3& destination, float durationSeconds);
    /// How fast the unit drawn by this instance is moving, in yards a second
    /// (zero standing). Its movement animation plays at this over the
    /// sequence's movingSpeed, as the client's does; see
    /// m2_track::locomotionPlaybackRate. Set it before playAnimation, which
    /// also reads it to carry the stride over from one movement sequence to
    /// the next.
    void setLocomotionSpeed(uint32_t instanceId, float yardsPerSecond);
    void startFadeIn(uint32_t instanceId, float durationSeconds);
    void setInstanceOpacity(uint32_t instanceId, float opacity);
    /// A spell kit's alpha on the unit (0x007265c0 case 14), on top of its
    /// own: faded to over `seconds`, or set at once where that is 0 or it is
    /// already there (0x00744030). Its weapons take it too.
    void setInstanceKitAlpha(uint32_t instanceId, float alpha, float seconds);
    /// Stop the instance's animation where it is, or `atMs` into its
    /// sequence (no further than its length), or let it run again
    /// (0x00735bb0, 0x00735dd0: the model's sequences held while its pause
    /// time is set; its global sequences run on). Answers whether it was
    /// stopped already.
    bool setInstanceAnimationFrozen(uint32_t instanceId, bool frozen, std::optional<float> atMs = std::nullopt);
    /// The unit's colour, which its direct light is multiplied by: a spell
    /// kit's (0x007265c0 cases 1 and 13, 0x00720db0); white for none.
    void setInstanceDiffuseColour(uint32_t instanceId, const glm::vec3& colour);
    void setInstanceScale(uint32_t instanceId, float scale);
    [[nodiscard]] const pipeline::M2Model* getModelData(uint32_t modelId) const;
    [[nodiscard]] const pipeline::M2Model* getInstanceModelData(uint32_t instanceId) const;
    void setActiveGeosets(uint32_t instanceId, const std::unordered_set<uint16_t>& geosets);
    /// Opt an instance into the Skin Extra head-detail batch. Only a character
    /// whose type 8 slot has been filled from CharSections should ask for it.
    void setDrawSkinExtra(uint32_t instanceId, bool enabled);

    /// Counts the head-batch diagnostic lines so it stops after a few.
    int headBatchCanaryCount_ = 0;
    void setGroupTextureOverride(uint32_t instanceId, uint16_t geosetGroup, VkTexture* texture);
    void setTextureSlotOverride(uint32_t instanceId, uint16_t textureSlot, VkTexture* texture);
    void clearTextureSlotOverride(uint32_t instanceId, uint16_t textureSlot);
    void setInstanceVisible(uint32_t instanceId, bool visible);

    /// What a unit's floor search finds under its feet (0x007c2a70).
    using FloorQuery = std::function<pipeline::wmo_doodad_light::ObjectFloorState(const glm::vec3&)>;
    /// Whether a point is in a WMO group drawn in the camera's interior pass.
    using InteriorPassQuery = std::function<bool(const glm::vec3&)>;
    /// Asks `floorAt` for every instance that has moved since it was last
    /// asked, and works out what each is drawn with this frame. The client
    /// keeps a unit's floor light as it walks (0x007a1bc0, 0x007c2a70); in an
    /// interior a unit is lit by the floor's vertex colour from a fixed
    /// direction and not by the sun, on a transition face partly by the
    /// zone's light (0x007a0d60), and its ambient eases toward whichever it
    /// stands in (0x007a1e90). Its direct light is halved, easing, while its
    /// feet are in the terrain's baked shadow (0x007a1bc0, 0x007a06a0). In a
    /// group of the camera's interior pass it takes the camera's fog colour
    /// (0x007c1730, `inInteriorPass`). `zoneAmbient` and `zoneDirect` are the
    /// zone's light now, `seconds` the frame's time.
    void refreshInteriorLights(const FloorQuery& floorAt, const InteriorPassQuery& inInteriorPass,
                               const glm::vec3& zoneAmbient, const glm::vec3& zoneDirect, float seconds);
    void removeInstance(uint32_t instanceId);
    bool getAnimationState(uint32_t instanceId, uint32_t& animationId, float& animationTimeMs, float& animationDurationMs) const;
    /// Everything an M2 instance needs to pose its particles as this one is
    /// posed: world matrix, bones, and the sequence and times tracks sample at.
    bool getInstancePose(uint32_t instanceId, glm::mat4& model,
                         const std::vector<glm::mat4>*& bones, int& sequenceIndex,
                         float& animationTimeMs, float& globalTimeMs) const;
    /// Footfall ($FSD) event times in ms for the sequence the instance is
    /// currently playing; nullptr if the model has none for that sequence.
    [[nodiscard]] const std::vector<uint32_t>* getFootstepEventTimes(uint32_t instanceId) const;
    [[nodiscard]] bool hasAnimation(uint32_t instanceId, uint32_t animationId) const;
    bool getAnimationSequences(uint32_t instanceId, std::vector<pipeline::M2Sequence>& out) const;
    bool getInstanceModelName(uint32_t instanceId, std::string& modelName) const;
    bool getInstanceBounds(uint32_t instanceId, glm::vec3& outCenter, float& outRadius) const;

    /// How tall this character stands, from its feet to the top of its head,
    /// with its scale applied. The model origin is at the feet.
    bool getInstanceHeight(uint32_t instanceId, float& outHeight) const;

    /// Where one of the skeleton's named bones sits, in model Z with the
    /// instance's scale applied. The key bone ids are WoW's own: 4 is the
    /// lower spine, 6 the head, 7 the jaw.
    ///
    /// A bone is where a thing actually is. A fraction of the bounding box is
    /// where it usually is, which is not the same for a race whose hair is
    /// half the distance from its chin to the top of the box.
    bool getInstanceKeyBonePivotZ(uint32_t instanceId, int32_t keyBoneId, float& outZ) const;
    bool getInstanceFootZ(uint32_t instanceId, float& outFootZ) const;
    bool getInstancePosition(uint32_t instanceId, glm::vec3& outPos) const;
    /// The instance's world matrix - position, facing and scale - which an
    /// effect parented to the unit rides on.
    bool getInstanceFrame(uint32_t instanceId, glm::mat4& outFrame) const;
    /// Where one of the instance's own model's events is this frame
    /// (0x008318a0): its position through its bone, then the model's
    /// placement. False when the model has no such event.
    bool getEventPosition(uint32_t instanceId, uint32_t eventId, glm::vec3& out) const;
    /// 0x00827780: an event's first time in the sequence an animation plays
    /// on the instance's model (the primary one, as playAnimation picks it),
    /// in ms from its start; 0 where the model has no such sequence or the
    /// event no time in it. Null for no instance.
    [[nodiscard]] std::optional<uint32_t> getAnimationEventTime(uint32_t instanceId, uint32_t animationId,
                                                                uint32_t eventId) const;
    /// Where the instance is drawn is moved by this, on top of its position
    /// (a mount transition carrying its rider, 0x007193f0); zero for none.
    void setInstanceRenderOffset(uint32_t instanceId, const glm::vec3& offset);

    /** Debug: Log all available animations for an instance */
    void dumpAnimations(uint32_t instanceId) const;

    /** Attach a weapon model to a character instance at the given attachment point. */
    bool attachWeapon(uint32_t charInstanceId, uint32_t attachmentId,
                      const pipeline::M2Model& weaponModel, uint32_t weaponModelId,
                      const std::string& texturePath,
                      const glm::mat4& localTransform = glm::mat4(1.0f));

    /** Detach a weapon from the given attachment point (drops its enchant effects too). */
    void detachWeapon(uint32_t charInstanceId, uint32_t attachmentId);

    // Free a model's GPU buffers and map entry once no instance references it.
    // For per-instance model ids (weapons/effects/player composites, which get
    // a fresh id per attach or spawn) - without this every reload or despawn
    // leaked the model. Do NOT call for displayId-keyed NPC models; those are
    // cached across despawn/respawn on purpose. Buffers are destroyed via the
    // frame-fence deferral path; shared textures stay in the cache.
    void unloadModelIfUnused(uint32_t modelId);

    // Model id an instance renders with (0 if the instance doesn't exist).
    [[nodiscard]] uint32_t getInstanceModelId(uint32_t instanceId) const {
        auto it = instances.find(instanceId);
        return it != instances.end() ? it->second.modelId : 0;
    }

    /**
     * Attach an enchant visual to the weapon at the given attachment point.
     * visualSlot is the ItemVisuals.dbc slot (0-4), which selects the attachment
     * point on the weapon model the effect hangs from.
     */
    bool attachWeaponEffect(uint32_t charInstanceId, uint32_t attachmentId, uint32_t visualSlot,
                            const pipeline::M2Model& effectModel, uint32_t effectModelId);

    /** Remove all enchant visuals from the weapon at the given attachment point. */
    void detachWeaponEffects(uint32_t charInstanceId, uint32_t attachmentId);

    /// A unit's blob shadow (0x00793980): the CreatureModelData box it is
    /// sized by, on the instance it is drawn with, or none.
    void setInstanceBlobShadow(uint32_t instanceId, const std::optional<blob_shadow::Box>& box);
    /// The bounds of the sequence an instance is playing (M2Sequence +0x20),
    /// which 0x0082ced0 gives 0x00793980 for an entity that is not a unit.
    [[nodiscard]] std::optional<blob_shadow::Box> instanceSequenceBounds(uint32_t instanceId) const;
    /// Every visible instance with a blob shadow, as this frame draws it.
    void collectBlobShadows(std::vector<blob_shadow::Caster>& out) const;

    /** Mark an instance as a scene backdrop: no culling, no character material heuristics. */
    void setInstanceSceneModel(uint32_t instanceId, bool isScene);


    /** Get the world-space transform of an attachment point on an instance. */
    bool getAttachmentTransform(uint32_t instanceId, uint32_t attachmentId, glm::mat4& outTransform) const;

    [[nodiscard]] size_t getInstanceCount() const { return instances.size(); }

    // Normal mapping / POM settings
    void setNormalMappingEnabled(bool enabled) { normalMappingEnabled_ = enabled; }
    void setNormalMapStrength(float strength) { normalMapStrength_ = strength; }
    void setPOMEnabled(bool enabled) { pomEnabled_ = enabled; }
    void setPOMQuality(int quality) { pomQuality_ = quality; }

    // Fog/lighting/shadow are now in per-frame UBO - keep stubs for callers that haven't been updated
    void setFog(const glm::vec3&, float, float) {}
    void setLighting(const float[3], const float[3], const float[3]) {}
    void setShadowMap(VkTexture*, const glm::mat4&) {}
    void clearShadowMap() {}

    // Pre-decoded BLP cache: set before calling loadModel() to skip main-thread BLP decode
    void setPredecodedBLPCache(std::unordered_map<std::string, pipeline::BLPImage>* cache) { predecodedBLPCache_ = cache; }

private:
    std::unordered_map<std::string, pipeline::BLPImage>* predecodedBLPCache_ = nullptr;
    // GPU representation of M2 model
    struct M2ModelGPU {
        VkBuffer vertexBuffer = VK_NULL_HANDLE;
        VmaAllocation vertexAlloc = VK_NULL_HANDLE;
        VkBuffer indexBuffer = VK_NULL_HANDLE;
        VmaAllocation indexAlloc = VK_NULL_HANDLE;
        uint32_t indexCount = 0;
        uint32_t vertexCount = 0;

        pipeline::M2Model data;  // Original model data
        std::vector<glm::mat4> bindPose;  // Inverse bind pose matrices

        // Tight bind-pose bounds from rendered vertices. M2 header
        // boundingBox/boundingRadius describe collision geometry on many
        // creatures and can cover little more than their feet.
        glm::vec3 visualBoundMin{0.0f};
        glm::vec3 visualBoundMax{0.0f};
        float visualBoundRadius = 0.0f;

        // Textures loaded from BLP (indexed by texture array position)
        std::vector<VkTexture*> textureIds;

        // Cached batch render order sorted by (priorityPlane, materialLayer).
        // Built once at load time - the sort only depends on the model's static
        // batch metadata, so doing it per-instance per-frame in render() was
        // pure overhead.
        std::vector<size_t> sortedBatchIndices;

        // Pre-classified at load time to avoid per-batch string ops in render loop
        bool isKoboldFlame = false;
        bool isSkyBird = false;
    };

    // Character instance
    /// What a draw pushes: character.vert reads the matrix, character.frag
    /// the instance's light. interiorAmbient: the ambient, w = 0 for the
    /// zone's light as the frame has it, 1 for this ambient with the zone's
    /// direct light, 2 for an interior floor's light. interiorDirect: that
    /// floor's direct colour, w = how far its direction has turned toward
    /// the sun's. lightFlags: x 1 in a group of the camera's interior pass
    /// (its fog colour, 0x007c1730), y the scale on the direct light
    /// (0x007a1e90's +0x8c), z the depth in yards within which the model
    /// does not receive the sun's shadow (its own extent: no self-shadow).
    /// diffuseColour: the unit's colour, which its
    /// direct light is multiplied by (CM2Model +0x180, 0x00720db0).
    struct CharPushConstants {
        glm::mat4 model{1.0f};
        glm::vec4 interiorAmbient{0.0f};
        glm::vec4 interiorDirect{0.0f};
        glm::vec4 lightFlags{0.0f, 1.0f, 0.0f, 0.0f};
        glm::vec4 diffuseColour{1.0f};
    };
    static_assert(sizeof(CharPushConstants) == 128, "CharPushConstants must match the shaders");

    struct CharacterInstance {
        uint32_t id;
        uint32_t modelId;

        glm::vec3 position;
        glm::vec3 renderOffset{0.0f};  ///< setInstanceRenderOffset
        glm::vec3 rotation;
        float scale;
        bool visible = true;  // For first-person camera hiding
        float torsoYawOverrideRad = 0.0f;

        // Animation state
        uint32_t currentAnimationId = 0;
        int currentSequenceIndex = -1;  // Index into M2Model::sequences
        int primarySequenceIndex = -1;  // variationIndex 0 of currentAnimationId; head of the variation chain
        int armSequenceIndex[2] = {-1, -1};  // Left, right arm on their own sequences; -1 follows the body
        uint32_t armAnimationId[2] = {0, 0};  // the animation each arm plays, while it has a sequence
        float armTime[2] = {0.0f, 0.0f};      // ms into each arm's sequence
        float armEventTime[2] = {-1.0f, -1.0f};  // when each arm's event comes, -1 for none
        uint8_t armEvents = 0;                 // kArmEvent*/kArmEnd* bits not yet taken
        std::vector<int8_t> boneArm;  // Per bone: 0 left arm, 1 right arm, -1 neither; built by playArmAnimation
        float animationTime = 0.0f;
        bool animationFrozen = false;  ///< setInstanceAnimationFrozen: the sequence held
        float locomotionSpeed = 0.0f;  // yards a second the unit moves at; see setLocomotionSpeed
        float playbackRate = 1.0f;     // the rate animationTime last advanced at
        float globalSequenceTime = 0.0f; // Separate timer for global sequences (accumulates without wrapping at sequence duration)
        bool animationLoop = true;
        /// The sequence the body is blending out of; see m2_track::SequenceBlend.
        /// Timed on globalSequenceTime, which runs whatever the sequence does.
        m2_track::SequenceBlend sequenceBlend;
        uint32_t oneShotReturnAnim = 0; // Anim to resume when a one-shot ends (0 = restAnimation)
        uint32_t restAnimation = 0;     // The stand state's loop, or 0 for Stand
        bool isDead = false;  // Prevents movement while in death state
        std::vector<glm::mat4> boneMatrices;  // Current bone transforms

        // Geoset visibility - which submesh IDs to render
        // Empty = render all (for non-character models)
        std::unordered_set<uint16_t> activeGeosets;
        /// Draw the Skin Extra (texture type 8) head-detail batch. True only
        /// where the instance has been set up to composite it - the player.
        bool drawSkinExtra = false;

        // Per-geoset-group texture overrides (group → VkTexture*)
        std::unordered_map<uint16_t, VkTexture*> groupTextureOverrides;

        // Per-texture-slot overrides (slot → VkTexture*)
        std::unordered_map<uint16_t, VkTexture*> textureSlotOverrides;

        // Weapon attachments (weapons parented to this instance's bones)
        std::vector<WeaponAttachment> weaponAttachments;

        // The unit's colour (setInstanceDiffuseColour).
        glm::vec3 diffuseColour{1.0f};

        // Opacity (for fade-in)
        float opacity = 1.0f;
        /// A spell kit's alpha (setInstanceKitAlpha), multiplying opacity.
        float kitAlpha = 1.0f;
        float kitAlphaFrom = 1.0f;
        float kitAlphaTo = 1.0f;
        float kitAlphaElapsed = 0.0f;
        float kitAlphaSeconds = 0.0f;
        float fadeInTime = 0.0f;     // elapsed fade time (seconds)
        float fadeInDuration = 0.0f; // total fade duration (0 = no fade)

        // Movement interpolation
        bool isMoving = false;
        glm::vec3 moveStart{0.0f};
        glm::vec3 moveEnd{0.0f};
        float moveDuration = 0.0f;   // seconds
        float moveElapsed = 0.0f;

        // Override model matrix (used for weapon instances positioned by parent bone)
        bool hasOverrideModelMatrix = false;
        glm::mat4 overrideModelMatrix{1.0f};

        // Standing on a WMO interior floor: lit by the floor's colour rather
        // than the zone's light (refreshInteriorLights, 0x007a0d60).
        pipeline::wmo_doodad_light::ObjectFloorState floor;
        // Where that was last asked, so a unit standing still is not asked again.
        bool interiorQueried = false;
        glm::vec3 interiorQueryPos{0.0f};
        // The ambient as it eases between floors, 0..255 (0x007a1e90), and
        // what the instance is drawn with this frame (CharPushConstants).
        bool lightKnown = false;
        glm::ivec3 easedAmbient{0};
        glm::vec4 drawAmbient{0.0f};
        glm::vec4 drawDirect{0.0f};
        // The direct light's scale as it eases (0x007a1e90, from 1 at
        // 0x00781a10), and lightFlags as drawn.
        float directScale = 1.0f;
        glm::vec4 drawFlags{0.0f, 1.0f, 0.0f, 0.0f};

        // Enchant visual attached to a weapon. It needs its animation advanced
        // even though its transform comes from the parent; it is drawn by its
        // own materials, as any M2.
        bool isEffectModel = false;

        // A scene rather than a character: the glue-screen backdrops. Two things
        // follow. Their origin can sit hundreds of units from their geometry, so
        // culling on it would drop them. And the material heuristics below exist to
        // rescue character textures - applied to a scene they erase it, because
        // Stormwind's walls are DXT5 with an unused alpha channel that the opaque
        // batches must ignore, exactly as the blend mode says.
        bool isSceneModel = false;

        /// The unit's box its blob shadow is sized by, in this model's space,
        /// when it has one (setInstanceBlobShadow).
        std::optional<blob_shadow::Box> blobShadow;

        const M2ModelGPU* cachedModel = nullptr;  // Avoid per-frame hash lookups

        // Per-instance bone SSBO (double-buffered per frame)
        VkBuffer boneBuffer[2] = {};
        VmaAllocation boneAlloc[2] = {};
        void* boneMapped[2] = {};
        VkDescriptorSet boneSet[2] = {};
        /// The shadow frame (shadowFrameSerial_) that last copied the bones,
        /// so the second and third cascades do not copy them again.
        uint64_t shadowBonesFrame = 0;
    };

    void setupModelBuffers(M2ModelGPU& gpuModel);
    void calculateBindPose(M2ModelGPU& gpuModel);
    void calculateBoneMatrices(CharacterInstance& instance);
    glm::mat4 getBoneTransform(const pipeline::M2Bone& bone, float animTime, float globalSeqTime,
                               int sequenceIndex, const std::vector<uint32_t>& globalSeqDurations,
                               const m2_track::BlendSample& blend);
    [[nodiscard]] glm::mat4 getModelMatrix(const CharacterInstance& instance) const;
    void destroyModelGPU(M2ModelGPU& gpuModel, bool defer = false);
    void destroyInstanceBones(CharacterInstance& inst, bool defer = false);

    // Attachment point lookup helper - shared by attachWeapon() and getAttachmentTransform()
    bool findAttachmentBone(uint32_t modelId, uint32_t attachmentId,
                           uint16_t& outBoneIndex, glm::vec3& outOffset) const;

public:
    /**
     * Build a composited character skin texture by alpha-blending overlay
     * layers onto a base skin BLP. Returns the resulting VkTexture*.
     */
    VkTexture* compositeTextures(const std::vector<std::string>& layerPaths);
    /// Which of the head's two regions a layer goes on, for art whose name
    /// does not say - a scalp or facial hair (0x004e90e0, 0x004e8ff0).
    void setFaceRegionLayer(const std::string& path, bool lower);

    /**
     * Build a composited character skin with explicit region-based equipment overlays.
     */
    VkTexture* compositeWithRegions(const std::string& basePath,
                                const std::vector<std::string>& baseLayers,
                                const std::vector<std::pair<int, std::string>>& regionLayers);

    /** Clear the composite texture cache (forces re-compositing on next call). */
    void clearCompositeCache();
    std::unordered_map<std::string, bool> faceRegionLayers_;  // lowercased path → on FaceLower

    /** Load a BLP texture from MPQ and return VkTexture* (cached). */
    /// texFlags: the M2 texture's wrap flags (bit 0 repeat U, bit 1 repeat V;
    /// clamp otherwise), as the client samples it. Repeat both by default.
    VkTexture* loadTexture(const std::string& path, uint32_t texFlags = 0x3);
    [[nodiscard]] VkTexture* getTransparentTexture() const { return transparentTexture_.get(); }

    /** Replace a loaded model's texture at the given slot. */
    void setModelTexture(uint32_t modelId, uint32_t textureSlot, VkTexture* texture);



private:
    // Create 1×1 fallback textures used when real textures are missing or still loading.
    // Called during both init and clear to ensure valid descriptor bindings at all times.
    void createFallbackTextures(VkDevice device);

    VkContext* vkCtx_ = nullptr;
    VkRenderPass renderPassOverride_ = VK_NULL_HANDLE;
    VkSampleCountFlagBits msaaSamplesOverride_ = VK_SAMPLE_COUNT_1_BIT;
    pipeline::AssetManager* assetManager = nullptr;

    // Vulkan pipelines (one per blend mode)
    VkPipeline opaquePipeline_ = VK_NULL_HANDLE;
    VkPipeline alphaTestPipeline_ = VK_NULL_HANDLE;
    VkPipeline alphaPipeline_ = VK_NULL_HANDLE;
    VkPipeline additivePipeline_ = VK_NULL_HANDLE;
    // The rest of the client's M2 blend states (Gx blend table at 0x00a2f964):
    // M2 blend 3 is NoAlphaAdd (ONE, ONE), 5 Mod (DST_COLOR, ZERO), 6 Mod2x
    // (DST_COLOR, SRC_COLOR). additivePipeline_ is Add (SRC_ALPHA, ONE), blend 4.
    VkPipeline noAlphaAddPipeline_ = VK_NULL_HANDLE;
    VkPipeline modPipeline_ = VK_NULL_HANDLE;
    VkPipeline mod2xPipeline_ = VK_NULL_HANDLE;
    // Variants of each blend pipeline for an M2 material's flags; index is
    // cull (none, back, front) + 3 * no-depth-test + 6 * no-depth-write.
    static constexpr uint32_t kPipelineVariantCount = 12u;
    std::unordered_map<VkPipeline, std::array<VkPipeline, kPipelineVariantCount>> pipelineVariants_;
    /// The client's per-material state on top of a blend pipeline: culled
    /// unless two-sided (0x4), depth test off for 0x8, depth write off for 0x10.
    VkPipeline pipelineVariant(VkPipeline base, uint16_t materialFlags, bool mirrored) const;
    void destroyPipelineVariants();
    // Whole-instance fades (ghost form, spawn fade-in): alpha blend with depth
    // write kept on, so the faded model still self-occludes instead of showing
    // backfaces and under-armor skin through the body.
    VkPipeline translucentPipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;

    // Descriptor set layouts
    VkDescriptorSetLayout perFrameLayout_ = VK_NULL_HANDLE;  // set 0 (owned by Renderer)
    VkDescriptorSetLayout materialSetLayout_ = VK_NULL_HANDLE;  // set 1
    VkDescriptorSetLayout boneSetLayout_ = VK_NULL_HANDLE;  // set 2

    // Descriptor pool
    VkDescriptorPool materialDescPools_[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    struct MaterialDescriptorKey {
        VkImageView diffuse = VK_NULL_HANDLE;
        VkImageView normal = VK_NULL_HANDLE;
        VkSampler diffuseSampler = VK_NULL_HANDLE;
        VkSampler normalSampler = VK_NULL_HANDLE;
        VkImageView second = VK_NULL_HANDLE;
        VkSampler secondSampler = VK_NULL_HANDLE;
        bool operator==(const MaterialDescriptorKey&) const = default;
    };
    struct MaterialDescriptorKeyHash {
        size_t operator()(const MaterialDescriptorKey& key) const {
            const size_t a = std::hash<VkImageView>{}(key.diffuse);
            const size_t b = std::hash<VkImageView>{}(key.normal);
            const size_t c = std::hash<VkSampler>{}(key.diffuseSampler);
            const size_t d = std::hash<VkSampler>{}(key.normalSampler);
            const size_t e = std::hash<VkImageView>{}(key.second);
            const size_t f = std::hash<VkSampler>{}(key.secondSampler);
            return a ^ (b << 1) ^ (c << 2) ^ (d << 3) ^ (e << 4) ^ (f << 5);
        }
    };
    std::unordered_map<MaterialDescriptorKey, VkDescriptorSet, MaterialDescriptorKeyHash>
        materialDescriptorCache_[2];
    VkDescriptorPool boneDescPool_ = VK_NULL_HANDLE;
    std::shared_ptr<std::atomic<uint64_t>> boneDescPoolGeneration_ =
        std::make_shared<std::atomic<uint64_t>>(0);
    uint32_t lastMaterialPoolResetFrame_ = 0xFFFFFFFFu;

    // Material UBO ring buffer - pre-allocated per frame slot, sub-allocated each draw
    VkBuffer materialRingBuffer_[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VmaAllocation materialRingAlloc_[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    void* materialRingMapped_[2] = {nullptr, nullptr};
    uint32_t materialRingOffset_[2] = {0, 0};
    uint32_t materialUboAlignment_ = 256;  // minUniformBufferOffsetAlignment
    static constexpr uint32_t MATERIAL_RING_CAPACITY = 4096;

    // Texture cache
    struct TextureCacheEntry {
        std::unique_ptr<VkTexture> texture;
        std::unique_ptr<VkTexture> normalHeightMap;
        float heightMapVariance = 0.0f;
        size_t approxBytes = 0;
        uint64_t lastUse = 0;
        bool hasAlpha = false;
        bool normalMapPending = false;  // deferred normal map generation
        uint8_t wrapFlags = 0x3;  // M2Texture wrap flags the sampler was made with
    };
    std::unordered_map<std::string, TextureCacheEntry> textureCache;
    struct NormalMapInfo {
        VkTexture* normalMap = nullptr;
        float heightMapVariance = 0.0f;
    };
    std::unordered_map<VkTexture*, NormalMapInfo> normalMapByTexPtr_;
    struct TextureProperties {
        bool hasAlpha = false;
    };
    std::unordered_map<VkTexture*, TextureProperties> texturePropsByPtr_;
    std::unordered_map<std::string, VkTexture*> compositeCache_;  // key → texture for reuse
    std::unordered_set<std::string> failedTextureCache_;  // negative cache for budget exhaustion
    std::unordered_map<std::string, uint64_t> failedTextureRetryAt_;
    std::unordered_set<std::string> loggedTextureLoadFails_;  // dedup warning logs
    uint64_t textureLookupSerial_ = 0;
    size_t textureCacheBytes_ = 0;
    uint64_t textureCacheCounter_ = 0;
    size_t textureCacheBudgetBytes_ = 1024ull * 1024 * 1024;
    uint32_t textureBudgetRejectWarnings_ = 0;
    std::unique_ptr<VkTexture> whiteTexture_;
    std::unique_ptr<VkTexture> transparentTexture_;
    std::unique_ptr<VkTexture> flatNormalTexture_;

    std::unordered_map<uint32_t, M2ModelGPU> models;
    std::unordered_map<uint32_t, CharacterInstance> instances;
    /// The instances a render() call draws, in the order it draws them.
    std::vector<std::pair<uint32_t, CharacterInstance*>> drawOrder_;
    /// render() is called from two recording threads - the characters' own and
    /// the one that draws the doodads and takes the blended characters among
    /// them - and shares the material ring, descriptor cache and drawOrder_.
    /// renderShadow takes it too: the shadow worker runs beside the doodad
    /// worker, and the two passes copy the pose into the same bone buffers.
    std::mutex renderMutex_;
    /// Whether an instance is drawn this frame: visible, in range and view, and
    /// with geometry.
    bool isDrawCandidate(const CharacterInstance& instance, const glm::vec3& camPos,
                         const Frustum& frustum, float renderRadiusSq) const;

    uint32_t nextInstanceId = 1;

    // Normal map generation (same algorithm as WMO renderer)
    std::unique_ptr<VkTexture> generateNormalHeightMap(
        const uint8_t* pixels, uint32_t width, uint32_t height, float& outVariance);

    // Background normal map generation - CPU work on thread pool, GPU upload on main thread
    struct NormalMapResult {
        std::string cacheKey;
        std::vector<uint8_t> pixels;  // RGBA normal map output
        uint32_t width, height;
        float variance;
    };
    // Completed results ready for GPU upload (populated by background threads)
    std::mutex normalMapResultsMutex_;
    std::condition_variable normalMapDoneCV_;  // signaled when pendingNormalMapCount_ reaches 0
    std::deque<NormalMapResult> completedNormalMaps_;
    std::atomic<int> pendingNormalMapCount_{0};  // in-flight background tasks

    // Pure CPU normal map generation (thread-safe, no GPU access)
    /// Start deriving a normal/height map for a texture already in the cache.
    /// Called for every surface this renderer draws, whether it came from a
    /// file or was composited in memory.
    bool queueNormalMapGeneration(const std::string& cacheKey,
                                  std::vector<uint8_t> pixels,
                                  uint32_t width, uint32_t height);

    static NormalMapResult generateNormalHeightMapCPU(
        std::string cacheKey, std::vector<uint8_t> pixels, uint32_t width, uint32_t height);
public:
    void processPendingNormalMaps(int budget = 4);
private:

    // Normal mapping / POM settings
    bool normalMappingEnabled_ = false;  // the client has no normal maps
    float normalMapStrength_ = 0.8f;
    bool pomEnabled_ = false;  // nor parallax
    int pomQuality_ = 1;  // 0=Low(16), 1=Medium(32), 2=High(64)

    // Maximum bones supported
    static constexpr int MAX_BONES = 240;
    uint32_t numAnimThreads_ = 1;
    std::vector<std::future<void>> animFutures_;
    std::vector<std::reference_wrapper<CharacterInstance>> toUpdate_;  // reused across frames

    // Shadow pipeline resources
    VkPipeline shadowPipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout shadowPipelineLayout_ = VK_NULL_HANDLE;
    /// The set the shadow pass binds. Five separate members before,
    /// built and torn down here and in three other renderers.
    ShadowParamsSet shadowParams_;

    /// Which texture a batch draws with. Shared by the main pass and the shadow
    /// pass, which needs it to cut a silhouette rather than a rectangle.
    [[nodiscard]] VkTexture* resolveBatchTexture(const CharacterInstance& inst,
                                   const M2ModelGPU& gm,
                                   const pipeline::M2Batch& b) const;
    /// A stage's texture: the batch's lookup entry `stage` on, with the
    /// instance's slot overrides.
    VkTexture* resolveStageTexture(const CharacterInstance& inst, const M2ModelGPU& gm,
                                   const pipeline::M2Batch& b, uint32_t stage) const;

    /// Per-batch texture sets for alpha-keyed shadow casters, one pool per
    /// frame in flight and reset at the top of each frame's shadow pass. Same
    /// arrangement M2Renderer uses for its foliage shadows.
    static constexpr uint32_t kShadowTexPoolFrames = 2;
    /// Counts beginShadowFrame calls; see CharacterInstance::shadowBonesFrame.
    uint64_t shadowFrameSerial_ = 0;
    VkDescriptorPool shadowTexPool_[kShadowTexPoolFrames] = {};
    std::unordered_map<VkImageView, VkDescriptorSet> shadowTexSetCache_;
    VkDescriptorSet shadowTexDescSet(VkTexture* tex, uint32_t frameIndex);
};

} // namespace rendering
} // namespace wowee
