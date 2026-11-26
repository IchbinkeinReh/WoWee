// GM helpers: /maxout, and the dot-command table for tab-completion.
// (/gmhelp and /gmcommands went with T369: 12340 does not know them, and the
// server's own .help lists its commands.)
// Also provides the gm_commands::getCompletions() function used by tab-completion.
// The actual GM commands (.gm, .tele, etc.) are sent to the server as SAY messages;
// the server (AzerothCore) does the real work.  This file just adds discoverability.
#include "ui/chat/i_chat_command.hpp"
#include "ui/chat/chat_command_registry.hpp"
#include "ui/chat/gm_command_data.hpp"
#include "ui/chat/chat_utils.hpp"
#include "ui/bis_gear_data.hpp"
#include "game/game_handler.hpp"
#include "game/game_utils.hpp"   // isActiveExpansion
#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace wowee { namespace ui {

// ---------------------------------------------------------------------------
// gm_commands namespace - GM command lookup helpers used by tab-completion
// (gm_command_data.hpp).
// ---------------------------------------------------------------------------
namespace gm_commands {

std::vector<std::string> getCompletions(const std::string& prefix) {
    // The logic lives with the table, in gm_command_data.hpp, so a test can
    // reach it without linking this file and the game handler behind it.
    return gmCompletionsFor(prefix);
}

const GmCommandEntry* find(const std::string& name) {
    for (const auto& cmd : kGmCommands) {
        if (cmd.name == name) return &cmd;
    }
    return nullptr;
}

} // namespace gm_commands

// ---------------------------------------------------------------------------
// /maxout [parts] - level, spells, talents, skills, gold and gear in one go.
//
// What the GM command window did. That window
// was opened from a "GM" button on this client's micro menu and from nowhere
// else, so handing the micro menu over took it away - it had been drawing
// nothing behind a flag no one could set. Here instead of there because this
// is where every other command FrameXML cannot reach already lives, and the
// SlashCmdList bridge carries it into the interface's chat for free.
// ---------------------------------------------------------------------------
class MaxOutCommand : public IChatCommand {
public:
    ChatCommandResult execute(ChatCommandContext& ctx) override {
        // Which parts, as words. The window had a checkbox each and defaulted
        // gold off, which is the one that cannot be undone by re-running it.
        std::string args = ctx.args;
        for (char& c : args) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        const auto asked = [&args](const char* what) {
            return args.find(what) != std::string::npos;
        };
        const bool all      = args.find_first_not_of(" \t") == std::string::npos;
        const bool doLevel  = all || asked("level");
        const bool doSpells = all || asked("spells");
        const bool doTalents= all || asked("talents");
        const bool doSkills = all || asked("skills");
        const bool doGear   = all || asked("gear");
        const bool doGold   = asked("gold");   // never by default

        const char* exp = game::isActiveExpansion("wotlk") ? "wotlk"
                        : game::isActiveExpansion("tbc")   ? "tbc"
                                                           : "classic";
        const int maxLevel = game::isActiveExpansion("wotlk") ? 80
                           : game::isActiveExpansion("tbc")   ? 70 : 60;

        // Order matters: level first, because some spells and talents need it.
        std::vector<std::string> cmds;
        if (doLevel)   cmds.push_back(".character level " + std::to_string(maxLevel));
        if (doSpells) {
            cmds.emplace_back(".learn all my class");
            cmds.emplace_back(".learn all my spells");
        }
        if (doTalents) cmds.emplace_back(".learn all my talents");
        if (doSkills)  cmds.emplace_back(".maxskill");
        if (doGold)    cmds.emplace_back(".modify money 10000000");   // 1000g
        if (doGear) {
            for (uint32_t id : getMaxOutGear(exp, ctx.gameHandler.getPlayerClass()))
                cmds.push_back(".additem " + std::to_string(id));
        }

        if (cmds.empty()) {
            ctx.gameHandler.addLocalChatMessage(chat_utils::makeSystemMessage(
                "Max Out: nichts verlangt. /maxout für alles außer Gold, "
                "oder Teile nennen: level spells talents skills gear gold."));
            return {};
        }

        const size_t count = cmds.size();
        // One per tick, or the server's flood protection drops most of them
        // and the run half happens with nothing said about it.
        ctx.gameHandler.queuePacedChat(std::move(cmds));
        ctx.gameHandler.addLocalChatMessage(chat_utils::makeSystemMessage(
            "Max Out: " + std::to_string(count) + " Befehle eingereiht (" +
            std::string(exp) + "). Der Server entscheidet, ob Ihr sie ausführen dürft."));
        return {};
    }
    [[nodiscard]] std::vector<std::string> aliases() const override { return {"maxout"}; }
    [[nodiscard]] std::string helpText() const override {
        return "Max out this character: /maxout [level spells talents skills gear gold]";
    }
};

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------
void registerGmCommands(ChatCommandRegistry& reg) {
    reg.registerCommand(std::make_unique<MaxOutCommand>());
}

} // namespace ui
} // namespace wowee
