#pragma once

// A unit's model rendered small, for the interface's portraits and model views.
//
// WoW's portraits are not pictures on disk: they are the character itself,
// rendered once into a texture through the model's own portrait camera and
// kept (SetPortraitTexture, 0x00619580) - a still picture of the head. The
// same offscreen pass the character-select screen uses draws them; a
// portrait draws it while its model is new and then keeps the image, unless
// it is asked to be live (setAnimated). The model frames - the paperdoll, the
// dressing rooms, an addon's - use the same pass, live and through the camera
// the client gives them (rendering/figure_camera.hpp).
//
// What is drawn is a UnitLook (ui/unit_look.hpp): a player from race,
// appearance bytes and what they are visibly wearing, anything else from the
// model its display id names.

#include "rendering/figure_camera.hpp"
#include "rendering/model_frame_view.hpp"
#include "ui/unit_look.hpp"

#include <cstdint>
#include <memory>

namespace wowee {
namespace pipeline { class AssetManager; }
namespace rendering { class CharacterPreview; class Renderer; }

namespace ui {

class UnitPortrait {
public:
    /// How much of the unit to show. A portrait is the face in a circle; a
    /// model frame is whatever its camera sees. The offscreen pass is the same
    /// either way - only the framing differs, which is why this is one class
    /// and not two.
    enum class Framing { Face, Figure };

    UnitPortrait();
    ~UnitPortrait();

    /// Builds the offscreen view on first use and keeps it in step with the
    /// look afterwards. Safe to call every frame; it reloads the model only
    /// when something about it actually changed.
    ///
    /// Answers whether there is actually a model to show. A load can fail -
    /// an M2 the install is missing - and the view then has nothing.
    bool show(const UnitLook& look, pipeline::AssetManager* assets, rendering::Renderer* renderer,
              float deltaTime);

    void setFraming(Framing framing) { framing_ = framing; }

    /// How a model frame looks at its model; applied as it changes.
    void setFigure(int camera, const rendering::figure_camera::Pose& pose);
    /// And the frame's light and fog on it.
    void setFigureLight(const rendering::model_frame::Light& light, const rendering::model_frame::Fog& fog);
    /// The model turns (SetRotation, 0x005970f0): its feet shuffle to the
    /// left (below 0) or the right until it has not been turned for a tenth of
    /// a second. `serial` counts the turns.
    void setTurning(int direction, uint32_t serial);
    /// SetSequence's animation, as `serial` changes.
    void setSequence(int animation, uint32_t serial);

    /// How big the offscreen image is, in pixels. A view built at another size
    /// keeps it, unless it is asked to be rebuilt.
    ///
    /// Worth setting: this is a full character pass every frame, and 640x800
    /// is enormously oversized for a face drawn into a circle fifty pixels
    /// across.
    void setTargetSize(int width, int height) {
        targetWidth_ = width;
        targetHeight_ = height;
    }
    /// The same, with the view built again where it was built at another.
    void fitTo(int width, int height, rendering::Renderer* renderer);

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
    bool load(const UnitLook& look);
    /// The model as loaded, framed.
    void frame();
    /// A model is about to be (re)loaded: frame it for a still or a live
    /// view and draw it again.
    void beginRebuild();
    /// Advance and draw a live view; draw a still one only while it is new.
    void present(float deltaTime);

    std::unique_ptr<rendering::CharacterPreview> preview_;
    bool animated_ = true;
    bool framedAnimated_ = true;
    int stillFramesLeft_ = 0;
    Framing framing_ = Framing::Face;
    int targetWidth_ = 640;
    int targetHeight_ = 800;
    int builtWidth_ = 0;
    int builtHeight_ = 0;
    bool registered_ = false;

    int camera_ = -1;
    rendering::figure_camera::Pose pose_;
    uint32_t turnSerial_ = 0;
    int turning_ = 0;
    float turnLeft_ = 0.0f;      ///< seconds until the shuffling ends
    uint32_t sequenceSerial_ = 0;

    /// What the loaded model was built from, so a reload happens only on a
    /// real change rather than every frame: a portrait rebuilt every frame
    /// looks like one that flickers.
    UnitLook loaded_;
    bool loadedWell_ = false;
};

} // namespace ui
} // namespace wowee
