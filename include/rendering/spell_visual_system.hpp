#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <optional>
#include <memory>
#include <glm/glm.hpp>

#include "rendering/spell_missile.hpp"
#include "rendering/spell_kit.hpp"
#include "rendering/spell_chain.hpp"
#include "rendering/swing_trail.hpp"
#include "rendering/camera_shake.hpp"
#include "rendering/mount_transition.hpp"

namespace wowee {
namespace pipeline { class AssetManager; }
namespace rendering {

class M2Renderer;
class Renderer;
class CharacterRenderer;

/// What else a kit's play names (0x00745230's struct): the unit it plays
/// for where it has no instance (a place kit's caster), its chain counter
/// (+0x24, -1 for none) and whether it is a waiting kit played at last
/// (+0x20).
struct SpellKitPlayExtra {
    uint64_t unitGuid = 0;
    int32_t counter = -1;
    bool replay = false;
};

class SpellVisualSystem {
public:
    SpellVisualSystem() = default;
    ~SpellVisualSystem() = default;

    // Initialize with references to the M2 renderer and parent renderer
    void initialize(M2Renderer* m2Renderer, Renderer* renderer);
    void shutdown();

    /// A visual's cast kit on its caster (0x0080e1b0), or its impact kit on
    /// a unit it hit (0x00801f10): CasterImpactKit where that unit is the
    /// caster, TargetImpactKit on any other, else the ImpactKit. Played as
    /// 0x00745230 plays a kit: each model at its attachment on the unit drawn
    /// by attachInstanceId, the WorldEffect where it stands. With no instance
    /// the kit's models go to worldPosition.
    void playSpellVisual(uint32_t visualId, const glm::vec3& worldPosition,
                         bool useImpactKit = false, uint32_t attachInstanceId = 0, bool onCaster = false,
                         uint32_t spellId = 0);

    /// The visual's PrecastKit on its caster while the cast runs
    /// (0x007fa2e0): its models repeat until castTimeMs has passed or the
    /// cast is cancelled.
    void playSpellVisualPrecast(uint32_t visualId, const glm::vec3& worldPosition,
                                uint32_t castTimeMs = 0, uint32_t attachInstanceId = 0, uint32_t spellId = 0);

    /// A SpellVisualKit by its id, as SMSG_PLAY_SPELL_VISUAL (type 1,
    /// 0x008006c0) and SMSG_PLAY_SPELL_IMPACT (type 0, 0x00800610) play one
    /// on a unit.
    void playKit(uint32_t kitId, spell_kit::KitType type, const glm::vec3& worldPosition, uint32_t renderInstanceId);

    /// A ground-aimed cast's area kits at its destination (0x0080e1b0): the
    /// InstantAreaKit (+0x5c), and the ImpactAreaKit (+0x60) unless a
    /// missile carries it there (0x00700e20 plays it as that lands).
    void playSpellAreaKits(uint32_t visualId, const glm::vec3& place, bool missileCarriesImpact,
                           uint32_t spellId = 0, uint64_t casterGuid = 0);

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
                            const MissileTrajectory* trajectory = nullptr, uint32_t spellId = 0);

    /// Whether the unit drawn by a CharacterRenderer instance holds a kit's
    /// weapon effects - its CreatureModelData +4 lacks 0x10 (0x0073a6c0,
    /// 0x006f8c50) - and then its AttachedEffectScale (+0x60); nothing when
    /// it does not.
    using WeaponEffectHolder = std::function<std::optional<float>(uint32_t renderInstanceId)>;
    void setWeaponEffectHolder(WeaponEffectHolder holder) { weaponEffectHolder_ = std::move(holder); }

    /// An aura's state kit (0x00724820): when a slot of the unit's auras
    /// takes a spell it did not have, the StateKit (SpellVisual +0x10) of
    /// the spell's visual goes on the unit, its models at their attachments,
    /// looping until the aura goes. Held back while a missile carrying the
    /// visual flies at the unit; it plays as the missile lands (0x00700e20).
    /// `casterGuid` is the aura's caster (0 for the unit itself), from which
    /// the kit's chains reach the unit (kit flag 0x1000, 0x007265c0).
    void applyAuraStateKit(uint64_t unitGuid, uint32_t spellId, uint32_t visualId, uint64_t casterGuid = 0);
    /// The aura gone from its slot (0x0071e930): every state kit of the spell
    /// leaves the unit (0x00743b40) and the visual's StateDoneKit (+0x14)
    /// plays once.
    void removeAuraStateKit(uint64_t unitGuid, uint32_t spellId, uint32_t visualId);
    /// SMSG_AURA_UPDATE's slots as the client takes each: one taking a spell
    /// it did not hold plays the spell's state kit (0x00724820), one losing
    /// its spell removes it (0x0071e930). The slots are kept here, so a
    /// refresh of the same spell plays nothing.
    void setUnitAuraSlot(uint64_t unitGuid, uint32_t slot, uint32_t spellId, uint64_t casterGuid = 0);
    /// SMSG_AURA_UPDATE_ALL: the slots it names, and every other one empty.
    struct AuraSlotSpell {
        uint32_t slot = 0;
        uint32_t spellId = 0;
        uint64_t casterGuid = 0;
    };
    void setUnitAuraSlots(uint64_t unitGuid, const std::vector<AuraSlotSpell>& slotSpells);
    /// SMSG_SPELL_GO for a unit (0x0080e1b0): where its kits' chains go -
    /// the hit list less the unit itself (0x00724f50), and the place it was
    /// cast at (target flag 0x40), unless the visual has Flags 0x1 and there
    /// are hits (0x00715400).
    void setUnitCastTargets(uint64_t unitGuid, uint32_t spellId, const std::vector<uint64_t>& hits,
                            const std::optional<glm::vec3>& destination);
    /// A unit's channel, UNIT_CHANNEL_SPELL and UNIT_FIELD_CHANNEL_OBJECT
    /// (0x0073eb50): as the spell changes, the old one's effects leave the
    /// unit (0x00743b40) and the new one's ChannelKit (SpellVisual +0x18)
    /// goes on it, held like a state kit (0x0072bc70).
    void setUnitChannel(uint64_t unitGuid, uint32_t spellId, uint64_t channelObject);
    /// SMSG_SPELL_UPDATE_CHAIN_TARGETS for a unit (0x00741210): its hits
    /// are the new targets (0x00724f50), its channel object the first where
    /// the handler moved it (0 for unchanged), and the spell's effects go
    /// and its channel's kit comes again (0x0073eb50).
    void updateChainTargets(uint64_t unitGuid, uint32_t spellId, const std::vector<uint64_t>& targets,
                            uint64_t channelObject = 0);
    /// Every unit's channel this frame; a unit not named has none.
    struct ChannelState {
        uint64_t unitGuid = 0;
        uint32_t spellId = 0;
        uint64_t channelObject = 0;
    };
    void setUnitChannels(const std::vector<ChannelState>& channels);
    /// The unit a CharacterRenderer instance draws, 0 for none.
    using InstanceUnitResolver = std::function<uint64_t(uint32_t renderInstanceId)>;
    void setInstanceUnitResolver(InstanceUnitResolver resolver) { instanceUnitResolver_ = std::move(resolver); }
    /// A unit's CreatureModelData GeoBox height (+0x58 less +0x4c), which
    /// with its scale sets where a chain meets it (0x00717ad0).
    void setUnitHeight(std::function<float(uint32_t renderInstanceId)> height) { unitHeight_ = std::move(height); }
    /// Whether a unit has its melee weapons in its hands (+0xb5c 1), which a
    /// kit's swing trails need (0x00715ba0).
    using MeleeDrawnQuery = std::function<bool(uint64_t unitGuid)>;
    void setMeleeDrawnQuery(MeleeDrawnQuery query) { meleeDrawnQuery_ = std::move(query); }
    /// Where a kit's worn item goes (CharProc 17): the unit, the equipment
    /// slot, the item's display and inventory type; display 0 gives the
    /// unit its own back (0x006f82d0, 0x00723730).
    using WornItemSink = std::function<void(uint64_t unitGuid, int equipSlot, uint32_t displayId,
                                            uint8_t inventoryType)>;
    void setWornItemSink(WornItemSink sink) { wornItemSink_ = std::move(sink); }
    /// Where an object that is not a unit stands and faces (a game object a
    /// channel is aimed at), for a chain's end (0x007fae90).
    using ObjectFrameResolver = std::function<bool(uint64_t guid, glm::mat4& frame)>;
    void setObjectFrameResolver(ObjectFrameResolver resolver) { objectFrameResolver_ = std::move(resolver); }
    /// Spell.dbc SpellVisual of a spell, 0 for none.
    using SpellVisualResolver = std::function<uint32_t(uint32_t spellId)>;
    void setSpellVisualResolver(SpellVisualResolver resolver) { spellVisualResolver_ = std::move(resolver); }
    /// The CharacterRenderer instance drawing a unit, 0 while it has none.
    using UnitInstanceResolver = std::function<uint32_t(uint64_t unitGuid)>;
    void setUnitInstanceResolver(UnitInstanceResolver resolver) { unitInstanceResolver_ = std::move(resolver); }
    /// The CharacterRenderer instance drawing a unit's mount (+0x98c), 0
    /// while it rides none.
    void setUnitMountInstanceResolver(UnitInstanceResolver resolver) {
        unitMountInstanceResolver_ = std::move(resolver);
    }
    /// A mount spell's mount (CharProc 16, 0x006f9670): the display of the
    /// creature its first aura-78 effect names - 0 while the creature is
    /// asked for (0x0067b6a0, 0x006f9610) - or nothing where it has none.
    using MountDisplayResolver = std::function<std::optional<uint32_t>(uint32_t spellId)>;
    void setMountDisplayResolver(MountDisplayResolver resolver) { mountDisplayResolver_ = std::move(resolver); }
    /// A display's model loaded for the character renderer, 0 where it
    /// cannot be (0x006f83d0).
    using MountModelLoader = std::function<uint32_t(uint32_t displayId)>;
    void setMountModelLoader(MountModelLoader loader) { mountModelLoader_ = std::move(loader); }
    /// Whether a unit rides (0x0051a230: its mount display, not leaving it).
    using UnitMountedQuery = std::function<bool(uint64_t unitGuid)>;
    void setUnitMountedQuery(UnitMountedQuery query) { unitMountedQuery_ = std::move(query); }
    /// The unit's mount display as the server has it (UNIT_FIELD_MOUNTDISPLAYID).
    using UnitMountFieldQuery = std::function<uint32_t(uint64_t unitGuid)>;
    void setUnitMountFieldQuery(UnitMountFieldQuery query) { unitMountFieldQuery_ = std::move(query); }
    /// Where a mount transition hands its unit over (0x007412b0, 0x00740450):
    /// the unit mounted on the display now, or 0 to put it down again.
    using MountSink = std::function<void(uint64_t unitGuid, uint32_t displayId)>;
    void setMountSink(MountSink sink) { mountSink_ = std::move(sink); }
    /// A unit was given its mount (0x0073d5d0): its transition ends there,
    /// its mount drawn from then on by the unit's own.
    void onUnitMounted(uint64_t unitGuid);
    /// Ground for a transition's mount: the highest floor in a column.
    using GroundQuery = std::function<std::optional<mount_transition::GroundHit>(const glm::vec3& top,
                                                                                 const glm::vec3& bottom)>;
    void setGroundQuery(GroundQuery query) { groundQuery_ = std::move(query); }
    /// Whether a unit shows its SpellVisual Flags 8 state kits (0x00720400):
    /// +0xa30 0x10000, its weapons away and no cast.
    using UnarmedKitsQuery = std::function<bool(uint64_t unitGuid)>;
    void setUnarmedKitsQuery(UnarmedKitsQuery query) { unarmedKitsQuery_ = std::move(query); }
    /// A unit's CreatureModelData AttachedEffectScale (+0x60), which sizes
    /// every kit model on it (0x006f8c50).
    using AttachedEffectScale = std::function<float(uint32_t renderInstanceId)>;
    void setAttachedEffectScale(AttachedEffectScale scale) { attachedEffectScale_ = std::move(scale); }
    /// A unit's CreatureModelData WorldEffectScale (+0x5c) (0x006f7950).
    void setWorldEffectScale(AttachedEffectScale scale) { worldEffectScale_ = std::move(scale); }
    /// What a kit's CharProc procedures read of its spell: its cast time
    /// (0x007ff180) and what it is aimed at (0x007fe1b0).
    struct KitSpellInfo {
        uint32_t castTimeMs = 0;
        uint32_t targetKind = 0;  ///< 2 enemies, 1 friends, 0 neither
    };
    using KitSpellResolver = std::function<KitSpellInfo(uint32_t spellId)>;
    void setKitSpellResolver(KitSpellResolver resolver) { kitSpellResolver_ = std::move(resolver); }
    /// The creature cache's type flags (+0x964 +0xc) of the unit an instance
    /// draws; 0 for a player or a unit with no row.
    using UnitTypeFlags = std::function<uint32_t(uint32_t renderInstanceId)>;
    void setUnitTypeFlags(UnitTypeFlags flags) { unitTypeFlags_ = std::move(flags); }
    /// Where the light tint goes (CharProc 6): its colour and how far toward
    /// it, 0..255 (0x007ee300's 0xd38b51 and 0xd38b50).
    using LightTintSink = std::function<void(const glm::vec3& colour, uint32_t amount)>;
    void setLightTintSink(LightTintSink sink) { lightTintSink_ = std::move(sink); }

    /// Where a camera shake goes: the camera's list (0x00606330).
    using CameraShakeSink = std::function<void(const camera_shake::Shake&, const glm::vec3& origin)>;
    void setCameraShakeSink(CameraShakeSink sink) { cameraShakeSink_ = std::move(sink); }
    /// A SpellEffectCameraShakes row's shakes at `origin` (0x007fa620): a
    /// kit's ShakeID, or SMSG_CAMERA_SHAKE's.
    void playCameraShakes(uint32_t spellEffectCameraShakesId, const glm::vec3& origin);
    /// A SoundEntries sound at a place, once.
    void playSoundAt(uint32_t soundId, const glm::vec3& position);

    // Advance lifetime timers and remove expired instances.
    void update(float deltaTime);

    // Remove all active precast visual instances (cast canceled/interrupted).
    void cancelAllPrecastVisuals();

    // Remove all active spell visual instances and reset caches.
    // Called on map change / combat reset.
    void reset();

private:
    // A kit model that plays once (or for the cast, a precast kit's).
    struct SpellVisualInstance {
        uint32_t instanceId;
        float elapsed;
        float duration;  // its lifetime in seconds
        bool isPrecast;  // true for precast effects (removed on cancel/interrupt)
        uint32_t attachmentId;  // the attachment it rides, where `attached`
        uint32_t attachInstanceId;  // CharacterRenderer instance the attachment belongs to
        bool attached = false;  // false: it stays where it was put in the world
        float scale = 1.0f;  // a kit weapon effect's, in its attachment's frame
        glm::mat4 local{1.0f};  // a SpellVisualKitModelAttach offset, in the attachment's frame
        /// 0x00744870: when its Stand has run once (seconds), its Decay,
        /// where `decays`, else its end; below 0 none.
        float switchAt = -1.0f;
        bool decays = false;
    };

    /// A model a kit hangs on a unit: a model column at its attachment
    /// (0x00744790), or a SpellVisualKitModelAttach row with its offset and
    /// turn (0x006f84f0). Attachment -1 is placed in the world at the unit
    /// (the kit's WorldEffect, +0x38, or a row without an attachment).
    struct KitModel {
        std::string path;
        int32_t attachment = -1;
        spell_kit::KitModelKind kind = spell_kit::KitModelKind::Column;
        glm::mat4 local{1.0f};
        float scale = 1.0f;
        float minScale = 0.0f;
        float maxScale = 1e30f;
    };
    /// A kit's LeftWeaponEffect or RightWeaponEffect: its
    /// SpellVisualEffectName model, Scale and allowed scales.
    struct KitWeaponEffect {
        std::string modelPath;
        bool left = false;
        float scale = 1.0f;
        float minScale = 0.0f;
        float maxScale = 1e30f;
    };
    struct KitRecord {
        uint32_t id = 0;
        std::vector<KitModel> models;
        std::vector<KitWeaponEffect> weaponEffects;  ///< +0x24, +0x28 (0x0073a6c0)
        uint32_t soundId = 0;  ///< SoundID (+0x3c), SoundEntries
        uint32_t flags = 0;  ///< SpellVisualKit Flags (+0x94)
        uint32_t shakeId = 0;  ///< ShakeID (+0x40), SpellEffectCameraShakes
        /// CharProc (+0x44) and its CharParamZero..Three (+0x54..+0x84).
        std::array<uint32_t, 4> charProc{};
        std::array<std::array<float, 4>, 4> charParam{};  // [proc][param]
    };
    std::unordered_map<uint32_t, KitRecord> kits_;  // SpellVisualKit id → its models
    struct VisualAuraKits {
        uint32_t stateKit = 0;      ///< SpellVisual +0x10
        uint32_t stateDoneKit = 0;  ///< SpellVisual +0x14
        uint32_t channelKit = 0;    ///< SpellVisual +0x18
        uint32_t flags = 0;         ///< SpellVisual Flags (+0x34)
    };
    std::unordered_map<uint32_t, VisualAuraKits> visualAuraKits_;  // visualId → its aura kits

    /// A kit model shown on a unit.
    struct KitModelInstance {
        uint32_t instanceId = 0;
        int32_t attachment = -1;  ///< -1: in the world, where it was put
        glm::mat4 local{1.0f};
        float scale = 1.0f;
        /// 0x007449c0: when its Stand has run once (seconds since it was put
        /// on), it holds its Hold; below 0 when it plays on as it is.
        float holdAt = -1.0f;
        float elapsed = 0.0f;
    };
    /// An aura's state kit on a unit.
    struct AuraKit {
        uint32_t spellId = 0;
        uint32_t visualId = 0;
        uint32_t kitId = 0;
        bool unarmedOnly = false;      ///< its visual has Flags 8
        bool playing = true;           ///< its kit is on the unit (not removed by 0x00720400)
        bool awaitingMissile = false;  ///< a missile carrying it is still flying at the unit
        uint64_t casterGuid = 0;       ///< the aura's caster, its chains' other end
        uint32_t boundInstance = 0;    ///< the instance its models hang on
        std::vector<KitModelInstance> models;
    };
    struct UnitAuraKits {
        std::unordered_map<uint32_t, uint32_t> slots;  // aura slot → its spell
        std::vector<AuraKit> auras;
        spell_kit::UnarmedKitBits unarmedBits;
    };
    std::unordered_map<uint64_t, UnitAuraKits> unitAuraKits_;
    UnitInstanceResolver unitInstanceResolver_;
    UnitInstanceResolver unitMountInstanceResolver_;
    CameraShakeSink cameraShakeSink_;
    /// A SpellVisual's kits for a cast: PrecastKit (+4), CastKit (+8),
    /// ImpactKit (+0xc), CasterImpactKit (+0x38), TargetImpactKit (+0x3c),
    /// InstantAreaKit (+0x5c) and ImpactAreaKit (+0x60).
    struct VisualKits {
        uint32_t precast = 0;
        uint32_t cast = 0;
        uint32_t impact = 0;
        uint32_t casterImpact = 0;
        uint32_t targetImpact = 0;
        uint32_t instantArea = 0;
        uint32_t impactArea = 0;
    };
    std::unordered_map<uint32_t, VisualKits> visualKits_;
    /// 0x00745230 on a unit (renderInstanceId, 0 for none) or at a place:
    /// the kit's models, its weapon effects (precast and cast kits), its
    /// camera shake and its colour. Returns a state kit's models.
    using KitPlayExtra = SpellKitPlayExtra;
    std::vector<KitModelInstance> playKitOnUnit(uint32_t kitId, spell_kit::KitType type,
                                               uint32_t renderInstanceId, const glm::vec3& position,
                                               const glm::vec3* place, uint32_t castTimeMs = 0,
                                               uint32_t spellId = 0, const KitPlayExtra& extra = {});
    /// The unit's virtual 0x104 (0x0072af60): a kit with Flags 0x1 of a
    /// spell whose visual has a chain kit (0x00800bf0) waits on its unit
    /// (0x00728050) for a chain's pulse to reach it, ten seconds at most.
    struct WaitingKit {
        uint64_t unitGuid = 0;
        uint32_t spellId = 0;
        uint32_t kitId = 0;
        spell_kit::KitType type = spell_kit::KitType::Cast;
        bool hasPlace = false;
        glm::vec3 place{0.0f};
        uint32_t castTimeMs = 0;
        int32_t counter = -1;
        uint32_t deadlineMs = 0;
    };
    std::vector<WaitingKit> waitingKits_;
    /// 0x00800bf0: whether any of the visual's cast, impact, state, channel,
    /// caster and target impact kits runs a chain (0x007fe470).
    bool visualHasChainKit(uint32_t visualId) const;
    /// The unit's virtual 0xc0 (0x00722760): its kits waiting on the spell
    /// with this counter play.
    void releaseWaitingKits(uint64_t unitGuid, uint32_t spellId, int32_t counter);
    /// 0x00728140: the waits run out unplayed.
    void expireWaitingKits();
    /// Each unit's chain counter (+0xf58), two on for each chain kit.
    std::unordered_map<uint64_t, int32_t> unitChainCounters_;
    /// A kit's camera shake where it plays (0x0073b140, 0x006f9840).
    void playKitShake(uint32_t kitId, const glm::vec3& origin);
    /// A kit's colour fade (CharProc 13) on the unit drawn by an instance,
    /// and its light tint (CharProc 6), for its spell.
    void playKitColourFade(uint32_t kitId, uint32_t renderInstanceId, uint32_t spellId);
    /// Whether a kit's CharProc 1 and 13 colour this unit for this spell.
    bool kitColoursUnit(uint32_t renderInstanceId, uint32_t spellId) const;
    /// Each unit's alpha from its auras' kits (CharProc 14), the latest's.
    void updateUnitAlphas();
    std::unordered_map<uint32_t, float> unitKitAlphas_;  // render instance → the alpha it was given
    /// CharProc 15's fades (0x0071a940), by render instance, until they go back.
    std::unordered_map<uint32_t, spell_kit::TimedAlpha> timedAlphas_;
    /// CharProc 11's holds (0x006f80b0): the unit's animation held until its
    /// effects of the spell go, and whether it was held already.
    struct AnimationHold {
        uint64_t unitGuid = 0;
        uint32_t spellId = 0;
        uint32_t renderInstanceId = 0;
        bool wasHeld = false;
        bool mountWasHeld = false;  ///< its mount's, or none had to be held
    };
    std::vector<AnimationHold> animationHolds_;
    /// CharProc 17's worn items, until the unit's effects of the spell go.
    struct WornItem {
        uint64_t unitGuid = 0;
        uint32_t spellId = 0;
        int equipSlot = -1;
    };
    std::vector<WornItem> wornItems_;
    WornItemSink wornItemSink_;
    /// CharProc 16's transitions (0x006f9670), one a unit: the effect (its
    /// unit, spell and kit) owning the mount's model (+0x98's, effect +0)
    /// and the AUMountTransitionObject (+0x90).
    struct MountTransition {
        uint64_t unitGuid = 0;
        uint32_t spellId = 0;
        bool precast = false;      ///< a precast kit's: it goes as the cast does
        uint32_t castEndMs = 0;
        uint32_t displayId = 0;    ///< 0 while its creature is asked for
        uint32_t modelInstance = 0;
        uint32_t riderInstance = 0;
        bool birthSeen = false;
        mount_transition::State state;
    };
    std::vector<MountTransition> mountTransitions_;
    MountDisplayResolver mountDisplayResolver_;
    MountModelLoader mountModelLoader_;
    UnitMountedQuery unitMountedQuery_;
    UnitMountFieldQuery unitMountFieldQuery_;
    MountSink mountSink_;
    GroundQuery groundQuery_;
    /// 0x007265c0 case 16 for a kit on a unit.
    void startMountTransition(uint32_t renderInstanceId, uint32_t spellId, spell_kit::KitType type,
                              uint32_t castTimeMs);
    /// 0x007fca30's walk: each transition stepped (0x007fb7f0), its mount
    /// placed (0x0071fbf0) and its rider carried (0x007193f0).
    void updateMountTransitions();
    /// 0x006f87c0's part: the transition goes - the unit mounted where it
    /// arrived (0x007412b0), `resync` then setting it to the server's
    /// (0x007fec00) - and its mount's model with it.
    void endMountTransition(size_t index, bool resync);
    /// The light tint playing (CharProc 6), and handed to the sink.
    std::optional<spell_kit::LightTint> lightTint_;
    bool lightTinted_ = false;
    void updateLightTint();

    /// The SpellChainEffects rows, loaded on first use.
    std::unordered_map<uint32_t, spell_chain::ChainEffect> chainEffects_;
    bool chainEffectsLoaded_ = false;
    const spell_chain::ChainEffect* chainEffect(uint32_t id);
    /// What a chain reads of a SpellVisual: MissileAttachment (+0x40) with
    /// MissileCastOffset, MissileDestinationAttachment (+0x28) with
    /// MissileImpactOffset (y negated), Flags 0x200 (0x007fc5f0, 0x007fabf0).
    struct VisualEnds {
        int32_t sourceAttachment = -1;
        int32_t destinationAttachment = -1;
        glm::vec3 castOffset{0.0f};
        glm::vec3 impactOffset{0.0f};
        uint32_t flags = 0;  ///< SpellVisual Flags (+0x34)
    };
    std::unordered_map<uint32_t, VisualEnds> visualEnds_;
    const VisualEnds* visualEndsForSpell(uint32_t spellId) const;
    /// A LightningObject (0x007fc5f0): node 0 the unit, the rest its targets
    /// (0 for the place), a bolt from node to node, each drawn by a
    /// CLightning while it shows.
    struct ChainObject {
        const spell_chain::ChainEffect* effect = nullptr;
        uint32_t spellId = 0;
        std::vector<uint64_t> nodes;
        std::optional<glm::vec3> place;  ///< flag 2: the place, with the impact offset
        int32_t sourceAttachment = -1;
        glm::vec3 castOffset{0.0f};
        glm::vec3 impactOffset{0.0f};  ///< where it meets an object that is not a unit
        std::vector<spell_chain::Bolt> bolts;
        std::vector<std::optional<spell_chain::Lightning>> lightning;  ///< one a bolt
        uint32_t endMs = 0;
        /// Flag 1 (the kit's ParamTwo): held by the effect that made it until
        /// that goes (0x007fc990), the unit and spell it belongs to.
        bool held = false;
        uint64_t ownerUnit = 0;
        uint32_t ownerSpell = 0;
    };
    std::vector<ChainObject> chains_;
    spell_chain::Rng chainRng_;
    /// SMSG_SPELL_GO's targets of each unit's last cast.
    struct CastTargets {
        std::vector<uint64_t> hits;
        std::optional<glm::vec3> place;
    };
    std::unordered_map<uint64_t, CastTargets> castTargets_;
    struct UnitChannel {
        uint32_t spellId = 0;
        uint64_t object = 0;
    };
    std::unordered_map<uint64_t, UnitChannel> unitChannels_;
    InstanceUnitResolver instanceUnitResolver_;
    std::function<float(uint32_t)> unitHeight_;
    ObjectFrameResolver objectFrameResolver_;
    /// 0x007265c0 cases 0 and 12 for a kit on `unitGuid` for `spellId`;
    /// `otherSource` names the unit kit flag 0x1000 draws from.
    void startKitChains(const KitRecord& kit, uint64_t unitGuid, uint32_t spellId, uint64_t otherSource);
    /// 0x00743b40's part for chains: those the unit's effects of the spell
    /// held let go (0x007fc990).
    void releaseChains(uint64_t unitGuid, uint32_t spellId);
    /// 0x0072bc70: the channel spell's ChannelKit on the unit.
    void applyChannelKit(uint64_t unitGuid, uint32_t spellId);
    /// 0x00743b40: every effect of the spell leaves the unit.
    void removeUnitSpellEffects(uint64_t unitGuid, uint32_t spellId);
    /// 0x007fca30 with 0x007fae90 and 0x009ab730: each bolt's ends, its
    /// lightning made, moved and dropped, and the strips handed on.
    void updateChains(float deltaTime);
    /// CharProc 8's swing trails (0x007e4ff0): one a weapon in the unit's
    /// hands, at its hand attachment.
    struct WeaponSwing {
        uint32_t renderInstanceId = 0;
        uint32_t attachment = 0;
        swing_trail::Trail trail;
    };
    std::vector<WeaponSwing> swings_;
    MeleeDrawnQuery meleeDrawnQuery_;
    /// 0x00715ba0: a trail on each weapon in the unit's hands.
    void startSwingTrails(uint32_t renderInstanceId, const swing_trail::Start& start);
    /// The strips of this frame - the swing trails, then the chains by their render layer
    /// (0x009ab070) - handed to the M2 renderer.
    void publishClientStrips();
    bool publishedStrips_ = false;
    /// 0x007faa40: where a chain leaves its unit.
    bool chainSourcePoint(uint32_t renderInstanceId, const ChainObject& chain, glm::vec3& out) const;
    /// 0x007fabf0: where a chain meets a unit for its spell.
    bool chainUnitPoint(uint32_t renderInstanceId, uint32_t spellId, glm::vec3& out) const;
    /// The unit's feet raised three quarters of its height, and its frame.
    bool unitMiddle(uint32_t renderInstanceId, glm::vec3& middle, glm::mat4& frame) const;
    KitSpellResolver kitSpellResolver_;
    UnitTypeFlags unitTypeFlags_;
    LightTintSink lightTintSink_;
    /// The unit's colour from its auras' kits (CharProc 1) - the latest's -
    /// or its fade, set on each unit's model; white once neither is left.
    void updateUnitColours();
    std::unordered_map<uint32_t, spell_kit::ColourFade> colourFades_;  // render instance → its fade
    std::unordered_set<uint32_t> colouredInstances_;  // instances last given a colour
    uint32_t colourClockMs_ = 0;
    /// SpellEffectCameraShakes id → its CameraShakes rows, loaded on first use.
    std::unordered_map<uint32_t, std::vector<camera_shake::Shake>> spellEffectShakes_;
    bool cameraShakesLoaded_ = false;
    void loadCameraShakes();
    SpellVisualResolver spellVisualResolver_;
    UnarmedKitsQuery unarmedKitsQuery_;
    AttachedEffectScale attachedEffectScale_;
    /// 0x00720400 for a unit: hide or show its Flags 8 kits as asked.
    void stepUnarmedKits(uint64_t unitGuid, UnitAuraKits& unit, bool force);
    void hideAuraKit(AuraKit& aura);
    /// Each frame: the unit's kits follow it, onto a new model when it has
    /// one, and its Flags 8 kits show as 0x00720400 has them.
    void updateAuraKits(float deltaTime);
    /// A kit model's place this frame on the unit drawn by renderInstanceId;
    /// false when it has no such attachment (0x006f8c50 removes the effect).
    bool kitModelTransform(uint32_t renderInstanceId, int32_t attachment, const glm::mat4& local,
                           float effectScale, float minScale, float maxScale, glm::mat4& out, float& scale);
    /// Where a world-placed kit model goes (0x006f8ae0): where the unit
    /// stands, turned as it faces and sized by it, or at `place`.
    glm::mat4 worldKitModelTransform(const KitModel& model, uint32_t renderInstanceId, const glm::vec3& position,
                                     const glm::vec3* place);
    /// Play a kit's models (0x00745230) of the given type on a unit or at a
    /// place: a state kit's returned, held until removed; the others into
    /// activeSpellVisuals_, once or (precast) for the cast.
    std::vector<KitModelInstance> playKitModels(const KitRecord& kit, spell_kit::KitType type,
                                                uint32_t renderInstanceId, const glm::vec3& position,
                                                const glm::vec3* place, uint32_t castTimeMs = 0);

    /// A one-shot or precast kit model just put on: its animation and
    /// lifetime as its kit's callback has them (0x00744870, 0x007435a0).
    void addKitModel(uint32_t instanceId, spell_kit::KitModelLife life, bool isPrecast, uint32_t castTimeMs,
                     bool attached, uint32_t attachment, uint32_t attachInstanceId, float scale,
                     const glm::mat4& local);
    /// 0x0073a6c0: hang a kit's weapon effects in the caster's hands.
    void playKitWeaponEffects(const std::vector<KitWeaponEffect>& effects, uint32_t attachInstanceId,
                              bool isPrecast, uint32_t castTimeMs);
    WeaponEffectHolder weaponEffectHolder_;
    /// A unit's CreatureModelData WorldEffectScale (+0x5c), which sizes
    /// the kit models it places in the world (0x006f7950).
    AttachedEffectScale worldEffectScale_;

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
        uint32_t casterInstanceId = 0;  // whose impact kit is its CasterImpactKit (0x00700e20)
        uint32_t spellId = 0;
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
    /// The M2Renderer model id for an effect model; 0 when it is not ready.
    ///
    /// A model not yet loaded is read, parsed and its textures decoded on a
    /// worker, and only uploaded here once that is done - each of those was
    /// 3 to 175 ms on the main thread, a hitch at every new spell a crowd
    /// cast. Until then the effect is left out, as the client leaves out a
    /// model still loading. With syncEffectLoads_ set (the player's own
    /// casts and the spells on them) it is loaded there and then, as before.
    /// A model that cannot be loaded is remembered and never read again.
    uint32_t acquireEffectModel(const std::string& modelPath);
    /// Upload the effect models whose worker has finished, a few a frame.
    void finishEffectModelLoads();
    /// Upload a prepared model (a PreparedEffectModel, private to the .cpp);
    /// false, and the model remembered as failed, when it cannot be.
    bool uploadEffectModel(uint32_t modelId, const std::string& modelPath, void* preparedModel);
    struct PendingEffectLoad;
    std::unordered_map<uint32_t, std::shared_ptr<PendingEffectLoad>> pendingEffectLoads_;
    bool syncEffectLoads_ = false;
    /// Whether the player's character is one of these render instances.
    [[nodiscard]] bool involvesPlayer(uint32_t instanceA, uint32_t instanceB = 0) const;
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
    std::unordered_map<std::string, uint32_t> spellVisualModelIds_;   // M2 path → M2Renderer modelId
    std::unordered_set<uint32_t> spellVisualFailedModels_;           // modelIds that failed to load (negative cache)
    uint32_t nextSpellVisualModelId_ = 999000; // Reserved range 999000-999799
    uint32_t nextProjectileModelId_ = 998000;  // Reserved range 998000-998999
    std::unordered_map<std::string, uint32_t> projectileModelIds_;
    bool spellVisualDbcLoaded_ = false;
    /// A one-shot model whose sequence has no length: what it is given.
    static constexpr float SPELL_VISUAL_DEFAULT_DURATION = 2.0f;
};

} // namespace rendering
} // namespace wowee
