// The frame as it was drawn.

#include <algorithm>
#include <cmath>
#include <vector>
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

/// The mean colour of a part of the frame, 0..255 each, and its luma
/// (0.299 R + 0.587 G + 0.114 B).
json meanOf(const FrameGrab& grab, const Part& part) {
    const uint32_t x0 = std::min(part.x, grab.width), y0 = std::min(part.y, grab.height);
    const uint32_t x1 = std::min(part.x + part.w, grab.width), y1 = std::min(part.y + part.h, grab.height);
    double r = 0.0, g = 0.0, b = 0.0;
    uint64_t n = 0;
    for (uint32_t y = y0; y < y1; ++y) {
        for (uint32_t x = x0; x < x1; ++x) {
            const size_t at = (static_cast<size_t>(y) * grab.width + x) * 4;
            if (at + 2 >= grab.bgra.size()) continue;
            b += grab.bgra[at];
            g += grab.bgra[at + 1];
            r += grab.bgra[at + 2];
            ++n;
        }
    }
    if (n == 0) return json{{"pixels", 0}};
    const auto round2 = [](double v) { return std::round(v * 100.0) / 100.0; };
    r /= static_cast<double>(n);
    g /= static_cast<double>(n);
    b /= static_cast<double>(n);
    return json{{"pixels", n}, {"r", round2(r)}, {"g", round2(g)}, {"b", round2(b)},
                {"luma", round2(0.299 * r + 0.587 * g + 0.114 * b)}};
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
        // Boxes whose mean brightness is wanted, in window pixels as crop.
        std::vector<Part> boxes;
        if (args.contains("boxes")) {
            for (const auto& box : args["boxes"]) {
                if (!box.is_array() || box.size() != 4) return s.fail("boxes: each [x, y, width, height]");
                boxes.push_back(partOf(glm::vec4(box[0].get<float>(), box[1].get<float>(), box[2].get<float>(),
                                                 box[3].get<float>()),
                                       *grab, window));
            }
        }
        const bool picture = args.value("picture", true);
        const auto most = args.value("max_width", static_cast<uint32_t>(1024));
        const bool png = args.value("format", std::string("jpeg")) == "png";
        const int quality = args.value("quality", 80);
        // The rest on the caller's thread, while the game goes on.
        s.finish = [grab, part, boxes, picture, most, png, quality](Script& done) {
            json means = json::array();
            for (const Part& box : boxes) {
                json m = meanOf(*grab, box);
                m["box"] = json::array({box.x, box.y, box.w, box.h});
                means.push_back(std::move(m));
            }
            if (!picture) {
                done.result = {{"frame", json::array({grab->width, grab->height})}, {"means", std::move(means)}};
                return;
            }
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
            if (!means.empty()) done.result["means"] = std::move(means);
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
                      optional("quality", integer("of a jpeg, 80 by default", 1, 100)),
                      optional("boxes", list("boxes [x, y, width, height] in window pixels whose mean colour and "
                                             "luma (0-255) are answered under means, from the whole frame",
                                             list("x, y, width, height", json{{"type", "number"}}, 4), 16)),
                      optional("picture", flag("false: only the means, no picture"))}),
                 .readOnly = true};
    tool.where = Tool::Where::Anywhere;
    tool.patience = 20.0;
    tool.build = screenshot;
    box.push_back(std::move(tool));
}

}  // namespace wowee::mcp
