#pragma once

// The headset, through OpenXR: wowee_vr only (cmake/WoweeVr.cmake).
//
// One instance for the life of the program. Application asks it for the
// headset before anything else is made, VkContext has it create the Vulkan
// instance and device the runtime wants, and each frame goes:
//
//   beginFrame      xrWaitFrame/xrBeginFrame, where the eyes and hands are,
//                   and the controllers' input applied to the client
//   eye 0, eye 1    the world drawn once for each eye (Application), the
//                   camera set to that eye between them (applyEyeCamera),
//                   each copied into the eye's swapchain at the end of its
//                   command buffer (recordAfterFrame) and the images handed
//                   back once it is submitted (afterSubmit)
//   endFrame        the projection layer, the interface's quad and the
//                   controller's ray, to the compositor
//
// The interface is drawn once a frame into an image of its own, the window's
// size, and shown on a quad layer: a giant screen ahead in the menus, a panel
// that follows the head in the world. The compositor draws quad layers over
// the projection, so it never sinks into a wall, and samples them itself, so
// its text stays crisp.
//
// Nothing here is in wowee.exe, and no shared header refers to it.

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <memory>
#include <string>
#include <vector>

struct SDL_Window;

namespace wowee {
namespace rendering {
class Camera;
class Renderer;
class VkContext;
}  // namespace rendering

namespace vr {

class Runtime {
public:
    static Runtime& get();

    // ---- start-up, before and during Vulkan's ----

    /// The OpenXR instance and the headset. False with the reason when there
    /// is no runtime, no headset, or the runtime cannot do Vulkan.
    bool initialize(std::string& whyNot);
    [[nodiscard]] bool initialized() const;

    /// For vk-bootstrap: a vkGetInstanceProcAddr whose vkCreateInstance and
    /// vkCreateDevice go through the runtime (XR_KHR_vulkan_enable2), so it
    /// can add what it needs to both.
    [[nodiscard]] PFN_vkGetInstanceProcAddr instanceProcAddr() const;
    /// The GPU the headset is on, once the instance exists; its name, for
    /// vk-bootstrap's selector.
    [[nodiscard]] std::string physicalDeviceName(VkInstance instance);

    /// The session, once VkContext has its device and queue.
    bool createSession(rendering::VkContext& vk);

    /// The images the client renders an eye into, in place of a swapchain's:
    /// one per frame in flight, each eye's size, in the UNORM twin of the
    /// headset's sRGB format (the client writes encoded colour itself; the
    /// copy into the headset's sRGB image is bit for bit). Owned here.
    bool createEyeTargets(rendering::VkContext& vk, uint32_t count, std::vector<VkImage>& images,
                          std::vector<VkImageView>& views, VkExtent2D& extent, VkFormat& format);
    void destroyEyeTargets(rendering::VkContext& vk);

    void shutdown();

    // ---- each frame ----

    /// Events, xrWaitFrame and xrBeginFrame, the poses, and the controllers
    /// applied to the client: before the frame's update, so what they do is
    /// in it. `inWorld` is whether the character is in the world.
    void beginFrame(rendering::Renderer* renderer, SDL_Window* window, bool inWorld, float deltaSeconds);

    /// How many eyes to draw the world for this frame: 2, or 0 when the
    /// compositor does not want a picture or the world is not up.
    [[nodiscard]] int eyesToRender(bool inWorld) const;

    /// The camera made one eye: its place, its look and its projection.
    /// restoreCamera puts back what the client had.
    void applyEyeCamera(int eye, rendering::Camera& camera);
    void restoreCamera(rendering::Camera& camera);

    /// Which engine frame is being recorded: an eye (0 or 1), or -1 for one
    /// drawn only for the interface (the menus).
    void setRecordingEye(int eye) { recordingEye_ = eye; }

    /// From the end of Renderer::endFrame, in the frame's command buffer: the
    /// interface into its image (once a frame), the eye into the headset, the
    /// mirror into the window.
    void recordAfterFrame(VkCommandBuffer cmd, VkImage engineImage);

    /// From VkContext::endFrame, once the command buffer is submitted.
    void afterSubmit();

    /// The layers to the compositor.
    void endFrame();

    /// A frame of the loading screen, which draws outside the main loop: a
    /// headset frame of its own (or the loop's, still open) showing only the
    /// interface's quad - the loading picture on the menus' big screen.
    /// recordAfterFrame and endFrame follow as for any other.
    void beginLoadingFrame(SDL_Window* window);

    /// Asked by the client to quit (the runtime is exiting or lost).
    [[nodiscard]] bool exitRequested() const;

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

private:
    Runtime();
    ~Runtime();
    struct Impl;
    std::unique_ptr<Impl> impl_;
    int recordingEye_ = -1;
};

/// Shown when the headset cannot be reached: a message box and the log.
void reportUnavailable(const std::string& why);

}  // namespace vr
}  // namespace wowee
