#include "ui/data_folder_screen.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "imgui.h"
#include "core/application.hpp"
#include "core/config_paths.hpp"
#include "core/logger.hpp"
#include "core/window.hpp"
#include "pipeline/data_stream.hpp"
#include "ui/data_folder_picker.hpp"
#include "ui/paper_ui.hpp"

#if defined(__ANDROID__)
#include <SDL3/SDL_system.h>
#include <jni.h>
#endif

namespace wowee::ui {

namespace {

ImVec4 fromU32(ImU32 colour) { return ImGui::ColorConvertU32ToFloat4(colour); }

/// How data gets into the default folder on this platform.
const char* platformFolderHelp() {
#if defined(WOWEE_IOS)
    // UIFileSharingEnabled + LSSupportsOpeningDocumentsInPlace (ios/Info.plist.in)
    // put the app's Documents folder - the default data folder - in the Files
    // app and in Finder's device view.
    return "Copy your extraction into the Wowee folder: in the Files app under On My iPhone "
           "(or iPad) > Wowee, or from a Mac in Finder's device view under Files > Wowee. "
           "Or choose a folder that already holds one.";
#else
    return "";
#endif
}

std::string trimmed(const char* text) {
    std::string s(text);
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

/// The server kept for the next start too, or forgotten (an empty address).
bool keepStream(const core::DataStreamChoice& choice) {
    if (!core::setDataStream(choice)) return false;
#if defined(__ANDROID__)
    // Where DataFolderActivity keeps it as well: WoweeActivity writes the
    // client's file from there before every start, and would put the server
    // that failed back over this one.
    auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
    if (env != nullptr && activity != nullptr) {
        jclass cls = env->GetObjectClass(activity);
        jmethodID keep = cls != nullptr
                             ? env->GetMethodID(cls, "keepStreamChoice", "(Ljava/lang/String;Ljava/lang/String;)V")
                             : nullptr;
        if (keep != nullptr) {
            jstring url = env->NewStringUTF(choice.url.c_str());
            jstring key = env->NewStringUTF(choice.key.c_str());
            env->CallVoidMethod(activity, keep, url, key);
            env->DeleteLocalRef(url);
            env->DeleteLocalRef(key);
        }
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            LOG_WARNING("Could not keep the streaming server in the app's preferences");
        }
        if (cls != nullptr) env->DeleteLocalRef(cls);
        env->DeleteLocalRef(activity);
    }
#endif
    return true;
}

}  // namespace

void DataFolderScreen::useServer(core::Application& app) {
    const std::string url = trimmed(streamUrl_);
    const std::string key = trimmed(streamKey_);
    pipeline::StreamUrl parsed;
    std::string error;
    if (url.empty()) {
        streamMessage_ = "Give the server's address.";
    } else if (!pipeline::parseStreamUrl(url, parsed, error)) {
        streamMessage_ = "That address will not do: " + error + ".";
    } else if (key.empty()) {
        streamMessage_ = "The server needs its access key.";
    } else if (!keepStream({url, key})) {
        streamMessage_ = "Could not save the server to " + core::getConfigRoot() + ".";
    } else if (app.startStreamingNow()) {
        // Straight in: the screen says what it waits for, and the login
        // screen follows - or what went wrong.
        streamHost_ = parsed.host;
        streamMessage_.clear();
    } else {
        streamMessage_ = "The game data will be streamed from " + parsed.host +
                         " when WoWee next starts. Close WoWee and open it again.";
    }
}

void DataFolderScreen::recheck() {
    folder_ = DataFolderPicker::currentFolder();
#if defined(WOWEE_IOS)
    // The default folder is the app's Documents folder whether or not startup
    // pointed WOW_DATA_PATH at it (it only does once data is there).
    if (!DataFolderPicker::hasCustomFolder() && !std::getenv("WOW_DATA_PATH")) {
        folder_ = core::userDataRoot().string();
    }
#endif
    found_ = !core::resolveDataFolderChoice(folder_).empty();
    checked_ = true;
}

void DataFolderScreen::render() {
    auto& picker = dataFolderPicker();
    picker.poll();
    // A choice made or forgotten: look at what it points to now.
    if (picker.message() != lastMessage_) {
        lastMessage_ = picker.message();
        checked_ = false;
    }
    if (!checked_) recheck();

    const PaperTheme theme;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float scale = std::clamp(std::min(viewport->WorkSize.x / 1280.0f,
                                            viewport->WorkSize.y / 760.0f), 0.62f, 2.6f);
    const float margin = 40.0f * scale;
    const float width = std::min(700.0f * scale, viewport->WorkSize.x - margin * 2.0f);

    ImGui::PushStyleColor(ImGuiCol_WindowBg, fromU32(theme.paperTop));
    ImGui::PushStyleColor(ImGuiCol_Border, fromU32(theme.paperEdge));
    ImGui::PushStyleColor(ImGuiCol_Text, fromU32(theme.ink));
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, fromU32(theme.inkSoft));
    ImGui::PushStyleColor(ImGuiCol_TitleBg, fromU32(theme.paperBottom));
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive, fromU32(theme.paperBottom));
    ImGui::PushStyleColor(ImGuiCol_Button, fromU32(theme.photoMat));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, fromU32(theme.highlighter));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, fromU32(theme.paperEdge));
    // The text fields in the card's own field colour, as on the login card.
    ImGui::PushStyleColor(ImGuiCol_FrameBg, fromU32(theme.fieldFill));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, fromU32(theme.highlighter));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, fromU32(theme.highlighter));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(22.0f * scale, 20.0f * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f * scale, 9.0f * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f * scale, 8.0f * scale));

    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + (viewport->WorkSize.x - width) * 0.5f,
                                   viewport->WorkPos.y + margin));
    ImGui::SetNextWindowSize(ImVec2(width, 0.0f));
    ImGui::Begin("Game data###datafolder", nullptr,
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::SetWindowFontScale(scale);

    auto& app = core::Application::getInstance();
    // Android chooses the folder before the client starts (DataFolderActivity),
    // and shows this screen only when the server it was given did not answer:
    // the server's part alone, then.
    if (!core::kPreLaunchDataMask) {
        const core::DataFolderView view = core::describeDataFolder(
            folder_, DataFolderPicker::hasCustomFolder(), found_, app.getDataPrompt(),
            platformFolderHelp());
        ImGui::TextWrapped("%s", view.status.c_str());
        ImGui::Spacing();
        ImGui::TextWrapped("%s", view.help.c_str());
        ImGui::Spacing();

        if (ImGui::Button("Check again")) recheck();
        ImGui::SameLine();
        ImGui::BeginDisabled(picker.isOpen());
        if (ImGui::Button(picker.isOpen() ? "Choosing..." : "Choose folder...")) {
            auto* window = app.getWindow();
            picker.open(window ? window->getSDLWindow() : nullptr);
        }
        ImGui::EndDisabled();
        if (view.offerDefault) {
            ImGui::SameLine();
            if (ImGui::Button("Use the default folder")) {
                picker.useDefault();
                checked_ = false;
            }
        }
        if (!picker.message().empty()) {
            ImGui::Spacing();
            ImGui::TextWrapped("%s", picker.message().c_str());
        }
    }

    // Or the game data streamed from a server: its address and access key.
    if (!streamLoaded_) {
        const core::DataStreamChoice kept = core::getDataStream();
        std::snprintf(streamUrl_, sizeof(streamUrl_), "%s", kept.url.c_str());
        std::snprintf(streamKey_, sizeof(streamKey_), "%s", kept.key.c_str());
        streamLoaded_ = true;
    }
    if (core::kPreLaunchDataMask) {
        ImGui::TextWrapped("The game data is streamed from a server:");
    } else {
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::TextWrapped("Or stream the game data from a server:");
    }
    const core::Application::StreamStart starting = app.getStreamStart();
    const bool busy = starting != core::Application::StreamStart::Idle;
    ImGui::BeginDisabled(busy);
    const float fieldW = ImGui::GetContentRegionAvail().x;
    ImGui::SetNextItemWidth(fieldW);
    ImGui::InputTextWithHint("##streamurl", "https://server/wowee-data/", streamUrl_, sizeof(streamUrl_));
    ImGui::SetNextItemWidth(fieldW);
    ImGui::InputTextWithHint("##streamkey", "Access key", streamKey_, sizeof(streamKey_),
                             ImGuiInputTextFlags_Password);
    // WOWEE_DIAG_STREAM_PRESS_URL and WOWEE_DIAG_STREAM_PRESS_KEY_FILE (a file
    // holding the key): the two fields filled and "Use this server" pressed
    // once, on the first frame - this screen's own start, run with nobody at
    // the keys. The key is read from a file so it is never on a command line.
    bool press = false;
    if (!diagPressDone_) {
        diagPressDone_ = true;
        const char* url = std::getenv("WOWEE_DIAG_STREAM_PRESS_URL");
        const char* keyFile = std::getenv("WOWEE_DIAG_STREAM_PRESS_KEY_FILE");
        if (url != nullptr && *url != '\0') {
            std::string key;
            if (keyFile != nullptr) {
                std::ifstream in(keyFile);
                std::getline(in, key);
            }
            std::snprintf(streamUrl_, sizeof(streamUrl_), "%s", url);
            std::snprintf(streamKey_, sizeof(streamKey_), "%s", key.c_str());
            press = true;
        }
    }
    if (ImGui::Button("Use this server") || press) useServer(app);
    if (!core::getDataStream().url.empty()) {
        ImGui::SameLine();
        if (ImGui::Button("Stop streaming")) {
            keepStream({});
            streamUrl_[0] = '\0';
            streamKey_[0] = '\0';
            streamMessage_ = core::kPreLaunchDataMask
                                 ? "Close WoWee and open it again to choose a folder."
                                 : "The game data will be read from the folder again when WoWee next starts.";
        }
    }
    ImGui::EndDisabled();
    if (busy) {
        const std::string host = streamHost_.empty() ? std::string("the server") : streamHost_;
        ImGui::TextWrapped("%s", (starting == core::Application::StreamStart::Connecting
                                      ? "Connecting to " + host + "..."
                                      : "Opening the game data from " + host + "...")
                                     .c_str());
    } else if (!app.getStreamError().empty()) {
        // What went wrong, rather than the same card again with nothing said.
        std::string host = streamHost_;
        if (host.empty()) {
            pipeline::StreamUrl kept;
            std::string ignored;
            if (pipeline::parseStreamUrl(core::getDataStream().url, kept, ignored)) host = kept.host;
        }
        const core::StreamFailureView failure = core::describeStreamFailure(host, app.getStreamError());
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, fromU32(theme.crayonRed));
        ImGui::TextUnformatted(failure.heading.c_str());
        ImGui::PopStyleColor();
        ImGui::TextWrapped("%s", failure.body.c_str());
    }
    if (!busy && !streamMessage_.empty()) ImGui::TextWrapped("%s", streamMessage_.c_str());

    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(12);
}

}  // namespace wowee::ui
