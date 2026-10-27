// System commands: /run, /dump, /reload, /stopmacro, /clear, /logout
// Moved from ChatPanel::sendChatMessage() if/else chain (Phase 3).
#include "ui/chat/i_chat_command.hpp"
#include "ui/chat/macro_evaluator.hpp"
#include "ui/chat_panel.hpp"
#include "ui/ui_services.hpp"
#include "game/game_handler.hpp"
#include "addons/addon_manager.hpp"
#include "core/application.hpp"
#include "ui/framexml_takeover.hpp"
#include "ui/chat/chat_utils.hpp"
#include "ui/chat/client_gps.hpp"
#include "core/coordinates.hpp"
#include "rendering/camera_controller.hpp"
#include "rendering/renderer.hpp"
#include "core/frame_profiler.hpp"
#include "core/logger.hpp"
#include "game/transport_manager.hpp"
#include <algorithm>
#include <cctype>
#include <cstdio>

namespace wowee { namespace ui {

// --- /run, /script ---
class RunCommand : public IChatCommand {
public:
    ChatCommandResult execute(ChatCommandContext& ctx) override {
        if (ctx.args.empty()) return {.handled = false, .clearInput = false};
        auto* am = ctx.services.addonManager;
        if (am) {
            am->runScript(ctx.args);
        } else {
            ctx.gameHandler.addUIError("Addon system not initialized.");
        }
        return {};
    }
    [[nodiscard]] std::vector<std::string> aliases() const override { return {"run", "script"}; }
    [[nodiscard]] std::string helpText() const override { return "Execute Lua code"; }
};

// --- /dump, /print ---
class DumpCommand : public IChatCommand {
public:
    ChatCommandResult execute(ChatCommandContext& ctx) override {
        if (ctx.args.empty()) return {.handled = false, .clearInput = false};
        auto* am = ctx.services.addonManager;
        if (am && am->isInitialized()) {
            // Wrap expression in print(tostring(...)) to display the value
            std::string wrapped = "local __v = " + ctx.args +
                "; if type(__v) == 'table' then "
                "  local parts = {} "
                "  for k,v in pairs(__v) do parts[#parts+1] = tostring(k)..'='..tostring(v) end "
                "  print('{' .. table.concat(parts, ', ') .. '}') "
                "else print(tostring(__v)) end";
            am->runScript(wrapped);
        } else {
            game::MessageChatData errMsg;
            errMsg.type = game::ChatType::SYSTEM;
            errMsg.language = game::ChatLanguage::UNIVERSAL;
            errMsg.message = "Addon system not initialized.";
            ctx.gameHandler.addLocalChatMessage(errMsg);
        }
        return {};
    }
    [[nodiscard]] std::vector<std::string> aliases() const override { return {"dump", "print"}; }
    [[nodiscard]] std::string helpText() const override { return "Evaluate & print Lua expression"; }
};

// --- /reload, /reloadui, /rl ---
class ReloadCommand : public IChatCommand {
public:
    ChatCommandResult execute(ChatCommandContext& ctx) override {
        auto* am = ctx.services.addonManager;
        if (am) {
            // What FrameXML's own /reload does (ChatFrame.lua: ReloadUI()):
            // ask for it, and let the frame take the interface down as the
            // client does (0x0052af40) - PLAYER_LEAVING_WORLD and
            // PLAYER_LOGOUT first, then the load and its login events. Doing
            // the reload here skipped the leaving half. Secure, as that
            // slash command's own handler is, rather than as typed /run text.
            am->runInterfaceCommand("ReloadUI()");
        } else {
            game::MessageChatData rlMsg;
            rlMsg.type = game::ChatType::SYSTEM;
            rlMsg.language = game::ChatLanguage::UNIVERSAL;
            rlMsg.message = "Addon system not available.";
            ctx.gameHandler.addLocalChatMessage(rlMsg);
        }
        return {};
    }
    [[nodiscard]] std::vector<std::string> aliases() const override { return {"reload", "reloadui", "rl"}; }
    [[nodiscard]] std::string helpText() const override { return "Reload all addons"; }
};

// --- /fxcheck ---
class FrameXmlCheckCommand : public IChatCommand {
public:
    ChatCommandResult execute(ChatCommandContext& ctx) override {
        frameXmlRequestCheck();

        // What the interface itself can see, asked through the same bindings
        // FrameXML uses. The widget report says whether a frame is shown; it
        // cannot say whether it *should* be, and those are the two readings
        // that have to be told apart. A hidden target frame is correct with no
        // target and a fault with one, and from the tree alone they are the
        // same line. print() reaches the log, so the answer lands beside the
        // report it belongs to.
        if (auto* am = ctx.services.addonManager) {
            am->runScript(
                "local function yn(v) return v and 'yes' or 'no' end\n"
                "local auras = 0\n"
                "for i = 1, 40 do if not UnitAura('player', i) then break end auras = i end\n"
                "print('[fxcheck] target=' .. yn(UnitExists('target')) ..\n"
                "      ' name=' .. tostring(UnitExists('target') and UnitName('target')) ..\n"
                "      ' | TargetFrame shown=' .. yn(TargetFrame and TargetFrame:IsShown()) ..\n"
                "      ' unit=' .. tostring(TargetFrame and TargetFrame.unit) ..\n"
                "      ' | player auras=' .. auras ..\n"
                "      ' BuffButton1=' .. tostring(BuffButton1 ~= nil) ..\n"
                "      ' | XP=' .. tostring(UnitXP('player')) .. '/' .. tostring(UnitXPMax('player')) ..\n"
                "      ' | bag0 slots=' .. tostring(GetContainerNumSlots(0)))\n");
        }

        game::MessageChatData msg;
        msg.type = game::ChatType::SYSTEM;
        msg.language = game::ChatLanguage::UNIVERSAL;
        msg.message = "FrameXML takeover check written to the log.";
        ctx.gameHandler.addLocalChatMessage(msg);
        return {};
    }
    [[nodiscard]] std::vector<std::string> aliases() const override { return {"fxcheck"}; }
    [[nodiscard]] std::string helpText() const override {
        return "Report the FrameXML takeover check for the interface as it is now";
    }
};

// --- /stopmacro [conditions] ---
class StopMacroCommand : public IChatCommand {
public:
    ChatCommandResult execute(ChatCommandContext& ctx) override {
        bool shouldStop = true;
        if (!ctx.args.empty()) {
            std::string condArg = ctx.args;
            while (!condArg.empty() && condArg.front() == ' ') condArg.erase(condArg.begin());
            if (!condArg.empty() && condArg.front() == '[') {
                // Append a sentinel action so evaluateMacroConditionals can signal a match.
                uint64_t tgtOver = static_cast<uint64_t>(-1);
                std::string hit = evaluateMacroConditionals(condArg + " __stop__", ctx.gameHandler, tgtOver);
                shouldStop = !hit.empty();
            }
        }
        if (shouldStop) ctx.panel.macroStopped() = true;
        return {};
    }
    [[nodiscard]] std::vector<std::string> aliases() const override { return {"stopmacro"}; }
    [[nodiscard]] std::string helpText() const override { return "Stop macro execution"; }
};

// --- /clear ---
class ClearCommand : public IChatCommand {
public:
    ChatCommandResult execute(ChatCommandContext& ctx) override {
        ctx.gameHandler.clearChatHistory();
        return {};
    }
    [[nodiscard]] std::vector<std::string> aliases() const override { return {"clear"}; }
    [[nodiscard]] std::string helpText() const override { return "Clear chat history"; }
};

// --- /difficulty ---
class DifficultyCommand : public IChatCommand {
public:
    ChatCommandResult execute(ChatCommandContext& ctx) override {
        std::string arg = ctx.args;
        // Trim whitespace
        size_t first = arg.find_first_not_of(" \t");
        size_t last  = arg.find_last_not_of(" \t");
        if (first != std::string::npos)
            arg = arg.substr(first, last - first + 1);
        else
            arg.clear();
        for (auto& ch : arg) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));

        uint32_t diff = 0;
        bool valid = true;
        if (arg == "normal" || arg == "0")         diff = 0;
        else if (arg == "heroic" || arg == "1")    diff = 1;
        else if (arg == "25" || arg == "25normal" || arg == "25man" || arg == "2")
            diff = 2;
        else if (arg == "25heroic" || arg == "25manheroic" || arg == "3")
            diff = 3;
        else valid = false;

        if (!valid || arg.empty()) {
            game::MessageChatData msg;
            msg.type = game::ChatType::SYSTEM;
            msg.language = game::ChatLanguage::UNIVERSAL;
            msg.message = "Usage: /difficulty normal|heroic|25|25heroic  (0-3)";
            ctx.gameHandler.addLocalChatMessage(msg);
        } else {
            static constexpr const char* kDiffNames[] = {
                "Normal (5-man)", "Heroic (5-man)", "Normal (25-man)", "Heroic (25-man)"
            };
            game::MessageChatData msg;
            msg.type = game::ChatType::SYSTEM;
            msg.language = game::ChatLanguage::UNIVERSAL;
            msg.message = std::string("Setting difficulty to: ") + kDiffNames[diff];
            ctx.gameHandler.addLocalChatMessage(msg);
            ctx.gameHandler.sendSetDifficulty(diff);
        }
        return {};
    }
    [[nodiscard]] std::vector<std::string> aliases() const override { return {"difficulty"}; }
    [[nodiscard]] std::string helpText() const override { return "Set dungeon difficulty"; }
};

// --- Registration ---
// ---------------------------------------------------------------------------
// /climb [on|off] - walk up anything, for reaching a place to look at it.
// ---------------------------------------------------------------------------
class ClimbCommand : public IChatCommand {
public:
    ChatCommandResult execute(ChatCommandContext& ctx) override {
        auto* renderer = ctx.services.renderer;
        auto* cam = renderer ? renderer->getCameraController() : nullptr;
        if (!cam) {
            ctx.gameHandler.addLocalChatMessage(
                chat_utils::makeSystemMessage("Climb: no camera to set it on."));
            return {};
        }
        std::string arg = ctx.args;
        for (char& c : arg) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        while (!arg.empty() && std::isspace(static_cast<unsigned char>(arg.front()))) arg.erase(arg.begin());
        while (!arg.empty() && std::isspace(static_cast<unsigned char>(arg.back()))) arg.pop_back();

        const bool want = arg.empty() ? !cam->ignoresSlopeLimit()
                                      : (arg == "on" || arg == "1" || arg == "yes");
        cam->setIgnoreSlopeLimit(want);
        ctx.gameHandler.addLocalChatMessage(chat_utils::makeSystemMessage(
            want ? "Climb: on - any slope is walkable."
                 : "Climb: off - the 50 degree limit is back."));
        return {};
    }
    [[nodiscard]] std::vector<std::string> aliases() const override { return {"climb", "slope"}; }
    [[nodiscard]] std::string helpText() const override {
        return "Walk up any slope, for reaching somewhere to look at it (/climb on|off)";
    }
};

// ---------------------------------------------------------------------------
// /gps - where the client has the player, as `.go xyz X Y Z MAP O` in the
// server's convention (not the server's own `.gps` answer), to compare the
// two after a teleport and to go back to a spot.
// ---------------------------------------------------------------------------
class GpsCommand : public IChatCommand {
public:
    ChatCommandResult execute(ChatCommandContext& ctx) override {
        const auto say = [&](const std::string& text) {
            ctx.gameHandler.addLocalChatMessage(chat_utils::makeSystemMessage(text));
            LOG_INFO("/gps: ", text);
        };
        const uint32_t mapId = ctx.gameHandler.getCurrentMapId();
        const auto& mi = ctx.gameHandler.getMovementInfo();
        const glm::vec3 sent = core::coords::canonicalToServer(glm::vec3(mi.x, mi.y, mi.z));
        const float facing = core::coords::canonicalToServerYaw(mi.orientation);
        auto* renderer = ctx.services.renderer;
        // Where the model stands, which is what the screen shows; the
        // movement the client last told the server below it if they part.
        glm::vec3 drawn = sent;
        if (renderer) {
            drawn = core::coords::canonicalToServer(
                core::coords::renderToCanonical(renderer->getCharacterPosition()));
        }
        say(client_gps::goXyzLine(mapId, drawn.x, drawn.y, drawn.z, facing));
        if (client_gps::differs(drawn.x, drawn.y, drawn.z, sent.x, sent.y, sent.z)) {
            say("Sent to the server: " + client_gps::goXyzLine(mapId, sent.x, sent.y, sent.z, facing));
        }
        // The view, as /cam puts it back: where the camera is going - its
        // distance, its pitch, its yaw from the facing (it orbits).
        if (auto* cam = renderer ? renderer->getCameraController() : nullptr) {
            const auto pose = cam->currentViewPose();
            say(client_gps::cameraLine({pose.distance, pose.pitch, pose.yaw}));
        }
        // On a transport: which, where the client has it, and the offset on
        // it that goes to the server with the position.
        if (ctx.gameHandler.isOnTransport()) {
            const uint64_t tguid = ctx.gameHandler.getPlayerTransportGuid();
            const glm::vec3 off = ctx.gameHandler.getPlayerTransportOffset();
            std::string line = "On transport " + std::to_string(tguid & 0xFFFFFFu);
            if (auto* tm = ctx.gameHandler.getTransportManager()) {
                if (const auto* tr = tm->getTransport(tguid)) {
                    const glm::vec3 at = core::coords::canonicalToServer(tr->position);
                    char buf[160];
                    std::snprintf(buf, sizeof(buf), " (entry %u) at %.3f %.3f %.3f",
                                  tr->entry, at.x, at.y, at.z);
                    line += buf;
                }
            }
            char buf[96];
            std::snprintf(buf, sizeof(buf), ", offset %.3f %.3f %.3f", off.x, off.y, off.z);
            say(line + buf);
        }
        if (const auto* t = renderer ? renderer->getZoneTexts() : nullptr) {
            const std::string zone = client_gps::zoneLine(t->zone, t->subzone);
            if (!zone.empty()) say(zone);
        }
        return {};
    }
    [[nodiscard]] std::vector<std::string> aliases() const override { return {"gps"}; }
    [[nodiscard]] std::string helpText() const override {
        return "The client's position as a .go xyz line (server convention)";
    }
};

// ---------------------------------------------------------------------------
// /cam <distance> <pitch> <yaw> - the camera as /gps reported it: its
// distance, pitch and yaw from the facing (degrees), or the whole "Camera:"
// line. At once, as SetView with no blend (0x00603330).
// ---------------------------------------------------------------------------
class CamCommand : public IChatCommand {
public:
    ChatCommandResult execute(ChatCommandContext& ctx) override {
        const auto say = [&](const std::string& text) {
            ctx.gameHandler.addLocalChatMessage(chat_utils::makeSystemMessage(text));
            LOG_INFO("/cam: ", text);
        };
        auto* renderer = ctx.services.renderer;
        auto* cam = renderer ? renderer->getCameraController() : nullptr;
        if (!cam) {
            say("Camera: no camera to set.");
            return {};
        }
        const auto pose = client_gps::parseCamera(ctx.args);
        if (!pose) {
            say("Usage: /cam <distance> <pitch> <yaw>, or a /gps Camera: line");
            return {};
        }
        cam->goToViewPose({pose->distance, pose->pitch, pose->yaw}, 0.0f);
        const auto now = cam->currentViewPose();
        say(client_gps::cameraLine({now.distance, now.pitch, now.yaw}));
        return {};
    }
    [[nodiscard]] std::vector<std::string> aliases() const override { return {"cam"}; }
    [[nodiscard]] std::string helpText() const override {
        return "Set the camera as /gps reported it (/cam distance pitch yaw)";
    }
};

// --- /profile [on|off|dump|csv [path|off]] ---
class ProfileCommand : public IChatCommand {
public:
    ChatCommandResult execute(ChatCommandContext& ctx) override {
        auto& prof = core::FrameProfiler::get();
        std::string arg = ctx.args;
        for (char& c : arg) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        while (!arg.empty() && std::isspace(static_cast<unsigned char>(arg.front()))) arg.erase(arg.begin());
        while (!arg.empty() && std::isspace(static_cast<unsigned char>(arg.back()))) arg.pop_back();
        const auto say = [&](const std::string& text) {
            ctx.gameHandler.addLocalChatMessage(chat_utils::makeSystemMessage(text));
        };
        // The settings the numbers are measured under, in the chat and the log
        // alongside the table (the overlay shows the same line).
        const auto saySettings = [&]() {
            if (auto* renderer = ctx.services.renderer) {
                const std::string line = "Settings: " + renderer->qualitySummary();
                say(line);
                LOG_WARNING(line);
            }
        };

        if (arg.empty() || arg == "on" || arg == "off") {
            const bool want = arg.empty() ? !core::FrameProfiler::enabled() : arg == "on";
            prof.setEnabled(want);
            say(want ? "Frame profiler: on. The table fills in after a second; /profile dump "
                       "writes it to the log."
                     : "Frame profiler: off.");
            if (want) saySettings();
        } else if (arg == "dump") {
            // A dump of nothing is no use, so asking for one starts it, and
            // the table lands in the log once a second has been measured.
            if (!core::FrameProfiler::enabled()) prof.setEnabled(true);
            prof.requestDump();
            say("Frame profiler: the next second's table goes to the log.");
            saySettings();
        } else if (arg == "ablate" || arg == "ablation") {
            // The scene pass's cost by switching each pass off for a few
            // seconds: what a phone's GPU timestamps cannot say, since a tiler
            // puts the whole pass on its first mark.
            auto* renderer = ctx.services.renderer;
            if (!renderer) {
                say("Pass ablation: no renderer.");
            } else if (!renderer->startPassAblation()) {
                say("Pass ablation: one is already running.");
            } else {
                say("Pass ablation: started. Stand still outdoors and leave the camera alone "
                    "for about a minute and a half; the table goes to the log, and "
                    "/perf ablate report shows it here.");
            }
        } else if (arg == "ablate report" || arg == "ablation report") {
            auto* renderer = ctx.services.renderer;
            const std::string report = renderer ? renderer->passAblationReport() : std::string();
            if (report.empty()) {
                say("Pass ablation: no finished run yet.");
            } else {
                size_t start = 0;
                while (start < report.size()) {
                    const size_t end = report.find('\n', start);
                    say(report.substr(start, end == std::string::npos ? std::string::npos : end - start));
                    if (end == std::string::npos) break;
                    start = end + 1;
                }
            }
        } else if (arg.rfind("csv", 0) == 0) {
            // The path as typed, not lowercased.
            std::string path = ctx.args;
            const size_t at = path.find_first_not_of(" \t");
            path = (at == std::string::npos) ? std::string() : path.substr(at + 3);
            while (!path.empty() && std::isspace(static_cast<unsigned char>(path.front()))) path.erase(path.begin());
            while (!path.empty() && std::isspace(static_cast<unsigned char>(path.back()))) path.pop_back();
            if (path == "off" || path == "stop") {
                prof.stopCsv();
                say("Frame profiler: CSV closed.");
            } else {
                if (path.empty()) path = "wowee_profile.csv";
                std::string error;
                if (prof.startCsv(path, error)) {
                    if (!core::FrameProfiler::enabled()) prof.setEnabled(true);
                    say("Frame profiler: a row per stage per second to " + path +
                        " until /profile csv off.");
                } else {
                    say("Frame profiler: " + error);
                }
            }
        } else {
            say("Usage: /profile [on|off|dump|csv [path|off]|ablate [report]]");
        }
        return {};
    }
    [[nodiscard]] std::vector<std::string> aliases() const override { return {"profile", "perf"}; }
    [[nodiscard]] std::string helpText() const override {
        return "Frame time by pass, CPU and GPU (/profile on|off|dump|csv [path|off]|ablate [report]; Ctrl+F12)";
    }
};

void registerSystemCommands(ChatCommandRegistry& reg) {
    reg.registerCommand(std::make_unique<ProfileCommand>());
    reg.registerCommand(std::make_unique<ClimbCommand>());
    reg.registerCommand(std::make_unique<GpsCommand>());
    reg.registerCommand(std::make_unique<CamCommand>());
    reg.registerCommand(std::make_unique<RunCommand>());
    reg.registerCommand(std::make_unique<DumpCommand>());
    reg.registerCommand(std::make_unique<ReloadCommand>());
    reg.registerCommand(std::make_unique<FrameXmlCheckCommand>());
    reg.registerCommand(std::make_unique<StopMacroCommand>());
    reg.registerCommand(std::make_unique<ClearCommand>());
    reg.registerCommand(std::make_unique<DifficultyCommand>());
}

} // namespace ui
} // namespace wowee
