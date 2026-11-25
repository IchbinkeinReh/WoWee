#include "mcp/hands.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <map>
#include <string_view>

#include <imgui.h>

#include "core/application.hpp"
#include "core/click_drag.hpp"
#include "core/logger.hpp"
#include "core/window.hpp"
#include "game/game_handler.hpp"
#include "mcp/game_view.hpp"
#include "mcp/lua_error.hpp"
#include "mcp/lua_json.hpp"
#include "mcp/ui_view.hpp"
#include "rendering/camera_controller.hpp"
#include "rendering/renderer.hpp"
#include "ui/keybinding_manager.hpp"

namespace wowee::mcp {

namespace {

constexpr float kDegrees = 57.29578f;

/// Told several times a second, or about nothing an act brings about.
constexpr std::array<std::string_view, 36> kNoise = {
    "WOWEE_ITEM_INFO_RECEIVED", "WORLD_STATE_UI_TIMER_UPDATE", "COMBAT_TEXT_UPDATE",
    "COMBAT_LOG_EVENT", "UPDATE_MOUSEOVER_UNIT", "CURSOR_UPDATE", "ACTIONBAR_UPDATE", "SPELL_UPDATE", "UNIT_AURA",
    "UNIT_HEALTH", "UNIT_MANA", "UNIT_RAGE", "UNIT_ENERGY", "UNIT_MAX", "UNIT_POWER", "UNIT_FOCUS", "UNIT_RUNIC",
    "CHAT_MSG_", "CRITERIA_UPDATE", "UNIT_COMBAT", "UNIT_THREAT", "PLAYER_AURAS", "BAG_UPDATE_COOLDOWN",
    "UNIT_PORTRAIT", "UNIT_MODEL", "MODIFIER_STATE", "ACTIONBAR_SLOT", "UNIT_STATS", "UNIT_RESIST", "UNIT_ATTACK",
    "UNIT_DAMAGE", "UNIT_RANGED", "UNIT_TARGET", "UNIT_FACTION", "UNIT_PET", "ACTIONBAR_PAGE"};

bool isNoise(const std::string& name) {
    return std::any_of(kNoise.begin(), kNoise.end(),
                       [&](std::string_view n) { return name.compare(0, n.size(), n) == 0; });
}

rendering::CameraController* cameraOf(ToolContext& context) {
    auto* renderer = context.app.getRenderer();
    return renderer ? renderer->getCameraController() : nullptr;
}

}  // namespace

void append(Steps& to, Steps more) {
    for (auto& fn : more) to.push_back(std::move(fn));
}

Aftermath::Aftermath(const ToolContext& context)
    : events(context.events.last()), refusals(context.refusals.last()), chat(context.chat.last()),
      log(context.log.last()) {}

bool Aftermath::heard(ToolContext& context, std::initializer_list<const char*> names) const {
    return !context.events
                .after(events, 1,
                       [&](const EventLine& l) {
                           return std::any_of(names.begin(), names.end(),
                                              [&](const char* n) { return l.name == n; });
                       })
                .empty();
}

json Aftermath::report(ToolContext& context) const {
    json out = json::object();
    // Each event once, with how often and what it last said.
    std::vector<std::string> order;
    std::map<std::string, std::pair<int, std::string>> seen;
    json errors = json::array();
    for (const auto& e : context.events.after(events, 400, [](const EventLine& l) { return !isNoise(l.name); })) {
        std::string first = e.value.args.empty() ? std::string() : e.value.args[0];
        if (first.size() > 80) first = first.substr(0, 80) + "...";
        if (e.value.name == "UI_ERROR_MESSAGE" || e.value.name == "UI_INFO_MESSAGE") {
            if (errors.size() < 5) errors.push_back(first);
            continue;
        }
        auto [it, fresh] = seen.try_emplace(e.value.name, 0, first);
        if (fresh) order.push_back(e.value.name);
        ++it->second.first;
        it->second.second = first;
    }
    json list = json::array();
    for (const auto& name : order) {
        if (list.size() >= 14) break;
        const auto& [count, first] = seen[name];
        std::string text = name;
        if (!first.empty()) text += "(" + first + ")";
        if (count > 1) text += " x" + std::to_string(count);
        list.push_back(std::move(text));
    }
    if (!list.empty()) out["events"] = std::move(list);
    if (!errors.empty()) out["messages"] = std::move(errors);
    if (json refused = refusedSince(context, refusals); !refused.empty()) out["refused"] = std::move(refused);
    json lines = json::array();
    for (const auto& c : context.chat.after(chat, 6, [](const EventLine& l) {
             return l.name == "CHAT_MSG_SYSTEM" || l.name == "CHAT_MSG_LOOT" || l.name == "CHAT_MSG_MONEY" ||
                    l.name == "CHAT_MSG_MONSTER_SAY" || l.name == "CHAT_MSG_MONSTER_WHISPER" ||
                    l.name == "CHAT_MSG_SKILL" || l.name == "CHAT_MSG_COMBAT_XP_GAIN";
         })) {
        if (!c.value.args.empty()) lines.push_back(plainText(c.value.args[0]).substr(0, 200));
    }
    if (!lines.empty()) out["chat"] = std::move(lines);
    if (json scripts = luaErrorsSince(context, log, 5); !scripts.empty()) out["lua_errors"] = std::move(scripts);
    json failures = json::array();
    for (const auto& l : context.log.after(log, 3, [](const LogLine& line) {
             return line.level >= static_cast<int>(core::kLogLevelError) && !luaErrorOfLogLine(line.text);
         })) {
        failures.push_back(l.value.text.substr(0, 300));
    }
    if (!failures.empty()) out["log_errors"] = std::move(failures);
    if (context.app.loadingScreenUp()) out["loading"] = true;
    if (json p = popups(context); !p.empty()) out["popups"] = std::move(p);
    return out;
}

Script::Fn finishWith(ToolContext& context, Aftermath after, std::function<json()> value) {
    return [&context, after, value = std::move(value)](Script& s) {
        json out = value ? value() : json::object();
        json more = after.report(context);
        if (out.is_object()) out.update(more);
        return s.done(std::move(out));
    };
}

json luaErrorsSince(ToolContext& context, uint64_t logSeq, size_t most) {
    json out = json::array();
    for (const auto& line : context.log.after(logSeq, 50, [](const LogLine& l) {
             return l.level >= static_cast<int>(core::LogLevel::WARNING) && luaErrorOfLogLine(l.text).has_value();
         })) {
        if (out.size() >= most) break;
        out.push_back(luaErrorJson(*luaErrorOfLogLine(line.value.text)));
    }
    return out;
}

json refusedSince(ToolContext& context, uint64_t refusalSeq) {
    json out = json::array();
    for (const auto& r : context.refusals.after(refusalSeq, 5, [](const RefusedLine&) { return true; })) {
        out.push_back(r.value.what + ": " + r.value.why);
    }
    return out;
}

Script::Fn worldShown(ToolContext& context, double seconds) {
    return [&context, seconds, until = -1.0](Script& s) mutable {
        if (until < 0.0) until = s.now() + seconds;
        return !context.app.loadingScreenUp() || s.now() > until ? Script::Step::Next : Script::Step::Again;
    };
}

Script::Fn wantKeysFree(ToolContext& context) {
    return [&context](Script& s) {
        if (ImGui::GetCurrentContext() && ImGui::GetIO().WantTextInput) {
            return s.fail("a field of the client's own takes the keys");
        }
        if (ui::interfaceTakingTypedInput()) {
            const json box = tryQueryLua(context, "local b = GetCurrentKeyBoardFocus() return b and b:GetName() or ''");
            return s.fail("an edit box takes the keys (" + (box.is_string() ? box.get<std::string>() : std::string()) +
                          "); press ESCAPE first");
        }
        return Script::Step::Next;
    };
}

Steps pressKey(ToolContext& context, SDL_Scancode key, SDL_Keymod mods, double seconds) {
    Steps out;
    out.push_back([&context, key, mods](Script&) {
        if (mods != SDL_KMOD_NONE) context.input.modifiers(mods, true);
        context.input.keyDown(key);
        return Script::Step::Next;
    });
    // At least a frame it is seen down in.
    out.push_back(Script::frames(1));
    out.push_back(Script::seconds(seconds));
    out.push_back([&context, key, mods](Script&) {
        context.input.keyUp(key);
        if (mods != SDL_KMOD_NONE) context.input.modifiers(mods, false);
        return Script::Step::Next;
    });
    out.push_back(Script::frames(2));
    return out;
}

Steps movePointer(ToolContext& context, std::function<std::optional<glm::vec2>()> where) {
    Steps out;
    out.push_back([&context, where = std::move(where)](Script& s) {
        const auto at = where();
        if (!at) {
            // `where` may have said why.
            return s.failed ? Script::Step::Done : s.fail("there is nothing to point at");
        }
        context.input.moveTo(*at);
        return Script::Step::Next;
    });
    // The hover is worked out as the frame is drawn, and acted on in the next.
    out.push_back(Script::frames(3));
    return out;
}

Steps pressButton(ToolContext& context, int button, SDL_Keymod mods, int clicks) {
    Steps out;
    if (mods != SDL_KMOD_NONE) {
        out.push_back([&context, mods](Script&) {
            context.input.modifiers(mods, true);
            return Script::Step::Next;
        });
        out.push_back(Script::frames(2));
    }
    for (int i = 1; i <= clicks; ++i) {
        out.push_back([&context, button, i](Script&) {
            context.input.button(button, true, i);
            return Script::Step::Next;
        });
        out.push_back(Script::frames(2));
        out.push_back(Script::seconds(0.04));
        out.push_back([&context, button, i](Script&) {
            context.input.button(button, false, i);
            return Script::Step::Next;
        });
        out.push_back(Script::frames(2));
    }
    if (mods != SDL_KMOD_NONE) {
        out.push_back([&context, mods](Script&) {
            context.input.modifiers(mods, false);
            return Script::Step::Next;
        });
    }
    out.push_back(Script::frames(2));
    return out;
}

Steps clickAt(ToolContext& context, std::function<std::optional<glm::vec2>()> where, int button, SDL_Keymod mods,
              int clicks) {
    Steps out = movePointer(context, std::move(where));
    append(out, pressButton(context, button, mods, clicks));
    return out;
}

Steps dragFromTo(ToolContext& context, glm::vec2 from, glm::vec2 to, int button, SDL_Keymod mods) {
    Steps out = movePointer(context, [from] { return std::optional<glm::vec2>(from); });
    if (mods != SDL_KMOD_NONE) {
        out.push_back([&context, mods](Script&) {
            context.input.modifiers(mods, true);
            return Script::Step::Next;
        });
        out.push_back(Script::frames(2));
    }
    out.push_back([&context, button](Script&) {
        context.input.button(button, true);
        return Script::Step::Next;
    });
    out.push_back(Script::frames(2));
    constexpr int kLegs = 8;
    for (int i = 1; i <= kLegs; ++i) {
        out.push_back([&context, from, to, i](Script&) {
            context.input.moveTo(glm::mix(from, to, static_cast<float>(i) / kLegs));
            return Script::Step::Next;
        });
        out.push_back(Script::frames(1));
    }
    out.push_back(Script::frames(2));
    out.push_back([&context, button](Script&) {
        context.input.button(button, false);
        return Script::Step::Next;
    });
    out.push_back(Script::frames(2));
    if (mods != SDL_KMOD_NONE) {
        out.push_back([&context, mods](Script&) {
            context.input.modifiers(mods, false);
            return Script::Step::Next;
        });
    }
    out.push_back(Script::frames(1));
    return out;
}

std::optional<glm::vec2> worldPoint(ToolContext& context) {
    auto* window = context.app.getWindow();
    if (!window) return std::nullopt;
    const glm::vec2 size(static_cast<float>(window->getWidth()), static_cast<float>(window->getHeight()));
    constexpr std::array<glm::vec2, 9> kAt = {{{0.5f, 0.35f}, {0.5f, 0.5f}, {0.6f, 0.4f}, {0.4f, 0.4f}, {0.7f, 0.6f},
                                               {0.3f, 0.6f}, {0.5f, 0.2f}, {0.8f, 0.3f}, {0.2f, 0.3f}}};
    for (const auto& at : kAt) {
        const glm::vec2 p = at * size;
        if (!interfaceAt(context, p)) return p;
    }
    return std::nullopt;
}

Steps dragView(ToolContext& context, int button, std::function<glm::vec2()> degreesLeft) {
    struct State {
        glm::vec2 at{0.0f};
        glm::vec2 left{0.0f};      ///< at the last nudge
        glm::vec2 nudged{0.0f};    ///< in pixels, the last
        glm::vec2 perPixel{-0.2f, 0.2f};  ///< what a pixel takes off, found by trying
        int frames = 0;
        int rest = 0;
        int settled = 0;
        bool tried = false;
    };
    auto state = std::make_shared<State>();
    Steps out;
    out.push_back([&context, state, degreesLeft](Script& s) {
        const glm::vec2 left = degreesLeft();
        if (std::fabs(left.x) < 1.0f && std::fabs(left.y) < 1.0f) state->settled = 99;
        const auto p = worldPoint(context);
        if (!p) return s.fail("the interface covers the window; nothing to drag at");
        state->at = *p;
        if (state->settled != 99) context.input.moveTo(*p);
        return Script::Step::Next;
    });
    out.push_back(Script::frames(2));
    out.push_back([&context, state, button](Script&) {
        if (state->settled != 99) context.input.button(button, true);
        return Script::Step::Next;
    });
    out.push_back(Script::frames(2));
    out.push_back([&context, state, degreesLeft](Script&) {
        if (state->settled == 99) return Script::Step::Next;
        // A nudge is an event of this frame, the camera's turn this frame's
        // and the character's the next one's: looked at sooner, it is taken
        // for the one before.
        if (state->rest > 0) {
            --state->rest;
            return Script::Step::Again;
        }
        const glm::vec2 left = degreesLeft();
        if (++state->frames > 60) return Script::Step::Next;
        if (std::fabs(left.x) < 1.0f && std::fabs(left.y) < 1.0f) {
            return ++state->settled >= 2 ? Script::Step::Next : Script::Step::Again;
        }
        state->settled = 0;
        if (state->tried) {
            // What the last nudge came to, axis by axis.
            for (int a = 0; a < 2; ++a) {
                const float moved = state->left[a] - left[a];
                if (std::fabs(state->nudged[a]) >= 2.0f && std::fabs(moved) > 0.05f) {
                    state->perPixel[a] = moved / state->nudged[a];
                }
            }
        }
        glm::vec2 step(0.0f);
        for (int a = 0; a < 2; ++a) {
            if (std::fabs(left[a]) < 1.0f) continue;
            const float per = std::fabs(state->perPixel[a]) > 0.005f ? state->perPixel[a] : 0.2f;
            // Past the dead zone a press has before it is a drag, first.
            const float most = state->tried ? 120.0f : core::clickDragThreshold() + 6.0f;
            step[a] = std::clamp(left[a] / per, -most, most);
            if (!state->tried && std::fabs(step[a]) < most) step[a] = std::copysign(most, step[a]);
        }
        state->left = left;
        state->nudged = step;
        state->tried = true;
        state->rest = 2;
        state->at += step;
        context.input.moveTo(state->at);
        return Script::Step::Again;
    });
    out.push_back([&context, state, button](Script&) {
        if (state->settled != 99 || state->frames > 0) context.input.button(button, false);
        return Script::Step::Next;
    });
    out.push_back(Script::frames(2));
    return out;
}

void HeldKey::set(bool down) {
    if (down == down_) return;
    down_ = down;
    if (down) context_.input.keyDown(key_);
    else context_.input.keyUp(key_);
}

Steps turnToward(ToolContext& context, std::function<std::optional<glm::vec3>()> target, bool byMouse) {
    const auto left = [&context, target]() -> float {
        const auto at = target();
        return at ? turnTo(playerAt(context), playerFacing(context), *at) : 0.0f;
    };
    if (byMouse) {
        // Turning left is the mouse to the left; dragView finds the sign.
        return dragView(context, SDL_BUTTON_RIGHT, [left] { return glm::vec2(left() * kDegrees, 0.0f); });
    }
    struct State {
        std::unique_ptr<HeldKey> a;
        std::unique_ptr<HeldKey> d;
        double until = -1.0;
    };
    auto state = std::make_shared<State>();
    Steps out;
    out.push_back([&context, state, left](Script& s) {
        if (!state->a) {
            state->a = std::make_unique<HeldKey>(context, SDL_SCANCODE_A);
            state->d = std::make_unique<HeldKey>(context, SDL_SCANCODE_D);
            state->until = s.now() + 6.0;
        }
        const float err = left();
        // What a frame's turn is, so that the key is let go of in time.
        auto* cc = cameraOf(context);
        const float rate = cc ? cc->getTurnRateRad() : 3.14159f;
        const float frame = rate * static_cast<float>(std::clamp(context.frameSeconds, 0.005, 0.1));
        const float close = std::max(0.02f, frame * 0.75f);
        if (std::fabs(err) <= close || s.now() > state->until) {
            state->a->set(false);
            state->d->set(false);
            return Script::Step::Next;
        }
        state->a->set(err > 0.0f);
        state->d->set(err < 0.0f);
        return Script::Step::Again;
    });
    out.push_back(Script::frames(2));
    // The keys go with the steps, however the call ends.
    return out;
}

Steps chatLine(ToolContext& context, std::string text) {
    Steps out;
    out.push_back(wantKeysFree(context));
    append(out, pressKey(context, SDL_SCANCODE_RETURN, SDL_KMOD_NONE, 0.05));
    out.push_back([&context](Script& s) {
        const json box = tryQueryLua(context, "local b = GetCurrentKeyBoardFocus() return b and b:GetName() or ''");
        const std::string name = box.is_string() ? box.get<std::string>() : std::string();
        if (name.find("EditBox") != std::string::npos) return Script::Step::Next;
        return s.elapsed() > 5.0 ? s.fail("the chat box did not open") : Script::Step::Again;
    });
    out.push_back([&context, text](Script&) {
        context.input.type(text);
        return Script::Step::Next;
    });
    out.push_back(Script::frames(3));
    append(out, pressKey(context, SDL_SCANCODE_RETURN, SDL_KMOD_NONE, 0.05));
    // A line that was not taken leaves the box open; it is not left so.
    out.push_back([&context](Script& s) {
        if (!ui::interfaceTakingTypedInput()) return Script::Step::Next;
        s.first(pressKey(context, SDL_SCANCODE_ESCAPE, SDL_KMOD_NONE, 0.05));
        return Script::Step::Next;
    });
    return out;
}

}  // namespace wowee::mcp
