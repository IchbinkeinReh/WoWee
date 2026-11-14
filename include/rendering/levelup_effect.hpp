#pragma once

#include <glm/glm.hpp>
#include <cstdint>
#include <vector>

namespace wowee {
namespace rendering {

class M2Renderer;

/// Manages spawning the real LevelUp.m2 spell effect at world positions.
/// The M2 model contains particle emitters that produce the golden pillar/ring effect.
class LevelUpEffect {
public:
    LevelUpEffect();
    ~LevelUpEffect();

    /// Load the LevelUp.m2 model (call once after M2Renderer is ready)
    /// @param m2Renderer The M2 renderer to register the model with
    /// @param m2FileData Raw bytes of Spell/LevelUp/LevelUp.m2
    /// @param skinFileData Raw bytes of Spell/LevelUp/LevelUp00.skin
    /// @return true if model loaded successfully
    bool loadModel(M2Renderer* m2Renderer,
                   const std::vector<uint8_t>& m2FileData,
                   const std::vector<uint8_t>& skinFileData);

    /// Trigger the level-up effect at a world position (render coords), on
    /// the unit drawn as `unitInstanceId` (0 for none).
    ///
    /// The client's is "HARDCODED Unit Level Up" (SpellVisualEffectName 21,
    /// Spells\LevelUp\LevelUp.mdl), played on a unit whose level changes
    /// (0x00730050 -> 0x0072e3a0 -> 0x006f9260) as a spell effect: its model's
    /// events go to 0x006f7b00, and its one event is a $SND of SoundEntries
    /// 888, LevelUp.wav - the level-up sound, at the listener at 0.65 of its
    /// volume when the unit is the active player's. Its sequence plays once
    /// (0x00744870 ends the effect when it does), so the sound is heard once.
    void trigger(const glm::vec3& position, uint32_t unitInstanceId);

    /// Remove expired effect instances
    void update(float deltaTime);

    [[nodiscard]] bool isModelLoaded() const { return modelLoaded_; }

private:
    static constexpr float EFFECT_DURATION = 3.5f;
    static constexpr uint32_t MODEL_ID = 999900;  // Unique model ID for level-up effect

    struct ActiveEffect {
        uint32_t instanceId;
        float elapsed;
        bool eventsOwned;
    };

    M2Renderer* m2Renderer_ = nullptr;
    bool modelLoaded_ = false;
    /// The model's first sequence, seconds: its events fire for that pass
    /// alone. The effect stays on screen longer (EFFECT_DURATION), and the
    /// sequence would otherwise come round again and sound a second time.
    float eventsSeconds_ = 0.0f;
    std::vector<ActiveEffect> activeEffects_;
};

} // namespace rendering
} // namespace wowee
