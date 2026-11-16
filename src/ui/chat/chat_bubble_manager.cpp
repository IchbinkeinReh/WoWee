// ChatBubbleManager - 3D-projected chat bubbles above entities.
// Moved from ChatPanel::renderBubbles / setupCallbacks (Phase 1.4).
#include "ui/chat/chat_bubble_manager.hpp"
#include "game/game_handler.hpp"
#include "rendering/renderer.hpp"
#include "rendering/camera.hpp"
#include "core/coordinates.hpp"
#include "core/window.hpp"
#include <imgui.h>
#include "ui/chat/chat_utils.hpp"
#include "ui/chat/chat_bubble_timing.hpp"
#include "ui/sight_cache.hpp"

#include <glm/glm.hpp>

namespace wowee { namespace ui {

void ChatBubbleManager::addBubble(uint64_t senderGuid, const std::string& message, bool isYell, bool own) {
    // 2000 ms and 750 a word, or 1000 and 500 for the player's own
    // (0x0056c1c0); a yell is timed as any other line.
    const float duration = static_cast<float>(chat_bubble::durationMs(message, own)) * 0.001f;

    // Replace existing bubble for same sender
    for (auto& b : bubbles_) {
        if (b.senderGuid == senderGuid) {
            b.message = message;
            b.elapsed = 0.0f;
            b.duration = duration;
            b.isYell = isYell;
            return;
        }
    }
    // Evict oldest if too many
    if (bubbles_.size() >= kMaxBubbles) {
        bubbles_.erase(bubbles_.begin());
    }
    bubbles_.push_back({.senderGuid = senderGuid, .message = message, .elapsed = 0.0f, .duration = duration,
                        .alpha = 0.0f, .isYell = isYell});
}

void ChatBubbleManager::render(game::GameHandler& gameHandler, const UIServices& services) {
    if (bubbles_.empty()) return;

    auto* renderer = services.renderer;
    auto* camera = renderer ? renderer->getCamera() : nullptr;
    if (!camera) return;

    auto* window = services.window;
    float screenW = window ? static_cast<float>(window->getWidth()) : 1280.0f;
    float screenH = window ? static_cast<float>(window->getHeight()) : 720.0f;

    // Get delta time from ImGui
    float dt = ImGui::GetIO().DeltaTime;

    glm::mat4 viewProj = camera->getProjectionMatrix() * camera->getViewMatrix();

    // Update and render bubbles
    for (int i = static_cast<int>(bubbles_.size()) - 1; i >= 0; --i) {
        auto& bubble = bubbles_[i];
        // 0x0056c500: up for its duration after a 250 ms fade in, then a
        // 250 ms fade out, and gone.
        bubble.elapsed += dt;
        const bool expiring = bubble.elapsed > bubble.duration + chat_bubble::kFadeSeconds;

        // Get entity position
        auto entity = gameHandler.getEntityManager().getEntity(bubble.senderGuid);
        glm::vec3 renderPos(0.0f);
        bool inSight = false;
        if (entity) {
            // Convert canonical → render coordinates, offset up by 2.5 units for bubble above head
            glm::vec3 canonical(entity->getX(), entity->getY(), entity->getZ() + 2.5f);
            renderPos = core::coords::canonicalToRender(canonical);
            // Faded out from 25 yards of the camera, and back in on coming
            // nearer (0x0056c500: squared distance 625).
            const glm::vec3 d = renderPos - camera->getPosition();
            inSight = glm::dot(d, d) < chat_bubble::kRangeYards * chat_bubble::kRangeYards;
            // Not through a wall. A bubble carries a name as well as a line, so
            // one drawn over the building someone is standing behind says they are
            // in front of it - the same fault the nameplates had, answered by the
            // same query.
            static SightCache sight;
            if (inSight && sight.blocked(renderer->queryWMORenderer(), bubble.senderGuid,
                                         camera->getPosition(), renderPos)) {
                inSight = false;
            }
        }
        bubble.alpha = chat_bubble::stepAlpha(bubble.alpha, !expiring && inSight, dt);
        if (expiring && bubble.alpha <= 0.0f) {
            bubbles_.erase(bubbles_.begin() + i);
            continue;
        }
        if (!entity || bubble.alpha <= 0.0f) continue;

        // Project to screen
        glm::vec4 clipPos = viewProj * glm::vec4(renderPos, 1.0f);
        if (clipPos.w <= 0.0f) continue;  // Behind camera

        glm::vec2 ndc(clipPos.x / clipPos.w, clipPos.y / clipPos.w);
        float screenX = (ndc.x * 0.5f + 0.5f) * screenW;
        // Camera bakes the Vulkan Y-flip into the projection matrix:
        // NDC y=-1 is top, y=1 is bottom - same convention as nameplate/minimap projection.
        float screenY = (ndc.y * 0.5f + 0.5f) * screenH;

        // Skip if off-screen
        if (screenX < -200.0f || screenX > screenW + 200.0f ||
            screenY < -100.0f || screenY > screenH + 100.0f) continue;

        const float alpha = bubble.alpha;

        // Into the background draw list, which is where the interface is
        // drawn - and drawn into before it, so the interface covers this.
        //
        // These were ImGui windows, and an ImGui window is above every draw
        // list the interface uses, whatever the interface is doing: a bubble
        // sat on top of the auction house, the bags, the map, anything open.
        // Nothing about the position was wrong; a window was simply the one
        // thing that cannot go underneath.
        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        ImFont* font = ImGui::GetFont();
        // A tenth larger than the interface's own text, with the wrap width
        // grown alike so a line breaks where it did, and room around it: at the
        // body size with 8 by 4 the words sat on the bubble's edge and were
        // hard to read over a busy scene.
        constexpr float kBubbleScale = 1.1f;
        const float fontSize = ImGui::GetFontSize() * kBubbleScale;
        constexpr float kWrapWidth = 200.0f * kBubbleScale;
        const ImVec2 padding(13.0f, 9.0f);

        const ImVec2 textSize =
            font->CalcTextSizeA(fontSize, FLT_MAX, kWrapWidth, bubble.message.c_str());
        const ImVec2 size(textSize.x + padding.x * 2.0f, textSize.y + padding.y * 2.0f);
        // The point is where the speaker's head is, and the bubble sits above
        // it, centred - which is what the window's pivot of (0.5, 1) did.
        const ImVec2 topLeft(screenX - size.x * 0.5f, screenY - size.y);

        ImVec4 background = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
        background.w = 0.7f * alpha;
        dl->AddRectFilled(topLeft, ImVec2(topLeft.x + size.x, topLeft.y + size.y),
                          ImGui::GetColorU32(background), 10.0f);

        // A creature's words in the light yellow the chat window gives them,
        // as the original client draws them; a player's in white. Yells are
        // red whoever shouts.
        const bool fromCreature = (bubble.senderGuid >> 48) != 0;
        const ImVec4 textColor = bubble.isYell
            ? ImVec4(1.0f, 0.2f, 0.2f, alpha)
            : fromCreature ? ImVec4(1.0f, 1.0f, 0.624f, alpha)
                           : ImVec4(1.0f, 1.0f, 1.0f, alpha);
        dl->AddText(font, fontSize, ImVec2(topLeft.x + padding.x, topLeft.y + padding.y),
                    ImGui::GetColorU32(textColor), bubble.message.c_str(), nullptr,
                    kWrapWidth);
    }
}

void ChatBubbleManager::setupCallback(game::GameHandler& gameHandler) {
    if (!callbackSet_) {
        // Resolve $-tokens here rather than at draw time: a bubble is built once
        // and drawn every frame. The chat panel already does this for the same
        // text, which is why an NPC line read correctly in the log and showed
        // its raw $r/$c/$g markers in the bubble over their head.
        gameHandler.setChatBubbleCallback(
            [this, &gameHandler](uint64_t guid, const std::string& msg, bool isYell, bool party) {
                // 0x00720010: the setting of the line's kind, at the time.
                if (!(party ? partyShown_ : show_)) return;
                addBubble(guid, chat_utils::replaceGenderPlaceholders(msg, gameHandler), isYell,
                          guid == gameHandler.getPlayerGuid());
            });
        callbackSet_ = true;
    }
}

} // namespace ui
} // namespace wowee
