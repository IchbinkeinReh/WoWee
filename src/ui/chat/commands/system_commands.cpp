// System commands: /run, /dump, /reload, /stopmacro, /clear, /logout
// Moved from ChatPanel::sendChatMessage() if/else chain (Phase 3).
#include "ui/chat/i_chat_command.hpp"
#include "ui/chat/macro_evaluator.hpp"
#include "ui/chat_panel.hpp"
#include "ui/ui_services.hpp"
#include "game/game_handler.hpp"
#include "addons/addon_manager.hpp"
#include "addons/lua_api_registrations.hpp"
#include "addons/lua_engine.hpp"
#include "audio/audio_coordinator.hpp"
#include "audio/audio_engine.hpp"
#include "audio/music_manager.hpp"
#include "core/application.hpp"
#include "core/entity_spawner.hpp"
#include "ui/framexml_takeover.hpp"
#include "ui/chat/chat_utils.hpp"
#include "ui/chat/client_gps.hpp"
#include "core/coordinates.hpp"
#include "rendering/camera_controller.hpp"
#include "rendering/renderer.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/lighting_manager.hpp"
#include "core/frame_profiler.hpp"
#include "core/logger.hpp"
#include "core/version.hpp"
#include "game/transport_manager.hpp"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

extern "C" {
#include <lua.h>
}

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
        // The time of day the light is drawn for, and the Debug Time
        // override when one is on - what /daytime takes back.
        if (auto* lighting = renderer ? renderer->getLightingManager() : nullptr) {
            say(client_gps::timeLine(lighting->getTimeOfDayHours(), renderer->getGameTimeOverride()));
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

// ---------------------------------------------------------------------------
// /daytime <hours|hh:mm|off> - the time of day the light is drawn for, as
// /gps reported it: hours, a clock, or the whole "Time:" line (its Debug Time
// when it names one). Sets the Debug Time override through the settings, as
// its controls do; "off" gives the server's time back. Not /time, which the
// interface already answers (the server's clock).
// ---------------------------------------------------------------------------
class DaytimeCommand : public IChatCommand {
public:
    ChatCommandResult execute(ChatCommandContext& ctx) override {
        const auto say = [&](const std::string& text) {
            ctx.gameHandler.addLocalChatMessage(chat_utils::makeSystemMessage(text));
            LOG_INFO("/daytime: ", text);
        };
        const auto arg = client_gps::parseTime(ctx.args);
        if (!arg) {
            say("Usage: /daytime <hours> | <hh:mm> | off, or a /gps Time: line");
            return {};
        }
        char script[128];
        if (arg->off) {
            std::snprintf(script, sizeof(script), "WoweeSetSetting('dbgtimeoverride', false)");
        } else {
            std::snprintf(script, sizeof(script),
                          "WoweeSetSetting('dbgtime', %.4f) WoweeSetSetting('dbgtimeoverride', true)",
                          static_cast<double>(arg->hours));
        }
        if (auto* am = ctx.services.addonManager; am && am->isInitialized()) {
            am->runScript(script);
        } else if (auto* renderer = ctx.services.renderer) {
            renderer->setGameTimeOverride(arg->off ? -1.0f : arg->hours);
        }
        say(arg->off ? std::string("Debug Time: off - the server's time of day again.")
                     : "Debug Time: " + client_gps::clockText(arg->hours));
        return {};
    }
    [[nodiscard]] std::vector<std::string> aliases() const override { return {"daytime"}; }
    [[nodiscard]] std::string helpText() const override {
        return "Draw the light at a time of day as /gps reported it (/daytime 19:30 | off)";
    }
};

// --- /mem ---
// What the process holds and who holds it: the process's resident and
// private bytes, each Vulkan heap's use against its budget, and the ray
// tracing's owners, GPU and CPU - readable in the chat on a phone, where the
// log is hard to get at. Also written to the log at warning.
class MemCommand : public IChatCommand {
public:
    ChatCommandResult execute(ChatCommandContext& ctx) override {
        auto* renderer = ctx.services.renderer;
        if (!renderer) return {};
        // "/mem low": as if the system had said it is short of memory
        // (Android's onLowMemory), to see what the ray tracing's guard does.
        if (ctx.args.find("low") != std::string::npos) {
            renderer->noteSystemLowMemory();
            ctx.gameHandler.addLocalChatMessage(
                chat_utils::makeSystemMessage("Memory: the system's low-memory signal, simulated."));
            return {};
        }
        // "/mem dump": every allocation the Vulkan allocator holds, to a file.
        if (ctx.args.find("dump") != std::string::npos) {
            if (auto* vk = renderer->getVkContext()) {
                const std::string path = vk->dumpAllocatorJson();
                LOG_WARNING("/mem: allocator dumped to ", path.empty() ? std::string("(failed)") : path);
            }
        }
        std::vector<rendering::MemoryEntry> extra;
        if (auto* addons = ctx.services.addonManager) {
            if (auto* engine = addons->getLuaEngine(); engine && engine->getState()) {
                lua_State* L = engine->getState();
                // "/mem gc": a full collection first, so the figure is what the
                // state holds rather than what it has not yet collected.
                if (ctx.args.find("gc") != std::string::npos) lua_gc(L, LUA_GCCOLLECT, 0);
                const uint64_t kb = static_cast<uint64_t>(lua_gc(L, LUA_GCCOUNT, 0));
                const uint64_t rest = static_cast<uint64_t>(lua_gc(L, LUA_GCCOUNTB, 0));
                extra.push_back({"Lua state", 0, kb * 1024 + rest});
            }
        }
        {
            const auto wav = audio::AudioEngine::decodedCacheStats();
            char name[192];
            std::snprintf(name, sizeof(name), "Decoded sounds (%zu)", wav.sounds);
            extra.push_back({name, 0, wav.bytes});
            if (auto* coord = ctx.services.audioCoordinator) {
                if (auto* music = coord->getMusicManager()) {
                    std::snprintf(name, sizeof(name), "Music files kept (%zu)", music->cachedMusicCount());
                    extra.push_back({name, 0, music->cachedMusicBytes()});
                }
            }
        }
        if (auto* spawner = core::Application::getInstance().getEntitySpawner()) spawner->memoryReport(extra);
        for (const std::string& line : renderer->memoryReport(extra)) {
            ctx.gameHandler.addLocalChatMessage(chat_utils::makeSystemMessage(line));
            LOG_WARNING("/mem: ", line);
        }
        return {};
    }
    [[nodiscard]] std::vector<std::string> aliases() const override { return {"mem"}; }
    [[nodiscard]] std::string helpText() const override {
        return "Show the client's memory: process, Vulkan heaps, ray tracing by part (/mem low: simulate low memory; /mem gc: collect Lua garbage first; /mem dump: allocator to a file)";
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

// --- /cvarlist, /cvar_reset, /cvar_default ---
//
// The client's own console command table registers these three
// (FUN_00769100("cvarlist", 0x00767220, 5, "List cvars"), and the two setters
// with the help strings "Set the value of a CVar to it's startup value" and
// "...to it's coded default value"). The 5 is the reachability mask - console and
// slash both - so a player typing /cvarlist in the chat box gets a listing in the
// real client, and got "unknown command" here.
//
// The state behind them is the CVar store's, which is why the answers come from
// addons::cVarConsoleRows rather than from anything in this file: the store is
// one of several places a value comes from, and a listing built beside it would
// say something the panels are already not doing.
namespace {

/// A console line in the chat frame, which is where the client's own prints go.
///
/// The one thing here that reads differently from the real client: its console
/// prints carry a message level in the line record - 0 for the ordinary ones, 3
/// for the errors (`No such cvar`, `Invalid difficulty`) - and these all arrive
/// as the same kind of system line. This client's chat messages carry no level to
/// put that distinction on.
void sayConsole(ChatCommandContext& ctx, const std::string& text) {
    ctx.gameHandler.addLocalChatMessage(chat_utils::makeSystemMessage(text));
}

/// The Lua state the CVars live in, or null before the interface is up.
lua_State* cVarLuaState(ChatCommandContext& ctx) {
    auto* am = ctx.services.addonManager;
    return am && am->isInitialized() ? am->getLuaEngine()->getState() : nullptr;
}

/// The first word of the arguments, cut the way the client cuts them.
///
/// The console commands run the line through their tokeniser with the delimiter
/// set " ,;\t\"\r\n" (FUN_0076f1e0) and take the first token into a 64-byte
/// buffer, so a quoted name works, a comma separates two names of which only the
/// first is looked at, and a name longer than 63 characters is not the name the
/// client goes looking for.
std::string firstConsoleToken(const std::string& args) {
    static constexpr std::string_view kDelimiters = " ,;\t\"\r\n";
    const size_t start = args.find_first_not_of(kDelimiters);
    if (start == std::string_view::npos) return {};
    const size_t end = args.find_first_of(kDelimiters, start);
    std::string token = std::string(args.substr(start, end == std::string::npos ? end
                                                                               : end - start));
    if (token.size() > 63) token.resize(63);
    return token;
}

/// The two words `/set` reads, cut the way its handler cuts them (0x007681f0):
/// one tokeniser called twice over the same running position, the name into a
/// 64-byte buffer and the value into a 2048-byte one. The difference from the
/// single token above is the second cap - a name stops at 63 characters, a value
/// at 2047 - and the fact that a value with no delimiter in it is one token
/// however long. Delimiters are the same set, so a quoted value is cut at the
/// quote here too, which is what the original does.
void consoleNameAndValue(const std::string& args, std::string& name, std::string& value) {
    static constexpr std::string_view kDelimiters = " ,;\t\"\r\n";
    size_t pos = 0;
    const auto next = [&](size_t cap) {
        pos = args.find_first_not_of(kDelimiters, pos);
        if (pos == std::string::npos) return std::string{};
        const size_t end = args.find_first_of(kDelimiters, pos);
        std::string token = args.substr(pos, end == std::string::npos ? std::string::npos
                                                                     : end - pos);
        pos = end == std::string::npos ? args.size() : end;
        if (token.size() > cap) token.resize(cap);
        return token;
    };
    name = next(63);
    value = next(2047);
}

} // namespace

class CVarListCommand : public IChatCommand {
public:
    ChatCommandResult execute(ChatCommandContext& ctx) override {
        lua_State* L = cVarLuaState(ctx);
        if (!L) return {};
        // No arguments: the client's handler reads none, so /cvarlist with a word
        // after it still lists everything.
        for (const addons::CVarConsoleRow& row : addons::cVarConsoleRows(L)) {
            std::string line = "  \"" + row.name + "\" is \"" + row.value + "\"";
            if (!row.defaultValue.empty())
                line += " (default \"" + row.defaultValue + "\")";
            if (!row.resetValue.empty())
                line += " (reset \"" + row.resetValue + "\")";
            ctx.gameHandler.addLocalChatMessage(chat_utils::makeSystemMessage(line));
        }
        return {};
    }
    [[nodiscard]] std::vector<std::string> aliases() const override { return {"cvarlist"}; }
    [[nodiscard]] std::string helpText() const override { return "List every CVar and its value"; }
};

class CVarResetCommand : public IChatCommand {
public:
    ChatCommandResult execute(ChatCommandContext& ctx) override {
        const std::string name = firstConsoleToken(ctx.args);
        lua_State* L = cVarLuaState(ctx);
        if (!name.empty()) {
            if (!addons::cVarConsoleHas(name)) {
                sayConsole(ctx, "No such cvar \"" + name + "\"");
                return {};
            }
            if (L) addons::resetStoredCVar(L, name);
            return {};
        }
        sayConsole(ctx, "Resetting all cvars");
        if (!L) return {};
        // Every name in the listing, which is what the client's walk covers
        // except the CVars it builds in a loop - the eight camera views'
        // Distance/Pitch/Yaw and the per-motion smoothing knobs. Those keep their
        // values here; see addons::clientCVarNames.
        for (const addons::CVarConsoleRow& row : addons::cVarConsoleRows(L))
            addons::resetStoredCVar(L, row.name);
        return {};
    }
    [[nodiscard]] std::vector<std::string> aliases() const override { return {"cvar_reset"}; }
    [[nodiscard]] std::string helpText() const override {
        return "Set a CVar to its startup value (/cvar_reset [name])";
    }
};

class CVarDefaultCommand : public IChatCommand {
public:
    ChatCommandResult execute(ChatCommandContext& ctx) override {
        const std::string name = firstConsoleToken(ctx.args);
        lua_State* L = cVarLuaState(ctx);
        if (!name.empty()) {
            if (!addons::cVarConsoleHas(name)) {
                sayConsole(ctx, "No such cvar \"" + name + "\"");
                return {};
            }
            if (L) addons::restoreCVarDefault(L, name);
            return {};
        }
        sayConsole(ctx, "Restoring all cvars");
        if (!L) return {};
        for (const addons::CVarConsoleRow& row : addons::cVarConsoleRows(L))
            addons::restoreCVarDefault(L, row.name);
        return {};
    }
    [[nodiscard]] std::vector<std::string> aliases() const override { return {"cvar_default"}; }
    [[nodiscard]] std::string helpText() const override {
        return "Set a CVar to its coded default (/cvar_default [name])";
    }
};

// --- /set <name> [value] ---
//
// `set` in the client's console command table (FUN_00769100(&DAT_00a385e0,
// 0x007681f0, 5, "Set the value of a CVar") - the name is "set", spelled at
// 0x00a385e0 rather than inline). Help category 5 "default" - nothing about that
// says where the name is reachable from, but FrameXML has no SLASH_SET of its own
// (only /settitle), so nothing else answers the name and a player typing /set got
// this client's help line instead.
//
// What the handler does is in setCVarFromConsole: the name is looked up
// case-insensitively, and - unlike the two commands above - a name that lookup
// cannot find is not an error. The client registers it with no flags and assigns
// the value, so `/set myaddon_thing 1` puts the name into the same store a panel
// reads and into `cvarlist`. Nothing is printed either way.
//
// The one place this client parts ways with it: with no name in front of it the
// original registers a CVar whose name is the empty string. The name is what the
// config file is keyed by, and one line keyed on nothing is a line this client
// would have to special-case on every read, so an empty name writes nothing.
class SetCVarConsoleCommand : public IChatCommand {
public:
    ChatCommandResult execute(ChatCommandContext& ctx) override {
        lua_State* L = cVarLuaState(ctx);
        if (!L) return {};
        std::string name;
        std::string value;
        consoleNameAndValue(ctx.args, name, value);
        // No value is a value: the handler's second tokeniser call leaves its
        // buffer empty and assigns that, which re-parses the numeric CVar to 0.
        addons::setCVarFromConsole(L, name, value);
        return {};
    }
    [[nodiscard]] std::vector<std::string> aliases() const override { return {"set"}; }
    [[nodiscard]] std::string helpText() const override {
        return "Set a CVar by name, making one up if there is none (/set <name> <value>)";
    }
};

// --- /changeplayerdifficulty <0|1> ---
//
// The last name the client's console command table reaches by slash that this
// client left unanswered: FUN_00769100("changePlayerDifficulty", 0x0052d060, 5,
// 0), the last 0 being no help string. It is the hard path rather than the
// dropdown's: the client turns the argument into a number with its own decimal
// reader (0x0076f0d0 - a sign, then digits, no 0x) and sends whenever the result
// is under 2, asking nothing first. Neither the "may this player change it" test
// (0x0052cbc0) nor the cooldown a change starts, both of which the interface's
// own leg (0x0052d8d0) obeys, is consulted - the realm is what refuses, through
// SMSG_CHANGEPLAYER_DIFFICULTY_RESULT, which this client already reads.
//
// A missing argument reads as 0 because that is what the reader answers for an
// empty token, so the bare form sends Normal rather than complaining.
class ChangePlayerDifficultyCommand : public IChatCommand {
public:
    ChatCommandResult execute(ChatCommandContext& ctx) override {
        const std::string token = firstConsoleToken(ctx.args);
        // Compared as the client compares it, on the unsigned result: "2" and up
        // and anything negative is refused here instead of being sent.
        const uint32_t difficulty =
            static_cast<uint32_t>(std::strtoull(token.c_str(), nullptr, 10));
        if (difficulty >= 2) {
            sayConsole(ctx, "Invalid difficulty");
            return {};
        }
        ctx.gameHandler.sendChangePlayerDifficulty(difficulty);
        return {};
    }
    // The registry keys on the alias as written and dispatches the typed name
    // lower-cased, so the client's camelCase spelling has to be stored lower.
    [[nodiscard]] std::vector<std::string> aliases() const override {
        return {"changeplayerdifficulty"};
    }
    [[nodiscard]] std::string helpText() const override {
        return "Set the difficulty in a dynamic instance (/changeplayerdifficulty 0|1)";
    }
};

// --- /pitchlimit <degrees> ---
//
// `pitchLimit` in the client's console command table
// (FUN_00769100("pitchLimit", 0x005fe400, 4, 0) - slash only, no help string).
// Its handler reads the argument twice with the console's float reader
// (0x0076fb80) and writes the two camera pitch limits: degrees * 0.017453292 into
// the maximum at 0x00ad1b4c and degrees * -0.017453292 into the minimum at
// 0x00ad1b50, both held there in radians. So the command sets a symmetric pair
// from one number, is bounded by nothing (not even abs: a negative argument
// inverts the pair and leaves the camera no pitch at all), prints nothing and
// reports success whatever it was given.
//
// The two globals start up as 0x3fc6d3f2 and 0xbfffd3f2, that is ±89 degrees;
// this client's clamps were constants at ±88 until the command needed them to
// move, so the defaults here are the client's own rather than the old constants.
// A missing argument reads as 0, which is a camera that cannot tilt - also the
// client's answer, and why this command does not complain about it.
class PitchLimitCommand : public IChatCommand {
public:
    ChatCommandResult execute(ChatCommandContext& ctx) override {
        auto* renderer = ctx.services.renderer;
        auto* cam = renderer ? renderer->getCameraController() : nullptr;
        if (!cam) return {};
        const std::string token = firstConsoleToken(ctx.args);
        // Degrees, because every pitch clamp in the camera works in degrees while
        // the client keeps its pair in radians; the setter converts both sides.
        cam->setPitchLimitDegrees(std::strtof(token.c_str(), nullptr));
        return {};
    }
    [[nodiscard]] std::vector<std::string> aliases() const override { return {"pitchlimit"}; }
    [[nodiscard]] std::string helpText() const override {
        return "How far the camera may tilt up and down (/pitchlimit <degrees>)";
    }
};

// --- /ver ---
//
// "ver" in the client's console command table (FUN_00769100(&DAT_00a38540,
// 0x00768420, 5, 0) - help category 5 "default", and the name is spelled right
// beside the "run" the executor goes looking for when a line matches nothing).
// The third argument is no permission: Cmd_Exec (0x007658a0) reads only the
// handler out of the entry, and the one reader of that dword in the whole client
// is the console's own help command, which groups names by category (the nine
// names are the table at 0x00adbc70). The handler is twenty-one bytes: one
// console print (0x00765270, level 0) of
// "WoW [Release] Build 12340 (Jun 24 2010)" and `return 1;`. No argument is read
// and nothing is checked, so `/ver` answers the same way twice in a row.
//
// What is printed here is this client's own build in the same shape - name,
// bracketed mode, build, build date - which is what the login screen and the
// settings panel already show. The original's numbers say which binary is
// running, and this is not that binary.
class VersionCommand : public IChatCommand {
public:
    ChatCommandResult execute(ChatCommandContext& ctx) override {
        sayConsole(ctx, std::string("Wowee [Release] ") + core::kVersionString);
        return {};
    }
    [[nodiscard]] std::vector<std::string> aliases() const override { return {"ver"}; }
    [[nodiscard]] std::string helpText() const override {
        return "Print which build this client is (/ver)";
    }
};

// --- /memusage, /heapusage2, /setmap ---
//
// Three of the names in the client's console command table share one handler:
// FUN_00769100 registers "MemUsage" and "HeapUsage2" (help category 4, game) and
// "setmap" (5, default) all against 0x008c8de0, and that handler is six bytes -
// `return 1;`. Run from the console - typed into its window, or after /console
// or /konsole, which is FrameXML's route into Cmd_Exec - any of the three does
// nothing at all, prints nothing, and reports success.
//
// Bare in the chat box the original answers differently, and it is worth being
// straight about it: these names are in no SLASH_ global, ChatEdit_ParseText
// consults nothing but SlashCmdList and the emote table, and its tail adds
// HELP_TEXT_SIMPLE to the chat frame. So a player typing /memusage into the
// 12340 client gets the help line, not silence. They get it here too only if
// this client does not answer the name - its own commands are bridged into
// SlashCmdList (see tools/client_command_bridge_check.py), because with no
// console window a name this client can run has to be typed bare or it cannot
// be typed at all. Listing them keeps the bridged name and what the console
// would have done for it in step.
//
// The two memory names that really report are left out on purpose. "HeapUsage"
// (0x004d27e0) walks the client's own allocator, through a 50000-byte buffer on
// the stack, and "ObjUsage" (0x004d49e0) walks its object list by the next
// pointer at +0xac and the size at +0xa4; what either prints is the state of an
// allocator and an object heap this client does not have.
class InertConsoleCommand : public IChatCommand {
public:
    ChatCommandResult execute(ChatCommandContext&) override { return {}; }
    [[nodiscard]] std::vector<std::string> aliases() const override {
        return {"memusage", "heapusage2", "setmap"};
    }
    [[nodiscard]] std::string helpText() const override {
        return "Does nothing, as the original's handler does";
    }
};

void registerSystemCommands(ChatCommandRegistry& reg) {
    reg.registerCommand(std::make_unique<ProfileCommand>());
    reg.registerCommand(std::make_unique<ClimbCommand>());
    reg.registerCommand(std::make_unique<GpsCommand>());
    reg.registerCommand(std::make_unique<CamCommand>());
    reg.registerCommand(std::make_unique<DaytimeCommand>());
    reg.registerCommand(std::make_unique<MemCommand>());
    reg.registerCommand(std::make_unique<RunCommand>());
    reg.registerCommand(std::make_unique<DumpCommand>());
    reg.registerCommand(std::make_unique<ReloadCommand>());
    reg.registerCommand(std::make_unique<FrameXmlCheckCommand>());
    reg.registerCommand(std::make_unique<StopMacroCommand>());
    reg.registerCommand(std::make_unique<ClearCommand>());
    reg.registerCommand(std::make_unique<DifficultyCommand>());
    reg.registerCommand(std::make_unique<CVarListCommand>());
    reg.registerCommand(std::make_unique<CVarResetCommand>());
    reg.registerCommand(std::make_unique<CVarDefaultCommand>());
    reg.registerCommand(std::make_unique<SetCVarConsoleCommand>());
    reg.registerCommand(std::make_unique<ChangePlayerDifficultyCommand>());
    reg.registerCommand(std::make_unique<PitchLimitCommand>());
    reg.registerCommand(std::make_unique<VersionCommand>());
    reg.registerCommand(std::make_unique<InertConsoleCommand>());
}

} // namespace ui
} // namespace wowee
