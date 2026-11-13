#include "ui/stream_progress.hpp"

#include "core/application.hpp"
#include "pipeline/data_stream.hpp"
#include "game/expansion_profile.hpp"
#include "ui/settings_i18n.hpp"

#include <imgui.h>

#include <chrono>
#include <cstdio>

namespace wowee::ui {

namespace {

/// "123.4" or "123,4" - German writes the decimal comma.
std::string oneDecimal(double v, bool german) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f", v);
    std::string s = buf;
    if (german) {
        for (char& c : s) {
            if (c == '.') c = ',';
        }
    }
    return s;
}

bool clientSpeaksGerman() {
    auto* registry = core::Application::getInstance().getExpansionRegistry();
    return registry && registry->getActive() && settingsLocaleGerman(registry->getActive()->locale);
}

}  // namespace

std::string streamProgressText(uint64_t bytes, uint64_t files, double rateBytesPerSecond, bool german) {
    const double mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
    std::string s = german ? "Spieldaten vom Server: " : "Game data from the server: ";
    s += oneDecimal(mb, german) + " MB ";
    s += german ? "in " : "in ";
    s += std::to_string(files);
    s += german ? (files == 1 ? " Datei" : " Dateien") : (files == 1 ? " file" : " files");
    if (rateBytesPerSecond >= 0.0) {
        s += " (" + oneDecimal(rateBytesPerSecond / (1024.0 * 1024.0), german) + " MB/s)";
    }
    return s;
}

void drawStreamProgress(ImDrawList* drawList, float width, float height) {
    auto& stream = pipeline::DataStream::instance();
    if (!drawList || !stream.active()) return;
    const auto t = stream.totals();
    // Only while data arrives: a warm cache shows nothing at all.
    constexpr double kShowFor = 4.0;
    if (t.fetchedBytes == 0 || t.secondsSinceFetch < 0.0 || t.secondsSinceFetch > kShowFor) return;

    // The rate over the last two seconds or so, from samples a second apart.
    static auto sampleAt = std::chrono::steady_clock::now();
    static uint64_t sampleBytes = t.fetchedBytes;
    static double rate = -1.0;
    const auto now = std::chrono::steady_clock::now();
    const double since = std::chrono::duration<double>(now - sampleAt).count();
    if (since >= 1.0) {
        const double current = static_cast<double>(t.fetchedBytes - sampleBytes) / since;
        rate = rate < 0.0 ? current : 0.5 * rate + 0.5 * current;
        sampleAt = now;
        sampleBytes = t.fetchedBytes;
    }

    const std::string text = streamProgressText(t.fetchedBytes, t.filesFetched, rate, clientSpeaksGerman());
    ImFont* font = ImGui::GetFont();
    const float size = ImGui::GetFontSize();
    const ImVec2 extent = font->CalcTextSizeA(size, width, 0.0f, text.c_str());
    const ImVec2 at((width - extent.x) * 0.5f, height - extent.y - size * 0.6f);
    // A dark band behind it: the loading pictures are bright in places.
    drawList->AddRectFilled(ImVec2(at.x - size * 0.6f, at.y - size * 0.25f),
                            ImVec2(at.x + extent.x + size * 0.6f, at.y + extent.y + size * 0.25f),
                            IM_COL32(0, 0, 0, 160), size * 0.3f);
    drawList->AddText(font, size, at, IM_COL32(255, 210, 0, 255), text.c_str());
}

}  // namespace wowee::ui
