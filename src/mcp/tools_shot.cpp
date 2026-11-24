// The frame as it was drawn.

#include <algorithm>
#include <memory>

#include "core/application.hpp"
#include "core/window.hpp"
#include "mcp/game_view.hpp"
#include "mcp/image.hpp"
#include "mcp/schema.hpp"
#include "mcp/tool_context.hpp"
#include "mcp/ui_view.hpp"
#include "ui/widget_tree.hpp"

namespace wowee::mcp {

namespace {

/// The screens an account name, a password or a key is typed into.
bool screenIsPrivate(ToolContext& context) {
    const std::string state = clientState(context);
    return state == "login" || state == "setup" || state == "disconnected";
}

struct Part {
    uint32_t x = 0, y = 0, w = 0, h = 0;
};

/// A rect in window points as pixels of the frame, which on a dense display
/// are more.
Part partOf(const glm::vec4& rect, const FrameGrab& grab, const core::Window* window) {
    const float sx = window && window->getWidth() > 0
                         ? static_cast<float>(grab.width) / static_cast<float>(window->getWidth())
                         : 1.0f;
    const float sy = window && window->getHeight() > 0
                         ? static_cast<float>(grab.height) / static_cast<float>(window->getHeight())
                         : 1.0f;
    const auto px = [](float v) { return static_cast<uint32_t>(std::max(0.0f, v)); };
    return {.x = px(rect.x * sx), .y = px(rect.y * sy), .w = px(rect.z * sx), .h = px(rect.w * sy)};
}

void screenshot(ToolContext& context, const json& args, Script& script) {
    script.then([&context](Script& s) {
        if (screenIsPrivate(context)) {
            return s.fail("no pictures of this screen: credentials are entered on it");
        }
        auto* window = context.app.getWindow();
        if (window && (SDL_GetWindowFlags(window->getSDLWindow()) & SDL_WINDOW_MINIMIZED)) {
            return s.fail("the window is minimised; nothing is drawn");
        }
        context.grab.wanted = true;
        context.grab.ready = false;
        context.grab.failed = false;
        return Script::Step::Next;
    });
    script.then([&context, args](Script& s) {
        if (context.grab.failed) {
            context.grab.failed = false;
            return s.fail("the frame could not be read back");
        }
        if (!context.grab.ready) {
            if (s.elapsed() > 8.0) {
                return s.fail("no frame was drawn");
            }
            // Asked again: another call may have taken the picture.
            context.grab.wanted = true;
            return Script::Step::Again;
        }
        auto grab = std::make_shared<FrameGrab>(std::move(context.grab));
        context.grab = {};
        Part part;
        const auto* window = context.app.getWindow();
        if (args.contains("window")) {
            const auto* w = widgetNamed(context, args["window"].get<std::string>());
            if (!w || !w->visible) return s.fail("that frame is not shown");
            part = partOf(widgetRect(context, *w), *grab, window);
        } else if (args.contains("crop")) {
            if (args["crop"].size() != 4) return s.fail("crop: [x, y, width, height]");
            part = partOf(glm::vec4(args["crop"][0].get<float>(), args["crop"][1].get<float>(),
                                    args["crop"][2].get<float>(), args["crop"][3].get<float>()),
                          *grab, window);
        }
        const auto most = args.value("max_width", static_cast<uint32_t>(1024));
        const bool png = args.value("format", std::string("jpeg")) == "png";
        const int quality = args.value("quality", 80);
        // The rest on the caller's thread, while the game goes on.
        s.finish = [grab, part, most, png, quality](Script& done) {
            const Picture whole = cutFrame(grab->bgra, grab->width, grab->height, part.x, part.y, part.w, part.h);
            const Picture small = shrink(whole, most);
            const auto bytes = png ? encodePng(small) : encodeJpeg(small, quality);
            if (bytes.empty()) {
                done.failed = true;
                done.result = "the picture could not be encoded";
                return;
            }
            done.imageBase64 = base64(bytes);
            done.imageMime = png ? "image/png" : "image/jpeg";
            done.result = {{"frame", json::array({grab->width, grab->height})},
                           {"picture", json::array({small.width, small.height})},
                           {"from", json::array({part.x, part.y, whole.width, whole.height})}};
        };
        return Script::Step::Done;
    });
    script.onEnd = [&context] { context.grab.wanted = false; };
}

}  // namespace

void addShotTools(Toolbox& box) {
    using namespace schema;
    Tool tool;
    tool.info = {.name = "screenshot",
                 .description = "The frame as it was drawn, as a picture; a part of it, or one window of the interface.",
                 .inputSchema = object(
                     {optional("max_width", integer("pixels, 1024 by default; 0 as it is", 0, 8192)),
                      optional("crop", list("x, y, width, height in window pixels", json{{"type", "number"}}, 4)),
                      optional("window", text("only this frame of the interface", 96)),
                      optional("format", choice("jpeg by default", {"jpeg", "png"})),
                      optional("quality", integer("of a jpeg, 80 by default", 1, 100))}),
                 .readOnly = true};
    tool.where = Tool::Where::Anywhere;
    tool.patience = 20.0;
    tool.build = screenshot;
    box.push_back(std::move(tool));
}

}  // namespace wowee::mcp
