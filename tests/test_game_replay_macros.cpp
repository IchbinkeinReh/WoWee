// Macros' own ids on the action bar (0x005a7a40), and whose they are: the
// account's in a file of the account's, the character's in its own. See
// game_replay.hpp.
#include <catch_amalgamated.hpp>

#include "mcp/gate.hpp"
#include "game_replay.hpp"
#include "core/env.hpp"
#include "game/macro_list.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

namespace fs = std::filesystem;
using replay::Session;
using wowee::game::ActionBarSlot;
using wowee::game::GameHandler;
using wowee::game::GameHandlerTestAccess;
using wowee::game::Opcode;
namespace macro_list = wowee::game::macro_list;

namespace {

struct ConfigSandbox {
    fs::path root;
    ConfigSandbox() {
        static std::atomic<int> counter{0};
        root = fs::temp_directory_path() /
               ("wowee_replay_macros_" + std::to_string(counter.fetch_add(1)) + "_" +
                std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        fs::create_directories(root);
        wowee::core::setEnvVar("WOWEE_CONFIG_ROOT", root.string().c_str());
    }
    ~ConfigSandbox() {
        wowee::core::unsetEnvVar("WOWEE_CONFIG_ROOT");
        std::error_code ec;
        fs::remove_all(root, ec);
    }
    void write(const std::string& file, const std::string& text) const {
        fs::create_directories((root / file).parent_path());
        std::ofstream(root / file, std::ios::binary) << text;
    }
    [[nodiscard]] std::string read(const std::string& file) const {
        std::ifstream in(root / file, std::ios::binary);
        std::ostringstream out;
        out << in.rdbuf();
        return out.str();
    }
};

void become(Session& s, const std::string& account, const std::string& character, uint64_t guid) {
    GameHandlerTestAccess::setCharacter(s.handler(), account, character, guid);
    s.handler().setPlayerGuid(guid);
}

// Player::SendActionButtons(0): the mode, then 144 slots.
wowee::network::Packet actionButtons(const std::map<int, uint32_t>& slots) {
    wowee::network::Packet p(wowee::game::wireOpcode(Opcode::SMSG_ACTION_BUTTONS));
    p.writeUInt8(0);
    for (int i = 0; i < 144; ++i) {
        const auto it = slots.find(i);
        p.writeUInt32(it == slots.end() ? 0 : it->second);
    }
    return p;
}

constexpr uint32_t kMine = macro_list::kCharacterBase + 1;

}  // namespace

TEST_CASE("a character's macro on the bar has the bit over the 24", "[replay][macros]") {
    ConfigSandbox config;
    Session s;
    s.setConnected(true);
    s.receive(actionButtons({{3, 0x41000001u}, {4, 0x40000001u}, {5, 0x40000026u}}));
    const auto& bar = s.handler().getActionBar();
    CHECK(bar[3].type == ActionBarSlot::MACRO);
    CHECK(bar[3].id == kMine);
    CHECK(bar[4].type == ActionBarSlot::MACRO);
    CHECK(bar[4].id == 1);
    // One of the account's at 38, which there is none of: nothing to say
    // about it until the character has one of that number.
    CHECK(bar[5].id == 38);

    s.handler().setActionBarSlot(7, ActionBarSlot::MACRO, kMine);
    s.handler().setActionBarSlot(8, ActionBarSlot::MACRO, 2);
    s.handler().setActionBarSlot(9, ActionBarSlot::SPELL, 0x1000085);
    auto sent = s.sent().of(Opcode::CMSG_SET_ACTION_BUTTON);
    REQUIRE(sent.size() == 3);
    for (auto& p : sent) p.setReadPos(1);
    CHECK(sent[0].readUInt32() == 0x41000001u);
    CHECK(sent[1].readUInt32() == 0x40000002u);
    CHECK(sent[2].readUInt32() == 0x85u);
}

// What this client once put on the bar for the character's: 37 to 54, with no
// bit of their own.
TEST_CASE("a character's macro of the old numbers is put right on the bar", "[replay][macros]") {
    ConfigSandbox config;
    config.write("characters/Anna.cfg", "character_guid=66\nmacro_38_text=/say hi\nmacro_38_name=Hi\n");
    Session s;
    s.setConnected(true);
    become(s, "ACC", "Anna", 66);
    s.handler().loadCharacterConfig();
    CHECK(s.handler().macroExists(macro_list::kCharacterBase + 38));
    CHECK_FALSE(s.handler().macroExists(38));
    s.receive(actionButtons({{5, 0x40000026u}}));
    CHECK(s.handler().getActionBar()[5].id == macro_list::kCharacterBase + 38);
    auto sent = s.sent().of(Opcode::CMSG_SET_ACTION_BUTTON);
    REQUIRE(sent.size() == 1);
    sent[0].setReadPos(1);
    CHECK(sent[0].readUInt32() == 0x41000026u);
}

TEST_CASE("the account's macros are every character's, a character's its own", "[replay][macros]") {
    ConfigSandbox config;
    Session s;
    GameHandler& gh = s.handler();
    become(s, "ACC", "Anna", 66);
    gh.loadCharacterConfig();
    gh.setMacroMeta(1, "Ours", "");
    gh.setMacroText(1, "/say ours");
    gh.setMacroMeta(kMine, "Mine", "");
    gh.setMacroText(kMine, "/say mine");
    CHECK(config.read("accounts/ACC/macros-cache.txt") ==
          "MACRO 1 \"Ours\" INV_Misc_QuestionMark\r\n/say ours\r\nEND\r\n");
    CHECK(config.read("characters/Anna.cfg").find("Mine") != std::string::npos);
    CHECK(config.read("characters/Anna.cfg").find("Ours") == std::string::npos);

    // The next character, with a copy of the account's of its own from before
    // there was the account's file: what that does not have comes over.
    config.write("characters/Bert.cfg", "character_guid=67\n"
                                        "macro_1_text=/say stale\nmacro_1_name=Ours\n"
                                        "macro_4_text=/say four\nmacro_4_name=Four\n");
    become(s, "ACC", "Bert", 67);
    gh.loadCharacterConfig();
    CHECK(gh.getMacroText(1) == "/say ours");
    CHECK(gh.getMacroText(4) == "/say four");
    CHECK_FALSE(gh.macroExists(kMine));

    // A client started later, as the first one again.
    Session later;
    become(later, "ACC", "Anna", 66);
    later.handler().loadCharacterConfig();
    CHECK(later.handler().getMacroText(1) == "/say ours");
    CHECK(later.handler().getMacroText(4) == "/say four");
    CHECK(later.handler().getMacroText(kMine) == "/say mine");

    // Another account has none of them.
    become(later, "OTHER", "Cora", 68);
    later.handler().loadCharacterConfig();
    CHECK(later.handler().getMacroIds().empty());
    later.handler().saveCharacterConfig();
    CHECK_FALSE(fs::exists(config.root / "accounts/OTHER/macros-cache.txt"));
    CHECK(config.read("accounts/ACC/macros-cache.txt").find("Ours") != std::string::npos);
}

TEST_CASE("nothing is written over a character's file before it is read", "[replay][macros]") {
    ConfigSandbox config;
    const std::string kept =
        "character_guid=66\nmacro_ids=12340\nmacro_16777217_text=/say mine\nmacro_16777217_name=Mine\n";
    config.write("characters/Anna.cfg", kept);
    Session s;
    become(s, "ACC", "Anna", 66);
    s.handler().setActionBarSlot(3, ActionBarSlot::SPELL, 133);
    CHECK(config.read("characters/Anna.cfg") == kept);
    s.handler().loadCharacterConfig();
    s.handler().setActionBarSlot(3, ActionBarSlot::SPELL, 133);
    CHECK(config.read("characters/Anna.cfg").find("Mine") != std::string::npos);
    CHECK(config.read("characters/Anna.cfg").find("action_bar_3_id=133") != std::string::npos);
}

// 0x00767350, 0x00518c80: the CVars of the character before are none of this one's,
// and one that has no file here takes the realm's, its time or not.
TEST_CASE("a character without a config here asks the realm for its own", "[replay][macros]") {
    ConfigSandbox config;
    Session s;
    s.setConnected(true);
    become(s, "ACC", "Anna", 66);
    config.write("account_data_times.txt", "ACC:Anna:1=500\n");
    bool kept = false;
    std::string begun;
    GameHandler::AccountDataBridge bridge;
    bridge.beginCharacterConfig = [&](const std::string& path, bool keepWithout) {
        begun = path;
        CHECK_FALSE(keepWithout);
        return kept;
    };
    s.handler().setAccountDataBridge(std::move(bridge));
    const auto times = [&] {
        wowee::network::Packet p(wowee::game::wireOpcode(Opcode::SMSG_ACCOUNT_DATA_TIMES));
        p.writeUInt32(1000);
        p.writeUInt8(1);
        p.writeUInt32(0x02);
        p.writeUInt32(500);
        s.receive(p);
        return s.sent().of(Opcode::CMSG_REQUEST_ACCOUNT_DATA).size();
    };
    kept = GENERATE(false, true);
    CHECK(times() == (kept ? 0u : 1u));
    CHECK(begun.find("characters/Anna.config-cache.wtf") != std::string::npos);
    CHECK((config.read("account_data_times.txt").find("ACC:Anna:1=0") != std::string::npos) == !kept);
    s.handler().setAccountDataBridge({});
}

// The MCP server's: mcp/settings_ledger.hpp. What is not to go up does not, and the next start takes
// the realm's again.
TEST_CASE("account data that is held back is the realm's at the next start", "[replay][macros][mcp]") {
    ConfigSandbox config;
    static bool allowed;
    static std::vector<uint32_t> asked;
    allowed = false;
    asked.clear();
    struct Gate {
        Gate() {
            wowee::mcp::gate::allowsAccountData = +[](uint32_t type, const std::string&, const std::string&) {
                asked.push_back(type);
                return allowed || type > 3;
            };
        }
        ~Gate() { wowee::mcp::gate::allowsAccountData = nullptr; }
    } gate;
    config.write("account_data_times.txt", "ACC:0=500\n");
    std::string cvars = "SET a \"1\"\n";
    const auto with = [&](Session& s) {
        s.setConnected(true);
        become(s, "ACC", "Anna", 66);
        GameHandler::AccountDataBridge bridge;
        bridge.config = [&](bool perCharacter) { return perCharacter ? std::string() : cvars; };
        s.handler().setAccountDataBridge(std::move(bridge));
    };
    const auto times = [](Session& s) {
        wowee::network::Packet p(wowee::game::wireOpcode(Opcode::SMSG_ACCOUNT_DATA_TIMES));
        p.writeUInt32(1000);
        p.writeUInt8(1);
        p.writeUInt32(0x01);
        p.writeUInt32(500);
        s.receive(p);
        return s.sent().of(Opcode::CMSG_REQUEST_ACCOUNT_DATA).size();
    };
    const auto upsOf = [](Session& s) { return s.sent().of(Opcode::CMSG_UPDATE_ACCOUNT_DATA).size(); };
    {
        Session s;
        with(s);
        CHECK(times(s) == 0);
        s.handler().tickAccountData(2.0f);
        CHECK(asked.empty());
        cvars = "SET a \"0\"\n";
        s.handler().tickAccountData(2.0f);
        CHECK(asked == std::vector<uint32_t>{0});
        CHECK(upsOf(s) == 0);
        // Not again for the same, and as long as the client goes it stands as it is.
        s.handler().tickAccountData(2.0f);
        CHECK(asked.size() == 1);
        CHECK(times(s) == 0);
        CHECK(config.read("account_data_times.txt").find("ACC:0=0") != std::string::npos);
        s.handler().setAccountDataBridge({});
    }
    {
        Session next;
        with(next);
        CHECK(times(next) == 1);
        next.handler().setAccountDataBridge({});
    }
    // Allowed, it goes up and is the newest.
    allowed = true;
    config.write("account_data_times.txt", "ACC:0=500\n");
    Session s;
    with(s);
    CHECK(times(s) == 0);
    cvars = "SET a \"2\"\n";
    s.handler().tickAccountData(2.0f);
    CHECK(upsOf(s) == 1);
    CHECK(config.read("account_data_times.txt").find("ACC:0=1000") != std::string::npos);
    s.handler().setAccountDataBridge({});
}
