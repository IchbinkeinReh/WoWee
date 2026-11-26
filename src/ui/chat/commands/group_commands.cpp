// Group commands: /readycheck, /ready, /notready, /yield, /afk, /dnd,
//                 /uninvite, /leave, /maintank, /mainassist, /clearmaintank,
//                 /clearmainassist, /raidinfo, /raidconvert, /lootmethod,
//                 /lootthreshold, /mark, /roll
// Moved from ChatPanel::sendChatMessage() if/else chain (Phase 3).
#include "game/group_defines.hpp"
#include "ui/chat/chat_utils.hpp"
#include "ui/chat/i_chat_command.hpp"
#include "ui/chat_panel.hpp"
#include "game/game_handler.hpp"
#include <algorithm>
#include <cctype>
#include <ctime>
#include <string>

namespace wowee { namespace ui {

// --- /readycheck ---
class ReadyCheckCommand : public IChatCommand {
public:
    ChatCommandResult execute(ChatCommandContext& ctx) override {
        ctx.gameHandler.initiateReadyCheck();
        return {};
    }
    [[nodiscard]] std::vector<std::string> aliases() const override { return {"readycheck"}; }
    [[nodiscard]] std::string helpText() const override { return "Initiate ready check"; }
};

// --- /ready ---
class ReadyCommand : public IChatCommand {
public:
    ChatCommandResult execute(ChatCommandContext& ctx) override {
        ctx.gameHandler.respondToReadyCheck(true);
        return {};
    }
    [[nodiscard]] std::vector<std::string> aliases() const override { return {"ready"}; }
    [[nodiscard]] std::string helpText() const override { return "Respond yes to ready check"; }
};

// --- /yield, /forfeit, /surrender ---
class YieldCommand : public IChatCommand {
public:
    ChatCommandResult execute(ChatCommandContext& ctx) override {
        ctx.gameHandler.forfeitDuel();
        return {};
    }
    [[nodiscard]] std::vector<std::string> aliases() const override { return {"yield", "forfeit", "surrender"}; }
    [[nodiscard]] std::string helpText() const override { return "Forfeit current duel"; }
};

// --- /afk, /away ---
class AfkCommand : public IChatCommand {
public:
    ChatCommandResult execute(ChatCommandContext& ctx) override {
        ctx.gameHandler.toggleAfk(ctx.args);
        return {};
    }
    [[nodiscard]] std::vector<std::string> aliases() const override { return {"afk", "away"}; }
    [[nodiscard]] std::string helpText() const override { return "Toggle AFK status"; }
};

// --- /dnd, /busy ---
class DndCommand : public IChatCommand {
public:
    ChatCommandResult execute(ChatCommandContext& ctx) override {
        ctx.gameHandler.toggleDnd(ctx.args);
        return {};
    }
    [[nodiscard]] std::vector<std::string> aliases() const override { return {"dnd", "busy"}; }
    [[nodiscard]] std::string helpText() const override { return "Toggle Do Not Disturb"; }
};

// --- /uninvite, /kick ---
class UninviteCommand : public IChatCommand {
public:
    ChatCommandResult execute(ChatCommandContext& ctx) override {
        if (!ctx.args.empty()) {
            ctx.gameHandler.uninvitePlayer(ctx.args);
        } else {
            game::MessageChatData msg;
            msg.type = game::ChatType::SYSTEM;
            msg.language = game::ChatLanguage::UNIVERSAL;
            msg.message = "Usage: /uninvite <player name>";
            ctx.gameHandler.addLocalChatMessage(msg);
        }
        return {};
    }
    [[nodiscard]] std::vector<std::string> aliases() const override { return {"uninvite", "kick"}; }
    [[nodiscard]] std::string helpText() const override { return "Remove player from group"; }
};

// --- /leave ---
// /leave - leave party (no args) or leave channel (with args, WoW-style overload)
class LeavePartyCommand : public IChatCommand {
public:
    ChatCommandResult execute(ChatCommandContext& ctx) override {
        if (!ctx.args.empty()) {
            // /leave ChannelName - leave a chat channel
            ctx.gameHandler.leaveChannel(ctx.args);
        } else {
            ctx.gameHandler.leaveParty();
        }
        return {};
    }
    [[nodiscard]] std::vector<std::string> aliases() const override { return {"leave"}; }
    [[nodiscard]] std::string helpText() const override { return "Leave party/raid or channel"; }
};

// --- /maintank, /mt ---
class MainTankCommand : public IChatCommand {
public:
    ChatCommandResult execute(ChatCommandContext& ctx) override {
        if (ctx.gameHandler.hasTarget()) {
            ctx.gameHandler.setMainTank(ctx.gameHandler.getTargetGuid());
        } else {
            game::MessageChatData msg;
            msg.type = game::ChatType::SYSTEM;
            msg.language = game::ChatLanguage::UNIVERSAL;
            msg.message = "You must target a player to set as main tank.";
            ctx.gameHandler.addLocalChatMessage(msg);
        }
        return {};
    }
    [[nodiscard]] std::vector<std::string> aliases() const override { return {"maintank", "mt"}; }
    [[nodiscard]] std::string helpText() const override { return "Set target as main tank"; }
};

// --- /mainassist, /ma ---
class MainAssistCommand : public IChatCommand {
public:
    ChatCommandResult execute(ChatCommandContext& ctx) override {
        if (ctx.gameHandler.hasTarget()) {
            ctx.gameHandler.setMainAssist(ctx.gameHandler.getTargetGuid());
        } else {
            game::MessageChatData msg;
            msg.type = game::ChatType::SYSTEM;
            msg.language = game::ChatLanguage::UNIVERSAL;
            msg.message = "You must target a player to set as main assist.";
            ctx.gameHandler.addLocalChatMessage(msg);
        }
        return {};
    }
    [[nodiscard]] std::vector<std::string> aliases() const override { return {"mainassist", "ma"}; }
    [[nodiscard]] std::string helpText() const override { return "Set target as main assist"; }
};

// --- /clearmaintank ---
class ClearMainTankCommand : public IChatCommand {
public:
    ChatCommandResult execute(ChatCommandContext& ctx) override {
        ctx.gameHandler.clearMainTank();
        return {};
    }
    [[nodiscard]] std::vector<std::string> aliases() const override { return {"clearmaintank"}; }
    [[nodiscard]] std::string helpText() const override { return "Clear main tank assignment"; }
};

// --- /clearmainassist ---
class ClearMainAssistCommand : public IChatCommand {
public:
    ChatCommandResult execute(ChatCommandContext& ctx) override {
        ctx.gameHandler.clearMainAssist();
        return {};
    }
    [[nodiscard]] std::vector<std::string> aliases() const override { return {"clearmainassist"}; }
    [[nodiscard]] std::string helpText() const override { return "Clear main assist assignment"; }
};

// --- /raidinfo ---
class RaidInfoCommand : public IChatCommand {
public:
    ChatCommandResult execute(ChatCommandContext& ctx) override {
        // Asked again first, for the next time this is run. The reply is a
        // packet and arrives well after this returns, so what is printed is
        // what the last request brought back - the one the interface makes at
        // login, which is why there is something to print at all.
        ctx.gameHandler.requestRaidInfo();
        const auto& lockouts = ctx.gameHandler.getInstanceLockouts();
        if (lockouts.empty()) {
            ctx.gameHandler.addSystemChatMessage("You are not saved to any instances.");
            return {};
        }
        ctx.gameHandler.addSystemChatMessage("Saved instances:");
        const auto now = static_cast<uint64_t>(std::time(nullptr));
        for (const auto& lo : lockouts) {
            std::string name = ctx.gameHandler.getMapName(lo.mapId);
            if (name.empty()) name = "Instance " + std::to_string(lo.mapId);
            if (const char* difficulty = game::instanceDifficultyName(lo.difficulty)) {
                name += " (";
                name += difficulty;
                name += ")";
            }
            // Days and hours, which is the granularity a lockout is read at.
            // A reset already past is possible - the server sends the time it
            // was set to, and nothing here is told when it comes round.
            std::string when = "expired";
            if (lo.resetTime > now) {
                const uint64_t left = lo.resetTime - now;
                const uint64_t days = left / 86400;
                const uint64_t hours = (left % 86400) / 3600;
                when = days > 0 ? std::to_string(days) + "d " + std::to_string(hours) + "h"
                                : std::to_string(hours) + "h";
            }
            ctx.gameHandler.addSystemChatMessage(
                "  " + name + " - resets in " + when +
                (lo.extended ? " (extended)" : ""));
        }
        return {};
    }
    [[nodiscard]] std::vector<std::string> aliases() const override { return {"raidinfo"}; }
    [[nodiscard]] std::string helpText() const override { return "Show raid instance lockouts"; }
};

// --- /random, /rnd ---
class RollCommand : public IChatCommand {
public:
    ChatCommandResult execute(ChatCommandContext& ctx) override {
        uint32_t minRoll = 1;
        uint32_t maxRoll = 100;
        if (!ctx.args.empty()) {
            size_t dashPos = ctx.args.find('-');
            size_t spacePos = ctx.args.find(' ');
            if (dashPos != std::string::npos) {
                try {
                    minRoll = std::stoul(ctx.args.substr(0, dashPos));
                    maxRoll = std::stoul(ctx.args.substr(dashPos + 1));
                } catch (...) {}
            } else if (spacePos != std::string::npos) {
                try {
                    minRoll = std::stoul(ctx.args.substr(0, spacePos));
                    maxRoll = std::stoul(ctx.args.substr(spacePos + 1));
                } catch (...) {}
            } else {
                try { maxRoll = std::stoul(ctx.args); } catch (...) {}
            }
        }
        ctx.gameHandler.randomRoll(minRoll, maxRoll);
        return {};
    }
    [[nodiscard]] std::vector<std::string> aliases() const override { return {"random", "rnd"}; }
    [[nodiscard]] std::string helpText() const override { return "Random dice roll"; }
};

// --- Registration ---
void registerGroupCommands(ChatCommandRegistry& reg) {
    reg.registerCommand(std::make_unique<ReadyCheckCommand>());
    reg.registerCommand(std::make_unique<ReadyCommand>());
    reg.registerCommand(std::make_unique<YieldCommand>());
    reg.registerCommand(std::make_unique<AfkCommand>());
    reg.registerCommand(std::make_unique<DndCommand>());
    reg.registerCommand(std::make_unique<UninviteCommand>());
    reg.registerCommand(std::make_unique<LeavePartyCommand>());
    reg.registerCommand(std::make_unique<MainTankCommand>());
    reg.registerCommand(std::make_unique<MainAssistCommand>());
    reg.registerCommand(std::make_unique<ClearMainTankCommand>());
    reg.registerCommand(std::make_unique<ClearMainAssistCommand>());
    reg.registerCommand(std::make_unique<RaidInfoCommand>());
    reg.registerCommand(std::make_unique<RollCommand>());
}

} // namespace ui
} // namespace wowee
