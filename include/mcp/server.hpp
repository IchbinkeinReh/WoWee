#pragma once

// The MCP server of the client (docs/MCP_SERVER.md): made by the main loop
// when the command line asked for one, and by nothing else.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <SDL3/SDL.h>

#include "mcp/http_server.hpp"
#include "mcp/main_queue.hpp"
#include "mcp/options.hpp"
#include "mcp/redact.hpp"
#include "mcp/tool_context.hpp"

namespace wowee::core { class Application; }
namespace wowee::rendering { class Renderer; }

namespace wowee::mcp {

class Server {
public:
    /// Null where it could not be started, which the log says.
    static std::unique_ptr<Server> start(core::Application& app, const Options& options);
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // ---- From the main loop, in this order in a frame. ----

    /// Before the frame's events are read: the calls that have come in, for a
    /// few milliseconds.
    void pump(float deltaTime);
    /// Each event off the queue, before anything else has it. True drops it.
    bool filterEvent(const SDL_Event& event);
    /// With the frame drawn and not yet shown to the next: a picture of it,
    /// where one was asked for.
    void afterFrame(rendering::Renderer& renderer);

private:
    Server(core::Application& app, const Options& options);
    bool listen();

    /// On a connection's thread: a tool run, its answer cleaned of secrets.
    ToolAnswer call(const std::string& name, const json& arguments);
    ToolAnswer answerOf(const Tool& tool, MainQueue::Outcome outcome, double patience);
    std::shared_ptr<Script> makeScript(const Tool& tool, const json& arguments);
    void refreshSecrets();

    // ---- What the client asks (gate.hpp). ----
    bool sendAllowed(uint16_t wireOpcode, std::span<const uint8_t> payload);
    void heard(const std::string& event, const std::vector<std::string>& args);
    static bool gateSend(uint16_t wireOpcode, std::span<const uint8_t> payload);
    static bool gateSetting(const std::string& name, const std::string& before, const std::string& after,
                            int accountData);
    static bool gateAccountData(uint32_t type, const std::string& before, const std::string& after);
    static bool gateLua();
    static bool gateQuit();
    static void gateEvent(const std::string& event, const std::vector<std::string>& args);
    static void logTap(int level, const std::string& text);

    Options options_;
    ToolContext context_;
    std::thread::id gameThread_;
    Toolbox tools_;
    MainQueue queue_;
    HttpServer http_;
    std::string token_;
    std::string streamKey_;
    std::filesystem::path tokenPath_;
    std::mutex redactMutex_;
    Redactor redactor_;
    double nextSecrets_ = 0.0;
    uint32_t sendRefusals_ = 0;
};

}  // namespace wowee::mcp
