#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <glm/glm.hpp>

#include "rendering/spell_missile.hpp"

namespace wowee {
namespace pipeline { class AssetManager; }
namespace rendering {

class M2Renderer;
class Renderer;
class CharacterRenderer;

class SpellVisualSystem {
public:
    SpellVisualSystem() = default;
    ~SpellVisualSystem() = default;

    // Initialize with references to the M2 renderer and parent renderer
    void initialize(M2Renderer* m2Renderer, Renderer* renderer);
    void shutdown();

    // Spawn a spell visual at a world position.
    // useImpactKit=false → CastKit path; useImpactKit=true → ImpactKit path
    // attachInstanceId: the CASTER's CharacterRenderer instance for hand/chest/
    // head bone tracking (0 = static effect at worldPosition). Effects used to
    // attach to the local player unconditionally, so every nearby unit's cast
    // kit landed on the player's hands.
    void playSpellVisual(uint32_t visualId, const glm::vec3& worldPosition,
                         bool useImpactKit = false, uint32_t attachInstanceId = 0);

    // Spawn a precast visual effect at a world position.
    // castTimeMs: server cast time in milliseconds (0 = use anim duration).
    // attachInstanceId: see playSpellVisual.
    void playSpellVisualPrecast(uint32_t visualId, const glm::vec3& worldPosition,
                                uint32_t castTimeMs = 0, uint32_t attachInstanceId = 0);

    // Launch a physical weapon projectile (arrow, bullet, or thrown item)
    // without invoking the spell visual pipeline.
    void playPhysicalProjectile(const std::string& modelPath,
                                const std::string& texturePath,
                                const glm::vec3& start,
                                const glm::vec3& end,
                                float duration,
                                bool spin);

    /// The missile half of a SpellVisual.dbc row - see spell_missile.hpp for
    /// how the client flies it.
    struct MissileVisual {
        std::string modelPath;               ///< SpellVisualEffectName[MissileModel]
        float scale = 1.0f;                  ///< SpellVisualEffectName Scale
        uint32_t flags = 0;                  ///< SpellVisual Flags
        int32_t sourceAttachment = -1;       ///< M2 attachment id; -1 = the default
        int32_t destinationAttachment = -1;  ///< M2 attachment id; -1 = the fallbacks
        glm::vec3 castOffset{0.0f};          ///< in the source attachment's frame
        glm::vec3 impactOffset{0.0f};        ///< in the destination attachment's frame
        uint32_t soundId = 0;                ///< SpellVisual MissileSound (SoundEntries), 0 = none
    };

    /// The visual's missile, or null when it flies none (no MissileModel, or
    /// one naming a weapon or ammunition, which the client takes from the
    /// caster's items rather than from the visual).
    const MissileVisual* findMissileVisual(uint32_t visualId);

    /// One end of a missile's flight. A unit end names its CharacterRenderer
    /// instance so the missile can leave from and home on its attachments;
    /// `position` is where it stands, used when it has no instance and for a
    /// point on the ground (renderInstanceId 0).
    struct MissileEnd {
        uint32_t renderInstanceId = 0;
        glm::vec3 position{0.0f};
    };

    /// What an ADJUST_MISSILE cast (flag 0x20000) hands its missile: the
    /// elevation and flight time from SMSG_SPELL_GO and the spell's
    /// SpellMissile row, which put it on an arc (0x00700880).
    struct MissileTrajectory {
        float elevation = 0.0f;
        float flightSeconds = 0.0f;
        uint32_t spellMissileId = 0;
    };

    /// Launch the visual's missile from `from` to `to` at `speed` yards per
    /// second. On arrival the missile is removed and the visual's impact kit
    /// plays on each of `impacts` - the client plays them then, not at
    /// SMSG_SPELL_GO (FUN_00700e20). False when nothing was launched (no
    /// missile, no speed, no model), so the caller plays the impacts itself.
    bool launchSpellMissile(uint32_t visualId, float speed, const MissileEnd& from,
                            const MissileEnd& to, std::vector<MissileEnd> impacts,
                            const MissileTrajectory* trajectory = nullptr);

    // Advance lifetime timers and remove expired instances.
    void update(float deltaTime);

    // Remove all active precast visual instances (cast canceled/interrupted).
    void cancelAllPrecastVisuals();

    // Remove all active spell visual instances and reset caches.
    // Called on map change / combat reset.
    void reset();

private:
    // Spell visual effects - transient M2 instances spawned by SMSG_PLAY_SPELL_VISUAL/IMPACT
    struct SpellVisualInstance {
        uint32_t instanceId;
        float elapsed;
        float duration;  // per-instance lifetime in seconds (from M2 anim or default)
        bool isPrecast;  // true for precast effects (removed on cancel/interrupt)
        uint32_t attachmentId;  // character attachment point to track (0=none/static)
        uint32_t attachInstanceId;  // CharacterRenderer instance the attachment belongs to
        // An effect with no bone of its own - an aura, an impact - is parented
        // to the unit it was cast on, as the client's CEffect::UpdateAttachment
        // parents the effect's model to the unit's: it keeps its spot in the
        // unit's own frame, so it moves and turns with it.
        bool followsUnit = false;
        glm::vec3 followOffset{0.0f};  // in the unit's frame
    };

    /// Parent the effect just added to its unit, from where it was placed.
    void followUnitFromSpawn(const glm::vec3& spawnPos);

    struct PhysicalProjectile {
        uint32_t instanceId = 0;
        glm::vec3 start{0.0f};
        glm::vec3 end{0.0f};
        glm::vec3 rotation{0.0f};
        float elapsed = 0.0f;
        float duration = 0.0f;
        bool spin = false;
    };

    // A missile in flight (CMissile).
    struct ActiveMissile {
        uint32_t instanceId = 0;
        uint32_t visualId = 0;
        glm::vec3 position{0.0f};
        float speed = 0.0f;
        float scale = 1.0f;
        // The target: its instance while it exists (0 once it has gone, or
        // for a point), the attachment the missile aims at on it (-1 = its
        // origin), and where the aim point was last seen - which is where the
        // missile lands if the target despawns, as FUN_006ff320 keeps it.
        uint32_t targetInstanceId = 0;
        int32_t targetAttachment = -1;
        glm::vec3 impactOffset{0.0f};
        glm::vec3 lastTarget{0.0f};
        std::vector<MissileEnd> impacts;  // where the impact kit plays on arrival
        float elapsed = 0.0f;
        float maxLifetime = 0.0f;
        uint32_t soundHandle = 0;  // its MissileSound, looping where it flies
        // An ADJUST_MISSILE cast's flight, planned at launch from `arcStart`.
        bool adjusted = false;
        spell_missile::MissileArc arc;
        glm::vec3 arcStart{0.0f};
    };

    void loadSpellVisualDbc();
    /// The M2Renderer model id for an effect model, loading it on first use;
    /// 0 when it cannot be loaded (remembered, so it is not read again).
    uint32_t acquireEffectModel(const std::string& modelPath);
    glm::vec3 missileSource(const MissileVisual& visual, const MissileEnd& from) const;
    /// Where the missile aims this frame; forgets a target that has gone.
    glm::vec3 missileTargetPoint(ActiveMissile& missile) const;
    void updateMissiles(float deltaTime);
    /// Start the missile's looping sound where it is; 0 when it has none or
    /// it cannot be played.
    uint32_t startMissileSound(uint32_t soundId, const glm::vec3& position);
    struct LoadedSound;
    const LoadedSound* soundEntry(uint32_t soundId);
    /// A SpellMissile.dbc row, null when there is none (or no file).
    const spell_missile::SpellMissileRow* spellMissileRow(uint32_t id);
    std::unordered_map<uint32_t, spell_missile::SpellMissileRow> spellMissileRows_;
    bool spellMissileDbcLoaded_ = false;

    M2Renderer* m2Renderer_ = nullptr;
    Renderer* renderer_ = nullptr;
    pipeline::AssetManager* cachedAssetManager_ = nullptr;

    std::vector<SpellVisualInstance> activeSpellVisuals_;
    std::vector<PhysicalProjectile> physicalProjectiles_;
    std::vector<ActiveMissile> activeMissiles_;
    std::unordered_map<uint32_t, MissileVisual> missileVisuals_;      // visualId → missile
    struct LoadedSound {
        std::vector<uint8_t> data;
        float volume = 1.0f;
    };
    std::unordered_map<uint32_t, LoadedSound> soundEntries_;           // SoundEntries id → its file, empty if none
    std::unordered_map<uint32_t, uint32_t> impactKitSounds_;           // visualId → its impact kit's SoundEntries id
    std::unordered_map<uint32_t, std::string> spellVisualPrecastPath_; // visualId → precast M2 path
    std::unordered_map<uint32_t, std::string> spellVisualCastPath_;   // visualId → cast M2 path
    std::unordered_map<uint32_t, std::string> spellVisualImpactPath_; // visualId → impact M2 path
    std::unordered_map<std::string, uint32_t> spellVisualModelIds_;   // M2 path → M2Renderer modelId
    std::unordered_set<uint32_t> spellVisualFailedModels_;           // modelIds that failed to load (negative cache)
    uint32_t nextSpellVisualModelId_ = 999000; // Reserved range 999000-999799
    uint32_t nextProjectileModelId_ = 998000;  // Reserved range 998000-998999
    std::unordered_map<std::string, uint32_t> projectileModelIds_;
    bool spellVisualDbcLoaded_ = false;
    static constexpr float SPELL_VISUAL_MAX_DURATION = 5.0f;
    static constexpr float SPELL_VISUAL_DEFAULT_DURATION = 2.0f;

    // Determine character attachment point from model path keywords
    static uint32_t classifyAttachmentId(const std::string& modelPath);

    // Apply height offset based on model path keywords (Hand → hands, Chest → chest, Base → ground)
    static glm::vec3 applyEffectHeightOffset(const glm::vec3& basePos, const std::string& modelPath);
};

} // namespace rendering
} // namespace wowee
