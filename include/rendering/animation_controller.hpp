#pragma once

#include <cstdint>
#include <functional>
#include <utility>
#include <string>
#include <vector>
#include <glm/glm.hpp>
#include "rendering/animation/footstep_driver.hpp"
#include "rendering/animation/body_yaw.hpp"
#include "rendering/animation/mount_motion.hpp"

namespace wowee {
namespace rendering {

class Renderer;

// ============================================================================
// AnimationController - the player's swings and emotes, and its mount
//
// The player's own unit is animated as every unit is (the CharacterRenderer's
// unit animation, 0x007385c0 and 0x00724500); this asks it the swings and
// emotes the player does, plays the speed its movement animation runs at,
// and seats it on its mount - whose animations are the unit's too
// (CharacterRenderer::setRiderMount).
//
// Responsibilities:
//   · The swing for the weapons in hand, the special attack, the ranged shot
//   · The player's emotes
//   · The mount's positioning and sounds
//   · Footstep and SFX drivers
// ============================================================================
class AnimationController {
public:
    AnimationController();
    ~AnimationController();

    void initialize(Renderer* renderer);

    // ── Per-frame update hooks (called from Renderer::update) ──────────────
    // The speed the player's movement animation plays at, and its mount.
    void updateCharacterAnimation();
    // Processes animation-driven footstep events (player + mount).
    void updateFootsteps(float deltaTime);
    // Tracks state transitions for activity SFX (jump, landing, swim) and
    // mount ambient sounds.
    // Store per-frame delta time (used inside animation state machine).
    void setDeltaTime(float dt) { lastDeltaTime_ = dt; }

    // ── Emote support ──────────────────────────────────────────────────────
    void playEmote(const std::string& emoteName);
    static std::string getEmoteText(const std::string& emoteName,
                                    const std::string* targetName = nullptr);
    static uint32_t getEmoteDbcId(const std::string& emoteName);
    /// A text emote's chat line (EmoteRegistry::lineByDbcId, 0x00503660).
    static std::string getEmoteLineByDbcId(uint32_t dbcId, bool senderIsMe,
                                           const std::string& senderName,
                                           const std::string& targetName,
                                           const std::string& playerName, bool female);
    static uint32_t getEmoteAnimByEmotesId(uint32_t emoteId);
    /// The Emotes.dbc row a chat speaker gestures with (EmoteRegistry::
    /// chatGestureEmote; 0x00ca1224).
    static uint32_t getChatGestureEmote(int kind);
    /// Emotes.dbc EmoteFlags of a row.
    static uint32_t getEmoteFlagsByEmotesId(uint32_t emoteId);
    /// True if the Emotes.dbc entry is a persistent STATE_ emote (loops until
    /// cleared) rather than a one-shot.
    static bool isStateEmoteById(uint32_t emoteId);

    // ── Combat ─────────────────────────────────────────────────────────────
    void resetCombatVisualState();

    // ── Turning on the spot ──────────────────────────────────────────────
    /// The shuffle the player's body yaw asks for this frame (Renderer,
    /// body_yaw::turnShuffle), played by the locomotion while standing.
    void setTurnShuffle(body_yaw::TurnShuffle shuffle) { turnShuffle_ = shuffle; }
    [[nodiscard]] body_yaw::TurnShuffle turnShuffle() const { return turnShuffle_; }
    /// Mounted, the yaw the mount's body is drawn at (degrees): the unit's
    /// body yaw, lagging the facing and shuffled after it as a standing
    /// unit's is - the mount is the unit's model (0x0073dab0).
    void setMountBodyYaw(float degrees) { mountBodyYawDeg_ = degrees; hasMountBodyYaw_ = true; }

    // ── Stand state ────────────────────────────────────────────────────────
    // WoW UnitStandStateType constants
    static constexpr uint8_t STAND_STATE_STAND      = 0;
    static constexpr uint8_t STAND_STATE_SIT         = 1;
    static constexpr uint8_t STAND_STATE_SIT_CHAIR   = 2;
    static constexpr uint8_t STAND_STATE_SLEEP       = 3;
    static constexpr uint8_t STAND_STATE_SIT_LOW     = 4;
    static constexpr uint8_t STAND_STATE_SIT_MED     = 5;
    static constexpr uint8_t STAND_STATE_SIT_HIGH    = 6;
    static constexpr uint8_t STAND_STATE_DEAD        = 7;
    static constexpr uint8_t STAND_STATE_KNEEL       = 8;
    static constexpr uint8_t STAND_STATE_SUBMERGED   = 9;

    // ── Effect triggers ────────────────────────────────────────────────────
    /// LevelUp.m2 at a position, on the unit drawn as `unitInstanceId` (0 for
    /// none): an effect of that unit's, whose $SND is the level-up sound.
    void triggerLevelUpEffect(const glm::vec3& position, uint32_t unitInstanceId);

    // ── Mount ──────────────────────────────────────────────────────────────
    void setMounted(uint32_t mountInstId, uint32_t mountDisplayId,
                    float heightOffset, const std::string& modelPath = "");
    void setTaxiFlight(bool onTaxi) { taxiFlight_ = onTaxi; }
    void setMountPitchRoll(float pitch, float roll) { mountPitch_ = pitch; mountRoll_ = roll; }
    void clearMount();
    [[nodiscard]] bool isMounted() const { return mountInstanceId_ != 0; }
    [[nodiscard]] uint32_t getMountInstanceId() const { return mountInstanceId_; }

    // ── Query helpers (used by Renderer) ───────────────────────────────────
    [[nodiscard]] float getMountHeightOffset() const { return mountHeightOffset_; }
    [[nodiscard]] bool isTaxiFlight() const { return taxiFlight_; }

private:
    Renderer* renderer_ = nullptr;

    // ── The mount's bank, bob and idle sound ─────────────────────────────
    MountMotion mountMotion_;
    float lastDeltaTime_ = 0.0f;

    // ── Footstep event tracking (delegated to FootstepDriver) ────────────
    FootstepDriver footstepDriver_;

    // ── SFX transition state (delegated to SfxStateDriver) ───────────────

    // ── Mount state (positioning needs renderer) ─────────────────────────
    uint32_t mountInstanceId_ = 0;
    float mountHeightOffset_ = 0.0f;
    float mountPitch_ = 0.0f;
    float mountRoll_ = 0.0f;       // External roll for taxi flights (the bank is MountMotion's)
    int mountSeatAttachmentId_ = -1;
    glm::vec3 smoothedMountSeatPos_ = glm::vec3(0.0f);
    bool mountSeatSmoothingInit_ = false;
    // Last frame's seat target, so the smoothing can tell a rider standing still
    // from one being carried. Pressing no movement keys is not the same as not
    // moving through the world, and on a boat the difference is the whole bug.
    glm::vec3 lastMountSeatTarget_ = glm::vec3(0.0f);
    bool taxiFlight_ = false;
    body_yaw::TurnShuffle turnShuffle_ = body_yaw::TurnShuffle::None;
    float mountBodyYawDeg_ = 0.0f;
    bool hasMountBodyYaw_ = false;

    // ── Private helpers ──────────────────────────────────────────────────
    void updateMount(float deltaTime);
    void applyMountPositioning(float mountBob, float mountRoll, float characterYaw);
};

} // namespace rendering
} // namespace wowee
