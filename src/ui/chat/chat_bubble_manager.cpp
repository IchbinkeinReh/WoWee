// ChatBubbleManager - the chat bubbles over the units that speak.
#include "ui/chat/chat_bubble_manager.hpp"
#include "game/game_handler.hpp"
#include "rendering/renderer.hpp"
#include "rendering/camera.hpp"
#include "core/coordinates.hpp"
#include "core/window.hpp"
#include <cmath>
#include <imgui.h>
#include "ui/chat/chat_utils.hpp"
#include "ui/chat/chat_bubble_timing.hpp"
#include "ui/chat/chat_bubble_view.hpp"
#include "addons/addon_manager.hpp"
#include "addons/lua_engine.hpp"
#include "core/entity_spawner.hpp"
#include "game/chat_bubble_rule.hpp"

#include <glm/glm.hpp>
#include <algorithm>

namespace wowee { namespace ui {

void ChatBubbleManager::addBubble(uint64_t senderGuid, const std::string& message, uint8_t chatType, bool own) {
    // 2000 ms and 750 a word, or 1000 and 500 for the player's own
    // (0x0056c1c0); a yell is timed as any other line.
    const float duration = static_cast<float>(chat_bubble::durationMs(message, own)) * 0.001f;

    // The one the unit had is over (0x00720010), and the new one the last made.
    std::erase_if(bubbles_, [senderGuid](const ChatBubble& b) { return b.senderGuid == senderGuid; });
    // Evict oldest if too many
    if (bubbles_.size() >= kMaxBubbles) {
        bubbles_.erase(bubbles_.begin());
    }
    bubbles_.push_back({.senderGuid = senderGuid, .message = chat_bubble::bubbleText(message), .elapsed = 0.0f,
                        .duration = duration, .alpha = 0.0f, .chatType = chatType, .id = ++lastId_});
}

void ChatBubbleManager::render(game::GameHandler& gameHandler, const UIServices& services) {
    auto* engine = services.addonManager ? services.addonManager->getLuaEngine() : nullptr;
    auto* renderer = services.renderer;
    auto* camera = renderer ? renderer->getCamera() : nullptr;
    if (!engine) return;
    if (bubbles_.empty() || !camera) {
        engine->showChatBubbles(0.0f, {});
        return;
    }

    auto* window = services.window;
    const float screenW = window ? static_cast<float>(window->getWidth()) : 1280.0f;
    const float screenH = window ? static_cast<float>(window->getHeight()) : 720.0f;
    const float dt = ImGui::GetIO().DeltaTime;
    const glm::mat4 viewProj = camera->getProjectionMatrix() * camera->getViewMatrix();

    std::vector<ChatBubbleView> views;
    for (size_t i = 0; i < bubbles_.size();) {
        auto& bubble = bubbles_[i];
        // 0x0056c500: up for its duration after a 250 ms fade in, then a
        // 250 ms fade out, and gone.
        bubble.elapsed += dt;
        const bool expiring = bubble.elapsed > bubble.duration + chat_bubble::kFadeSeconds;

        // A speaker no longer there takes its bubble with it, at once
        // (0x0056d050, 0x0056cf80).
        auto entity = gameHandler.getEntityManager().getEntity(bubble.senderGuid);
        if (!entity) {
            bubbles_.erase(bubbles_.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        // Faded out from 25 yards of the active player - the speaker's own
        // position against the player's, not the camera's - and back in on
        // coming nearer (0x0056c500: squared distance 625). Walls do not
        // hide it: the client asks only whether the speaker is in range.
        const auto& me = gameHandler.getMovementInfo();
        const float dx = entity->getX() - me.x, dy = entity->getY() - me.y, dz = entity->getZ() - me.z;
        const bool inSight = dx * dx + dy * dy + dz * dz < chat_bubble::kRangeYards * chat_bubble::kRangeYards;
        bubble.alpha = chat_bubble::stepAlpha(bubble.alpha, !expiring && inSight, dt);
        if (expiring && bubble.alpha <= 0.0f) {
            bubbles_.erase(bubbles_.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        ++i;
        if (!gameHandler.isUiVisible()) continue;   // 0x00ac80a8

        // Over the unit, as high as its name's point and 0.7 (0x0056c500).
        glm::vec3 at = core::coords::canonicalToRender(glm::vec3(entity->getX(), entity->getY(), entity->getZ()));
        float nameZ = at.z + 2.3f;
        if (services.entitySpawner) {
            glm::vec3 base;
            if (services.entitySpawner->getRenderPositionForGuid(bubble.senderGuid, base)) {
                at = base;
                nameZ = at.z + 2.3f;
            }
            if (const auto name = services.entitySpawner->unitNamePosition(bubble.senderGuid)) nameZ = name->z;
        }
        at.z = nameZ + chat_bubble::kOverName;
        const glm::vec4 clip = viewProj * glm::vec4(at, 1.0f);
        if (clip.w <= 0.0f) continue;   // behind the camera
        // The projection has Vulkan's flip: -1 is the top.
        const float px = (clip.x / clip.w * 0.5f + 0.5f) * screenW;
        const float py = (clip.y / clip.w * 0.5f + 0.5f) * screenH;
        if (px < -400.0f || px > screenW + 400.0f || py < -400.0f || py > screenH + 400.0f) continue;
        const float unit = screenH / 768.0f;
        views.push_back({.id = bubble.id, .text = bubble.message, .type = chat_bubble::typeName(bubble.chatType),
                         .alpha = bubble.alpha, .x = px / unit, .y = (screenH - py) / unit});
    }
    // The client's units (DDC, 0x0047bf90): the screen's diagonal is 1.
    engine->showChatBubbles(std::sqrt(screenW * screenW + screenH * screenH) / (screenH / 768.0f), views);
}

void ChatBubbleManager::setupCallback(game::GameHandler& gameHandler) {
    if (!callbackSet_) {
        // Resolve $-tokens here rather than at draw time: a bubble is built once
        // and drawn every frame. The chat panel already does this for the same
        // text, which is why an NPC line read correctly in the log and showed
        // its raw $r/$c/$g markers in the bubble over their head.
        gameHandler.setChatBubbleCallback(
            [this, &gameHandler](uint64_t guid, const std::string& msg, uint8_t chatType) {
                // 0x00720010: the setting of the line's kind, at the time.
                const bool party = game::chat_bubble_rule::settingFor(chatType) ==
                                   game::chat_bubble_rule::Setting::ChatBubblesParty;
                if (!(party ? partyShown_ : show_)) return;
                addBubble(guid, chat_utils::replaceGenderPlaceholders(msg, gameHandler), chatType,
                          guid == gameHandler.getPlayerGuid());
            });
        callbackSet_ = true;
    }
}

} // namespace ui
} // namespace wowee
