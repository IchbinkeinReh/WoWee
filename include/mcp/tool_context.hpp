#pragma once

// What the tools of the MCP server have to hand.

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "mcp/input_driver.hpp"
#include "mcp/json_rpc.hpp"
#include "mcp/options.hpp"
#include "mcp/origin.hpp"
#include "mcp/ring_log.hpp"
#include "mcp/script.hpp"
#include "mcp/settings_ledger.hpp"

namespace wowee::core { class Application; }

namespace wowee::mcp {

using json = nlohmann::json;

struct LogLine {
    int level = 0;  ///< core::LogLevel
    std::string text;
    double at = 0.0;
};

struct EventLine {
    std::string name;
    std::vector<std::string> args;
    double at = 0.0;
};

/// A packet that was not let out, or a thing not done (send_guard.hpp).
struct RefusedLine {
    std::string what;
    std::string why;
    double at = 0.0;
};

/// A frame's picture, asked for by a tool and taken when the next frame has
/// been drawn (Server::afterFrame).
struct FrameGrab {
    bool wanted = false;
    bool ready = false;
    bool failed = false;
    std::vector<uint8_t> bgra;
    uint32_t width = 0;
    uint32_t height = 0;
};

struct ToolContext {
    ToolContext(core::Application& application, RingLog<LogLine>& logLines) : app(application), log(logLines) {}

    core::Application& app;
    Options options;
    InputDriver input;
    Origin origin;
    /// Written from any thread, and kept past the server's end for a line
    /// that is being logged as it ends.
    RingLog<LogLine>& log;
    RingLog<EventLine> events{4000};
    RingLog<EventLine> chat{1000};
    RingLog<EventLine> combat{2000};
    RingLog<RefusedLine> refusals{200};
    /// The game's thread's only.
    SettingsLedger settings;
    /// The last CMSG_SET_SELECTION let out (GuardContext::selectedOnRealm).
    std::atomic<uint64_t> selectedOnRealm{0};
    FrameGrab grab;
    /// Seconds since the server started, as the lines above have it.
    double now = 0.0;
    double frameSeconds = 0.0;  ///< a running mean
    uint64_t frame = 0;
    /// Set by the quit tool while it ends the client.
    bool quitting = false;
    /// While the world loads behind its screen and the game's loop stands
    /// still: only the calls that may answer then are run (Tool::duringLoad),
    /// with the screen's status line and progress.
    bool loading = false;
    std::string loadingStatus;
    float loadingProgress = 0.0f;
};

struct Tool {
    /// Where the client has to be for it.
    enum class Where { Anywhere, World };

    ToolInfo info;
    Where where = Where::World;
    /// Presses something: one at a time, and counted as the server's doing.
    bool hands = false;
    /// Answers behind a loading screen too, while the world loads (looks
    /// only; ToolContext::loading says it is so).
    bool duringLoad = false;
    /// How long a call may take, in seconds.
    double patience = 15.0;
    /// The same, where the arguments say.
    std::function<double(const json& args)> patienceOf;
    /// Puts the call's steps into the script.
    std::function<void(ToolContext&, const json& args, Script&)> build;
};

using Toolbox = std::vector<Tool>;

/// A tool that only looks: one step.
Tool lookTool(std::string name, std::string description, json schema,
              std::function<json(ToolContext&, const json&)> look, Tool::Where where = Tool::Where::World);
/// A tool that presses something.
Tool handsTool(std::string name, std::string description, json schema,
               std::function<void(ToolContext&, const json&, Script&)> build, double patience = 30.0,
               Tool::Where where = Tool::Where::World);

/// What went wrong in a looking tool, told to the caller as it is: a sentence,
/// or the parts of it.
class ToolError : public std::exception {
public:
    explicit ToolError(std::string why) : why_(std::move(why)), value_(why_) {}
    explicit ToolError(const char* why) : ToolError(std::string(why)) {}
    explicit ToolError(json value) : why_(value.dump(-1, ' ', false, json::error_handler_t::replace)),
                                     value_(std::move(value)) {}
    [[nodiscard]] const char* what() const noexcept override { return why_.c_str(); }
    [[nodiscard]] const json& value() const { return value_; }

private:
    std::string why_;
    json value_;
};

void addStateTools(Toolbox& box);
void addEntityTools(Toolbox& box);
void addTerrainTools(Toolbox& box);
void addUiTools(Toolbox& box);
void addCharacterTools(Toolbox& box);
void addLogTools(Toolbox& box);
void addShotTools(Toolbox& box);
void addMoveTools(Toolbox& box);
void addPointTools(Toolbox& box);
void addKeyTools(Toolbox& box);
void addWaitTools(Toolbox& box);
void addActionsTool(Toolbox& box);
void addClientTools(Toolbox& box);
void addSoundTools(Toolbox& box);

}  // namespace wowee::mcp
