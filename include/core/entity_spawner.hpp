#pragma once

#include "game/character.hpp"
#include "game/guild_emblem.hpp"
#include "game/game_services.hpp"
#include "pipeline/blp_loader.hpp"
#include "rendering/blob_shadow.hpp"
#include "rendering/animation/body_yaw.hpp"
#include "core/character_geosets.hpp"
#include "core/weapon_attachment.hpp"
#include "game/vehicle.hpp"
#include "game/vehicle_camera.hpp"
#include "game/vehicle_transition.hpp"
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <array>
#include <chrono>
#include <map>
#include <optional>
#include <future>
#include <mutex>
#include <glm/glm.hpp>

namespace wowee {

// Forward declarations
namespace rendering { class Renderer; namespace unit_event { class FootstepTerrainSounds; } }
namespace pipeline { class AssetManager; class DBCFile; struct M2Model; struct WMOModel; }
namespace audio { enum class VoiceType; }
namespace game { class GameHandler; class Entity; struct CorpseLook; }

namespace core {

class AssetPrefetch;

class Application;

class EntitySpawner {
public:
    EntitySpawner(rendering::Renderer* renderer,
                  pipeline::AssetManager* assetManager,
                  game::GameHandler* gameHandler,
                  game::GameServices* gameServices);
    ~EntitySpawner();

    EntitySpawner(const EntitySpawner&) = delete;
    EntitySpawner& operator=(const EntitySpawner&) = delete;

    // Lifecycle
    void initialize();   // Build DBC lookups
    void update();       // Process all spawn/despawn queues (called from Application::update)
    void shutdown();     // Clear all instances and queues

    // Queue-based spawn API (called from GameHandler callbacks)
    void queueCreatureSpawn(uint64_t guid, uint32_t displayId,
                            float x, float y, float z, float orientation, float scale = 1.0f);
    void queuePlayerSpawn(uint64_t guid, uint8_t raceId, uint8_t genderId,
                          uint32_t appearanceBytes, uint8_t facialFeatures,
                          float x, float y, float z, float orientation);
    void queueGameObjectSpawn(uint64_t guid, uint32_t entry, uint32_t displayId,
                              float x, float y, float z, float orientation, float scale = 1.0f);
    void queuePlayerEquipment(uint64_t guid,
                              const std::array<uint32_t, 19>& displayInfoIds,
                              const std::array<uint8_t, 19>& inventoryTypes);
    /// The server's word on what a player wears: kept, and queued with any
    /// spell's worn items over it.
    void queuePlayerEquipmentFromServer(uint64_t guid,
                                        const std::array<uint32_t, 19>& displayInfoIds,
                                        const std::array<uint8_t, 19>& inventoryTypes);
    /// A spell's worn item over what a player wears in an equipment slot
    /// (CharProc 17, 0x006f82d0); display 0 gives it its own back (0x00723730).
    void setPlayerItemOverride(uint64_t guid, int equipSlot, uint32_t displayId, uint8_t inventoryType);

    // Immediate despawn
    void despawnCreature(uint64_t guid);
    void despawnPlayer(uint64_t guid);
    void despawnGameObject(uint64_t guid);
    /// The server took a game object away (0x004d7610, 0x004d7230). While an
    /// animation state holds it - 0x0070d1e0 takes a hold for any state but
    /// Closed, Opened and Destroyed, 0x0070d7e0 lets it go as the state ends
    /// - it is only marked (GameObject_C +0xbc 0x100000) and goes as that
    /// ends (0x004d77e0); otherwise at once.
    void destroyGameObject(uint64_t guid);

    /// A CORPSE object as CGCorpse_C draws it (0x00705670, 0x00705b20):
    /// bones as the race's death skeleton; otherwise its display's model,
    /// laid down dead and, for a character model, dressed in its items.
    void spawnCorpse(uint64_t guid, const game::CorpseLook& look, float x, float y, float z, float orientation);
    /// Takes away what spawnCorpse drew besides a character model.
    void despawnCorpse(uint64_t guid);
    /// A corpse's lootable bit changed: the loot sparkle added or taken
    /// away (0x007061e0).
    void setCorpseLootable(uint64_t guid, bool lootable);
    /// Whether spawnCorpse drew this guid through the player or creature path.
    bool isCorpse(uint64_t guid) const { return corpseGuids_.count(guid) > 0; }

    // A GAMEOBJECT_QUERY_RESPONSE arrived. Game object animation does not
    // depend on the type, so there is nothing to revisit; kept as the hook.
    void onGameObjectInfoReceived(uint32_t entry);

    // Clear all queues and instances (logout, reconnect)
    void clearAllQueues();
    void despawnAllCreatures();
    void despawnAllPlayers();
    void despawnAllGameObjects();

    // Full reset - despawns entities, waits for async loads, clears all state.
    // Used by logoutToLogin() and loadOnlineWorldTerrain() for clean slate.
    void resetAllState();

    // Rebuild lookup tables (for reloadExpansionData after expansion change)
    void rebuildLookups();

    // Check if any spawn/async work is still pending
    bool hasWorkPending() const;
    // What is still queued, queue by queue, for the loading screen's log.
    std::string pendingWorkSummary() const;
    /// Milliseconds update() has spent in each of its stages since the last
    /// call, as text, and the counters cleared: the loading screen's profile.
    std::string takeStageTimes();

    // Status queries
    bool isCreatureSpawned(uint64_t guid) const { return creatureInstances_.count(guid) > 0; }
    bool isCreaturePending(uint64_t guid) const { return pendingCreatureSpawnGuids_.count(guid) > 0; }
    bool isPlayerSpawned(uint64_t guid) const { return playerInstances_.count(guid) > 0; }
    bool isPlayerPending(uint64_t guid) const { return pendingPlayerSpawnGuids_.count(guid) > 0; }
    bool isGameObjectSpawned(uint64_t guid) const { return gameObjectInstances_.count(guid) > 0; }

    /// What the server says a game object is doing: open, closed, destroyed.
    ///
    /// A door is a pose. The state arrives twice - once in the create block for
    /// an object that is already open when it comes into view, and again
    /// whenever it changes - and the first of those used to arrive before the
    /// model existed and was dropped, so every door in the world was drawn in
    /// its closed pose however the server had it.
    void applyGameObjectState(uint64_t guid, uint8_t goState);
    /// The object became the one a window is open with (open), or stopped
    /// being it: its type's OPEN (slot 0x98) or CLOSE (slot 0x9c) - see
    /// game/go_interaction_anim.hpp.
    void applyGameObjectInteraction(uint64_t guid, bool open);
    /// The server's custom animation (SMSG_GAMEOBJECT_CUSTOM_ANIM, 0x0070be90:
    /// Custom0..3) or despawn animation (SMSG_GAMEOBJECT_DESPAWN_ANIM,
    /// 0x0070bef0: Despawn) for the object: through its type's slot 0x4c, or
    /// noted for a model still loading (+0x208).
    void playGameObjectCustomAnim(uint64_t guid, uint8_t state);

    // Quick instance ID lookups (returns 0 if not found)
    uint32_t getCreatureInstanceId(uint64_t guid) const {
        auto it = creatureInstances_.find(guid); return (it != creatureInstances_.end()) ? it->second : 0;
    }
    uint32_t getPlayerInstanceId(uint64_t guid) const {
        auto it = playerInstances_.find(guid); return (it != playerInstances_.end()) ? it->second : 0;
    }

    // Render bounds/position queries (used by click targeting, etc.)
    bool getRenderBoundsForGuid(uint64_t guid, glm::vec3& outCenter, float& outRadius) const;
    /// A game object whose model is loaded and draws nothing (M2Renderer::
    /// instanceDrawsNothing). False while there is no model yet.
    [[nodiscard]] bool gameObjectDrawsNothing(uint64_t guid) const;
    uint32_t characterInstanceIdForGuid(uint64_t guid) const;
    bool getRenderFootZForGuid(uint64_t guid, float& outFootZ) const;
    /// Where the client puts a unit's name (0x0071fef0): its model's
    /// PlayerNameMounted attachment while it rides (the mount's model),
    /// else PlayerName, else its position raised by 1.25 of its
    /// CreatureModelData height times its scale. Nothing when it has no
    /// model drawn, or neither attachment nor a height.
    std::optional<glm::vec3> unitNamePosition(uint64_t guid) const;
    /// The frame a questgiver's mark hangs on: a unit's name attachment
    /// (0x00719310) or a game object's (0x0070cf30). Nothing without one.
    std::optional<glm::mat4> questMarkerFrame(uint64_t guid) const;
    /// Where the circle under a unit goes and how big it is (0x00725980,
    /// 0x00720330): at the unit's position - its mount's while it rides - and
    /// sized by its CreatureModelData box, joined to its mount's when riding
    /// (0x0071ed80), and its drawn scale; 1.2 when it has no box. Nothing
    /// when it has no model drawn.
    struct SelectionCirclePlacement {
        glm::vec3 position{0.0f};
        float radius = 0.0f;
    };
    std::optional<SelectionCirclePlacement> selectionCircle(uint64_t guid) const;
    bool getRenderPositionForGuid(uint64_t guid, glm::vec3& outPos) const;

    // Display data lookups
    bool areCreatureLookupsBuilt() const { return creatureLookupsBuilt_; }
    bool areGameObjectLookupsBuilt() const { return gameObjectLookupsBuilt_; }
    std::string getModelPathForDisplayId(uint32_t displayId) const;
    /// The skin textures a creature display wears, by M2 texture type - 11,
    /// 12 and 13 are skin1, skin2 and skin3 in CreatureDisplayInfo.dbc.
    ///
    /// A creature's M2 does not name these: the model declares the slots and
    /// the display row fills them, which is how one model serves a dozen
    /// recolours. Anything loading a creature model outside the spawner needs
    /// them too, or it draws an untextured shape - which is what the target
    /// frame's portrait did until this was shared.
    ///
    /// Paths are resolved against the model's own directory, and only ones the
    /// install actually has are returned.
    std::vector<std::pair<uint32_t, std::string>>
    getCreatureSkinPaths(uint32_t displayId, const std::string& modelPath) const;

    /// A humanoid creature's appearance, if this display is one.
    ///
    /// Most NPCs worth looking at are: a guard, a questgiver, an innkeeper.
    /// CreatureDisplayInfoExtra gives them a race, a sex and the same skin,
    /// face, hair and facial-hair choices a player character has - so the way
    /// to draw one is the way a character is drawn, not the way a creature is.
    /// A creature's skin fields are empty for these, which is why loading one
    /// as a creature gives an untextured shape.
    ///
    /// The appearance bytes are packed as a character's are: skin, face, hair
    /// style and hair colour, one byte each from the bottom up.
    /// The pre-composited skin for a humanoid display, or empty.
    ///
    /// This is the whole appearance already baked - skin, face, hair and the
    /// armour the NPC wears - which is how the game draws them and how this
    /// client can, without compositing anything.
    std::string getHumanoidBakePath(uint32_t displayId) const;

    bool getHumanoidAppearance(uint32_t displayId, uint8_t& race, uint8_t& sex,
                               uint32_t& appearanceBytes, uint8_t& facialHair) const;

    /// What that humanoid is wearing, as (ItemDisplayInfo id, inventory type)
    /// pairs - the shape applyEquipment reads. CreatureDisplayInfoExtra holds
    /// eleven slots in its own order, and this is that order translated into
    /// the inventory types the rest of the client speaks.
    std::vector<std::pair<uint32_t, uint8_t>>
    getHumanoidEquipment(uint32_t displayId) const;
    std::string getGameObjectModelPathForDisplayId(uint32_t displayId) const;

    /// The hull a transport is drawn with, or "" to use the display lookup.
    ///
    /// A preloaded transport can be spawned before its displayId is known, so
    /// the vehicle has to be recognisable from its GameObject entry too.
    ///
    /// One function because there were two copies of this table, in
    /// entity_spawner_player.cpp and entity_spawner_processing.cpp, and the
    /// queued path in the second is the one most spawns actually take -
    /// so fixing the first left the bug on screen.
    static std::string transportModelPath(uint32_t entry, uint32_t displayId);
    audio::VoiceType detectVoiceTypeFromDisplayId(uint32_t displayId) const;

    /// Every other unit's weapons as its items and sheath state have them,
    /// re-dressed when either changes (0x0072dbc0, 0x00731f40): creatures
    /// from UNIT_VIRTUAL_ITEM_SLOT_ID (classic and TBC: _SLOT_DISPLAY and
    /// _INFO), players from their PLAYER_VISIBLE_ITEM entries. The state is
    /// the client's own for each unit (CGUnit_C +0xb5c), taken from
    /// UNIT_FIELD_BYTES_2 byte 0 when its model is made (0x0073f660) and
    /// whenever the field changes (0x00737aa0), and changed as the client
    /// changes it: by its animation, cast or attack (0x00738180), its stand
    /// state (0x0073f060) and a channel at a fishing bobber (0x0073a520).
    /// Once a frame.
    void updateUnitWeapons();
    /// A spell's cast beginning on a unit other than the active player
    /// (0x007fa2e0, with 0x0073a6c0 for its kits).
    void onUnitSpellCastBegin(uint64_t guid, uint32_t spellId);
    /// A unit's melee swing (SMSG_ATTACKERSTATEUPDATE): melee drawn
    /// (0x00756800 case 0x14a).
    void onUnitAttackSwing(uint64_t guid);
    /// What 0x007fa2e0 and 0x0073a6c0 read of a spell cast by a unit of
    /// this display: Spell.dbc's Attributes, its visual's missile model and
    /// whether its precast or cast kit has a weapon effect, and the
    /// display's CreatureModelData flag 0x10.
    SpellSheathInput spellSheathInput(uint32_t spellId, uint32_t displayId) const;
    /// ChrClasses Flags (+0x24) without 8: the class may draw a ranged
    /// weapon (0x00736d30).
    bool classMayDrawRanged(uint32_t classId) const;
    /// Paints a tabard again as the client does (0x006db510): a player's
    /// when its guild changes (0x006e1bb0), a player's or corpse's when its
    /// guild's design arrives or changes (0x006d2840, 0x00705950, and
    /// 0x006e1b40 for the guild's members after a PLAYER_GUILD_TIMESTAMP
    /// the client had not seen asked for it again, 0x006e1c60). Once a
    /// frame.
    void refreshGuildTabards();
    /// The BehaviorID (AnimationData +0x18) of the animation an instance
    /// plays, or kNoAnimationBehavior (0x00721ed0's reading of it).
    uint32_t animationBehavior(uint32_t instanceId) const;
    /// The animation an instance plays as 0x00738180 reads it: its id, and
    /// its AnimationData WeaponFlags (+8) and BehaviorID (+0x18).
    struct AnimationRecord {
        uint32_t animId = 0xFFFFFFFFu;
        bool known = false;
        uint32_t weaponFlags = 0;
        uint32_t behavior = kNoAnimationBehavior;
    };
    AnimationRecord animationRecord(uint32_t instanceId) const;
    /// Another unit's sheath state as the client keeps it (+0xb5c), where
    /// it has one.
    [[nodiscard]] std::optional<SheathState> unitSheathState(uint64_t guid) const {
        auto it = unitSheath_.find(guid);
        if (it == unitSheath_.end()) return std::nullopt;
        return it->second.state;
    }
    /// A display's CreatureModelData Flags (+4); nothing when a record is
    /// missing (0x00717a20).
    std::optional<uint32_t> creatureModelFlags(uint32_t displayId) const;
    /// Another unit's combat ready for the weapons it was last dressed in
    /// (0x00714dd0); ReadyUnarmed before that.
    [[nodiscard]] uint32_t unitMeleeReady(uint64_t guid) const {
        auto it = unitWeaponsShown_.find(guid);
        return it != unitWeaponsShown_.end() ? it->second.meleeReady : 25u;
    }
    /// Whether the unit a CharacterRenderer instance draws holds a kit's
    /// weapon effects (CreatureModelData +4 without 0x10, 0x0073a6c0), and
    /// then its AttachedEffectScale (+0x60, 0x006f8c50).
    std::optional<float> kitWeaponEffectHolder(uint32_t renderInstanceId) const;
    /// The unit's CreatureModelData AttachedEffectScale (+0x60), which sizes
    /// every kit model hung on it (0x006f8c50); 1 when it has none.
    float kitAttachedEffectScale(uint32_t renderInstanceId) const;
    /// Its WorldEffectScale (+0x5c), which sizes the kit models it places
    /// in the world (0x006f7950).
    float kitWorldEffectScale(uint32_t renderInstanceId) const;
    /// The creature cache's type flags (+0x964 +0xc) of the creature an
    /// instance draws; 0 for a player or one not cached.
    uint32_t creatureTypeFlags(uint32_t renderInstanceId) const;
    /// Whether another unit shows its SpellVisual Flags 8 state kits
    /// (0x00720400): idle, its weapons away and casting nothing.
    bool unarmedKitsShown(uint64_t guid) const;
    /// The unit a CharacterRenderer instance draws, 0 for none.
    uint64_t unitGuidForInstance(uint32_t renderInstanceId) const;
    /// Whether another unit has its melee weapons in its hands (+0xb5c 1).
    bool unitMeleeDrawn(uint64_t guid) const;
    /// Its CreatureModelData GeoBox height, +0x58 less +0x4c (0x00717ad0);
    /// 0 when it has none.
    float unitGeoBoxHeight(uint32_t renderInstanceId) const;

    /// Every unit's and corpse's blob shadow for this frame (0x00793980): a
    /// corpse that is not bones by the bounds of its model's sequence; a unit by its
    /// CreatureModelData box on the instance it is drawn with - the mount's
    /// when it rides one - or none. `localPlayerInstance` is the renderer's
    /// own character.
    void updateBlobShadows(uint32_t localPlayerInstance);

    /// The sounds this frame's model events play through the units they
    /// belong to (unit_model_events.hpp): the unit's own model's, and its
    /// mount's (0x0073d5d0 hands the mount the unit's callback).
    /// `localPlayerInstance` is the renderer's own character.
    void playUnitModelEvents(uint32_t localPlayerInstance);

    // Mount
    void setMountDisplayId(uint32_t displayId) { pendingMountDisplayId_ = displayId; }
    uint32_t getMountInstanceId() const { return mountInstanceId_; }
    uint32_t getMountModelId() const { return mountModelId_; }

    struct RemotePlayerMount {
        uint32_t displayId = 0;
        uint32_t modelId = 0;
        uint32_t instanceId = 0;
        float riderHeight = 0.0f;  ///< the seat over the mount's feet, by its size
        float seatZ = 0.0f;        ///< the seat's height in the mount model
        float scale = 1.0f;        ///< the mount's size (0x0071c0e0)
    };
    void setRemotePlayerMountDisplayId(uint64_t guid, uint32_t displayId);
    /// A rider kept out of the world until its mount is drawn under it
    /// (rider_mount_wait): its model, name, shadow and circle.
    bool isUnitHeldForMount(uint64_t guid) const { return riderMountWaits_.count(guid) > 0; }
    /// The pose a unit's rider holds on its mount (+0xb7c, 0x0073d5d0):
    /// Mount, or what a mount aura's kit set where the rider's model has it.
    uint32_t riderPose(uint64_t guid, uint32_t riderInstance) const;
    /// A kit's worn head item on an NPC dressed by its
    /// CreatureDisplayInfoExtra (CharProc 17, 0x006f82d0); 0 gives it its
    /// own NPCItemDisplay head back (0x00723730).
    void setNpcHeadItem(uint64_t guid, uint32_t displayId);
    /// A display's model for the character renderer, as a mount's is loaded
    /// (its skins from CreatureDisplayInfo); 0 where it cannot be.
    uint32_t loadMountModel(uint32_t displayId);
    /// The size a unit's model is drawn at (0x0071c0e0 while it rides
    /// nothing): what its instance was given, 1 for one drawn at 1.
    float unitModelScale(uint64_t guid) const;
    /// A unit mounted on a display now, or put down for 0 - a mount
    /// transition handing it over (0x007412b0, 0x00740450).
    void mountUnitNow(uint64_t guid, uint32_t displayId, bool localPlayer);
    /// The instance drawing another unit's mount - a player's or a
    /// creature's, for CGUnit_C mounts both alike (0x00740450, 0x0073d5d0)
    /// - 0 while it rides none.
    uint32_t remotePlayerMountInstance(uint64_t guid) const {
        auto it = remotePlayerMounts_.find(guid);
        return it != remotePlayerMounts_.end() ? it->second.instanceId : 0u;
    }
    const RemotePlayerMount* getRemotePlayerMount(uint64_t guid) const {
        auto it = remotePlayerMounts_.find(guid);
        return it != remotePlayerMounts_.end() ? &it->second : nullptr;
    }

    /// Every unit in a vehicle's seat drawn there (0x0074a7f0 state 3):
    /// hung on the vehicle model's seat attachment, playing the seat's ride
    /// animation, mirroring the vehicle where the seat says so, hidden where
    /// it says so; and let go when it leaves the seat. Each frame, after the
    /// units are placed (entity_spawner_vehicle.cpp).
    void updateVehiclePassengers();
    /// The local player's passenger state while it is in or on its way into
    /// or out of a seat (CGUnit_C +0xf60 +0x14), and where it is drawn.
    [[nodiscard]] std::optional<game::vehicle::SeatState> localPassengerState() const;
    [[nodiscard]] std::optional<glm::vec3> localPassengerDrawnPosition() const;
    /// What the local passenger's camera looks at, as its vehicle camera
    /// (VehicleCamera_C) has it this frame: seated, the seat's CameraOffset
    /// about it chased at the seat's CameraPosChaseRate (0x00759200,
    /// 0x00759d80) and its facing at CameraFacingChaseRate (0x0075a1c0) -
    /// how far that facing turned since the last frame, which the camera
    /// turns with (0x00604490); on its way in or out, where it is drawn.
    /// It runs out its way out after the passenger is gone, and ends with
    /// it (0x0075af40 case 5). `keysTurn` and `freeLook` as
    /// stepCameraFacing takes them.
    struct VehicleCameraView {
        glm::vec3 pivot{0.0f};
        float turned = 0.0f;  ///< radians, render yaw
        /// Where its pull-in looks from (0x00759580): the vehicle's place
        /// raised by its height (0x00718080); none on the way in or out.
        std::optional<glm::vec3> collisionOrigin;
    };
    [[nodiscard]] std::optional<VehicleCameraView> localVehicleCameraView(bool keysTurn, bool freeLook);
    /// The vehicle model the camera's closeness fades for the local player
    /// - the one it sits in or drives (0x004f8660) - and its row's fade
    /// scalars where Flags 0x80000 gives them (0x00606f90).
    struct CameraFadeVehicle {
        uint32_t instanceId = 0;
        std::optional<glm::vec2> scalars;
        /// Its passengers' models, and theirs, which 0x00737390 fades with it.
        std::vector<uint32_t> riders;
    };
    [[nodiscard]] std::optional<CameraFadeVehicle> localCameraFadeVehicle();
    /// Whether a monster move taking `mover` to `transport`'s `seat` waits on
    /// its vehicle's enter or exit animation (0x0074bcb0); where it does the
    /// vehicle plays it and the move is let go after the seat's delay, at the
    /// animation's event for the seat, at its end or after three seconds
    /// (0x0074be10, 0x00748070, 0x00757060 / 0x007570f0, 0x00757280).
    bool holdVehicleMove(uint64_t mover, uint64_t transport, uint8_t seat);
    /// The local player's seat camera, due now (0x0075aac0 -> 0x0075a630
    /// -> 0x0074c0e0): whether it is in a seat then (states 1 to 3), the
    /// seat, and the time the camera takes. Taken once.
    struct SeatCameraCue {
        bool seated = false;
        const game::vehicle::SeatRecord* seat = nullptr;
        const game::vehicle::VehicleRecord* vehicle = nullptr;
        float seconds = 0.0f;
    };
    [[nodiscard]] std::optional<SeatCameraCue> takeSeatCameraCue();
    /// A seat change whose camera move is still to come.
    [[nodiscard]] bool seatCameraCuePending() const { return localSeatCamera_.pending; }

    // Local player GUID exclusion (EntitySpawner skips spawning the local player)
    void setLocalPlayerGuid(uint64_t guid) { spawnedPlayerGuid_ = guid; }

    // Weapon model IDs (shared counter for creature weapons + equipment helm/weapon models)
    uint32_t allocateWeaponModelId() { return nextWeaponModelId_++; }

    // Maximum weapon attachment operations per tick (used by Application update loop)
    static constexpr int MAX_WEAPON_ATTACHES_PER_TICK = 2;

    // Dead creature tracking (spawn in corpse/death pose)
    void markCreatureDead(uint64_t guid) { deadCreatureGuids_.insert(guid); }
    void unmarkCreatureDead(uint64_t guid) { deadCreatureGuids_.erase(guid); }
    void clearDeadCreatureGuids() { deadCreatureGuids_.clear(); }

    // Mount state management
    void clearMountState();

    // Transport registration API (used by setupUICallbacks transport spawn callback)
    void queueTransportRegistration(uint64_t guid, uint32_t entry, uint32_t displayId,
                                     float x, float y, float z, float orientation);
    void setTransportPendingMove(uint64_t guid, float x, float y, float z, float orientation);
    bool hasTransportRegistrationPending(uint64_t guid) const;
    void updateTransportRegistration(uint64_t guid, uint32_t displayId,
                                      float x, float y, float z, float orientation);

    // Test transport setup flag
    bool isTestTransportSetup() const { return testTransportSetup_; }
    void setTestTransportSetup(bool v) { testTransportSetup_ = v; }

    // Creature animation state accessors (for movement sync in Application::update)
    std::unordered_map<uint64_t, uint32_t>& getCreatureInstances() { return creatureInstances_; }
    const std::unordered_map<uint64_t, uint32_t>& getCreatureInstances() const { return creatureInstances_; }
    std::unordered_map<uint64_t, uint32_t>& getCreatureModelIds() { return creatureModelIds_; }
    const std::unordered_map<uint64_t, uint32_t>& getCreatureDisplayIds() const { return creatureDisplayIds_; }
    std::unordered_map<uint64_t, glm::vec3>& getCreatureRenderPosCache() { return creatureRenderPosCache_; }
    std::unordered_map<uint64_t, bool>& getCreatureWasMoving() { return creatureWasMoving_; }
    /// A unit's drawn body behind its facing (FUN_0073dab0), for the
    /// instance it was turned for and the key bones that instance has.
    struct UnitBody {
        rendering::body_yaw::State state;
        uint32_t instanceId = 0;
        bool hasSpine = false;
        bool hasHead = false;
        /// Shuffling round on the spot (+0xa38 0x1800), as last turned, and
        /// to the left (0x800).
        bool turning = false;
        bool turningLeft = false;
    };
    std::unordered_map<uint64_t, UnitBody>& getUnitBodies() { return unitBodies_; }
    std::unordered_map<uint64_t, bool>& getCreatureWasSwimming() { return creatureWasSwimming_; }
    std::unordered_map<uint64_t, bool>& getCreatureWasFlying() { return creatureWasFlying_; }
    std::unordered_map<uint64_t, bool>& getCreatureWasWalking() { return creatureWasWalking_; }
    std::unordered_map<uint64_t, bool>& getCreatureSwimmingState() { return creatureSwimmingState_; }
    std::unordered_map<uint64_t, bool>& getCreatureWalkingState() { return creatureWalkingState_; }
    std::unordered_map<uint64_t, bool>& getCreatureFlyingState() { return creatureFlyingState_; }
    std::unordered_map<uint64_t, uint32_t>& getCreatureActiveEmotes() { return creatureActiveEmotes_; }
    std::unordered_map<uint32_t, bool>& getModelIdIsWolfLike() { return modelIdIsWolfLike_; }

    // Player instance accessors (for movement sync in Application::update)
    std::unordered_map<uint64_t, uint32_t>& getPlayerInstances() { return playerInstances_; }
    const std::unordered_map<uint64_t, uint32_t>& getPlayerInstances() const { return playerInstances_; }

    // GameObject instance accessors
    auto& getGameObjectInstances() { return gameObjectInstances_; }
    const auto& getGameObjectInstances() const { return gameObjectInstances_; }
    /// The placement angles a game object is drawn at: its update block's
    /// rotation (0x00714250 keeps it, 0x00712ee0 draws by it), or a turn to
    /// its facing where the block carried none.
    [[nodiscard]] glm::vec3 gameObjectPlacement(uint64_t guid, float canonicalYaw) const;

    // Display data accessors (needed by Application for gryphon/wyvern display IDs)
    const auto& getDisplayDataMap() const { return displayDataMap_; }

    /// Everything needed to draw a creature somewhere other than the world:
    /// the model it uses and the skins that go on it.
    ///
    /// Both halves already live here - CreatureDisplayInfo.dbc gives the model
    /// id and the skin names, CreatureModelData.dbc turns the model id into a
    /// path - and they were reachable only from inside the spawner. A preview
    /// frame wants the same answer and reading the two files again would put a
    /// second copy of the layout somewhere else, which is how a display id ends
    /// up meaning two different creatures.
    struct CreatureModel {
        std::string m2Path;
        std::string skin1, skin2, skin3;
        uint32_t    modelId = 0;
        /// A model with no skin draws as a white shape, which is worse than an
        /// empty frame - so a caller with nowhere to get textures should treat
        /// this as nothing to draw.
        [[nodiscard]] bool valid() const { return !m2Path.empty() && !skin1.empty(); }
    };
    uint32_t getGryphonDisplayId() const { return gryphonDisplayId_; }
    uint32_t getWyvernDisplayId() const { return wyvernDisplayId_; }

    // Character section lookups (needed for player character skin compositing)
    /// The key for charSectionsCache_, in one place.
    ///
    /// It was written out twice - once where the cache is filled and once where
    /// it is read - and the two had to agree bit for bit or every lookup missed
    /// silently. Two copies of a packing rule is the same hazard as two copies
    /// of any other rule, and cheaper to remove than to remember.
    static constexpr uint64_t charSectionKey(uint8_t race, uint8_t sex, uint8_t section,
                                             uint8_t variation, uint8_t color, int texIndex) {
        return (static_cast<uint64_t>(race) << 26) |
               (static_cast<uint64_t>(sex & 0xF) << 22) |
               (static_cast<uint64_t>(section & 0xF) << 18) |
               (static_cast<uint64_t>(variation & 0xFF) << 10) |
               (static_cast<uint64_t>(color & 0xFF) << 2) |
               static_cast<uint64_t>(texIndex);
    }

    std::string lookupCharSection(uint8_t race, uint8_t sex, uint8_t section,
                                  uint8_t variation, uint8_t color, int texIndex = 0) const;
    /// CharHairGeosets and CharacterFacialHairStyles, as the character
    /// component reads them (core::characterGeosetDefaults).
    const core::AppearanceGeosetTables& getAppearanceGeosetTables() const { return appearanceTables_; }


    // Creature M2 sync loader (used by spawnPlayerCharacter in Application)

private:
    /// A CreatureModelData float of the unit an instance draws, 1 where
    /// it has none.
    float kitModelDataScale(uint32_t renderInstanceId, const char* column) const;
    /// Each player's equipment as the server last said, and the spell's
    /// worn items over it.
    struct ServerEquipment {
        std::array<uint32_t, 19> displayInfoIds{};
        std::array<uint8_t, 19> inventoryTypes{};
    };
    std::unordered_map<uint64_t, ServerEquipment> serverEquipment_;
    std::unordered_map<uint64_t, std::map<int, std::pair<uint32_t, uint8_t>>> playerItemOverrides_;
    void queueDressedEquipment(uint64_t guid);
    /// A CreatureModelData float column of the unit an instance draws.
    std::optional<float> modelDataColumn(uint32_t renderInstanceId, const char* column) const;
    /// Vehicle.dbc and VehicleSeat.dbc (0x00654800, 0x00654a60), loaded once.
    void loadVehicleTables();
    game::vehicle::VehicleTables vehicleTables_;
    bool vehicleTablesLoaded_ = false;
    /// A unit's passenger component (CGUnit_C +0xf60): its way into, in and
    /// out of a seat, and what is drawn of it.
    struct SeatedPassenger {
        game::vehicle::SeatState state = game::vehicle::SeatState::None;  ///< +0x14
        uint64_t vehicleGuid = 0;      ///< +0x28: the vehicle it goes to or sits in
        uint8_t seat = 0xff;           ///< +0x31
        uint64_t prevVehicleGuid = 0;  ///< +0x20: the one it comes from
        uint8_t prevSeat = 0xff;       ///< +0x30
        uint32_t seatId = 0;           ///< +0x54: the seat its moves read
        uint64_t rootGuid = 0;         ///< +0x58: the vehicle its seats count from
        bool voluntary = false;        ///< movement flags2 0x40 at the change
        uint32_t startMs = 0;          ///< +0x34
        uint32_t endMs = 0;            ///< +0x38
        glm::vec3 from{0.0f};          ///< +0x84
        uint64_t anchorGuid = 0;       ///< +0x40: the transport `from` rides on
        glm::vec3 fromLocal{0.0f};     ///< +0x90: `from` less the anchor's place
        float startFacing = 0.0f;      ///< +0xb4
        float seatFacing = 0.0f;       ///< +0xb8
        float drawnFacing = 0.0f;      ///< +0xbc
        game::vehicle::Arc arc;
        bool planned = false;          ///< +0x10 0x1: 0x0074a200 has run for the state
        // What is drawn of it.
        uint32_t instanceId = 0;
        bool hidden = false;
        uint32_t rideLoopVehicleInstance = 0;  ///< +0x70: the vehicle its seat animates
        int32_t rideLoopBone = -1;
    };
    std::unordered_map<uint64_t, SeatedPassenger> seatedPassengers_;
    /// The local player's camera move, until it is due and taken.
    struct LocalSeatCamera {
        bool pending = false;
        uint32_t startMs = 0;
        uint32_t durationMs = 0;
        bool fixed = false;
        bool waitsForFlight = false;
    };
    LocalSeatCamera localSeatCamera_;
    /// The local passenger's vehicle camera's chase (VehicleCamera_C).
    struct LocalVehicleCamera {
        bool snap = true;                  ///< +0x8 0x20: the next update starts the chase over
        glm::vec3 rel{0.0f};               ///< +0x64: on the vehicle
        glm::vec3 relVelocity{0.0f};       ///< +0x70
        game::vehicle::CameraFacing facing;  ///< +0x7c..+0x98, +0x8 0x2 / 0x4
        uint32_t seatId = 0;               ///< +0x10: the seat of the last change
        bool watchesVehicle = false;       ///< +0xb0: the vehicle, from a control seat (0x0075a930)
        uint32_t lastMs = 0;               ///< +0xa0
        bool active = false;               ///< the camera is there (CGUnit_C +0xf64)
        game::vehicle::CameraModeClock clock;  ///< +0xa4, +0xa8, +0xac
        bool fixed = false;                ///< +0x8 0x1: durationMs is final
        uint32_t durationMs = 0;           ///< +0x9c
        glm::vec3 point{0.0f};             ///< +0x2c: what it looks at
        glm::vec3 start{0.0f};             ///< +0x20: `point` at the change, on startFrame
        uint64_t startFrame = 0;           ///< the transport it is held on, 0 for the world
        glm::vec3 spring{0.0f};            ///< +0x4c: on its way, on startFrame
        glm::vec3 springVelocity{0.0f};    ///< +0x58
    };
    LocalVehicleCamera localVehicleCamera_;
    /// Where a transport is drawn; the world's origin for none.
    [[nodiscard]] glm::vec3 framePosition(uint64_t guid) const;
    /// The transport a vehicle is on, none for the world (vfunc +0x40).
    [[nodiscard]] uint64_t vehicleTransportOf(uint64_t vehicleGuid) const;
    /// A passenger's move held for its vehicle's animation (the passenger
    /// component's +0x10 0x200 and what it keeps beside it).
    struct HeldVehicleMove {
        uint64_t vehicleGuid = 0;          ///< +0x68
        uint32_t vehicleInstance = 0;
        int32_t bone = 0;
        uint32_t animation = 0;
        bool timed = false;                ///< +0x10 0x80
        uint32_t startMs = 0;
        uint32_t releaseMs = 0;            ///< +0x64
        std::optional<uint32_t> eventMs;   ///< the "$VG"/"$VT" event for its seat
    };
    std::unordered_map<uint64_t, HeldVehicleMove> heldVehicleMoves_;
    void stepHeldVehicleMoves(uint32_t nowMs);
    void releaseSeatedPassenger(uint64_t guid, SeatedPassenger& seated, bool landing);
    /// The vehicle a unit's seats count from while it is in a passenger
    /// state, else the one it would (0x00748040, 0x0074c650).
    uint64_t seatRootOf(uint64_t guid) const;
    /// 0x007489c0: the passenger to a new state, at `nowMs`.
    void setSeatState(uint64_t guid, SeatedPassenger& p, game::vehicle::SeatState state, uint32_t nowMs);
    /// UNIT_ENTERING_VEHICLE, UNIT_ENTERED_VEHICLE, UNIT_EXITING_VEHICLE and
    /// UNIT_EXITED_VEHICLE for a change of state of the player or a group
    /// member (0x0074ad70 -> 0x00749fb0 -> 0x00749650).
    void fireSeatEvents(uint64_t guid, const SeatedPassenger& p, game::vehicle::SeatState from,
                        game::vehicle::SeatState to);
    /// A SoundEntries.dbc row's Name, "" for none.
    std::string soundEntryName(uint32_t id);
    std::unordered_map<uint32_t, std::string> soundEntryNames_;
    bool soundEntryNamesLoaded_ = false;
    /// The vehicle's animation for a seat taken (0x00756f80) and let go
    /// (0x007577e0).
    void startVehicleRideLoop(SeatedPassenger& p, const game::vehicle::SeatRecord& seat);
    void stopVehicleRideLoop(uint64_t guid, SeatedPassenger& p);
    /// 0x0074a200: a new state's time and arc.
    void planSeatState(uint64_t guid, SeatedPassenger& p, uint32_t nowMs);
    /// The model a vehicle is drawn by: its mount's while it rides one.
    uint32_t vehicleInstanceFor(uint64_t guid) const;
    /// A unit's place and facing as drawn by its own movement.
    glm::vec3 unitRenderPosition(uint64_t guid) const;

    // Dependencies (non-owning)
    rendering::Renderer* renderer_;
    pipeline::AssetManager* assetManager_;
    game::GameHandler* gameHandler_;
    game::GameServices* gameServices_;

    // --- Creature display data (from DBC files) ---
    struct CreatureDisplayData {
        uint32_t modelId = 0;
        std::string skin1, skin2, skin3;  // Texture names from CreatureDisplayInfo.dbc
        uint32_t extraDisplayId = 0;      // Link to CreatureDisplayInfoExtra.dbc
        /// CreatureDisplayInfo.CreatureModelScale, which is how one model is
        /// reused at different sizes: the boar model is 0.6 for a piglet and
        /// 1.5 for a giant, over ten displays. Nothing read it, so every
        /// creature sharing a model drew at that model's own size.
        float displayScale = 1.0f;
        /// CreatureDisplayInfo.ParticleColorID: the ParticleColor record that
        /// recolours the model's emitters naming slot 11, 12 or 13.
        uint32_t particleColorId = 0;
    };

    /// CreatureModelData.ModelScale for a model id, the second half of the
    /// same product. Kept beside the display data rather than in it because it
    /// belongs to the model and several displays share one.
    /// CreatureModelData.ModelScale for the model a display names.
    ///
    /// Keyed by the display, not by a model id: the renderer's model handles
    /// are a counter of its own and share no numbering with the DBC, so
    /// asking this with one silently answered 1.0 for every creature there
    /// has ever been. 306 displays want 1.25 and two want 2.0.
    float creatureModelScale(uint32_t displayId) const;

    /// CreatureDisplayInfo.CreatureModelScale for a display id, or 1.0 where
    /// the display is unknown - an unknown display is already drawn at the
    /// model's own size, and guessing smaller would hide it.
    float creatureDisplayScale(uint32_t displayId) const;


    /// The size the client draws a creature at, before the server's
    /// OBJECT_FIELD_SCALE_X: display scale times model scale, overridden by
    /// the creature's beast family where that is larger or it is a pet.
    float creatureRenderScale(uint64_t guid, uint32_t displayId) const;
    float creatureRenderScaleFor(const game::Entity* entity, uint32_t displayId) const;

    /// Re-apply creatureRenderScale to spawned creatures whose level, family
    /// or server scale has changed or arrived since they were spawned.
    void refreshCreatureScales();

    /// Apply the textures a creature display names: its own skin variations,
    /// and for a humanoid the composited body, face, hair and equipment. Once
    /// per display, since the model is shared by every creature using it.
    /// Start a creature in the pose the server already has it in - dead,
    /// mid-emote, or newly arrived, and fade it in.
    void playCreatureSpawnPose(uint64_t guid, uint32_t instanceId);

    /// Choose one mesh per clothing group for a character-style NPC, leaving
    /// every other batch of the model untouched. An NPC has no inventory to
    /// build a geoset set from, so its equipment comes from
    /// CreatureDisplayInfoExtra.
    void normalizeHumanoidClothingGeosets(uint32_t instanceId, uint32_t modelId,
                                          uint32_t displayId);

    /// Per-instance colouring of a humanoid NPC: hair, skin, and the
    /// head-detail sheet. Per instance rather than per model, so two NPCs
    /// sharing a model still get their own hair colour.
    void applyHumanoidInstanceOverrides(uint32_t instanceId, uint32_t modelId,
                                        uint32_t displayId);

    void applyCreatureDisplayTextures(uint32_t displayId, uint32_t modelId,
                                      const CreatureDisplayData& dispData);

    struct HumanoidDisplayExtra {
        uint8_t raceId = 0;
        uint8_t sexId = 0;
        uint8_t skinId = 0;
        uint8_t faceId = 0;
        uint8_t hairStyleId = 0;
        uint8_t hairColorId = 0;
        uint8_t facialHairId = 0;
        std::string bakeName;  // Pre-baked texture path if available
        // Equipment display IDs (from columns 8-18)
        // 0=helm, 1=shoulder, 2=shirt, 3=chest, 4=belt, 5=legs, 6=feet, 7=wrist, 8=hands, 9=tabard, 10=cape
        uint32_t equipDisplayId[11] = {0};
    };
    std::unordered_map<uint32_t, CreatureDisplayData> displayDataMap_;  // displayId → display data
    std::unordered_map<uint32_t, HumanoidDisplayExtra> humanoidExtraMap_;  // extraDisplayId → humanoid data
    std::unordered_map<uint32_t, std::string> modelIdToPath_;   // modelId → M2 path (from CreatureModelData.dbc)
    std::unordered_map<uint32_t, float> modelIdToScale_;        // modelId → CreatureModelData.ModelScale
    /// CreatureModelData's GeoBox (+0x44) and MountHeight (+0x40), by model id.
    struct ModelGeoBox {
        rendering::blob_shadow::Box box;
        float mountHeight = 0.0f;
    };
    std::unordered_map<uint32_t, ModelGeoBox> modelIdToGeoBox_;
    /// The CreatureModelData box of the model a display names, if it has one.
    const ModelGeoBox* geoBoxForDisplay(uint32_t displayId) const;
    // CreatureFamily.dbc: the size range a beast family grows through by level.
    struct FamilyScale {
        float minScale = 1.0f;
        int32_t minScaleLevel = 0;
        float maxScale = 1.0f;
        int32_t maxScaleLevel = 0;
    };
    std::unordered_map<uint32_t, FamilyScale> familyScale_;      // familyId → scale range
    core::AppearanceGeosetTables appearanceTables_;
    // What a humanoid NPC will wear, prepared off the main thread before it
    // spawns (see core::AssetPrefetch), and per display what that is.
    std::unique_ptr<core::AssetPrefetch> assetPrefetch_;
    struct DressFiles {
        std::vector<std::string> models;
        std::vector<std::string> textures;
    };
    std::unordered_map<uint32_t, DressFiles> creatureDressFiles_;
    /// Whether everything a display's NPC wears has been prepared; asks for
    /// what has not. True for a display that wears nothing of the kind.
    bool creatureDressPrepared(uint32_t displayId);
    bool dressPrefetchBypass_ = false;
    // update()'s stages, timed for takeStageTimes.
    static constexpr int kUpdateStageCount = 8;
    double updateStageMs_[kUpdateStageCount] = {};
public:
    /// Behind a loading screen nothing is seen to stutter, and the warmup
    /// waits for the spawn queue to drain: NPCs are dressed on the spot there
    /// rather than held for the prefetch workers, which would only keep the
    /// loading screen up longer.
    void setDressPrefetchBypass(bool bypass) { dressPrefetchBypass_ = bypass; }
private:
    /// A unit's name attachment (0x00719310): PlayerNameMounted on its
    /// mount's model while it rides, else PlayerName. `mountInstance` is the
    /// mount it found, 0 on foot.
    std::optional<glm::mat4> unitNameAttachment(uint64_t guid, uint32_t& mountInstance) const;
    /// A player's character component before its equipment: race, sex,
    /// class, skin, face and the hair and facial rows (PLAYER_BYTES).
    core::CharacterLook playerLook(uint64_t guid, uint8_t raceId, uint8_t genderId,
                                   uint32_t appearanceBytes, uint8_t facialFeatures) const;
    /// The geoset ids of a loaded model's batches.
    std::vector<uint16_t> modelSubmeshIds(uint32_t modelId) const;
    bool creatureLookupsBuilt_ = false;

    // CharSections.dbc lookup cache
    std::unordered_map<uint64_t, std::string> charSectionsCache_;
    bool charSectionsCacheBuilt_ = false;
    void buildCharSectionsCache();

    // Creature display lookup building
    void buildCreatureDisplayLookups();

    // Weapon M2 loading helper
    bool loadWeaponM2(const std::string& m2Path, pipeline::M2Model& outModel);

    // GameObject display lookups
    std::unordered_map<uint32_t, std::string> gameObjectDisplayIdToPath_;
    bool gameObjectLookupsBuilt_ = false;
    void buildGameObjectDisplayLookups();

    // --- Creature instances and tracking ---
    // Drop the pending-spawn marker for guid unless a queue entry still references it.
    void erasePendingGuidIfUnqueued(uint64_t guid);

    std::unordered_map<uint64_t, uint32_t> creatureInstances_;  // guid → render instanceId
    std::unordered_map<uint64_t, uint32_t> creatureModelIds_;   // guid → loaded modelId
    std::unordered_map<uint32_t, uint32_t> npcHeadItems_;  // instance → a kit's head item display
    std::unordered_map<uint64_t, uint32_t> creatureDisplayIds_; // guid → active displayId
    // Latest server-requested display, including creatures whose async model load
    // has not completed yet. This prevents an older queued display from winning.
    std::unordered_map<uint64_t, uint32_t> requestedCreatureDisplayIds_;
    std::unordered_map<uint64_t, glm::vec3> creatureRenderPosCache_; // guid → last synced render position
    std::unordered_map<uint64_t, bool> creatureWasMoving_;
    std::unordered_map<uint64_t, UnitBody> unitBodies_;
    std::unordered_map<uint64_t, bool> creatureWasSwimming_;
    std::unordered_map<uint64_t, bool> creatureWasFlying_;
    std::unordered_map<uint64_t, bool> creatureWasWalking_;
    std::unordered_map<uint64_t, bool> creatureSwimmingState_;
    std::unordered_map<uint64_t, bool> creatureWalkingState_;
    std::unordered_map<uint64_t, bool> creatureFlyingState_;
    // Server waypoint scripts hold work/chop emotes for many seconds. Locomotion
    // temporarily overrides them, then the idle transition restores this loop.
    std::unordered_map<uint64_t, uint32_t> creatureActiveEmotes_;
    std::unordered_map<uint64_t, bool> creatureWasStealthed_;
    uint32_t stealthSyncFrameCounter_ = 0;  // throttles syncCreatureStealthVisuals()
    std::unordered_map<uint64_t, float> creatureAppliedScale_;  // guid → scale the instance was given
    // A creature's particle effects. The character renderer draws no
    // particles, so a creature whose model has emitters gets a particles-only
    // twin in the M2 renderer, posed from it every frame.
    std::unordered_map<uint64_t, uint32_t> creatureParticleTwins_;  // guid → M2 instance id
    std::unordered_map<uint32_t, std::array<glm::vec3, 9>> particleColors_;  // ParticleColor.dbc
    void spawnCreatureParticleTwin(uint64_t guid, uint32_t displayId, uint32_t charModelId,
                                   uint32_t charInstanceId);
    void syncCreatureParticleTwins();
    /// A particles-only twin of a character-renderer instance; 0 when its
    /// model has no emitters. See creatureParticleTwins_.
    uint32_t spawnParticleTwin(uint32_t displayId, uint32_t charModelId, uint32_t charInstanceId);
    /// Mounts' twins - a Felsteed's burning hooves - by the mount's own
    /// instance id, since a mount is not in creatureInstances_.
    std::unordered_map<uint32_t, uint32_t> mountParticleTwins_;
    void spawnMountParticleTwin(uint32_t displayId, uint32_t charModelId, uint32_t mountInstanceId);
    void removeMountParticleTwin(uint32_t mountInstanceId);
    /// The model of the game object a held ground-targeted spell summons, at
    /// the cursor's place (0x0080cce0 -> 0x007fe9e0, drawn by 0x004f66c0).
    void syncGroundTargetModel();
    uint32_t groundTargetModelEntry_ = 0;
    void removeCreatureParticleTwin(uint64_t guid);
    uint32_t scaleSyncFrameCounter_ = 0;  // throttles refreshCreatureScales()
    /// What updateUnitWeapons last dressed each unit with.
    struct UnitWeaponsShown {
        uint32_t instanceId = 0;
        uint32_t modelId = 0;
        std::array<uint32_t, 3> entries{};
        std::array<uint32_t, 6> info{};
        uint8_t sheathState = 0;
        uint32_t unitFlags = 0;
        uint32_t unitFlags2 = 0;
        bool offHandFollowsAnimation = false;
        /// The combat ready its weapons give it (0x00714dd0).
        uint32_t meleeReady = 25;
    };
    std::unordered_map<uint64_t, UnitWeaponsShown> unitWeaponsShown_;
    /// Each other unit's sheath state as the client keeps it (+0xb5c), and
    /// what it last saw of what changes it.
    struct UnitSheath {
        uint32_t instanceId = 0;
        SheathState state = SheathState::Melee;
        std::optional<SheathState> fieldSeen;
        uint8_t standSeen = 0;
        uint64_t channelObjectSeen = 0;
        /// What 0x00738180 last ran for.
        uint32_t animId = 0xFFFFFFFFu;
        uint32_t castSpellId = 0;
        bool attacking = false;
        /// +0xa30 0x10000 (animationKitIdle).
        bool kitIdle = false;
    };
    std::unordered_map<uint64_t, UnitSheath> unitSheath_;
    /// A unit's weapon slots as updateUnitWeapons reads them: item entries,
    /// or before WotLK displays with their UNIT_VIRTUAL_ITEM_INFO pairs.
    struct UnitWeaponEntries {
        std::array<uint32_t, 3> entries{};
        std::array<uint32_t, 6> info{};
        bool byDisplay = false;
    };
    std::optional<UnitWeaponEntries> unitWeaponEntries(uint64_t guid, const game::Entity& entity,
                                                       bool isPlayer) const;
    /// The slots' items (Sheath, InventoryType, class, subclass) and
    /// displays; false while an item is not known yet (its query is asked
    /// for).
    bool resolveUnitWeaponItems(const UnitWeaponEntries& slots, std::array<UnitWeaponItem, 3>& items,
                                std::array<uint32_t, 3>& displays) const;
    /// 0x00736d30 for another unit: its state changes to `requested` when
    /// the client lets it.
    void setUnitSheathState(uint64_t guid, UnitSheath& sheath, SheathState requested, bool fromServer);
    /// Corpses drawn through the player or creature paths, those through the
    /// creature path, and bones: M2 instances and their models by path.
    std::unordered_set<uint64_t> corpseGuids_;
    std::unordered_set<uint64_t> corpseCreatureGuids_;
    std::unordered_map<uint64_t, uint32_t> corpseBonesInstances_;
    std::unordered_map<std::string, uint32_t> corpseBonesModelIds_;
    /// Every corpse drawn: where it lies, the animation it lies in, and its
    /// loot sparkle's M2 instance (0 for none).
    struct CorpseSite {
        glm::vec3 position{0.0f};  ///< canonical
        uint32_t pose = 0;
        uint32_t sparkleInstance = 0;
        uint32_t guildId = 0;  ///< CORPSE_FIELD_GUILD
    };
    std::unordered_map<uint64_t, CorpseSite> corpseSites_;
    /// The loot sparkle's model ("HARDCODED Loot Art", 0x006f7520), loaded
    /// on first use; 0 when there is none.
    uint32_t lootSparkleModelId_ = 0;
    bool lootSparkleModelTried_ = false;
    /// The animation a corpse lies in, or nothing for a guid that is none.
    std::optional<uint32_t> corpsePose(uint64_t guid) const;
    /// Whether a corpse's display is a character model (CreatureModelData
    /// flag 4, 0x00705b20); nothing when its records are missing.
    std::optional<bool> corpseDisplayIsCharacter(uint32_t displayId) const;
    void spawnCorpseBones(uint64_t guid, const std::string& modelPath, float x, float y, float z,
                          float orientation);
    /// AnimationData.dbc, for animationBehavior; loaded on first use.
    mutable std::shared_ptr<pipeline::DBCFile> animationDataDbc_;
    mutable bool animationDataLoaded_ = false;
    /// Hangs a unit's three weapon slots as 0x0072dbc0 does. False while an
    /// item is not known yet (its query is asked for).
    bool dressUnitWeapons(uint32_t instanceId, const UnitWeaponEntries& slots, const UnitWeaponDress& dress);
    std::unordered_map<uint32_t, bool> modelIdIsWolfLike_;

    void syncCreatureStealthVisuals();

    // --- Creature async loads ---
    struct PreparedCreatureModel {
        uint64_t guid;
        uint32_t displayId;
        uint32_t modelId;
        float x, y, z, orientation;
        float scale = 1.0f;
        std::shared_ptr<pipeline::M2Model> model;
        std::unordered_map<std::string, pipeline::BLPImage> predecodedTextures;
        bool valid = false;
        bool permanent_failure = false;
    };
    struct AsyncCreatureLoad {
        std::future<PreparedCreatureModel> future;
    };
    std::vector<AsyncCreatureLoad> asyncCreatureLoads_;
    std::unordered_set<uint32_t> asyncCreatureDisplayLoads_;
    void processAsyncCreatureResults(bool unlimited = false);
    static constexpr int MAX_ASYNC_CREATURE_LOADS = 4;
    std::unordered_set<uint64_t> deadCreatureGuids_;

    // --- NPC equipment attachment models (helm, shoulders) ---
    // Equipment is shared across NPCs, so a crowd wearing the same gear would
    // otherwise re-read, re-parse and re-upload the same M2 once per spawn. Cache
    // it by resolved path. A modelId of 0 negative-caches a path that has no model,
    // so a missing file is not retried against the disk on every spawn.
    struct CachedAttachmentModel {
        uint32_t modelId = 0;                      // 0 = no usable model
        std::shared_ptr<pipeline::M2Model> model;  // shared_ptr: M2Model is incomplete here
    };
    // Parsed geometry, keyed by path. A null value negative-caches a missing file so
    // it is not probed against the disk on every spawn.
    std::unordered_map<std::string, std::shared_ptr<pipeline::M2Model>> attachmentModelData_;
    // Renderer model id, keyed by "path|texture". attachWeapon() binds the texture to
    // the model id, so two recolours of one mesh must not share an id or the last
    // NPC spawned would retexture every other NPC wearing that mesh.
    std::unordered_map<std::string, uint32_t> attachmentModelIds_;
    /// Resolve the first candidate path with a usable model, reading and parsing it
    /// at most once. Returns modelId == 0 when no candidate yields a valid model.
    CachedAttachmentModel getOrLoadAttachmentModel(
        const std::vector<std::string>& candidatePaths, const std::string& texturePath);

    std::unordered_map<uint32_t, uint32_t> displayIdModelCache_;
    std::unordered_set<uint32_t> displayIdTexturesApplied_;
    std::unordered_map<uint32_t, std::unordered_map<std::string, pipeline::BLPImage>> displayIdPredecodedTextures_;
    mutable std::unordered_set<uint32_t> warnedMissingDisplayDataIds_;
    mutable std::unordered_set<uint32_t> warnedMissingModelPathIds_;
    /// Says which model the first few humanoid displays resolved to, so an
    /// asset overlay that re-points them can be told apart from one that never
    /// reached the client. Five is enough to see and few enough to ignore.
    mutable int humanoidDisplayCanaryCount_ = 0;
    /// Says what the first few NPCs resolved for their head-detail texture.
    int npcHeadDetailCanaryCount_ = 0;
    uint32_t nextCreatureModelId_ = 5000;
    uint32_t gryphonDisplayId_ = 0;
    uint32_t wyvernDisplayId_ = 0;

    // --- Creature spawn queue ---
    struct PendingCreatureSpawn {
        uint64_t guid;
        uint32_t displayId;
        float x, y, z, orientation;
        float scale = 1.0f;
    };
    std::deque<PendingCreatureSpawn> pendingCreatureSpawns_;
    static constexpr int MAX_SPAWNS_PER_FRAME = 3;
    static constexpr auto CREATURE_SPAWN_RETRY_WINDOW = std::chrono::seconds(5);
    std::unordered_set<uint64_t> pendingCreatureSpawnGuids_;
    std::unordered_map<uint64_t, std::chrono::steady_clock::time_point>
        creatureSpawnRetryDeadlines_;
    /// How many retry windows a creature has already used up.
    ///
    /// Running out used to abandon the spawn for good, and nothing ever asked
    /// again: the server does not re-send an object that is already in range,
    /// so the creature stayed missing until something made it re-send - which
    /// is what walking out of the zone and back does, and why they "appear
    /// when I zone again". A window that expires now buys another one, up to
    /// this many, so a spawn that keeps failing is delayed rather than lost.
    std::unordered_map<uint64_t, int> creatureSpawnRetryWindowsUsed_;
    static constexpr int MAX_CREATURE_SPAWN_RETRY_WINDOWS = 6;
    std::unordered_set<uint32_t> nonRenderableCreatureDisplayIds_;
    std::unordered_set<uint64_t> creaturePermanentFailureGuids_;
    void processCreatureSpawnQueue(bool unlimited = false);

    // --- NPC composite loads ---
    struct DeferredNpcComposite {
        uint32_t modelId;
        uint32_t displayId;
        std::string basePath;
        std::vector<std::string> overlayPaths;
        std::vector<std::pair<int, std::string>> regionLayers;
        std::vector<uint32_t> skinTextureSlots;
        bool hasComposite = false;
        bool hasSimpleSkin = false;
        std::string bakedSkinPath;
        bool hasBakedSkin = false;
        std::vector<uint32_t> hairTextureSlots;
        std::string hairTexturePath;
        bool useBakedForHair = false;
    };
    struct PreparedNpcComposite {
        DeferredNpcComposite info;
        std::unordered_map<std::string, pipeline::BLPImage> predecodedTextures;
    };
    struct AsyncNpcCompositeLoad {
        std::future<PreparedNpcComposite> future;
    };
    std::vector<AsyncNpcCompositeLoad> asyncNpcCompositeLoads_;
    void processAsyncNpcCompositeResults(bool unlimited = false);

    // --- Player instances ---
    std::unordered_map<uint64_t, uint32_t> playerInstances_;  // guid → render instanceId
    struct OnlinePlayerAppearanceState {
        uint32_t instanceId = 0;
        uint32_t modelId = 0;
        uint8_t raceId = 0;
        uint8_t genderId = 0;
        uint32_t appearanceBytes = 0;
        uint8_t facialFeatures = 0;
        std::string bodySkinPath;
        std::vector<std::string> underwearPaths;
    };
    std::unordered_map<uint64_t, OnlinePlayerAppearanceState> onlinePlayerAppearance_;
    std::unordered_map<uint64_t, std::pair<std::array<uint32_t, 19>, std::array<uint8_t, 19>>> pendingOnlinePlayerEquipment_;
    /// Players and corpses painted wearing a tabard: what they wear, and
    /// the guild and design it was painted with.
    struct PaintedTabard {
        std::array<uint32_t, 19> displayIds{};
        std::array<uint8_t, 19> inventoryTypes{};
        uint32_t guildId = 0;
        std::optional<game::GuildEmblem> emblem;
        /// Frames left before a repaint that has not landed is asked again.
        uint16_t repaintWait = 0;
    };
    std::unordered_map<uint64_t, PaintedTabard> paintedTabards_;
    uint32_t tabardEmblemGeneration_ = 0;
    std::deque<std::pair<uint64_t, std::pair<std::array<uint32_t, 19>, std::array<uint8_t, 19>>>> deferredEquipmentQueue_;
    void processDeferredEquipmentQueue();
    struct PreparedEquipmentUpdate {
        uint64_t guid;
        std::array<uint32_t, 19> displayInfoIds;
        std::array<uint8_t, 19> inventoryTypes;
        std::unordered_map<std::string, pipeline::BLPImage> predecodedTextures;
    };
    struct AsyncEquipmentLoad {
        std::future<PreparedEquipmentUpdate> future;
    };
    std::vector<AsyncEquipmentLoad> asyncEquipmentLoads_;
    void processAsyncEquipmentResults();
    std::vector<std::string> resolveEquipmentTexturePaths(uint64_t guid,
        const std::array<uint32_t, 19>& displayInfoIds,
        const std::array<uint8_t, 19>& inventoryTypes) const;
    std::unordered_map<uint32_t, uint32_t> playerModelCache_;  // playerModelCacheKey → model id
    static uint32_t playerModelCacheKey(uint8_t raceId, uint8_t genderId) {
        return (static_cast<uint32_t>(raceId) << 8) | static_cast<uint32_t>(genderId);
    }
    /// The model a player of this race and gender is drawn with, or 0 when it
    /// is not on the GPU - never loaded, or dropped with the world it was in.
    uint32_t readyPlayerModelId(uint32_t cacheKey);

    // --- Player model async loads ---
    // A character model keeps nearly every sequence in an .anim file of its
    // own, and reading them all takes longer than a frame. They are read on a
    // worker, like a creature's, and the finished model is uploaded on the main
    // thread; the players waiting on it stay queued until then.
    struct PreparedPlayerModel {
        uint32_t cacheKey = 0;
        std::string m2Path;
        std::shared_ptr<pipeline::M2Model> model;
        bool valid = false;
    };
    std::unordered_map<uint32_t, std::future<PreparedPlayerModel>> asyncPlayerModelLoads_;  // by cache key
    std::unordered_set<uint32_t> failedPlayerModelKeys_;
    void processAsyncPlayerModelResults();
    static constexpr int MAX_ASYNC_PLAYER_MODEL_LOADS = 2;
    /// Which runtime texture slot of a player model takes which art. Type 8 is
    /// Skin Extra - the head detail sheet, not the underwear, though on the
    /// models the game shipped the underwear art is what ends up in it.
    struct PlayerTextureSlots { int skin = -1; int hair = -1; int skinExtra = -1; };
    std::unordered_map<uint32_t, PlayerTextureSlots> playerTextureSlotsByModelId_;
    uint32_t nextPlayerModelId_ = 60000;

    // --- Player spawn queue ---
    struct PendingPlayerSpawn {
        uint64_t guid;
        uint8_t raceId;
        uint8_t genderId;
        uint32_t appearanceBytes;
        uint8_t facialFeatures;
        float x, y, z, orientation;
    };
    std::deque<PendingPlayerSpawn> pendingPlayerSpawns_;
    std::unordered_set<uint64_t> pendingPlayerSpawnGuids_;
    void processPlayerSpawnQueue();

    // --- GameObject instances ---
    struct GameObjectInstanceInfo {
        uint32_t modelId = 0;
        uint32_t instanceId = 0;
        bool isWmo = false;
        float scale = 1.0f;
    };

    std::unordered_map<uint32_t, uint32_t> gameObjectDisplayIdModelCache_;
    std::unordered_set<uint32_t> gameObjectDisplayIdFailedCache_;
    std::unordered_map<uint32_t, uint32_t> gameObjectDisplayIdWmoCache_;
    std::unordered_map<uint64_t, GameObjectInstanceInfo> gameObjectInstances_;
    /// States that arrived before their model did, applied when it spawns.
    std::unordered_map<uint64_t, uint8_t> gameObjectPendingState_;
    /// The last GOState applied to each object, so a change knows where from.
    std::unordered_map<uint64_t, uint8_t> gameObjectServerState_;
    /// The client's animation state each object is in (0x00ada938's index,
    /// the type handler's +0x10).
    std::unordered_map<uint64_t, uint8_t> gameObjectAnimState_;
    /// When a custom animation an interaction started ends, and the object
    /// goes back to its server state's pose (0x0070d7e0).
    std::unordered_map<uint64_t, std::chrono::steady_clock::time_point> gameObjectCustomAnimEnd_;
    void updateGameObjectCustomAnims();
    /// A custom animation asked for while the model was still loading
    /// (GameObject_C +0x208), played as it arrives (0x00713f50).
    std::unordered_map<uint64_t, uint8_t> gameObjectPendingCustomAnim_;
    void applyPendingGameObjectCustomAnim(uint64_t guid);
    /// Taken away by the server while an animation held them (+0xbc
    /// 0x100000); see destroyGameObject.
    std::unordered_set<uint64_t> gameObjectDestroyPending_;
    /// When a transition (Open, Close, Destroy, Rebuild) has played and lets
    /// its hold go (0x0070d1e0's +0x18).
    std::unordered_map<uint64_t, std::chrono::steady_clock::time_point> gameObjectTransitionEnd_;
    void noteGameObjectTransitionEnd(uint64_t guid, uint32_t instanceId, uint8_t state, uint32_t anim);
    [[nodiscard]] bool gameObjectAnimationHolds(uint64_t guid) const;
    void updateGameObjectDestroyPending();
    /// 0x0070d510: puts the object in an animation state, not again if it is
    /// in it already.
    void enterGameObjectAnimState(uint64_t guid, uint32_t instanceId, uint8_t state);
    struct PendingTransportMove {
        float x = 0.0f, y = 0.0f, z = 0.0f, orientation = 0.0f;
    };
    struct PendingTransportRegistration {
        uint64_t guid = 0;
        uint32_t entry = 0;
        uint32_t displayId = 0;
        float x = 0.0f, y = 0.0f, z = 0.0f, orientation = 0.0f;
        int retryFrames = 0;  // waiting for the GO's render instance to spawn
    };
    std::unordered_map<uint64_t, PendingTransportMove> pendingTransportMoves_;
    std::deque<PendingTransportRegistration> pendingTransportRegistrations_;
    uint32_t nextGameObjectModelId_ = 20000;
    uint32_t nextGameObjectWmoModelId_ = 40000;
    bool testTransportSetup_ = false;

    // --- GameObject spawn queue ---
    struct PendingGameObjectSpawn {
        uint64_t guid;
        uint32_t entry;
        uint32_t displayId;
        float x, y, z, orientation;
        float scale = 1.0f;
    };
    std::deque<PendingGameObjectSpawn> pendingGameObjectSpawns_;
    void processGameObjectSpawnQueue();

    // --- Async WMO loading for game objects ---
    struct PreparedGameObjectWMO {
        uint64_t guid;
        uint32_t entry;
        uint32_t displayId;
        float x, y, z, orientation;
        float scale = 1.0f;
        std::shared_ptr<pipeline::WMOModel> wmoModel;
        std::unordered_map<std::string, pipeline::BLPImage> predecodedTextures;
        bool valid = false;
        bool isWmo = false;
        std::string modelPath;
    };
    struct AsyncGameObjectLoad {
        std::future<PreparedGameObjectWMO> future;
    };

    // A game object's M2, read, parsed and its textures decoded on a worker
    // (prepareGameObjectM2), then uploaded on the main thread
    // (uploadGameObjectM2). The spawn queue did all of it on the main thread,
    // one model at a time: entering Dalaran with a cold file cache, 287 game
    // objects queued behind it took over a minute.
    struct PreparedGameObjectM2 {
        uint32_t displayId = 0;
        std::string modelPath;
        std::shared_ptr<pipeline::M2Model> model;  // null: failed, see error
        std::unordered_map<std::string, pipeline::BLPImage> predecodedTextures;
        std::string error;
        std::string skinMissing;
    };
    struct AsyncGameObjectM2Load {
        uint32_t displayId = 0;
        std::future<PreparedGameObjectM2> future;
    };
    static PreparedGameObjectM2 prepareGameObjectM2(pipeline::AssetManager& am, uint32_t displayId,
                                                    const std::string& modelPath);
    uint32_t uploadGameObjectM2(PreparedGameObjectM2& prepared);
    void processAsyncGameObjectM2Results();
    std::vector<AsyncGameObjectM2Load> asyncGameObjectM2Loads_;
    std::unordered_set<uint32_t> asyncGameObjectM2Displays_;

    // A parsed WMO whose GPU upload is being spread across frames. Uploading a
    // transport in one go cost ~40ms - its textures are large, and that upload
    // has to happen on the main thread even though a worker decoded them. The
    // result is held here because it owns the model and the decoded pixels.
    struct PendingWmoUpload {
        PreparedGameObjectWMO result;
        uint32_t modelId = 0;
    };
    std::vector<PendingWmoUpload> pendingWmoUploads_;
    void processPendingWmoUploads();
    void finishWmoSpawn(const PreparedGameObjectWMO& result, uint32_t modelId);
    std::vector<AsyncGameObjectLoad> asyncGameObjectLoads_;
    void processAsyncGameObjectResults();
    struct PendingTransportDoodadBatch {
        uint64_t guid = 0;
        uint32_t modelId = 0;
        uint32_t instanceId = 0;
        size_t nextIndex = 0;
        size_t doodadBudget = 0;
        size_t spawnedDoodads = 0;
        float x = 0.0f, y = 0.0f, z = 0.0f, orientation = 0.0f;
    };
    std::vector<PendingTransportDoodadBatch> pendingTransportDoodadBatches_;
    static constexpr size_t MAX_TRANSPORT_DOODADS_PER_FRAME = 4;
    void processPendingTransportRegistrations();
    void processPendingTransportDoodads();

    // --- Mount ---
    uint32_t mountInstanceId_ = 0;
    /// When a unit's $FDX last played its Stand sound, of all units
    /// (0x00ca12d4), in steady-clock ms; and the draws the slots' chances take.
    uint32_t lastStandSoundMs_ = 0;
    uint32_t unitEventRandom_ = 0x2545F491u;
    /// FootstepTerrainLookup by creature footstep id and terrain type, built
    /// once its tables are read (0x004cf990).
    std::shared_ptr<const rendering::unit_event::FootstepTerrainSounds> footstepSounds_;
    uint32_t mountModelId_ = 0;
    uint32_t mountDisplayId_ = 0;  ///< the display mountInstanceId_ draws
    uint32_t pendingMountDisplayId_ = 0;
    void processPendingMount();
    std::unordered_map<uint64_t, RemotePlayerMount> remotePlayerMounts_;
    std::unordered_map<uint64_t, uint32_t> pendingRemotePlayerMounts_;
    void processPendingRemotePlayerMounts();
    /// One player's mount set; false where the player is not drawn yet.
    bool applyRemotePlayerMount(uint64_t guid, uint32_t displayId);
    bool loadRemoteMountModel(uint32_t displayId, uint32_t& modelId,
                              std::string& modelPath, float& riderHeight);
    void removeRemotePlayerMount(uint64_t guid);
    /// Riders not yet drawn that wait for their mounts (rider_mount_wait).
    struct RiderMountWait {
        uint32_t riderInstance = 0;
        std::chrono::steady_clock::time_point since;
    };
    std::unordered_map<uint64_t, RiderMountWait> riderMountWaits_;
    /// Holds a rider back where it has not been drawn yet and its mount is
    /// not under it.
    void holdRiderForMount(uint64_t guid, uint32_t displayId);
    /// Each frame: the riders whose mounts have come, or not in time, shown.
    void syncRiderMountWaits();

    // --- Local player GUID exclusion ---
    uint64_t spawnedPlayerGuid_ = 0;

    // --- Weapon model ID counter ---
    // Weapon model ids share CharacterRenderer's models map with NPC
    // composites, which are keyed by creature displayId (1..~35000). Starting
    // at 1000 collided with loaded NPC models once enough weapon reloads
    // advanced the counter - attachWeapon then instanced an NPC mesh as the
    // weapon. Reserve a range no displayId can reach.
    uint32_t nextWeaponModelId_ = 0x40000000u;

    // --- Spawn internal methods ---
    void spawnOnlineCreature(uint64_t guid, uint32_t displayId,
                             float x, float y, float z, float orientation, float scale = 1.0f);
    void spawnOnlinePlayer(uint64_t guid, uint8_t raceId, uint8_t genderId,
                           uint32_t appearanceBytes, uint8_t facialFeatures,
                           float x, float y, float z, float orientation);
    void setOnlinePlayerEquipment(uint64_t guid,
                                  const std::array<uint32_t, 19>& displayInfoIds,
                                  const std::array<uint8_t, 19>& inventoryTypes);
    void spawnOnlineGameObject(uint64_t guid, uint32_t entry, uint32_t displayId,
                               float x, float y, float z, float orientation, float scale = 1.0f);
};

} // namespace core
} // namespace wowee
