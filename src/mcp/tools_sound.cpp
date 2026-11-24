// What is heard, and the client's recording of itself.

#include <algorithm>
#include <cctype>
#include <string>

#include "audio/audio_engine.hpp"
#include "core/application.hpp"
#include "mcp/game_view.hpp"
#include "mcp/hands.hpp"
#include "mcp/schema.hpp"
#include "mcp/tool_context.hpp"
#include "rendering/renderer.hpp"
#include "ui/game_screen.hpp"
#include "ui/ui_manager.hpp"

namespace wowee::mcp {

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

json soundJson(ToolContext& context, const audio::AudioEngine::Shown& sound) {
    json out = {{"file", sound.name.empty() ? std::string("?") : sound.name},
                {"kind", sound.kind},
                {"volume", rounded(sound.volume, 2)}};
    if (sound.spatial) {
        out["pos"] = vec3Json(sound.position);
        out["distance"] = rounded(sound.distance, 1);
        out["bearing"] = rounded(bearingTo(context, sound.position), 0);
    }
    if (sound.loops) out["loops"] = true;
    out["heard"] = sound.heard;
    if (*sound.why) out["why"] = sound.why;
    return out;
}

json sounds(ToolContext& context, const json& args) {
    const auto& engine = audio::AudioEngine::instance();
    if (!engine.isInitialized()) throw ToolError("there is no sound");
    const std::string grep = lower(args.value("grep", std::string()));
    const auto wanted = [&](const audio::AudioEngine::Shown& s) {
        return grep.empty() || lower(s.name).find(grep) != std::string::npos;
    };
    json playing = json::array();
    for (const auto& s : engine.playing()) {
        if (!wanted(s)) continue;
        json j = soundJson(context, s);
        j["at_s"] = rounded(s.seconds, 2);
        if (s.length > 0.0) j["length_s"] = rounded(s.length, 2);
        playing.push_back(std::move(j));
    }
    json out = {{"playing", std::move(playing)},
                {"master_volume", rounded(engine.getMasterVolume(), 2)},
                {"effects_volume", rounded(engine.getEffectsVolume(), 2)},
                {"suspended", engine.isSuspended()},
                {"held_by_loading_screen", engine.isWorldSoundHeld()}};
    if (const double seconds = args.value("begun_within", 0.0); seconds > 0.0) {
        json begun = json::array();
        for (const auto& s : engine.recent(seconds)) {
            if (!wanted(s)) continue;
            json j = soundJson(context, s);
            j["ago_s"] = rounded(s.ago, 2);
            begun.push_back(std::move(j));
        }
        out["begun"] = std::move(begun);
    }
    return out;
}

void record(ToolContext& context, const json& args, Script& script) {
    const Aftermath after(context);
    const std::string action = args["action"].get<std::string>();
    script.then([&context, action](Script& s) {
        auto* renderer = context.app.getRenderer();
        auto* ui = context.app.getUIManager();
        if (!renderer || !ui) return s.fail("there is nothing to record");
        const bool running = renderer->isRecording();
        if (action == "start" && running) return s.fail("a recording is being made: " + ui->getGameScreen().recordingPath());
        if (action == "stop" && !running) return s.fail("no recording is being made");
        if (action == "start") ui->getGameScreen().startRecording();
        if (action == "stop") ui->getGameScreen().stopRecording();
        return Script::Step::Next;
    });
    script.then(Script::frames(1));
    script.then(finishWith(context, after, [&context, action] {
        auto* renderer = context.app.getRenderer();
        auto* ui = context.app.getUIManager();
        json out = {{"recording", renderer && renderer->isRecording()}};
        if (!ui) return out;
        const auto& screen = ui->getGameScreen();
        if (!screen.recordingPath().empty()) out["file"] = screen.recordingPath();
        if (action == "stop") {
            const auto& stats = screen.recordingStats();
            out["seconds"] = rounded(stats.seconds, 2);
            out["frames"] = stats.framesWritten;
            out["frames_dropped"] = stats.framesDropped;
            out["sound"] = stats.hasAudio;
            out["encoder"] = stats.encoder;
        }
        return out;
    }));
}

}  // namespace

void addSoundTools(Toolbox& box) {
    using namespace schema;
    box.push_back(lookTool(
        "sounds",
        "The sounds going on: file, effect/stream/music, volume, place, distance and bearing of a 3D one, whether it "
        "is heard and why not. With begun_within, the ones begun in the last seconds too, ended or not.",
        object({optional("begun_within", number("seconds, 30 at most", 0, 30)),
                optional("grep", text("only files with this in their name", 64))}),
        sounds));
    box.push_back(handsTool("record",
                            "The client's own recording of what it shows and plays (as /record): start, stop, or "
                            "status. The answer has the file, which is the recorder's own name in its own folder.",
                            object({required("action", choice("", {"start", "stop", "status"}))}), record, 60.0));
}

}  // namespace wowee::mcp
