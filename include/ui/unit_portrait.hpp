#pragma once

// A unit's model rendered small, for the interface's portraits and model views.
//
// WoW's portraits are not pictures on disk: they are the character itself,
// rendered once into a texture through the model's own portrait camera and
// kept (SetPortraitTexture, 0x00619580) - a still picture of the head. The
// same offscreen pass the character-select screen uses draws them; a
// portrait draws it while its model is new and then keeps the image, unless
// it is asked to be live (setAnimated). The paperdoll and the other model
// views use the same pass, live and framed on the whole figure.
//
// The units: the player, the target, the pet, the focus, the party and the
// NPC a window belongs to. They differ only in whose appearance is loaded - a
// player from race, appearance bytes and what they are visibly wearing,
// anything else from the model its display id names.

#include "game/guild_emblem.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace wowee {
namespace game { class GameHandler; struct EquipmentItem; }
namespace pipeline { class AssetManager; }
namespace rendering { class CharacterPreview; class Renderer; }

namespace ui {

class UnitPortrait {
public:
    /// How much of the character to show. A portrait is the face in a circle;
    /// the paperdoll wants the whole figure in a tall rectangle. The offscreen
    /// pass is the same either way - only the framing differs, which is why
    /// this is one class and not two.
    enum class Framing { Face, FullBody };

    UnitPortrait();
    ~UnitPortrait();

    /// Builds the offscreen view on first use and keeps it in step with the
    /// player's appearance and equipment afterwards. Safe to call every frame;
    /// it reloads the model only when something about it actually changed.
    void update(game::GameHandler& gameHandler, pipeline::AssetManager* assets,
                rendering::Renderer* renderer, float deltaTime);

    /// Answers whether there is actually a model to show. A load can fail -
    /// a race this client has no model path for, an M2 the install is missing
    /// - and the view then keeps whatever it had, which for a frame that has
    /// just changed unit means the previous one's face. The caller blanks the
    /// texture instead.
    ///
    /// Show another player, from the appearance the world already reads for
    /// them: race, gender, the packed appearance bytes and facial features.
    ///
    /// Dressed in what they are visibly wearing, which for a portrait framed
    /// on the head is the helm and the shoulders - the two pieces that change
    /// a face most. An empty list leaves the model as it is rather than
    /// stripping it, because "nothing known yet" and "wearing nothing" arrive
    /// looking the same and only one of them should undress anybody.
    bool updatePlayer(uint8_t race, uint8_t gender, uint32_t appearanceBytes,
                      uint8_t facialFeatures,
                      const std::vector<game::EquipmentItem>& equipment,
                      pipeline::AssetManager* assets,
                      rendering::Renderer* renderer, float deltaTime);

    /// Show a creature instead, by the model its display id names.
    ///
    /// Kept apart from update() rather than folded into it: a player is built
    /// from race, appearance bytes and equipment, and a creature is a path and
    /// nothing else. The two share the offscreen view and the framing and
    /// agree on nothing else, so one function taking both would be two
    /// functions sharing a name.
    ///
    /// Reloads only when the path changes, so this is safe every frame.
    bool updateCreature(const std::string& m2Path,
                        const std::vector<std::pair<uint32_t, std::string>>& skins,
                        pipeline::AssetManager* assets,
                        rendering::Renderer* renderer, float deltaTime);

    /// A pre-composited skin to put on the character once it is built,
    /// replacing the one made from CharSections.
    ///
    /// CreateDisplayInfoExtra carries one for nearly every humanoid NPC, and
    /// it is the whole appearance already baked - skin, face, hair and the
    /// armour they wear. Set before the update that should use it; it is part
    /// of what decides a rebuild, so changing it is enough to apply it.
    void setBakedSkin(const std::string& path) { pendingBake_ = path; }

    /// The class of the unit updatePlayer draws, or 0 where none is known.
    /// A death knight's eyes glow whatever the face (0x004ed900: component
    /// +0x20 == 6), so a player's portrait needs it as the world's model does.
    /// Set before the update that should use it; a change rebuilds.
    void setUnitClass(uint8_t classId) { pendingClass_ = classId; }

    /// The guild's design for a player's guild tabard (0x006db510), or none.
    /// Set before the update that should use it; a change rebuilds.
    void setGuildEmblem(const std::optional<game::GuildEmblem>& emblem) { pendingEmblem_ = emblem; }

    /// Set before the first update, since framing is applied when the model
    /// loads and the model loads once.
    void setFraming(Framing framing) { framing_ = framing; }

    /// How big the offscreen image is, in pixels. Set before the first update,
    /// because the view is built once and keeps the size it was built at.
    ///
    /// Worth setting: this is a full character pass every frame, and the
    /// paperdoll's 640x800 is enormously oversized for a face drawn into a
    /// circle fifty pixels across.
    void setTargetSize(int width, int height) {
        targetWidth_ = width;
        targetHeight_ = height;
    }

    /// Turn the figure by this much, in radians. What the paperdoll's rotate
    /// buttons drive.
    void rotate(float yawDelta);

    /// A live, animated view, or the still picture the client's
    /// SetPortraitTexture draws (0x00619580): rendered once when the model is
    /// built and kept. The model views - the paperdoll, the dressing room,
    /// inspect - are live, as the client's model frames are; the unit
    /// portraits are still unless the "animatedportraits" setting asks.
    /// Safe to set every frame.
    void setAnimated(bool animated) { animated_ = animated; }

    /// The rendered portrait, or zero until the first composite has run. The
    /// value is a VkDescriptorSet, carried as an integer so this header does
    /// not drag Vulkan into the widget tree.
    [[nodiscard]] uint64_t textureId() const;

    void shutdown(rendering::Renderer* renderer);

private:
    /// A model is about to be (re)loaded: frame it for a still or a live
    /// view and draw it again.
    void beginRebuild();
    /// Advance and draw a live view; draw a still one only while it is new.
    void present(float deltaTime);

    std::unique_ptr<rendering::CharacterPreview> preview_;
    bool animated_ = true;
    bool framedAnimated_ = true;
    int stillFramesLeft_ = 0;
    bool initialized_ = false;
    Framing framing_ = Framing::Face;
    int targetWidth_ = 640;
    int targetHeight_ = 800;
    bool registered_ = false;

    // What the loaded model was built from, so a reload happens only on a real
    // change rather than every frame.
    /// The creature model currently loaded, empty while a player is loaded.
    /// Also the guard against reloading: a portrait rebuilt every frame looks
    /// like one that flickers, and the two are indistinguishable from outside.
    std::string loadedCreaturePath_;
    /// The bake asked for, and the one already on the model.
    std::string pendingBake_;
    std::string loadedBake_;
    /// The class asked for, and the one the model was built with.
    uint8_t pendingClass_ = 0;
    uint8_t loadedClass_ = 0;
    /// The guild design asked for, and the one painted.
    std::optional<game::GuildEmblem> pendingEmblem_;
    std::optional<game::GuildEmblem> loadedEmblem_;

    uint64_t loadedGuid_ = 0;
    /// Race and gender as well, because another player is identified by these
    /// rather than by a guid that this only ever sees one of at a time.
    uint8_t  loadedRace_ = 0xFF;
    uint8_t  loadedGender_ = 0xFF;
    uint32_t loadedAppearance_ = 0;
    uint8_t  loadedFacialFeatures_ = 0;
    // uint64_t, not size_t: the hash is sixty-four bits and storing it in a
    // pointer-sized field would truncate on a 32-bit build, where two outfits
    // sharing the low half would stop the portrait redrawing.
    uint64_t loadedEquipHash_ = 0;
};

} // namespace ui
} // namespace wowee
