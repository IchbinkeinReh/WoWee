#include "mcp/server.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <ctime>
#include <utility>

#include "addons/lua_api_registrations.hpp"
#include "auth/auth_handler.hpp"
#include "core/application.hpp"
#include "core/config_paths.hpp"
#include "core/local_time.hpp"
#include "core/logger.hpp"
#include "core/version.hpp"
#include "core/window.hpp"
#include "game/entity.hpp"
#include "game/game_handler.hpp"
#include "game/game_utils.hpp"
#include "game/opcode_table.hpp"
#include "mcp/event_args.hpp"
#include "mcp/game_view.hpp"
#include "mcp/gate.hpp"
#include "mcp/owner_guid.hpp"
#include "mcp/send_guard.hpp"
#include "mcp/token_file.hpp"
#include "rendering/renderer.hpp"

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace wowee::mcp {

namespace {

/// The one there is, for the questions of gate.hpp.
Server* gServer = nullptr;

/// The log's lines, and when they came: any thread may be writing one while
/// the server goes.
RingLog<LogLine>& logLines() {
    static RingLog<LogLine> lines{4000};
    return lines;
}
std::atomic<double> gNow{0.0};

/// What the game's thread gives the calls of a frame.
constexpr std::chrono::microseconds kFrameBudget{4000};

constexpr const char* kInstructions =
    "This is WoWee, a client for World of Warcraft 3.3.5a, seen and played from inside.\n"
    "Begin with `state`, then `actions` (what can be done now, with the tool and arguments for each).\n"
    "Places are .gps coordinates in yards; bearings are degrees clockwise from where the character faces. Pixels are "
    "the window's, from the top left.\n"
    "Everything is done by keys and the mouse as a player would: a tool that fails says what was in the way. Look again "
    "after acting; `log`, `events`, `chat` and `combat_log` take the `next` of their last answer as `since`.\n"
    "Nothing is done towards other players unless the server was started for it (`server_info`).";

long long processId() {
#ifdef _WIN32
    return _getpid();
#else
    return getpid();
#endif
}

/// What an event carries, cut to what is worth keeping.

}  // namespace

Server::Server(core::Application& app, const Options& options)
    : options_(options), context_(app, logLines()), gameThread_(std::this_thread::get_id()) {
    context_.options = options;
    context_.input.setOrigin(&context_.origin);
    if (auto* window = app.getWindow()) context_.input.setWindow(window->getSDLWindow());
    addStateTools(tools_);
    addEntityTools(tools_);
    addTerrainTools(tools_);
    addUiTools(tools_);
    addCharacterTools(tools_);
    addLogTools(tools_);
    addShotTools(tools_);
    addSoundTools(tools_);
    addActionsTool(tools_);
    addMoveTools(tools_);
    addPointTools(tools_);
    addKeyTools(tools_);
    addWaitTools(tools_);
    addClientTools(tools_);
    if (!options_.lua) std::erase_if(tools_, [](const Tool& t) { return t.info.name == "lua"; });
}

std::unique_ptr<Server> Server::start(core::Application& app, const Options& options) {
    if (gServer) return nullptr;
    std::unique_ptr<Server> server(new Server(app, options));
    if (!server->listen()) return nullptr;
    gServer = server.get();
    gate::allowsSend = &Server::gateSend;
    gate::allowsLua = &Server::gateLua;
    gate::allowsQuit = &Server::gateQuit;
    gate::settingChanged = &Server::gateSetting;
    gate::allowsAccountData = &Server::gateAccountData;
    gate::heardEvent = &Server::gateEvent;
    core::Logger::getInstance().setTap(&Server::logTap);
    return server;
}

bool Server::listen() {
    token_ = makeToken();
    if (token_.empty()) {
        LOG_ERROR("MCP server: no randomness for a token; not started");
        return false;
    }
    streamKey_ = core::getDataStream().key;
    refreshSecrets();

    std::string error;
    const auto answer = [this](const std::string& body) {
        RpcTarget target;
        target.serverVersion = core::kVersionString;
        target.instructions = kInstructions;
        target.tools = [this] {
            std::vector<ToolInfo> out;
            for (const auto& t : tools_) out.push_back(t.info);
            return out;
        };
        target.call = [this](const std::string& name, const json& arguments) { return call(name, arguments); };
        return answerRpc(body, target);
    };
    if (!http_.start(options_.port, token_, answer, error)) {
        LOG_ERROR("MCP server: ", error, "; not started");
        return false;
    }
    tokenPath_ = tokenFilePath(core::getConfigRoot());
    if (!writeTokenFile(tokenPath_, {.port = http_.port(),
                                     .token = token_,
                                     .pid = static_cast<unsigned long>(processId())})) {
        LOG_ERROR("MCP server: the token could not be written to ", tokenPath_.string(), "; not started");
        http_.stop();
        return false;
    }
    LOG_WARNING("MCP server: listening on http://127.0.0.1:", http_.port(), "/mcp, the token is in ",
                tokenPath_.string(), options_.lua ? ", with Lua" : "",
                options_.allow ? ", allowed towards other players: " + socialList(options_.allow) : std::string());
    return true;
}

Server::~Server() {
    core::Logger::getInstance().setTap(nullptr);
    gate::allowsSend = nullptr;
    gate::allowsLua = nullptr;
    gate::allowsQuit = nullptr;
    gate::settingChanged = nullptr;
    gate::allowsAccountData = nullptr;
    gate::heardEvent = nullptr;
    addons::putBackCVars();
    // Those who wait are told first, or the threads that wait would be waited
    // for.
    queue_.close();
    http_.stop();
    context_.input.releaseAll();
    context_.input.releasePointer();
    if (!tokenPath_.empty()) removeTokenFile(tokenPath_, token_);
    gServer = nullptr;
}

void Server::pump(float deltaTime) {
    context_.now += deltaTime;
    gNow.store(context_.now, std::memory_order_relaxed);
    ++context_.frame;
    context_.frameSeconds = context_.frameSeconds <= 0.0 ? deltaTime : context_.frameSeconds * 0.9 + deltaTime * 0.1;
    if (auto* window = context_.app.getWindow()) context_.input.setWindow(window->getSDLWindow());
    context_.input.tick();
    if (context_.now >= nextSecrets_) {
        nextSecrets_ = context_.now + 2.0;
        refreshSecrets();
    }
    queue_.pump(context_.now, kFrameBudget);
}

bool Server::filterEvent(const SDL_Event& event) {
    return context_.input.filter(event, queue_.handsBusy());
}

void Server::afterFrame(rendering::Renderer& renderer) {
    auto& grab = context_.grab;
    if (!grab.wanted) return;
    grab.wanted = false;
    grab.ready = renderer.readFrame(grab.bgra, grab.width, grab.height);
    grab.failed = !grab.ready;
}

void Server::refreshSecrets() {
    const std::lock_guard<std::mutex> lock(redactMutex_);
    redactor_.clear();
    redactor_.add(token_);
    redactor_.add(streamKey_);
    if (auto* auth = context_.app.getAuthHandler()) redactor_.add(auth->getUsername(), true);
    if (auto* gh = context_.app.getGameHandler()) redactor_.add(gh->getAccountName(), true);
}

std::shared_ptr<Script> Server::makeScript(const Tool& tool, const json& arguments) {
    auto script = std::make_shared<Script>();
    if (tool.where == Tool::Where::World && !inWorld(context_)) {
        script->then([this](Script& s) {
            return s.fail("the character is not in the world; the client is at: " + clientState(context_));
        });
        return script;
    }
    tool.build(context_, arguments, *script);
    if (tool.hands) {
        context_.origin.beginAct();
        // What the tool itself wants done at the end comes first.
        script->onEnd = [this, own = std::move(script->onEnd), raw = script.get()] {
            if (own) own();
            if (raw->failed) context_.input.releaseAll();
            context_.origin.endAct();
        };
    }
    return script;
}

ToolAnswer Server::answerOf(const Tool& tool, MainQueue::Outcome outcome, double patience) {
    using Kind = MainQueue::Outcome::Kind;
    ToolAnswer answer;
    switch (outcome.kind) {
        case Kind::Finished: {
            auto& script = *outcome.script;
            if (script.finish && !script.failed) script.finish(script);
            answer.value = std::move(script.result);
            answer.failed = script.failed;
            answer.imageBase64 = std::move(script.imageBase64);
            answer.imageMime = std::move(script.imageMime);
            break;
        }
        case Kind::TimedOut: {
            const double quiet =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - queue_.lastPump()).count();
            answer.failed = true;
            answer.value = tool.info.name + " did not end within " + std::to_string(static_cast<int>(patience)) + " s";
            if (quiet > 1.5) {
                answer.value = answer.value.get<std::string>() + "; the game has not drawn a frame for " +
                               std::to_string(static_cast<int>(quiet)) + " s (loading?)";
            }
            break;
        }
        case Kind::Busy:
            answer.failed = true;
            answer.value = "too many calls are waiting";
            break;
        case Kind::Closed:
            answer.failed = true;
            answer.value = "the client is ending";
            break;
    }
    return answer;
}

ToolAnswer Server::call(const std::string& name, const json& arguments) {
    const auto it =
        std::find_if(tools_.begin(), tools_.end(), [&](const Tool& t) { return t.info.name == name; });
    if (it == tools_.end()) return {.value = "no such tool", .failed = true, .imageBase64 = {}, .imageMime = {}};
    const Tool& tool = *it;
    const double patience = tool.patienceOf ? tool.patienceOf(arguments) : tool.patience;
    auto outcome = queue_.submit([this, &tool, arguments] { return makeScript(tool, arguments); }, tool.hands,
                                 std::chrono::milliseconds(static_cast<long long>(patience * 1000.0)));
    ToolAnswer answer = answerOf(tool, std::move(outcome), patience);
    const std::lock_guard<std::mutex> lock(redactMutex_);
    redactor_.apply(answer.value);
    return answer;
}

bool Server::sendAllowed(uint16_t wireOpcode, std::span<const uint8_t> payload) {
    const auto* table = game::getActiveOpcodeTable();
    const auto logical = table ? table->fromWire(wireOpcode) : std::nullopt;
    const char* name = logical ? game::OpcodeTable::logicalToName(*logical) : nullptr;
    // Whom the realm has selected, whoever's doing it was; the game's thread
    // is the only one that selects.
    const auto selection = name ? selectionOf(name, payload) : std::nullopt;
    const auto goes = [&] {
        if (selection) context_.selectedOnRealm = *selection;
        return true;
    };
    if (!context_.origin.isServer()) return goes();
    if (!name) return true;
    // What is sent from another thread is held to the rules without asking
    // the game, which is this one's: no name is its own there.
    auto* gh = std::this_thread::get_id() == gameThread_ ? context_.app.getGameHandler() : nullptr;
    GuardContext guard;
    guard.allow = options_.allow;
    guard.wotlkWire = game::isActiveExpansion("wotlk");
    guard.selectedOnRealm = context_.selectedOnRealm;
    if (gh) {
        guard.ownGuid = gh->getPlayerGuid();
        guard.targetGuid = gh->getTargetGuid();
        if (const auto seat = gh->vehicleRoster().passenger(guard.ownGuid)) guard.vehicleGuid = seat->vehicleGuid;
        if (const auto* c = gh->getActiveCharacter()) guard.ownName = c->name;
        guard.belongsToOtherPlayer = [gh](uint64_t guid) {
            const auto e = gh->getEntityManager().getEntity(guid);
            if (!e) return false;
            const uint64_t owner = ownerGuid(*e);
            return owner != 0 && owner != gh->getPlayerGuid() && isPlayerGuid(owner);
        };
    }
    const Verdict verdict = judgeSend(name, payload, guard);
    if (verdict.allowed()) return goes();
    context_.refusals.push({.what = name, .why = verdict.why, .at = context_.now});
    // A line each would be a line a frame for what is sent every frame.
    if (++sendRefusals_ <= 20) LOG_WARNING("MCP server: not sent: ", name, " - ", verdict.why);
    return false;
}

void Server::heard(const std::string& event, const std::vector<std::string>& args) {
    if (event == "COMBAT_LOG_EVENT") return;  // the filtered copy of the next
    EventLine line{.name = event, .args = shownArgs(args), .at = context_.now};
    if (event == "COMBAT_LOG_EVENT_UNFILTERED") {
        context_.combat.push(std::move(line));
    } else if (event.rfind("CHAT_MSG_", 0) == 0) {
        if (event != "CHAT_MSG_ADDON") context_.chat.push(std::move(line));
    } else {
        context_.events.push(std::move(line));
    }
}

bool Server::gateSend(uint16_t wireOpcode, std::span<const uint8_t> payload) {
    return !gServer || gServer->sendAllowed(wireOpcode, payload);
}

namespace {

std::string clockNow() {
    const std::tm local = core::localTime(std::time(nullptr));
    char text[16] = {};
    std::strftime(text, sizeof(text), "%H:%M:%S", &local);
    return text;
}

}  // namespace

bool Server::gateSetting(const std::string& name, const std::string& before, const std::string& after,
                         int accountData) {
    if (!gServer || std::this_thread::get_id() != gServer->gameThread_ || !gServer->context_.origin.isServer())
        return false;
    gServer->context_.settings.changed(name, before, after, clockNow());
    const bool allowed = (gServer->options_.allow & static_cast<uint32_t>(Social::Settings)) != 0;
    // From here on: a key pressed before it is looked at whether to send it up makes it no person's.
    if (accountData >= 0) gServer->context_.settings.mayGoUp(static_cast<uint32_t>(accountData), true, allowed);
    return !allowed;
}

bool Server::gateAccountData(uint32_t type, const std::string& before, const std::string& after) {
    if (!gServer) return true;
    auto& context = gServer->context_;
    const bool byServer = context.origin.isServer();
    const bool allowed = (gServer->options_.allow & static_cast<uint32_t>(Social::Settings)) != 0;
    if (byServer && before != after) {
        if (type == 2 || type == 3) {
            for (const auto& b : bindingChanges(before, after))
                context.settings.changed("key " + b.key, b.before, b.after, clockNow());
        } else if (type == 7) {
            context.settings.changed("chat windows", "", "changed", clockNow());
        }
    }
    // With nothing changed it is not the server's doing, and is not held for good; but it does
    // not go now either, as the packet would not (send_guard.hpp).
    if (before == after) return allowed || !isSettingsData(type) || (!byServer && !context.settings.held(type));
    if (context.settings.mayGoUp(type, byServer, allowed)) return true;
    if (context.settings.firstToldOf(type)) {
        context.refusals.push({.what = "CMSG_UPDATE_ACCOUNT_DATA",
                               .why = std::string(settingsDataName(type)) +
                                      " changed here are not kept on the realm (--mcp-allow=settings)",
                               .at = context.now});
        LOG_WARNING("MCP server: ", settingsDataName(type), " are not sent up any more: changed by its doing");
    }
    return false;
}

bool Server::gateLua() {
    if (!gServer || gServer->options_.lua || !gServer->context_.origin.isServer()) return true;
    gServer->context_.refusals.push(
        {.what = "Lua", .why = "the server was not started with --mcp-lua", .at = gServer->context_.now});
    return false;
}

bool Server::gateQuit() {
    if (!gServer || gServer->context_.quitting || !gServer->context_.origin.isServer()) return true;
    gServer->context_.refusals.push(
        {.what = "quit", .why = "the quit tool ends the client", .at = gServer->context_.now});
    LOG_WARNING("MCP server: quitting from inside the game refused; the quit tool does that");
    return false;
}

void Server::gateEvent(const std::string& event, const std::vector<std::string>& args) {
    if (gServer) gServer->heard(event, args);
}

void Server::logTap(int level, const std::string& text) {
    // From any thread, under the logger's lock: nothing here may log.
    logLines().push({.level = level, .text = text, .at = gNow.load(std::memory_order_relaxed)});
}

}  // namespace wowee::mcp
