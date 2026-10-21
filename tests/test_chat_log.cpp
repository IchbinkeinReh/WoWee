// LoggingChat's lines as ChatFrame.cpp writes them (0x00509dd0, 0x00503660).
#include <catch_amalgamated.hpp>

#include <map>

#include "game/chat_log.hpp"

namespace cl = wowee::game::chat_log;

namespace {
// A few of GlobalStrings.lua's, %s and %d put in in order.
const cl::Format kFormat = [](const std::string& key, const std::vector<std::string>& args) {
    static const std::map<std::string, std::string> strings = {
        {"CHAT_SAY_GET", "%s says: "},          {"CHAT_CHANNEL_GET", "%s: "},
        {"CHAT_MONSTER_SAY_GET", "%s says: "},  {"CHAT_CHANNEL_LIST_GET", "|Hchannel:CHANNEL:%d|h[%s]|h "},
        {"CHAT_IGNORED", "%s is ignoring you."}, {"CHAT_YOU_JOINED_NOTICE", "Joined Channel: [%s]"},
    };
    const auto it = strings.find(key);
    if (it == strings.end()) return std::string();
    std::string out;
    size_t a = 0;
    const std::string& f = it->second;
    for (size_t i = 0; i < f.size(); ++i) {
        if (f[i] == '%' && i + 1 < f.size() && (f[i + 1] == 's' || f[i + 1] == 'd')) {
            out += a < args.size() ? args[a++] : std::string();
            ++i;
        } else {
            out += f[i];
        }
    }
    return out;
};
}  // namespace

TEST_CASE("A said line: header, language, plain text", "[chat_log]") {
    cl::Line l{.type = 1, .text = "Look: |cff1eff00|Hitem:2589:0|h[Linen Cloth]|h|r 100%", .sender = "Bob",
               .channel = {}, .channelNumber = 0, .language = "Darnassian", .target = {}};
    CHECK(cl::line(l, kFormat) == "Bob says: [Darnassian] Look: Linen Cloth 100%");
}

TEST_CASE("A channel line: numbered, the zone left off", "[chat_log]") {
    cl::Line l{.type = 0x11, .text = "wts ||pipes||", .sender = "Al", .channel = "General - Elwynn Forest",
               .channelNumber = 1, .language = {}, .target = {}};
    CHECK(cl::line(l, kFormat) == "[1. General] Al: wts |pipes|");
}

TEST_CASE("The kinds written their own way", "[chat_log]") {
    // A monster's speech keeps its %s for the speaker.
    cl::Line m{.type = 0x0c, .text = "%s laughs.", .sender = "Hogger", .channel = {}, .channelNumber = 0,
               .language = {}, .target = {}};
    CHECK(cl::line(m, kFormat) == "Hogger says: Hogger laughs.");
    // A text emote is its own words; the system's has no header.
    m.type = cl::type::kTextEmote;
    m.text = "Bob waves.";
    CHECK(cl::line(m, kFormat) == "Bob waves.");
    cl::Line s{.type = 0, .text = "Welcome", .sender = {}, .channel = {}, .channelNumber = 0, .language = {},
               .target = {}};
    CHECK(cl::line(s, kFormat) == "Welcome");
    cl::Line n{.type = cl::type::kChannelNotice, .text = "YOU_JOINED", .sender = {}, .channel = "Trade",
               .channelNumber = 2, .language = {}, .target = {}};
    CHECK(cl::line(n, kFormat) == "Joined Channel: [2. Trade]");
    cl::Line ig{.type = cl::type::kIgnored, .text = {}, .sender = "Eve", .channel = {}, .channelNumber = 0,
                .language = {}, .target = {}};
    CHECK(cl::line(ig, kFormat) == "Eve is ignoring you.");
    cl::Line list{.type = cl::type::kChannelList, .text = "Bob, Al", .sender = {}, .channel = "Trade",
                  .channelNumber = 2, .language = {}, .target = {}};
    CHECK(cl::line(list, kFormat) == "|Hchannel:CHANNEL:2|h[Trade]|h Bob, Al");
}
