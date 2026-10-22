// The headset, through OpenXR. See vr/vr_runtime.hpp for the frame's shape.

#include "vr/vr_runtime.hpp"

#include "core/input.hpp"
#include "core/logger.hpp"
#include "rendering/camera.hpp"
#include "rendering/camera_controller.hpp"
#include "rendering/renderer.hpp"
#include "rendering/vk_context.hpp"
#include "vr/vr_math.hpp"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_vulkan.h>
#include <vk_mem_alloc.h>

#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <optional>

namespace wowee::vr {

namespace {

// ---- what the runtime said ----

XrInstance gInstance = XR_NULL_HANDLE;
XrSystemId gSystem = XR_NULL_SYSTEM_ID;
PFN_xrGetVulkanGraphicsRequirements2KHR pGetRequirements = nullptr;
PFN_xrCreateVulkanInstanceKHR pCreateVulkanInstance = nullptr;
PFN_xrGetVulkanGraphicsDevice2KHR pGetVulkanDevice = nullptr;
PFN_xrCreateVulkanDeviceKHR pCreateVulkanDevice = nullptr;

std::string xrText(XrResult r) {
    char buf[XR_MAX_RESULT_STRING_SIZE] = {};
    if (gInstance != XR_NULL_HANDLE && XR_SUCCEEDED(xrResultToString(gInstance, r, buf))) return buf;
    return "XrResult " + std::to_string(static_cast<int>(r));
}

bool xrOk(XrResult r, const char* what) {
    if (XR_SUCCEEDED(r)) return true;
    LOG_ERROR("OpenXR: ", what, " failed: ", xrText(r));
    return false;
}

// ---- vkCreateInstance and vkCreateDevice, through the runtime ----
//
// XR_KHR_vulkan_enable2 has the runtime create both, with the application's
// own create info plus whatever it needs - the extensions for sharing images
// with its compositor among them. vk-bootstrap builds the create infos and
// calls the two through the vkGetInstanceProcAddr it is handed, so handing it
// this one is all it takes; everything else goes to the loader as before.

VKAPI_ATTR VkResult VKAPI_CALL hookCreateInstance(const VkInstanceCreateInfo* createInfo,
                                                  const VkAllocationCallbacks* allocator, VkInstance* instance) {
    XrVulkanInstanceCreateInfoKHR info{XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
    info.systemId = gSystem;
    info.pfnGetInstanceProcAddr = &vkGetInstanceProcAddr;
    info.vulkanCreateInfo = createInfo;
    info.vulkanAllocator = allocator;
    VkResult vkResult = VK_SUCCESS;
    const XrResult r = pCreateVulkanInstance(gInstance, &info, instance, &vkResult);
    if (XR_FAILED(r)) {
        LOG_ERROR("OpenXR: the runtime would not create the Vulkan instance: ", xrText(r),
                  " (Vulkan ", static_cast<int>(vkResult), ")");
        return vkResult != VK_SUCCESS ? vkResult : VK_ERROR_INITIALIZATION_FAILED;
    }
    if (vkResult == VK_SUCCESS) LOG_WARNING("OpenXR: Vulkan instance created through the runtime");
    return vkResult;
}

VKAPI_ATTR VkResult VKAPI_CALL hookCreateDevice(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo* createInfo,
                                                const VkAllocationCallbacks* allocator, VkDevice* device) {
    XrVulkanDeviceCreateInfoKHR info{XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
    info.systemId = gSystem;
    info.pfnGetInstanceProcAddr = &vkGetInstanceProcAddr;
    info.vulkanPhysicalDevice = physicalDevice;
    info.vulkanCreateInfo = createInfo;
    info.vulkanAllocator = allocator;
    VkResult vkResult = VK_SUCCESS;
    const XrResult r = pCreateVulkanDevice(gInstance, &info, device, &vkResult);
    if (XR_FAILED(r)) {
        LOG_ERROR("OpenXR: the runtime would not create the Vulkan device: ", xrText(r),
                  " (Vulkan ", static_cast<int>(vkResult), ")");
        return vkResult != VK_SUCCESS ? vkResult : VK_ERROR_INITIALIZATION_FAILED;
    }
    if (vkResult == VK_SUCCESS) LOG_WARNING("OpenXR: Vulkan device created through the runtime");
    return vkResult;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL hookGetInstanceProcAddr(VkInstance instance, const char* name) {
    if (name && std::strcmp(name, "vkCreateInstance") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&hookCreateInstance);
    if (name && std::strcmp(name, "vkCreateDevice") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&hookCreateDevice);
    return vkGetInstanceProcAddr(instance, name);
}

// ---- formats ----

/// The UNORM twin of an sRGB format: the client's colour is already encoded,
/// so it renders into this and the bytes go into the sRGB image unchanged -
/// which the compositor then decodes, as it should.
VkFormat unormTwin(VkFormat f) {
    switch (f) {
        case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_UNORM;
        case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
        default: return f;
    }
}

Pose toPose(const XrPosef& p) {
    Pose out;
    out.orientation = glm::quat(p.orientation.w, p.orientation.x, p.orientation.y, p.orientation.z);
    out.position = glm::vec3(p.position.x, p.position.y, p.position.z);
    return out;
}

XrPosef toXr(const Pose& p) {
    XrPosef out;
    out.orientation = {p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w};
    out.position = {p.position.x, p.position.y, p.position.z};
    return out;
}

/// A head's yaw about OpenXR's up axis: 0 looking down -Z, positive turning
/// left (toward -X), as a rotation about +Y turns -Z.
float xrYawOf(const glm::vec3& forward) { return std::atan2(-forward.x, -forward.z); }

void imageBarrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
                  VkPipelineStageFlags srcStage, VkAccessFlags srcAccess,
                  VkPipelineStageFlags dstStage, VkAccessFlags dstAccess) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = from;
    b.newLayout = to;
    b.srcAccessMask = srcAccess;
    b.dstAccessMask = dstAccess;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

/// One OpenXR swapchain and its images.
struct XrChain {
    XrSwapchain handle = XR_NULL_HANDLE;
    std::vector<VkImage> images;
    VkExtent2D extent{0, 0};
    uint32_t acquired = UINT32_MAX;  ///< the image held between acquire and release
};

/// An image of the client's own, with its memory.
struct OwnImage {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
};

bool createOwnImage(rendering::VkContext& vk, VkFormat format, VkExtent2D extent, VkImageUsageFlags usage,
                    OwnImage& out) {
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = format;
    ici.extent = {extent.width, extent.height, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = usage;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    if (vmaCreateImage(vk.getAllocator(), &ici, &aci, &out.image, &out.allocation, nullptr) != VK_SUCCESS) {
        LOG_ERROR("VR: could not create a ", extent.width, "x", extent.height, " image");
        return false;
    }
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = out.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = format;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(vk.getDevice(), &vci, nullptr, &out.view) != VK_SUCCESS) {
        vmaDestroyImage(vk.getAllocator(), out.image, out.allocation);
        out = {};
        return false;
    }
    return true;
}

void destroyOwnImage(rendering::VkContext& vk, OwnImage& img) {
    if (img.view) vkDestroyImageView(vk.getDevice(), img.view, nullptr);
    if (img.image) vmaDestroyImage(vk.getAllocator(), img.image, img.allocation);
    img = {};
}

// ---- settings, from the environment for now ----

bool envOn(const char* name, bool fallback) {
    const char* v = std::getenv(name);
    if (!v || !*v) return fallback;
    return *v != '0';
}

float envFloat(const char* name, float fallback) {
    const char* v = std::getenv(name);
    if (!v || !*v) return fallback;
    char* end = nullptr;
    const float f = std::strtof(v, &end);
    return end != v && std::isfinite(f) ? f : fallback;
}

}  // namespace

// ---------------------------------------------------------------------------

struct Runtime::Impl {
    // The session.
    rendering::VkContext* vk = nullptr;
    XrSession session = XR_NULL_HANDLE;
    XrSessionState state = XR_SESSION_STATE_UNKNOWN;
    bool running = false;
    bool exitRequested = false;
    XrSpace local = XR_NULL_HANDLE;
    XrSpace view = XR_NULL_HANDLE;
    XrViewConfigurationType viewConfig = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    XrEnvironmentBlendMode blendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    VkFormat xrFormat = VK_FORMAT_UNDEFINED;  ///< the headset's (sRGB)
    VkExtent2D eyeExtent{0, 0};
    std::array<XrChain, 2> eyes;
    XrChain uiChain;
    XrChain laserChain;
    bool laserFilled = false;

    // The client's images: the eyes it renders into, and the interface's.
    std::vector<OwnImage> eyeTargets;
    OwnImage ui;
    VkExtent2D uiExtent{0, 0};
    VkFramebuffer uiFramebuffer = VK_NULL_HANDLE;

    // The frame.
    XrFrameState frameState{XR_TYPE_FRAME_STATE};
    bool frameBegun = false;
    std::array<XrView, 2> views{XrView{XR_TYPE_VIEW}, XrView{XR_TYPE_VIEW}};
    bool viewsValid = false;
    std::array<bool, 2> eyeRendered{false, false};
    bool uiDrawnThisFrame = false;
    bool uiOnQuad = false;  ///< the quad layer has an image to show
    Pose head;
    bool headValid = false;

    // The interface's quad, in the local space.
    Pose quad;
    glm::vec2 quadSize{2.8f, 1.575f};
    bool quadPlaced = false;
    bool quadInWorld = false;  ///< placed for the world (follows) or the menus (stays)
    LazyFollow follow{30.0f, 150.0f};

    // Input.
    XrActionSet actionSet = XR_NULL_HANDLE;
    XrAction moveAction = XR_NULL_HANDLE, turnAction = XR_NULL_HANDLE, poseAction = XR_NULL_HANDLE,
             clickAction = XR_NULL_HANDLE, rightClickAction = XR_NULL_HANDLE, jumpAction = XR_NULL_HANDLE,
             targetAction = XR_NULL_HANDLE, autorunAction = XR_NULL_HANDLE, menuAction = XR_NULL_HANDLE,
             recenterAction = XR_NULL_HANDLE;
    std::array<XrPath, 2> hands{XR_NULL_PATH, XR_NULL_PATH};
    std::array<XrSpace, 2> aimSpaces{XR_NULL_HANDLE, XR_NULL_HANDLE};
    std::array<std::optional<Pose>, 2> aim;
    int pointerHand = 1;  ///< right
    std::array<bool, 2> clickDown{false, false};
    std::array<bool, 2> rightDown{false, false};
    bool mouseLeftHeld = false;
    bool mouseRightHeld = false;
    bool pointerOnQuad = false;
    float laserLength = 0.0f;
    std::array<bool, 4> buttonWas{false, false, false, false};  // jump, target, autorun, menu
    bool recenterWas = false;
    bool tabHeld = false;
    bool escapeHeld = false;
    Turn turn;
    bool moving = false;
    std::array<bool, 4> keysHeld{false, false, false, false};  // W S Q E

    // The body: the world yaw OpenXR's -Z is turned to, and where the head
    // was when that was settled.
    float bodyYawDeg = 0.0f;
    glm::vec3 headOrigin{0.0f};
    bool bodySettled = false;
    bool wasInWorld = false;

    // The camera as the client left it.
    glm::vec3 anchor{0.0f};
    glm::vec3 savedPosition{0.0f};
    bool cameraSaved = false;

    // The mirror on the desktop window.
    VkSwapchainKHR mirror = VK_NULL_HANDLE;
    std::vector<VkImage> mirrorImages;
    VkExtent2D mirrorExtent{0, 0};
    std::vector<VkSemaphore> mirrorRendered;
    VkSemaphore mirrorSpare = VK_NULL_HANDLE;
    std::vector<VkSemaphore> mirrorAcquired;
    bool mirrorDirty = true;
    bool mirrorFailed = false;
    bool mirrorDoneThisFrame = false;

    // ----

    bool startFrame();
    bool createActions();
    bool createSwapchain(XrChain& chain, VkExtent2D extent, uint32_t usage, bool staticImage, const char* what);
    void destroySwapchain(XrChain& chain);
    bool acquire(XrChain& chain);
    void releaseAll();
    void pollEvents();
    void readInput(rendering::Renderer* renderer, SDL_Window* window, bool inWorld, float dt);
    void placeQuad(bool inWorld, float dt, bool recenter);
    bool ensureUiImage(SDL_Window* window);
    void destroyUiImage();
    void recordUi(VkCommandBuffer cmd);
    void copyToChain(VkCommandBuffer cmd, VkImage src, XrChain& chain);
    void recordMirror(VkCommandBuffer cmd, VkImage src, VkExtent2D srcExtent, VkImageLayout srcLayout);
    bool rebuildMirror();
    void destroyMirror();
    void fillLaser(VkCommandBuffer cmd);
    void settleBody(rendering::CameraController* cc);
};

Runtime& Runtime::get() {
    static Runtime runtime;
    return runtime;
}

Runtime::Runtime() : impl_(std::make_unique<Impl>()) {}

Runtime::~Runtime() {
    if (gInstance != XR_NULL_HANDLE) {
        xrDestroyInstance(gInstance);
        gInstance = XR_NULL_HANDLE;
    }
}

bool Runtime::initialized() const { return gInstance != XR_NULL_HANDLE && gSystem != XR_NULL_SYSTEM_ID; }
bool Runtime::exitRequested() const { return impl_->exitRequested; }
PFN_vkGetInstanceProcAddr Runtime::instanceProcAddr() const { return &hookGetInstanceProcAddr; }

// ---------------------------------------------------------------------------
// Start-up

bool Runtime::initialize(std::string& whyNot) {
    if (initialized()) return true;

    uint32_t extCount = 0;
    XrResult r = xrEnumerateInstanceExtensionProperties(nullptr, 0, &extCount, nullptr);
    if (XR_FAILED(r)) {
        whyNot = "No OpenXR runtime was found (" + xrText(r) +
                 ").\n\nOpen the Meta Quest Link app on this PC and, under Settings > General, "
                 "set Meta Quest Link as the active OpenXR runtime (or set SteamVR as the "
                 "OpenXR runtime in SteamVR's settings).";
        return false;
    }
    std::vector<XrExtensionProperties> exts(extCount, XrExtensionProperties{XR_TYPE_EXTENSION_PROPERTIES});
    xrEnumerateInstanceExtensionProperties(nullptr, extCount, &extCount, exts.data());
    bool hasVulkan2 = false;
    for (const auto& e : exts) {
        if (std::strcmp(e.extensionName, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME) == 0) hasVulkan2 = true;
    }
    if (!hasVulkan2) {
        whyNot = "The active OpenXR runtime does not support Vulkan (XR_KHR_vulkan_enable2).\n\n"
                 "Set Meta Quest Link or SteamVR as the active OpenXR runtime.";
        return false;
    }

    const char* enabled[] = {XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME};
    XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
    std::strncpy(ici.applicationInfo.applicationName, "WoWee", XR_MAX_APPLICATION_NAME_SIZE - 1);
    ici.applicationInfo.applicationVersion = 1;
    std::strncpy(ici.applicationInfo.engineName, "WoWee", XR_MAX_ENGINE_NAME_SIZE - 1);
    ici.applicationInfo.engineVersion = 1;
    ici.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    ici.enabledExtensionCount = 1;
    ici.enabledExtensionNames = enabled;
    r = xrCreateInstance(&ici, &gInstance);
    if (XR_FAILED(r)) {
        gInstance = XR_NULL_HANDLE;
        whyNot = "The OpenXR runtime would not start (" + xrText(r) +
                 ").\n\nMake sure the Meta Quest Link app is running and the headset is connected.";
        return false;
    }
    XrInstanceProperties props{XR_TYPE_INSTANCE_PROPERTIES};
    if (XR_SUCCEEDED(xrGetInstanceProperties(gInstance, &props))) {
        LOG_WARNING("OpenXR runtime: ", props.runtimeName, " ", XR_VERSION_MAJOR(props.runtimeVersion), ".",
                    XR_VERSION_MINOR(props.runtimeVersion), ".", XR_VERSION_PATCH(props.runtimeVersion));
    }

    XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    r = xrGetSystem(gInstance, &sgi, &gSystem);
    if (XR_FAILED(r)) {
        gSystem = XR_NULL_SYSTEM_ID;
        whyNot = "No headset is available (" + xrText(r) +
                 ").\n\nConnect the Quest by its USB-C cable, start Quest Link in the headset, "
                 "and put it on - then start WoWee VR again.";
        xrDestroyInstance(gInstance);
        gInstance = XR_NULL_HANDLE;
        return false;
    }
    XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
    if (XR_SUCCEEDED(xrGetSystemProperties(gInstance, gSystem, &sp))) {
        LOG_WARNING("OpenXR headset: ", sp.systemName, " (vendor ", sp.vendorId, ")");
    }

    auto load = [](const char* name, auto& fn) {
        return XR_SUCCEEDED(xrGetInstanceProcAddr(gInstance, name, reinterpret_cast<PFN_xrVoidFunction*>(&fn))) &&
               fn != nullptr;
    };
    if (!load("xrGetVulkanGraphicsRequirements2KHR", pGetRequirements) ||
        !load("xrCreateVulkanInstanceKHR", pCreateVulkanInstance) ||
        !load("xrGetVulkanGraphicsDevice2KHR", pGetVulkanDevice) ||
        !load("xrCreateVulkanDeviceKHR", pCreateVulkanDevice)) {
        whyNot = "The OpenXR runtime is missing its Vulkan functions.";
        xrDestroyInstance(gInstance);
        gInstance = XR_NULL_HANDLE;
        return false;
    }
    // Asked before the instance is made, as the extension requires.
    XrGraphicsRequirementsVulkan2KHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
    if (XR_SUCCEEDED(pGetRequirements(gInstance, gSystem, &req))) {
        LOG_WARNING("OpenXR Vulkan: API ", XR_VERSION_MAJOR(req.minApiVersionSupported), ".",
                    XR_VERSION_MINOR(req.minApiVersionSupported), " to ",
                    XR_VERSION_MAJOR(req.maxApiVersionSupported), ".",
                    XR_VERSION_MINOR(req.maxApiVersionSupported));
    }

    // The view configuration's blend mode: opaque on a Quest.
    uint32_t modeCount = 0;
    xrEnumerateEnvironmentBlendModes(gInstance, gSystem, impl_->viewConfig, 0, &modeCount, nullptr);
    std::vector<XrEnvironmentBlendMode> modes(modeCount);
    if (modeCount > 0 &&
        XR_SUCCEEDED(xrEnumerateEnvironmentBlendModes(gInstance, gSystem, impl_->viewConfig, modeCount, &modeCount,
                                                      modes.data()))) {
        impl_->blendMode = modes[0];
    }
    return true;
}

std::string Runtime::physicalDeviceName(VkInstance instance) {
    if (!initialized() || !pGetVulkanDevice) return {};
    XrVulkanGraphicsDeviceGetInfoKHR info{XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
    info.systemId = gSystem;
    info.vulkanInstance = instance;
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    if (!xrOk(pGetVulkanDevice(gInstance, &info, &pd), "xrGetVulkanGraphicsDevice2KHR") || !pd) return {};
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(pd, &props);
    LOG_WARNING("OpenXR: the headset is on ", props.deviceName);
    return props.deviceName;
}

bool Runtime::createSession(rendering::VkContext& vk) {
    Impl& m = *impl_;
    m.vk = &vk;
    XrGraphicsBindingVulkan2KHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR};
    binding.instance = vk.getInstance();
    binding.physicalDevice = vk.getPhysicalDevice();
    binding.device = vk.getDevice();
    binding.queueFamilyIndex = vk.getGraphicsQueueFamily();
    binding.queueIndex = 0;
    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &binding;
    sci.systemId = gSystem;
    if (!xrOk(xrCreateSession(gInstance, &sci, &m.session), "xrCreateSession")) return false;

    XrReferenceSpaceCreateInfo rsci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rsci.poseInReferenceSpace = toXr(Pose{});
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    if (!xrOk(xrCreateReferenceSpace(m.session, &rsci, &m.local), "local space")) return false;
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    if (!xrOk(xrCreateReferenceSpace(m.session, &rsci, &m.view), "view space")) return false;

    // The eyes' size: the runtime's recommendation, the larger of the two.
    uint32_t viewCount = 0;
    xrEnumerateViewConfigurationViews(gInstance, gSystem, m.viewConfig, 0, &viewCount, nullptr);
    std::vector<XrViewConfigurationView> vcv(viewCount, XrViewConfigurationView{XR_TYPE_VIEW_CONFIGURATION_VIEW});
    if (viewCount < 2 ||
        !xrOk(xrEnumerateViewConfigurationViews(gInstance, gSystem, m.viewConfig, viewCount, &viewCount, vcv.data()),
              "view configuration")) {
        return false;
    }
    const float scale = std::clamp(envFloat("WOWEE_VR_RESOLUTION_SCALE", 1.0f), 0.3f, 2.0f);
    uint32_t w = std::max(vcv[0].recommendedImageRectWidth, vcv[1].recommendedImageRectWidth);
    uint32_t h = std::max(vcv[0].recommendedImageRectHeight, vcv[1].recommendedImageRectHeight);
    w = std::min(static_cast<uint32_t>(std::lround(w * scale)), vcv[0].maxImageRectWidth);
    h = std::min(static_cast<uint32_t>(std::lround(h * scale)), vcv[0].maxImageRectHeight);
    m.eyeExtent = {w, h};
    LOG_WARNING("OpenXR: eyes ", w, "x", h, " (recommended ", vcv[0].recommendedImageRectWidth, "x",
                vcv[0].recommendedImageRectHeight, ", scale ", scale, ")");

    // The format: an sRGB one the compositor decodes.
    uint32_t fmtCount = 0;
    xrEnumerateSwapchainFormats(m.session, 0, &fmtCount, nullptr);
    std::vector<int64_t> formats(fmtCount);
    xrEnumerateSwapchainFormats(m.session, fmtCount, &fmtCount, formats.data());
    for (VkFormat want : {VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_SRGB}) {
        if (std::find(formats.begin(), formats.end(), static_cast<int64_t>(want)) != formats.end()) {
            m.xrFormat = want;
            break;
        }
    }
    if (m.xrFormat == VK_FORMAT_UNDEFINED) {
        LOG_ERROR("OpenXR: the runtime offers no 8-bit sRGB swapchain format");
        return false;
    }

    for (int e = 0; e < 2; ++e) {
        if (!m.createSwapchain(m.eyes[e], m.eyeExtent,
                               XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT, false,
                               e == 0 ? "left eye" : "right eye")) {
            return false;
        }
    }
    if (!m.createSwapchain(m.laserChain, VkExtent2D{4, 64},
                           XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT, true,
                           "pointer")) {
        LOG_WARNING("OpenXR: no pointer ray (its swapchain could not be made)");
    }
    if (!m.createActions()) LOG_WARNING("OpenXR: controllers unavailable (actions could not be made)");

    Turn::Settings ts;
    ts.smooth = envOn("WOWEE_VR_SMOOTH_TURN", false);
    ts.stepDeg = envFloat("WOWEE_VR_SNAP_DEGREES", 30.0f);
    m.turn = Turn(ts);
    LOG_WARNING("OpenXR: session created; ", ts.smooth ? "smooth" : "snap", " turning");
    return true;
}

bool Runtime::Impl::createSwapchain(XrChain& chain, VkExtent2D extent, uint32_t usage, bool staticImage,
                                    const char* what) {
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.createFlags = staticImage ? XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT : 0;
    ci.usageFlags = usage;
    ci.format = xrFormat;
    ci.sampleCount = 1;
    ci.width = extent.width;
    ci.height = extent.height;
    ci.faceCount = 1;
    ci.arraySize = 1;
    ci.mipCount = 1;
    if (!xrOk(xrCreateSwapchain(session, &ci, &chain.handle), what)) {
        chain.handle = XR_NULL_HANDLE;
        return false;
    }
    uint32_t count = 0;
    xrEnumerateSwapchainImages(chain.handle, 0, &count, nullptr);
    std::vector<XrSwapchainImageVulkan2KHR> imgs(count, XrSwapchainImageVulkan2KHR{XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
    xrEnumerateSwapchainImages(chain.handle, count, &count,
                               reinterpret_cast<XrSwapchainImageBaseHeader*>(imgs.data()));
    chain.images.clear();
    for (const auto& i : imgs) chain.images.push_back(i.image);
    chain.extent = extent;
    chain.acquired = UINT32_MAX;
    return true;
}

void Runtime::Impl::destroySwapchain(XrChain& chain) {
    if (chain.handle != XR_NULL_HANDLE) xrDestroySwapchain(chain.handle);
    chain = {};
}

bool Runtime::Impl::acquire(XrChain& chain) {
    if (chain.handle == XR_NULL_HANDLE) return false;
    if (chain.acquired != UINT32_MAX) return true;
    uint32_t index = 0;
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (!xrOk(xrAcquireSwapchainImage(chain.handle, &ai, &index), "xrAcquireSwapchainImage")) return false;
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    if (!xrOk(xrWaitSwapchainImage(chain.handle, &wi), "xrWaitSwapchainImage")) {
        // Acquired all the same: it has to go back.
        XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xrReleaseSwapchainImage(chain.handle, &ri);
        return false;
    }
    chain.acquired = index;
    return true;
}

void Runtime::Impl::releaseAll() {
    for (XrChain* c : {&eyes[0], &eyes[1], &uiChain, &laserChain}) {
        if (c->handle == XR_NULL_HANDLE || c->acquired == UINT32_MAX) continue;
        XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xrReleaseSwapchainImage(c->handle, &ri);
        c->acquired = UINT32_MAX;
    }
}

bool Runtime::Impl::createActions() {
    XrActionSetCreateInfo asci{XR_TYPE_ACTION_SET_CREATE_INFO};
    std::strcpy(asci.actionSetName, "gameplay");
    std::strcpy(asci.localizedActionSetName, "Gameplay");
    if (!xrOk(xrCreateActionSet(gInstance, &asci, &actionSet), "action set")) return false;
    xrStringToPath(gInstance, "/user/hand/left", &hands[0]);
    xrStringToPath(gInstance, "/user/hand/right", &hands[1]);

    auto make = [&](XrAction& out, const char* name, const char* label, XrActionType type, bool perHand) {
        XrActionCreateInfo aci{XR_TYPE_ACTION_CREATE_INFO};
        aci.actionType = type;
        std::strcpy(aci.actionName, name);
        std::strcpy(aci.localizedActionName, label);
        if (perHand) {
            aci.countSubactionPaths = 2;
            aci.subactionPaths = hands.data();
        }
        return xrOk(xrCreateAction(actionSet, &aci, &out), name);
    };
    bool ok = make(moveAction, "move", "Move", XR_ACTION_TYPE_VECTOR2F_INPUT, false) &&
              make(turnAction, "turn", "Turn", XR_ACTION_TYPE_VECTOR2F_INPUT, false) &&
              make(poseAction, "pointer", "Pointer", XR_ACTION_TYPE_POSE_INPUT, true) &&
              make(clickAction, "click", "Click", XR_ACTION_TYPE_FLOAT_INPUT, true) &&
              make(rightClickAction, "right_click", "Right click", XR_ACTION_TYPE_FLOAT_INPUT, true) &&
              make(jumpAction, "jump", "Jump", XR_ACTION_TYPE_BOOLEAN_INPUT, false) &&
              make(targetAction, "target", "Target nearest enemy", XR_ACTION_TYPE_BOOLEAN_INPUT, false) &&
              make(autorunAction, "autorun", "Auto-run", XR_ACTION_TYPE_BOOLEAN_INPUT, false) &&
              make(menuAction, "menu", "Game menu", XR_ACTION_TYPE_BOOLEAN_INPUT, false) &&
              make(recenterAction, "recenter", "Recenter", XR_ACTION_TYPE_BOOLEAN_INPUT, false);
    if (!ok) return false;

    auto suggest = [&](const char* profile, std::initializer_list<std::pair<XrAction, const char*>> list) {
        std::vector<XrActionSuggestedBinding> b;
        for (const auto& [action, path] : list) {
            XrPath p = XR_NULL_PATH;
            if (XR_SUCCEEDED(xrStringToPath(gInstance, path, &p))) b.push_back({action, p});
        }
        XrPath prof = XR_NULL_PATH;
        xrStringToPath(gInstance, profile, &prof);
        XrInteractionProfileSuggestedBinding s{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        s.interactionProfile = prof;
        s.countSuggestedBindings = static_cast<uint32_t>(b.size());
        s.suggestedBindings = b.data();
        const XrResult r = xrSuggestInteractionProfileBindings(gInstance, &s);
        if (XR_FAILED(r)) LOG_WARNING("OpenXR: bindings for ", profile, " refused: ", xrText(r));
    };
    suggest("/interaction_profiles/oculus/touch_controller",
            {{moveAction, "/user/hand/left/input/thumbstick"},
             {turnAction, "/user/hand/right/input/thumbstick"},
             {poseAction, "/user/hand/left/input/aim/pose"},
             {poseAction, "/user/hand/right/input/aim/pose"},
             {clickAction, "/user/hand/left/input/trigger/value"},
             {clickAction, "/user/hand/right/input/trigger/value"},
             {rightClickAction, "/user/hand/left/input/squeeze/value"},
             {rightClickAction, "/user/hand/right/input/squeeze/value"},
             {jumpAction, "/user/hand/right/input/a/click"},
             {targetAction, "/user/hand/right/input/b/click"},
             {autorunAction, "/user/hand/left/input/x/click"},
             {menuAction, "/user/hand/left/input/y/click"},
             {menuAction, "/user/hand/left/input/menu/click"},
             {recenterAction, "/user/hand/right/input/thumbstick/click"}});
    suggest("/interaction_profiles/khr/simple_controller",
            {{poseAction, "/user/hand/left/input/aim/pose"},
             {poseAction, "/user/hand/right/input/aim/pose"},
             {clickAction, "/user/hand/left/input/select/click"},
             {clickAction, "/user/hand/right/input/select/click"},
             {menuAction, "/user/hand/left/input/menu/click"},
             {menuAction, "/user/hand/right/input/menu/click"}});

    for (int h = 0; h < 2; ++h) {
        XrActionSpaceCreateInfo sci{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        sci.action = poseAction;
        sci.subactionPath = hands[h];
        sci.poseInActionSpace = toXr(Pose{});
        xrOk(xrCreateActionSpace(session, &sci, &aimSpaces[h]), "aim space");
    }
    XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attach.countActionSets = 1;
    attach.actionSets = &actionSet;
    return xrOk(xrAttachSessionActionSets(session, &attach), "attach actions");
}

bool Runtime::createEyeTargets(rendering::VkContext& vk, uint32_t count, std::vector<VkImage>& images,
                               std::vector<VkImageView>& views, VkExtent2D& extent, VkFormat& format) {
    Impl& m = *impl_;
    destroyEyeTargets(vk);
    const VkFormat f = unormTwin(m.xrFormat);
    for (uint32_t i = 0; i < count; ++i) {
        OwnImage img;
        if (!createOwnImage(vk, f, m.eyeExtent,
                            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                            img)) {
            destroyEyeTargets(vk);
            return false;
        }
        m.eyeTargets.push_back(img);
    }
    images.clear();
    views.clear();
    for (const auto& t : m.eyeTargets) {
        images.push_back(t.image);
        views.push_back(t.view);
    }
    extent = m.eyeExtent;
    format = f;
    LOG_WARNING("VR: rendering each eye into ", count, " images of ", extent.width, "x", extent.height);
    return true;
}

void Runtime::destroyEyeTargets(rendering::VkContext& vk) {
    for (auto& t : impl_->eyeTargets) destroyOwnImage(vk, t);
    impl_->eyeTargets.clear();
}

void Runtime::shutdown() {
    Impl& m = *impl_;
    if (!m.vk) return;
    rendering::VkContext& vk = *m.vk;
    if (vk.getDevice()) vkDeviceWaitIdle(vk.getDevice());
    m.releaseAll();
    m.destroyMirror();
    m.destroyUiImage();
    destroyEyeTargets(vk);
    for (auto& e : m.eyes) m.destroySwapchain(e);
    m.destroySwapchain(m.uiChain);
    m.destroySwapchain(m.laserChain);
    for (auto& s : m.aimSpaces) {
        if (s != XR_NULL_HANDLE) xrDestroySpace(s);
        s = XR_NULL_HANDLE;
    }
    if (m.actionSet != XR_NULL_HANDLE) xrDestroyActionSet(m.actionSet);
    m.actionSet = XR_NULL_HANDLE;
    if (m.local != XR_NULL_HANDLE) xrDestroySpace(m.local);
    if (m.view != XR_NULL_HANDLE) xrDestroySpace(m.view);
    m.local = m.view = XR_NULL_HANDLE;
    if (m.session != XR_NULL_HANDLE) {
        if (m.running) xrEndSession(m.session);
        xrDestroySession(m.session);
    }
    m.session = XR_NULL_HANDLE;
    m.running = false;
    m.vk = nullptr;
}

// ---------------------------------------------------------------------------
// Each frame

void Runtime::Impl::pollEvents() {
    XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(gInstance, &ev) == XR_SUCCESS) {
        switch (ev.type) {
            case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
                const auto& s = *reinterpret_cast<XrEventDataSessionStateChanged*>(&ev);
                state = s.state;
                LOG_WARNING("OpenXR session state ", static_cast<int>(state));
                if (state == XR_SESSION_STATE_READY && !running) {
                    XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
                    bi.primaryViewConfigurationType = viewConfig;
                    running = xrOk(xrBeginSession(session, &bi), "xrBeginSession");
                    if (running) LOG_WARNING("OpenXR: session running - the headset is showing WoWee");
                } else if (state == XR_SESSION_STATE_STOPPING && running) {
                    xrEndSession(session);
                    running = false;
                    LOG_WARNING("OpenXR: session stopped");
                } else if (state == XR_SESSION_STATE_EXITING || state == XR_SESSION_STATE_LOSS_PENDING) {
                    exitRequested = true;
                    LOG_WARNING("OpenXR: the runtime asked WoWee to quit");
                }
                break;
            }
            case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
                exitRequested = true;
                LOG_WARNING("OpenXR: the runtime is going away");
                break;
            case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING:
                // The headset was recentred from its own menu: settle again.
                bodySettled = false;
                quadPlaced = false;
                break;
            default:
                break;
        }
        ev = XrEventDataBuffer{XR_TYPE_EVENT_DATA_BUFFER};
    }
}

void Runtime::beginFrame(rendering::Renderer* renderer, SDL_Window* window, bool inWorld, float deltaSeconds) {
    Impl& m = *impl_;
    recordingEye_ = -1;
    m.eyeRendered = {false, false};
    m.uiDrawnThisFrame = false;
    m.mirrorDoneThisFrame = false;
    m.viewsValid = false;
    if (m.session == XR_NULL_HANDLE) return;
    m.pollEvents();
    if (!m.running) {
        // Nothing to pace against: the window is all there is.
        SDL_Delay(10);
        return;
    }

    if (!m.startFrame()) return;
    m.readInput(renderer, window, inWorld, deltaSeconds);
}

bool Runtime::Impl::startFrame() {
    XrFrameWaitInfo wi{XR_TYPE_FRAME_WAIT_INFO};
    frameState = XrFrameState{XR_TYPE_FRAME_STATE};
    if (!xrOk(xrWaitFrame(session, &wi, &frameState), "xrWaitFrame")) return false;
    XrFrameBeginInfo bi{XR_TYPE_FRAME_BEGIN_INFO};
    const XrResult br = xrBeginFrame(session, &bi);
    if (XR_FAILED(br)) {
        xrOk(br, "xrBeginFrame");
        return false;
    }
    frameBegun = true;

    // The eyes, for the time this frame will be seen.
    XrViewLocateInfo vli{XR_TYPE_VIEW_LOCATE_INFO};
    vli.viewConfigurationType = viewConfig;
    vli.displayTime = frameState.predictedDisplayTime;
    vli.space = local;
    XrViewState vs{XR_TYPE_VIEW_STATE};
    uint32_t count = 0;
    views = {XrView{XR_TYPE_VIEW}, XrView{XR_TYPE_VIEW}};
    if (XR_SUCCEEDED(xrLocateViews(session, &vli, &vs, 2, &count, views.data())) && count == 2 &&
        (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) &&
        (vs.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT)) {
        viewsValid = true;
    }
    XrSpaceLocation hl{XR_TYPE_SPACE_LOCATION};
    headValid = false;
    if (XR_SUCCEEDED(xrLocateSpace(view, local, frameState.predictedDisplayTime, &hl)) &&
        (hl.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
        head = toPose(hl.pose);
        headValid = true;
    }
    return true;
}

void Runtime::beginLoadingFrame(SDL_Window* window) {
    Impl& m = *impl_;
    recordingEye_ = -1;
    m.uiDrawnThisFrame = false;
    m.mirrorDoneThisFrame = false;
    if (m.session == XR_NULL_HANDLE) return;
    if (window) {
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(window, &w, &h);
        if (w > 0 && h > 0) m.uiExtent = {static_cast<uint32_t>(w), static_cast<uint32_t>(h)};
    }
    // The main loop's frame, if it is still open, is this one's; otherwise a
    // frame of its own. Either way the loading picture is shown on the menus'
    // big screen.
    if (!m.frameBegun) {
        m.pollEvents();
        if (!m.running) return;
        m.eyeRendered = {false, false};
        m.viewsValid = false;
        if (!m.startFrame()) return;
    }
    m.placeQuad(false, 0.0f, false);
}

int Runtime::eyesToRender(bool inWorld) const {
    const Impl& m = *impl_;
    if (!inWorld || !m.frameBegun || !m.frameState.shouldRender || !m.viewsValid) return 0;
    if (m.eyes[0].handle == XR_NULL_HANDLE || m.eyes[1].handle == XR_NULL_HANDLE) return 0;
    return 2;
}

void Runtime::Impl::settleBody(rendering::CameraController* cc) {
    if (!headValid) return;
    const glm::vec3 fwd = head.orientation * glm::vec3(0.0f, 0.0f, -1.0f);
    // The head's yaw in the world with the body at 0, and the body turned so
    // that it comes out at the character's own: looking ahead is looking
    // where the character looks.
    const float headYaw = worldYawDeg(xrToWorld(fwd, 0.0f));
    bodyYawDeg = wrapDegrees((cc ? cc->getYaw() : 0.0f) - headYaw);
    headOrigin = head.position;
    bodySettled = true;
}

void Runtime::Impl::placeQuad(bool inWorld, float dt, bool recenter) {
    if (!headValid) return;
    const glm::vec3 fwd = head.orientation * glm::vec3(0.0f, 0.0f, -1.0f);
    const float headYawDeg = glm::degrees(xrYawOf(fwd));
    const float aspect = uiExtent.width > 0 ? static_cast<float>(uiExtent.height) / uiExtent.width : 9.0f / 16.0f;
    if (!quadPlaced || recenter || inWorld != quadInWorld) {
        follow.snapTo(headYawDeg);
        quadPlaced = true;
        quadInWorld = inWorld;
        if (!inWorld) {
            // The menus: one big screen, standing where it was put.
            const float width = envFloat("WOWEE_VR_MENU_WIDTH", 3.0f);
            const float dist = envFloat("WOWEE_VR_MENU_DISTANCE", 2.6f);
            const glm::quat yaw = glm::angleAxis(glm::radians(headYawDeg), glm::vec3(0, 1, 0));
            quad.orientation = yaw;
            quad.position = head.position + yaw * glm::vec3(0.0f, 0.0f, -dist);
            quadSize = glm::vec2(width, width * aspect);
        }
    }
    if (inWorld) {
        // The world: a panel ahead that the head can look around, brought
        // back in front when it looks well away. Its distance and height
        // follow the head at once, so leaning does not push into it.
        const float width = envFloat("WOWEE_VR_HUD_WIDTH", 1.9f);
        const float dist = envFloat("WOWEE_VR_HUD_DISTANCE", 1.4f);
        const float yawDeg = follow.update(headYawDeg, dt);
        const glm::quat yaw = glm::angleAxis(glm::radians(yawDeg), glm::vec3(0, 1, 0));
        quad.orientation = yaw;
        quad.position = head.position + yaw * glm::vec3(0.0f, -0.05f, -dist);
        quadSize = glm::vec2(width, width * aspect);
    }
}

void Runtime::Impl::readInput(rendering::Renderer* renderer, SDL_Window* window, bool inWorld, float dt) {
    if (window) {
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(window, &w, &h);
        if (w > 0 && h > 0) uiExtent = {static_cast<uint32_t>(w), static_cast<uint32_t>(h)};
    }
    rendering::CameraController* cc = renderer ? renderer->getCameraController() : nullptr;
    auto& input = core::Input::getInstance();
    ImGuiIO* io = ImGui::GetCurrentContext() ? &ImGui::GetIO() : nullptr;

    bool recenter = false;
    glm::vec2 move(0.0f), turnStick(0.0f);
    float click[2] = {0.0f, 0.0f}, rclick[2] = {0.0f, 0.0f};
    bool buttons[4] = {false, false, false, false};
    if (actionSet != XR_NULL_HANDLE) {
        XrActiveActionSet active{actionSet, XR_NULL_PATH};
        XrActionsSyncInfo si{XR_TYPE_ACTIONS_SYNC_INFO};
        si.countActiveActionSets = 1;
        si.activeActionSets = &active;
        if (XR_SUCCEEDED(xrSyncActions(session, &si))) {
            auto vec2 = [&](XrAction a) {
                XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
                gi.action = a;
                XrActionStateVector2f st{XR_TYPE_ACTION_STATE_VECTOR2F};
                if (XR_SUCCEEDED(xrGetActionStateVector2f(session, &gi, &st)) && st.isActive)
                    return glm::vec2(st.currentState.x, st.currentState.y);
                return glm::vec2(0.0f);
            };
            auto flt = [&](XrAction a, XrPath sub) {
                XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
                gi.action = a;
                gi.subactionPath = sub;
                XrActionStateFloat st{XR_TYPE_ACTION_STATE_FLOAT};
                if (XR_SUCCEEDED(xrGetActionStateFloat(session, &gi, &st)) && st.isActive) return st.currentState;
                return 0.0f;
            };
            auto boolean = [&](XrAction a) {
                XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
                gi.action = a;
                XrActionStateBoolean st{XR_TYPE_ACTION_STATE_BOOLEAN};
                return XR_SUCCEEDED(xrGetActionStateBoolean(session, &gi, &st)) && st.isActive &&
                       st.currentState == XR_TRUE;
            };
            move = vec2(moveAction);
            turnStick = vec2(turnAction);
            for (int h = 0; h < 2; ++h) {
                click[h] = flt(clickAction, hands[h]);
                rclick[h] = flt(rightClickAction, hands[h]);
            }
            buttons[0] = boolean(jumpAction);
            buttons[1] = boolean(targetAction);
            buttons[2] = boolean(autorunAction);
            buttons[3] = boolean(menuAction);
            const bool rc = boolean(recenterAction);
            recenter = rc && !recenterWas;
            recenterWas = rc;
        }
        for (int h = 0; h < 2; ++h) {
            aim[h].reset();
            if (aimSpaces[h] == XR_NULL_HANDLE) continue;
            XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
            if (XR_SUCCEEDED(xrLocateSpace(aimSpaces[h], local, frameState.predictedDisplayTime, &loc)) &&
                (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) &&
                (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) {
                aim[h] = toPose(loc.pose);
            }
        }
    }

    // The body and the interface's panel, once there is a head to place
    // them by; again on the recenter click, or entering or leaving the world.
    if (inWorld != wasInWorld) {
        bodySettled = false;
        wasInWorld = inWorld;
    }
    if (recenter) bodySettled = false;
    if (!bodySettled && inWorld) settleBody(cc);
    placeQuad(inWorld, dt, recenter);

    // ---- the pointer ----
    // Triggers with a little hysteresis; the hand that pulled last points.
    bool clickNow[2], rightNow[2];
    for (int h = 0; h < 2; ++h) {
        clickNow[h] = clickDown[h] ? click[h] > 0.4f : click[h] > 0.6f;
        rightNow[h] = rightDown[h] ? rclick[h] > 0.4f : rclick[h] > 0.6f;
        if ((clickNow[h] && !clickDown[h]) || (rightNow[h] && !rightDown[h])) pointerHand = h;
    }
    if (!aim[pointerHand] && aim[1 - pointerHand]) pointerHand = 1 - pointerHand;
    std::optional<QuadHit> hit;
    laserLength = 0.0f;
    if (aim[pointerHand] && uiExtent.width > 0) {
        const Pose& a = *aim[pointerHand];
        const glm::vec3 dir = a.orientation * glm::vec3(0.0f, 0.0f, -1.0f);
        hit = rayQuadHit(a.position, dir, quad, quadSize,
                         glm::ivec2(static_cast<int>(uiExtent.width), static_cast<int>(uiExtent.height)));
        laserLength = hit ? hit->distance : 2.0f;
    }
    if (io) {
        if (hit) {
            const float sx = io->DisplayFramebufferScale.x > 0.0f ? io->DisplayFramebufferScale.x : 1.0f;
            const float sy = io->DisplayFramebufferScale.y > 0.0f ? io->DisplayFramebufferScale.y : 1.0f;
            io->AddMousePosEvent(hit->pixel.x / sx, hit->pixel.y / sy);
            pointerOnQuad = true;
        } else if (pointerOnQuad) {
            io->AddMousePosEvent(-FLT_MAX, -FLT_MAX);
            pointerOnQuad = false;
        }
        // A press counts only on the interface; a release is always passed
        // on, so a button cannot be left held when the ray slips off.
        const bool left = clickNow[pointerHand];
        const bool right = rightNow[pointerHand];
        if (left != mouseLeftHeld && (!left || hit)) {
            io->AddMouseButtonEvent(ImGuiMouseButton_Left, left);
            mouseLeftHeld = left;
        }
        if (right != mouseRightHeld && (!right || hit)) {
            io->AddMouseButtonEvent(ImGuiMouseButton_Right, right);
            mouseRightHeld = right;
        }
    }
    for (int h = 0; h < 2; ++h) {
        clickDown[h] = clickNow[h];
        rightDown[h] = rightNow[h];
    }

    // ---- buttons ----
    auto key = [&](ImGuiKey imguiKey, SDL_Scancode sc, bool down, bool& held) {
        if (down == held) return;
        held = down;
        if (io) io->AddKeyEvent(imguiKey, down);
        input.setVirtualKey(sc, down);
    };
    key(ImGuiKey_Tab, SDL_SCANCODE_TAB, buttons[1], tabHeld);
    key(ImGuiKey_Escape, SDL_SCANCODE_ESCAPE, buttons[3], escapeHeld);
    const bool jumpPressed = buttons[0] && !buttonWas[0];
    const bool autorunPressed = buttons[2] && !buttonWas[2];
    for (int i = 0; i < 4; ++i) buttonWas[i] = buttons[i];

    // ---- the character ----
    if (!inWorld || !cc) {
        // Nothing to walk: let go of anything held.
        const SDL_Scancode moveKeys[4] = {SDL_SCANCODE_W, SDL_SCANCODE_S, SDL_SCANCODE_Q, SDL_SCANCODE_E};
        for (int i = 0; i < 4; ++i) {
            if (keysHeld[i]) input.setVirtualKey(moveKeys[i], false);
            keysHeld[i] = false;
        }
        moving = false;
        return;
    }

    // First person: the camera at the character's eyes.
    if (!cc->isFirstPersonView()) cc->zoomBy(100.0f);

    // The turn: the body yaw, which the head's own turns ride on.
    bodyYawDeg = wrapDegrees(bodyYawDeg + turn.update(turnStick.x, dt));

    // The view yaw and pitch follow the head, so the character walks and
    // faces where the player looks - and swims and flies that way too.
    if (headValid && bodySettled && !cc->isIntroActive()) {
        const glm::vec3 fwdXr = head.orientation * glm::vec3(0.0f, 0.0f, -1.0f);
        const glm::vec3 fwd = xrToWorld(fwdXr, bodyYawDeg);
        const float targetYaw = worldYawDeg(fwd);
        const float targetPitch = glm::degrees(std::asin(std::clamp(fwd.z, -1.0f, 1.0f)));
        const float sens = std::max(cc->getMouseSensitivity(), 1e-4f);
        const float dYaw = wrapDegrees(targetYaw - cc->getYaw());
        const float dPitch = targetPitch - cc->getPitch();
        const float invert = cc->isInvertMouse() ? 1.0f : -1.0f;
        if (std::abs(dYaw) > 0.01f || std::abs(dPitch) > 0.01f) {
            cc->applyLookDelta(-dYaw / sens, dPitch / (sens * invert));
        }
    }

    // The left stick: the movement keys, the facing following the view
    // while it is pushed - so up on the stick is where the head looks.
    const MoveKeys mk = stickToMoveKeys(move, 0.25f);
    const bool want[4] = {mk.forward, mk.back, mk.left, mk.right};
    const SDL_Scancode moveKeys[4] = {SDL_SCANCODE_W, SDL_SCANCODE_S, SDL_SCANCODE_Q, SDL_SCANCODE_E};
    for (int i = 0; i < 4; ++i) {
        if (want[i] != keysHeld[i]) input.setVirtualKey(moveKeys[i], want[i]);
        keysHeld[i] = want[i];
    }
    moving = mk.any();
    if (moving || cc->isAutoRunning()) cc->setSteering(true);
    if (jumpPressed) cc->pressBindingJump();
    if (autorunPressed) cc->toggleAutoRun();
}

void Runtime::applyEyeCamera(int eye, rendering::Camera& camera) {
    Impl& m = *impl_;
    if (eye < 0 || eye > 1 || !m.viewsValid) return;
    if (!m.cameraSaved) {
        // The first-person camera stands half a yard behind the head, along
        // the view (CameraController's MIN_DISTANCE); the eyes are at the head.
        m.savedPosition = camera.getPosition();
        m.anchor = camera.getPosition() + camera.getForward() * 0.5f;
        m.cameraSaved = true;
    }
    const XrView& v = m.views[static_cast<size_t>(eye)];
    const Pose pose = toPose(v.pose);
    const glm::vec3 offset = (pose.position - m.headOrigin) * kYardsPerMetre;
    camera.setPosition(m.anchor + xrToWorld(offset, m.bodyYawDeg));
    const glm::vec3 fwd = xrToWorld(pose.orientation * glm::vec3(0.0f, 0.0f, -1.0f), m.bodyYawDeg);
    const glm::vec3 up = xrToWorld(pose.orientation * glm::vec3(0.0f, 1.0f, 0.0f), m.bodyYawDeg);
    camera.setLookDirection(fwd, up);
    const Fov fov{v.fov.angleLeft, v.fov.angleRight, v.fov.angleUp, v.fov.angleDown};
    camera.setProjectionOverride(eyeForwardProjection(fov, camera.getNearPlane(), camera.getFarPlane()));
}

void Runtime::restoreCamera(rendering::Camera& camera) {
    Impl& m = *impl_;
    if (!m.cameraSaved) return;
    camera.clearLookOverride();
    camera.setPosition(m.savedPosition);
    camera.clearProjectionOverride();
    m.cameraSaved = false;
}

// ---------------------------------------------------------------------------
// Recording

bool Runtime::Impl::ensureUiImage(SDL_Window*) {
    if (!vk || uiExtent.width == 0 || uiExtent.height == 0) return false;
    if (ui.image && uiChain.handle != XR_NULL_HANDLE && uiChain.extent.width == uiExtent.width &&
        uiChain.extent.height == uiExtent.height) {
        return true;
    }
    // A new size (the window was resized): everything made again.
    vkDeviceWaitIdle(vk->getDevice());
    destroyUiImage();
    if (!createOwnImage(*vk, vk->getSwapchainFormat(), uiExtent,
                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, ui)) {
        return false;
    }
    VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fi.renderPass = vk->getOverlayClearRenderPass();
    fi.attachmentCount = 1;
    fi.pAttachments = &ui.view;
    fi.width = uiExtent.width;
    fi.height = uiExtent.height;
    fi.layers = 1;
    if (!fi.renderPass || vkCreateFramebuffer(vk->getDevice(), &fi, nullptr, &uiFramebuffer) != VK_SUCCESS) {
        destroyUiImage();
        return false;
    }
    if (!createSwapchain(uiChain, uiExtent, XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT,
                         false, "interface")) {
        destroyUiImage();
        return false;
    }
    LOG_WARNING("VR: interface panel ", uiExtent.width, "x", uiExtent.height);
    return true;
}

void Runtime::Impl::destroyUiImage() {
    if (vk && uiFramebuffer) vkDestroyFramebuffer(vk->getDevice(), uiFramebuffer, nullptr);
    uiFramebuffer = VK_NULL_HANDLE;
    if (vk) destroyOwnImage(*vk, ui);
    destroySwapchain(uiChain);
    uiOnQuad = false;
}

void Runtime::Impl::copyToChain(VkCommandBuffer cmd, VkImage src, XrChain& chain) {
    if (!acquire(chain)) return;
    VkImage dst = chain.images[chain.acquired];
    imageBarrier(cmd, dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.extent = {chain.extent.width, chain.extent.height, 1};
    vkCmdCopyImage(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                   &region);
    // Handed back in the layout OpenXR expects a colour image in.
    imageBarrier(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
}

void Runtime::Impl::recordUi(VkCommandBuffer cmd) {
    if (!ensureUiImage(nullptr)) return;
    ImDrawData* draw = ImGui::GetDrawData();
    VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rp.renderPass = vk->getOverlayClearRenderPass();
    rp.framebuffer = uiFramebuffer;
    rp.renderArea.extent = uiExtent;
    VkClearValue clear{};
    clear.color = {{0.0f, 0.0f, 0.0f, 0.0f}};  // transparent where nothing is drawn
    rp.clearValueCount = 1;
    rp.pClearValues = &clear;
    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp{0.0f, 0.0f, static_cast<float>(uiExtent.width), static_cast<float>(uiExtent.height), 0.0f, 1.0f};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    VkRect2D sc{{0, 0}, uiExtent};
    vkCmdSetScissor(cmd, 0, 1, &sc);
    if (draw && draw->Valid) ImGui_ImplVulkan_RenderDrawData(draw, cmd);
    vkCmdEndRenderPass(cmd);
    // The pass leaves it ready to present; it is copied instead.
    imageBarrier(cmd, ui.image, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    if (running && frameBegun) {
        copyToChain(cmd, ui.image, uiChain);
        uiOnQuad = uiChain.acquired != UINT32_MAX || uiOnQuad;
    }
}

void Runtime::Impl::fillLaser(VkCommandBuffer cmd) {
    if (laserFilled || laserChain.handle == XR_NULL_HANDLE || !acquire(laserChain)) return;
    VkImage img = laserChain.images[laserChain.acquired];
    imageBarrier(cmd, img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    // A pale blue, premultiplied, mostly opaque. The image is sRGB: the
    // clear colour is linear and the hardware encodes it.
    VkClearColorValue c{{0.15f * 0.85f, 0.45f * 0.85f, 1.0f * 0.85f, 0.85f}};
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c, 1, &range);
    imageBarrier(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    laserFilled = true;
}

void Runtime::recordAfterFrame(VkCommandBuffer cmd, VkImage engineImage) {
    Impl& m = *impl_;
    if (!m.vk || cmd == VK_NULL_HANDLE) return;

    // The interface, once a frame, in whichever engine frame comes first.
    if (!m.uiDrawnThisFrame) {
        m.recordUi(cmd);
        m.uiDrawnThisFrame = true;
    }
    if (m.running && m.frameBegun) m.fillLaser(cmd);

    // The eye, into the headset.
    const VkExtent2D ext = m.vk->getSwapchainExtent();
    imageBarrier(cmd, engineImage, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                 VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                 VK_ACCESS_TRANSFER_READ_BIT);
    if (recordingEye_ >= 0 && recordingEye_ < 2 && m.running && m.frameBegun) {
        m.copyToChain(cmd, engineImage, m.eyes[static_cast<size_t>(recordingEye_)]);
        m.eyeRendered[static_cast<size_t>(recordingEye_)] = m.eyes[static_cast<size_t>(recordingEye_)].acquired != UINT32_MAX;
    }

    // The window: the left eye in the world, the interface in the menus.
    if (!m.mirrorDoneThisFrame && recordingEye_ <= 0) {
        if (recordingEye_ == 0) {
            m.recordMirror(cmd, engineImage, ext, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        } else if (m.ui.image) {
            m.recordMirror(cmd, m.ui.image, m.uiExtent, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        }
        m.mirrorDoneThisFrame = true;
    }
    imageBarrier(cmd, engineImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0);
}

void Runtime::afterSubmit() { impl_->releaseAll(); }

void Runtime::endFrame() {
    Impl& m = *impl_;
    recordingEye_ = -1;
    if (!m.frameBegun) return;
    m.frameBegun = false;
    m.releaseAll();  // anything recorded but never submitted

    std::array<XrCompositionLayerProjectionView, 2> pv{
        XrCompositionLayerProjectionView{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
        XrCompositionLayerProjectionView{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    XrCompositionLayerProjection proj{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrCompositionLayerQuad uiQuad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    XrCompositionLayerQuad laser{XR_TYPE_COMPOSITION_LAYER_QUAD};
    std::vector<const XrCompositionLayerBaseHeader*> layers;

    if (m.frameState.shouldRender) {
        if (m.eyeRendered[0] && m.eyeRendered[1]) {
            for (int e = 0; e < 2; ++e) {
                pv[e].pose = m.views[e].pose;
                pv[e].fov = m.views[e].fov;
                pv[e].subImage.swapchain = m.eyes[e].handle;
                pv[e].subImage.imageRect.offset = {0, 0};
                pv[e].subImage.imageRect.extent = {static_cast<int32_t>(m.eyeExtent.width),
                                                   static_cast<int32_t>(m.eyeExtent.height)};
                pv[e].subImage.imageArrayIndex = 0;
            }
            proj.space = m.local;
            proj.viewCount = 2;
            proj.views = pv.data();
            layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&proj));
        }
        if (m.uiOnQuad && m.uiChain.handle != XR_NULL_HANDLE) {
            uiQuad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            uiQuad.space = m.local;
            uiQuad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            uiQuad.subImage.swapchain = m.uiChain.handle;
            uiQuad.subImage.imageRect.extent = {static_cast<int32_t>(m.uiChain.extent.width),
                                                static_cast<int32_t>(m.uiChain.extent.height)};
            uiQuad.pose = toXr(m.quad);
            uiQuad.size = {m.quadSize.x, m.quadSize.y};
            layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&uiQuad));
        }
        // The pointer's ray: a thin strip from the controller along its aim,
        // turned about its own length to face the head.
        if (m.laserFilled && m.aim[m.pointerHand] && m.laserLength > 0.0f && m.headValid) {
            const Pose& a = *m.aim[m.pointerHand];
            const glm::vec3 dir = glm::normalize(a.orientation * glm::vec3(0.0f, 0.0f, -1.0f));
            const glm::vec3 centre = a.position + dir * (m.laserLength * 0.5f);
            glm::vec3 toHead = m.head.position - centre;
            toHead -= dir * glm::dot(toHead, dir);
            if (glm::length(toHead) > 1e-4f) {
                const glm::vec3 z = glm::normalize(toHead);
                const glm::vec3 y = dir;
                const glm::vec3 x = glm::normalize(glm::cross(y, z));
                Pose lp;
                lp.orientation = glm::quat_cast(glm::mat3(x, y, z));
                lp.position = centre;
                laser.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                laser.space = m.local;
                laser.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                laser.subImage.swapchain = m.laserChain.handle;
                laser.subImage.imageRect.extent = {static_cast<int32_t>(m.laserChain.extent.width),
                                                   static_cast<int32_t>(m.laserChain.extent.height)};
                laser.pose = toXr(lp);
                laser.size = {0.004f, m.laserLength};
                layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&laser));
            }
        }
    }

    XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
    ei.displayTime = m.frameState.predictedDisplayTime;
    ei.environmentBlendMode = m.blendMode;
    ei.layerCount = static_cast<uint32_t>(layers.size());
    ei.layers = layers.data();
    xrOk(xrEndFrame(m.session, &ei), "xrEndFrame");
}

// ---------------------------------------------------------------------------
// The mirror: the headset's view on the desktop window, so the monitor shows
// something. Its own swapchain on the window's surface, presented beside the
// frame (VkContext::addExtraPresent), taken only when an image is free now -
// the headset never waits for the monitor.

bool Runtime::Impl::rebuildMirror() {
    if (!vk || mirrorFailed) return false;
    VkSurfaceKHR surface = vk->getSurface();
    if (!surface) return false;
    VkSurfaceCapabilitiesKHR caps{};
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vk->getPhysicalDevice(), surface, &caps) != VK_SUCCESS) return false;
    if (caps.currentExtent.width == 0 || caps.currentExtent.height == 0) return false;  // minimised
    vkDeviceWaitIdle(vk->getDevice());
    destroyMirror();
    uint32_t fmtCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(vk->getPhysicalDevice(), surface, &fmtCount, nullptr);
    std::vector<VkSurfaceFormatKHR> fmts(fmtCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(vk->getPhysicalDevice(), surface, &fmtCount, fmts.data());
    VkSurfaceFormatKHR chosen = fmts.empty() ? VkSurfaceFormatKHR{} : fmts[0];
    for (const auto& f : fmts) {
        if (f.format == vk->getSwapchainFormat()) chosen = f;
    }
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
        LOG_WARNING("VR: the window cannot be blitted to; no mirror");
        mirrorFailed = true;
        return false;
    }
    uint32_t pmCount = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(vk->getPhysicalDevice(), surface, &pmCount, nullptr);
    std::vector<VkPresentModeKHR> pms(pmCount);
    vkGetPhysicalDeviceSurfacePresentModesKHR(vk->getPhysicalDevice(), surface, &pmCount, pms.data());
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    for (VkPresentModeKHR want : {VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR}) {
        if (std::find(pms.begin(), pms.end(), want) != pms.end()) {
            mode = want;
            break;
        }
    }
    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = surface;
    ci.minImageCount = std::max(caps.minImageCount, 2u) + 1;
    if (caps.maxImageCount > 0) ci.minImageCount = std::min(ci.minImageCount, caps.maxImageCount);
    ci.imageFormat = chosen.format;
    ci.imageColorSpace = chosen.colorSpace;
    ci.imageExtent = caps.currentExtent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = mode;
    ci.clipped = VK_TRUE;
    if (vkCreateSwapchainKHR(vk->getDevice(), &ci, nullptr, &mirror) != VK_SUCCESS) {
        LOG_WARNING("VR: no mirror on the window (its swapchain could not be made)");
        mirror = VK_NULL_HANDLE;
        mirrorFailed = true;
        return false;
    }
    uint32_t count = 0;
    vkGetSwapchainImagesKHR(vk->getDevice(), mirror, &count, nullptr);
    mirrorImages.resize(count);
    vkGetSwapchainImagesKHR(vk->getDevice(), mirror, &count, mirrorImages.data());
    mirrorExtent = caps.currentExtent;
    VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    mirrorRendered.assign(count, VK_NULL_HANDLE);
    mirrorAcquired.assign(count, VK_NULL_HANDLE);
    for (uint32_t i = 0; i < count; ++i) {
        vkCreateSemaphore(vk->getDevice(), &si, nullptr, &mirrorRendered[i]);
        vkCreateSemaphore(vk->getDevice(), &si, nullptr, &mirrorAcquired[i]);
    }
    vkCreateSemaphore(vk->getDevice(), &si, nullptr, &mirrorSpare);
    mirrorDirty = false;
    return true;
}

void Runtime::Impl::destroyMirror() {
    if (!vk) return;
    VkDevice d = vk->getDevice();
    for (VkSemaphore s : mirrorRendered) if (s) vkDestroySemaphore(d, s, nullptr);
    for (VkSemaphore s : mirrorAcquired) if (s) vkDestroySemaphore(d, s, nullptr);
    if (mirrorSpare) vkDestroySemaphore(d, mirrorSpare, nullptr);
    mirrorRendered.clear();
    mirrorAcquired.clear();
    mirrorSpare = VK_NULL_HANDLE;
    if (mirror) vkDestroySwapchainKHR(d, mirror, nullptr);
    mirror = VK_NULL_HANDLE;
    mirrorImages.clear();
}

void Runtime::Impl::recordMirror(VkCommandBuffer cmd, VkImage src, VkExtent2D srcExtent, VkImageLayout srcLayout) {
    if (mirrorDirty && !rebuildMirror()) return;
    if (!mirror) return;
    uint32_t index = 0;
    const VkResult r = vkAcquireNextImageKHR(vk->getDevice(), mirror, 0, mirrorSpare, VK_NULL_HANDLE, &index);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        mirrorDirty = true;
        return;
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) return;  // none free now: not this frame
    // The semaphore the acquire signalled becomes this image's; its old one
    // is the next spare.
    std::swap(mirrorSpare, mirrorAcquired[index]);
    VkImage dst = mirrorImages[index];
    imageBarrier(cmd, dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkClearColorValue black{{0.0f, 0.0f, 0.0f, 1.0f}};
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
    // Fitted inside the window, its own shape kept.
    const float sAspect = static_cast<float>(srcExtent.width) / std::max(1u, srcExtent.height);
    const float dAspect = static_cast<float>(mirrorExtent.width) / std::max(1u, mirrorExtent.height);
    int32_t w = static_cast<int32_t>(mirrorExtent.width), h = static_cast<int32_t>(mirrorExtent.height);
    if (sAspect > dAspect) h = static_cast<int32_t>(w / sAspect);
    else w = static_cast<int32_t>(h * sAspect);
    const int32_t x = (static_cast<int32_t>(mirrorExtent.width) - w) / 2;
    const int32_t y = (static_cast<int32_t>(mirrorExtent.height) - h) / 2;
    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[1] = {static_cast<int32_t>(srcExtent.width), static_cast<int32_t>(srcExtent.height), 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstOffsets[0] = {x, y, 0};
    blit.dstOffsets[1] = {x + w, y + h, 1};
    imageBarrier(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                 VK_ACCESS_TRANSFER_WRITE_BIT);
    vkCmdBlitImage(cmd, src, srcLayout, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
    imageBarrier(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0);
    rendering::VkContext::ExtraPresent present;
    present.swapchain = mirror;
    present.imageIndex = index;
    present.acquired = mirrorAcquired[index];
    present.rendered = mirrorRendered[index];
    present.onResult = [this](VkResult result) {
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) mirrorDirty = true;
    };
    vk->addExtraPresent(std::move(present));
}

// ---------------------------------------------------------------------------

void reportUnavailable(const std::string& why) {
    LOG_ERROR("WoWee VR cannot start: ", why);
    // WOWEE_VR_NO_MESSAGEBOX=1: the log only, for a run nobody is watching.
    if (envOn("WOWEE_VR_NO_MESSAGEBOX", false)) return;
    const std::string text = why + "\n\nwowee.exe still plays on the monitor without a headset.";
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "WoWee VR", text.c_str(), nullptr);
}

}  // namespace wowee::vr
