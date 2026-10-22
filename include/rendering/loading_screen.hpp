#pragma once

#include <vulkan/vulkan.h>
#include <array>
#include <cstdint>
#include <string>

struct SDL_Window;
struct ImDrawList;

namespace wowee::pipeline { class AssetManager; }

namespace wowee {
namespace rendering {

class VkContext;

class LoadingScreen {
public:
    LoadingScreen();
    ~LoadingScreen();

    /// The map's picture (0x00409ed0) and the bar's textures (0x0040a990).
    /// From here until shutdown() the world's sound is held, as behind the
    /// client's loading screen nothing of the world plays (0x0040a920,
    /// audio/world_sound_hold.hpp).
    bool initialize(pipeline::AssetManager* assets, uint32_t mapId);
    void shutdown();

    // Render the loading screen: the picture and the bar (0x0040a270)
    void render();
    /// The same picture and bar over whatever this frame's interface draws
    /// (ImGui's foreground list), for the world's first frames: they run
    /// behind it, as everything before them did (Application::worldSettle).
    void drawOverlay();

    void setProgress(float progress) { loadProgress = progress; }
    /// What is being loaded, for the log: the client's screen says nothing.
    void setStatus(const std::string& status);

    // Must be set before initialize() for Vulkan texture upload
    void setVkContext(VkContext* ctx) { vkCtx = ctx; }
    void setSDLWindow(SDL_Window* win) { sdlWindow = win; }

private:
    struct Texture {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkDescriptorSet descriptor = VK_NULL_HANDLE; // ImGui texture handle
    };
    bool loadTexture(pipeline::AssetManager* assets, const std::string& path, Texture& out);
    bool upload(const uint8_t* rgba, int width, int height, Texture& out);
    void drawPicture(ImDrawList* drawList, float screenW, float screenH) const;
    void release(Texture& tex);

    VkContext* vkCtx = nullptr;
    SDL_Window* sdlWindow = nullptr;

    Texture picture_;
    /// The picture is the 16:10 "Wide" one (0x00b2fed8).
    bool widePicture_ = false;
    std::array<Texture, 2> bar_;  ///< loading_screen::kBar's pieces

    float loadProgress = 0.0f;
    /// This screen holds the world's sound (AudioEngine::beginWorldSoundHold).
    bool holdsWorldSound_ = false;
};

} // namespace rendering
} // namespace wowee
