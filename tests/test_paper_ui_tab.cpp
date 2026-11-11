// Tab moves the keyboard one field a press (the login screen: account name,
// password, code). The field it hands the keyboard to is drawn later in the
// same frame and saw the same press; it passed the focus on again, so a press
// skipped two or three fields.

#include <catch_amalgamated.hpp>

#include "imgui.h"
#include "ui/paper_ui.hpp"
#include "ui/text_edit.hpp"

#include <array>

using wowee::ui::PaperUI;
using wowee::ui::TextEdit;

namespace {

struct Screen {
    PaperUI ui;
    std::array<TextEdit, 3> edits;
    std::array<bool, 3> focused{};

    void frame() {
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(800, 600);
        io.DeltaTime = 1.0f / 60.0f;
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(800, 600));
        ImGui::Begin("screen");
        ui.begin(io.DeltaTime, 1.0f);
        const char* ids[3] = {"account", "password", "pin"};
        for (int i = 0; i < 3; ++i) {
            const float y = 20.0f + 60.0f * static_cast<float>(i);
            const auto r = ui.field(ids[i], ImVec2(20, y), ImVec2(300, y + 40), edits[static_cast<size_t>(i)],
                                    PaperUI::FieldOpts{});
            focused[static_cast<size_t>(i)] = r.focused;
        }
        ui.end();
        ImGui::End();
        ImGui::EndFrame();
    }
    int which() const {
        for (int i = 0; i < 3; ++i)
            if (focused[static_cast<size_t>(i)]) return i;
        return -1;
    }
};

void pressTab(Screen& s, bool shift) {
    ImGuiIO& io = ImGui::GetIO();
    io.AddKeyEvent(ImGuiMod_Shift, shift);
    io.AddKeyEvent(ImGuiKey_Tab, true);
    s.frame();
    io.AddKeyEvent(ImGuiKey_Tab, false);
    io.AddKeyEvent(ImGuiMod_Shift, false);
    s.frame();
    s.frame();
}

}  // namespace

TEST_CASE("Tab moves the keyboard one field a press", "[paper_ui]") {
    ImGui::CreateContext();
    ImGui::GetIO().Fonts->AddFontDefault();
    ImGui::GetIO().Fonts->Build();
    {
        Screen s;
        s.frame();
        s.ui.focus("account");
        s.frame();
        REQUIRE(s.which() == 0);
        pressTab(s, false);
        CHECK(s.which() == 1);
        pressTab(s, false);
        CHECK(s.which() == 2);
        pressTab(s, false);  // off the last, round to the first
        CHECK(s.which() == 0);
        pressTab(s, true);   // Shift+Tab off the first, round to the last
        CHECK(s.which() == 2);
        pressTab(s, true);
        CHECK(s.which() == 1);
    }
    ImGui::DestroyContext();
}
