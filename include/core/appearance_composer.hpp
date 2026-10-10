#pragma once

#include "core/geoset_rules.hpp"
#include "core/character_geosets.hpp"
#include "core/weapon_attachment.hpp"
#include "game/character.hpp"
#include "game/inventory.hpp"
#include "pipeline/char_sections.hpp"
#include "rendering/animation/weapon_type.hpp"
#include <string>
#include <vector>
#include <unordered_set>
#include <cstdint>
#include <map>
#include <optional>
#include <array>

namespace wowee {

namespace rendering { class Renderer; }
namespace pipeline { class AssetManager; class DBCLayout; struct M2Model; }
namespace game { class GameHandler; class Inventory; }

namespace core {

class EntitySpawner;

/// Resolved texture paths from CharSections.dbc for player character compositing.
struct PlayerTextureInfo {
    std::string bodySkinPath;
    std::string faceLowerPath;
    std::string faceUpperPath;
    std::string hairTexturePath;
    /// CharSections' second texture on the skin row - the "Extra" art an HD
    /// character model asks for as texture type 8. Empty on the stock models,
    /// which have no type-8 texture and never look for one.
    std::string skinExtraPath;
    std::vector<std::string> underwearPaths;
    /// The head's layers in the client's order: face, facial hair, scalp.
    std::vector<pipeline::FaceRegionLayer> faceLayers;
};

/// Handles player character visual appearance: skin compositing, geoset selection,
/// texture path lookups, and equipment weapon rendering.
class AppearanceComposer {
public:
    AppearanceComposer(rendering::Renderer* renderer,
                       pipeline::AssetManager* assetManager,
                       game::GameHandler* gameHandler,
                       EntitySpawner* entitySpawner);

    // Player model path resolution

    // Resolve texture paths from CharSections.dbc and fill model texture slots.
    // Call BEFORE charRenderer->loadModel().
    /// `useFemaleModel` is the body a nonbinary character chose. The skin
    /// textures are picked per sex, so a female body reading male skins is
    /// the same mismatch the model path had.
    PlayerTextureInfo resolvePlayerTextures(pipeline::M2Model& model,
                                            game::Race race, game::Gender gender,
                                            uint32_t appearanceBytes,
                                            bool useFemaleModel = false,
                                            int facialHairId = -1);

    // Apply composited textures to loaded model instance.
    // Call AFTER charRenderer->loadModel(). Saves skin state for re-compositing.
    void compositePlayerSkin(uint32_t modelSlotId, const PlayerTextureInfo& texInfo);

    /// The active character's character component before its equipment:
    /// race, sex, class, skin, face and its hair and facial rows.
    core::CharacterLook playerLook(const game::Character& ch) const;
    /// What an instance of the player's model draws for that look, worn
    /// items included (core::characterGeosets, 0x004ed900).
    std::unordered_set<uint16_t> playerGeosets(const core::CharacterLook& look,
                                               uint32_t instanceId) const;

    // Equipment weapon loading (reads inventory, attaches weapon M2 models)
    void loadEquippedWeapons();

    /// The local player's sheath state: the client's own (CGUnit_C +0xb5c),
    /// set by the client and told to the server (0x00736d30), taken from
    /// UNIT_FIELD_BYTES_2 byte 0 at spawn and when the server moves it
    /// (0x00737aa0).
    [[nodiscard]] SheathState sheathState() const { return sheath_; }
    /// Whether the player shows its SpellVisual Flags 8 state kits
    /// (0x00720400): idle, its weapons away and casting nothing.
    [[nodiscard]] bool unarmedKitsShown() const {
        return kitIdle_ && sheath_ == SheathState::Unarmed && animationSheathKey_.castSpellId == 0;
    }
    /// 0x00736d30: sets the state when it changes - the ranged state only
    /// for a class that may draw a ranged weapon - and tells the server
    /// (CMSG_SET_SHEATHED) unless `fromServer`.
    void setSheathState(SheathState state, bool fromServer = false, bool animated = false);
    /// Sets a state as the client asks for it (0x00736d30 with its sending).
    void requestSheathState(SheathState state) { setSheathState(state); }
    /// The sheath key (0x006e23a0): the next state, reached for with the
    /// arms (0x00736b60), and false when the player may not change it now
    /// or has nothing to change to.
    bool toggleSheath();
    /// A spell's cast beginning on the player (0x007fa2e0, 0x0073a6c0).
    void onSpellCastBegin(uint32_t spellId);
    /// The player sending a text emote (0x006dd9e0): whether it is sent,
    /// and its weapons put away when it is.
    TextEmoteVerdict onTextEmote(uint32_t textEmoteId);
    /// Once a frame: the field's changes (0x00737aa0), the stand state's
    /// (0x0073f060), the channel object's (0x0073a520), the animation's,
    /// cast's and attack's (0x00738180), then the weapons dressed again
    /// when what 0x0072dbc0 reads - the sheath state, the disarm bits, the
    /// animation's hands - has changed.
    void updateWeaponsFromFields();



    // Saved skin state accessors (used by game_screen.cpp for equipment re-compositing)
    [[nodiscard]] const std::string& getBodySkinPath() const { return bodySkinPath_; }
    [[nodiscard]] const std::vector<std::string>& getUnderwearPaths() const { return underwearPaths_; }
    [[nodiscard]] uint32_t getSkinTextureSlotIndex() const { return skinTextureSlotIndex_; }
    [[nodiscard]] uint32_t getCloakTextureSlotIndex() const { return cloakTextureSlotIndex_; }

    /// A spell's worn item over what the player wears in an equipment slot
    /// (CharProc 17, 0x006f82d0); display 0 takes it off again (0x00723730).
    void setItemOverride(int equipSlot, uint32_t displayId, uint8_t inventoryType);
    /// The player's inventory as it is drawn: those items over the equipped.
    [[nodiscard]] game::Inventory dressedInventory(const game::Inventory& inventory) const;
    /// The display worn over an equipment slot, if a spell put one there.
    [[nodiscard]] std::optional<uint32_t> itemOverrideDisplay(int equipSlot) const;

private:
    std::map<int, std::pair<uint32_t, uint8_t>> itemOverrides_;  ///< equipment slot → display, inventory type
    bool loadWeaponM2(const std::string& m2Path, pipeline::M2Model& outModel);

    /// Attach the equipped head item's model. Other players resolve this through
    /// EntitySpawner; the local character had no equivalent at all.
    void loadEquippedHelm(game::Inventory& inventory);
    /// The helm, the shoulders and the three weapons, each where the sheath
    /// state puts it.
    void attachEquippedWeapons();
    /// The player's main hand, off hand and ranged item as 0x0072dbc0
    /// reads them; `storage` holds them.
    UnitWeaponItems playerWeaponItems(std::array<UnitWeaponItem, 3>& storage) const;
    /// ChrClasses +0x24 without 8: the class may draw a ranged weapon.
    bool classMayDrawRanged() const;
    /// UNIT_FIELD_CHANNEL_OBJECT of the player; 0 for none.
    uint64_t playerChannelObject() const;
    /// The field's state for the player, or nothing without the field.
    std::optional<SheathState> fieldSheathState() const;

    // Attach the enchant visual (sharpening-stone glint, weapon glow) of the item in
    // the given equipment slot to the weapon already attached at attachmentId.
    void applyEnchantVisuals(uint32_t charInstanceId, int equipSlotIndex, uint32_t attachmentId);

    rendering::Renderer* renderer_;
    pipeline::AssetManager* assetManager_;
    game::GameHandler* gameHandler_;
    EntitySpawner* entitySpawner_;

    // Saved at spawn for skin re-compositing on equipment changes
    std::string bodySkinPath_;
    std::vector<std::string> underwearPaths_;
    uint32_t skinTextureSlotIndex_ = 0;
    uint32_t cloakTextureSlotIndex_ = 0;

    /// What the weapons were last dressed by.
    struct DressedKey {
        uint32_t instanceId = 0;
        uint8_t state = 0xFF;
        uint32_t unitFlags = 0;
        uint32_t unitFlags2 = 0;
        bool offHandFollowsAnimation = false;
        bool operator==(const DressedKey&) const = default;
    };
    DressedKey dressedKey_;
    /// The key the fields give now.
    DressedKey currentDressKey() const;
    /// From ranged to melee since the last dressing (0x00731f40).
    bool rangedJustPutAway_ = false;
    /// The client's own state and the instance it is kept for: taken from
    /// the field when the player's model is made (0x0073f660).
    SheathState sheath_ = SheathState::Melee;
    uint32_t sheathInstanceId_ = 0;
    /// The field's state as last seen, for 0x00737aa0.
    std::optional<SheathState> fieldSheathSeen_;
    /// What 0x00738180 last ran for; it runs again when the animation, the
    /// cast or the attack changes.
    struct AnimationSheathKey {
        uint32_t animId = 0xFFFFFFFFu;
        uint32_t castSpellId = 0;
        bool attacking = false;
        bool operator==(const AnimationSheathKey&) const = default;
    };
    AnimationSheathKey animationSheathKey_;
    /// +0xa30 0x10000 (animationKitIdle).
    bool kitIdle_ = false;
    /// The sheath key's reach while its arms play (0x00736b60); the
    /// weapons are where it has left them.
    std::optional<SheathReach> reach_;
    /// Start the arms' reaches; a hand whose arm cannot play one moves at
    /// once.
    void playReach(const ReachPlay& play, const UnitWeaponItems& items);
    /// A hand's event or its reach's end (0x00732500, 0x00737bd0).
    void reachHandEvent(int hand, bool ended, const UnitWeaponItems& items);
    /// The reach's arms, their events and its end, once a frame.
    void updateSheathReach();
    /// Whether each hand's reach has played its swap's sound; the swap runs
    /// at the hand's event, and wowee runs it again at the reach's end.
    bool reachSounded_[2] = {false, false};
    /// 0x004d07b0, from 0x00732500: the moved weapon's Material.dbc sheathe
    /// or unsheathe sound, 2 yards above the unit.
    void playSheathSwapSound(int hand, const UnitWeaponItems& items);
    /// Any reach stopped where it is (0x00736d30's immediate change).
    void stopSheathReach();
    /// The stand state and channel object last seen, for 0x0073f060 and
    /// 0x0073a520.
    uint8_t standSeen_ = 0;
    uint64_t channelObjectSeen_ = 0;
};

} // namespace core
} // namespace wowee
