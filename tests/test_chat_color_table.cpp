// The client's chat type colour table (0x00b746e8): its defaults (0x009fa558),
// ChangeChatColor, SetChatColorNameByClass and ResetChatColors (0x004feaf0,
// 0x004fedc0, 0x004fed10), and chat-cache.txt's COLORS section (0x004fd300).

#include <catch_amalgamated.hpp>

#include "addons/chat_color_table.hpp"

using wowee::addons::ChatColorTable;

namespace {

const ChatColorTable::Entry* find(const ChatColorTable& t, const std::string& name) {
    for (const auto& e : t.entries()) {
        if (e.name == name) return &e;
    }
    return nullptr;
}

}  // namespace

TEST_CASE("The defaults are the client's, with ten numbered channels", "[chat_colors]") {
    ChatColorTable t;
    REQUIRE(t.entries().size() == ChatColorTable::kFixed + ChatColorTable::kChannels);
    const auto* guild = find(t, "GUILD");
    REQUIRE(guild);
    CHECK(guild->r == 64);
    CHECK(guild->g == 255);
    CHECK(guild->b == 64);
    const auto* c7 = find(t, "CHANNEL7");
    REQUIRE(c7);
    CHECK(c7->r == 255);
    CHECK(c7->g == 192);
    CHECK(c7->b == 192);
}

TEST_CASE("A colour change reaches the named type only, case aside", "[chat_colors]") {
    ChatColorTable t;
    CHECK(t.setColor("guild", 1, 2, 3));
    CHECK(find(t, "GUILD")->b == 3);
    CHECK_FALSE(t.setColor("NO_SUCH_TYPE", 1, 2, 3));
}

TEST_CASE("Reset restores colours and keeps the class flags", "[chat_colors]") {
    ChatColorTable t;
    t.setColor("CHANNEL", 9, 9, 9);
    t.setColor("CHANNEL3", 1, 1, 1);
    t.setColor("SAY", 0, 0, 0);
    t.setNameByClass("SAY", true);
    t.resetColors();
    CHECK(find(t, "SAY")->r == 255);
    CHECK(find(t, "SAY")->nameByClass);
    // The channels take CHANNEL's colour, CHANNEL itself back at its own.
    CHECK(find(t, "CHANNEL3")->r == 255);
    CHECK(find(t, "CHANNEL3")->g == 192);
}

TEST_CASE("The COLORS section reads back what it wrote", "[chat_colors]") {
    ChatColorTable t;
    t.setColor("WHISPER", 10, 20, 30);
    t.setNameByClass("RAID", true);
    const std::string text = "VERSION 5\n\n" + t.serialize();
    CHECK(text.find("WHISPER 10 20 30 N\n") != std::string::npos);
    CHECK(text.find("RAID 255 127 0 Y\n") != std::string::npos);
    ChatColorTable u;
    u.parse(text);
    CHECK(find(u, "WHISPER")->g == 20);
    CHECK(find(u, "RAID")->nameByClass);
    CHECK_FALSE(find(u, "SAY")->nameByClass);
}
