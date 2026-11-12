#include "ui/data_folder_screen.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

#include "imgui.h"
#include "core/application.hpp"
#include "core/config_paths.hpp"
#include "core/window.hpp"
#include "pipeline/data_stream.hpp"
#include "ui/data_folder_picker.hpp"
#include "ui/paper_ui.hpp"

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

}  // namespace

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

    const core::DataFolderView view = core::describeDataFolder(
        folder_, DataFolderPicker::hasCustomFolder(), found_,
        core::Application::getInstance().getDataPrompt(), platformFolderHelp());
    ImGui::TextWrapped("%s", view.status.c_str());
    ImGui::Spacing();
    ImGui::TextWrapped("%s", view.help.c_str());
    ImGui::Spacing();

    if (ImGui::Button("Check again")) recheck();
    ImGui::SameLine();
    ImGui::BeginDisabled(picker.isOpen());
    if (ImGui::Button(picker.isOpen() ? "Choosing..." : "Choose folder...")) {
        auto* window = core::Application::getInstance().getWindow();
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

    // Or the game data streamed from a server: its address and access key.
    if (!streamLoaded_) {
        const core::DataStreamChoice kept = core::getDataStream();
        std::snprintf(streamUrl_, sizeof(streamUrl_), "%s", kept.url.c_str());
        std::snprintf(streamKey_, sizeof(streamKey_), "%s", kept.key.c_str());
        streamLoaded_ = true;
    }
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextWrapped("Or stream the game data from a server:");
    const float fieldW = ImGui::GetContentRegionAvail().x;
    ImGui::SetNextItemWidth(fieldW);
    ImGui::InputTextWithHint("##streamurl", "https://server/wowee-data/", streamUrl_, sizeof(streamUrl_));
    ImGui::SetNextItemWidth(fieldW);
    ImGui::InputTextWithHint("##streamkey", "Access key", streamKey_, sizeof(streamKey_),
                             ImGuiInputTextFlags_Password);
    if (ImGui::Button("Use this server")) {
        const std::string url = streamUrl_;
        const std::string key = streamKey_;
        pipeline::StreamUrl parsed;
        std::string error;
        if (url.empty()) {
            streamMessage_ = "Give the server's address.";
        } else if (!pipeline::parseStreamUrl(url, parsed, error)) {
            streamMessage_ = "That address will not do: " + error + ".";
        } else if (key.empty()) {
            streamMessage_ = "The server needs its access key.";
        } else if (!core::setDataStream({url, key})) {
            streamMessage_ = "Could not save the server to " + core::getConfigRoot() + ".";
        } else {
            streamMessage_ = "The game data will be streamed from " + parsed.host +
                             " when WoWee next starts. Close WoWee and open it again.";
        }
    }
    if (!core::getDataStream().url.empty()) {
        ImGui::SameLine();
        if (ImGui::Button("Stop streaming")) {
            core::setDataStream({});
            streamUrl_[0] = '\0';
            streamKey_[0] = '\0';
            streamMessage_ = "The game data will be read from the folder again when WoWee next starts.";
        }
    }
    if (!streamMessage_.empty()) ImGui::TextWrapped("%s", streamMessage_.c_str());

    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(12);
}

}  // namespace wowee::ui
