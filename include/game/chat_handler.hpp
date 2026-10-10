#pragma once

#include <deque>

#include "game/chat_channels.hpp"
#include "game/chat_filters.hpp"

#include "game/world_packets.hpp"
#include "game/opcode_table.hpp"
#include "game/handler_types.hpp"
#include "network/packet.hpp"
#include <deque>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <set>
#include <vector>

namespace wowee {
namespace game {

class GameHandler;
struct TextSubject;

class ChatHandler {
public:
    using PacketHandler = std::function<void(network::Packet&)>;
    using DispatchTable = std::unordered_map<LogicalOpcode, PacketHandler>;

    explicit ChatHandler(GameHandler& owner);

    void registerOpcodes(DispatchTable& table);

    // --- Public API (delegated from GameHandler) ---
    void sendChatMessage(ChatType type, const std::string& message, const std::string& target = "");
    void sendAddonMessage(ChatType type, const std::string& message, const std::string& target = "");
    void sendTextEmote(uint32_t textEmoteId, uint64_t targetGuid = 0);
    /// Join a channel the way 0x00507a80 does: a ChatChannels.dbc shortcut
    /// ("Allgemein") is that row's channel, asked for by id with the place in
    /// its name; anything else is a channel of the player's own. A slot - the
    /// channel's number - is taken now, before the server answers. Nothing
    /// when the zone channel has no place to be named after yet, or all ten
    /// slots are taken; otherwise what was asked for, which the interface's
    /// JoinChannelByName answers with.
    std::optional<chat_channels::Join> joinChannel(const std::string& channelName,
                                                   const std::string& password = "",
                                                   bool byLogin = false);
    /// Leave a channel by its number, its name or its shortcut (0x005093f0).
    void leaveChannel(const std::string& channelName);
    /// Ask the server for a channel's members; the answer arrives as
    /// SMSG_CHANNEL_LIST and fires CHANNEL_ROSTER_UPDATE.
    void requestChannelList(const std::string& channelName);
    [[nodiscard]] std::string getChannelByIndex(int index) const;
    [[nodiscard]] int getChannelIndex(const std::string& channelName) const;
    /// The joined channels' names in number order, for the channel panel.
    /// A channel's number is not its place in this list - see channelSlots.
    [[nodiscard]] const std::vector<std::string>& getJoinedChannels() const { return joinedChannels_; }
    /// The client's channel slots, numbered as /1, /2 and GetChannelList
    /// number them.
    [[nodiscard]] const chat_channels::Slots& channelSlots() const { return channelSlots_; }
    /// ChatChannels.dbc as the client reads it, loaded on first use. Empty
    /// until the assets are there.
    [[nodiscard]] const std::vector<chat_channels::Row>& channelRows() const;
    /// The channels a chat window carries when nothing has been saved for it:
    /// each zone channel the client joins by itself, by shortcut and id
    /// (0x00508320's default branch hands them to 0x00501a70).
    [[nodiscard]] std::vector<std::pair<std::string, int>> defaultWindowChannels() const;
    /// Join the zone channels once the player has a zone and is not on a
    /// flight path, as 0x00508090 waits to (0x005204c0 calls it when the
    /// zone's text changes, 0x00720000's flag update when a flight ends).
    /// Called every tick; does its work once per login.
    void updateLoginChannelJoin();
    /// Whether the player owns this channel - the one who created it, or who
    /// it passed to. SMSG_CHANNEL_NOTIFY says so with OWNER_CHANGED, which
    /// carries the new owner's guid: AzerothCore's Channel::MakeOwnerChanged
    /// writes _ownerGUID into it.
    [[nodiscard]] bool ownsChannel(const std::string& name) const {
        return ownedChannels_.count(name) != 0;
    }
    /// Who sent the line with this id, or zero if it is not remembered.
    [[nodiscard]] uint64_t chatLineSender(uint32_t lineId) const {
        for (const auto& [id, guid] : chatLineSenders_)
            if (id == lineId) return guid;
        return 0;
    }

    /// The roster of a channel by name, empty until a list is asked for. The
    /// list used to be printed to chat and dropped, so the channel panel had
    /// no members to show and reported every channel as having none.
    [[nodiscard]] const std::vector<ChannelMember>& getChannelRoster(const std::string& channel) const {
        static const std::vector<ChannelMember> empty;
        auto it = channelRosters_.find(channel);
        return (it != channelRosters_.end()) ? it->second : empty;
    }
    void autoJoinDefaultChannels();
    void addLocalChatMessage(const MessageChatData& msg);

    /// Put a line of a given kind into chat, locally.
    ///
    /// The kind matters: CHAT_MSG_MONEY, CHAT_MSG_COMBAT_XP_GAIN and their like
    /// are chat messages, and the interface's handler reads twelve arguments
    /// off every one of them - arg4 is a channel name and it calls strlen on it
    /// without checking. Firing one of these with just the text raised there and
    /// took the rest of the handler with it, so the line never appeared at all.
    void addLocalChatLine(ChatType type, const std::string& message);
    void addSystemChatMessage(const std::string& message);

    /// Announces a message as CHAT_MSG_<TYPE> with WoW's argument order. Both
    /// the server path and the local one go through here, because an interface
    /// cannot tell the difference and should not have to.
    void fireChatEvent(const MessageChatData& msg);
    void toggleAfk(const std::string& message);
    void toggleDnd(const std::string& message);
    void replyToLastWhisper(const std::string& message);

    // ---- Methods moved from GameHandler ----
    void submitGmTicket(const std::string& text);
    /// Replace the text of the ticket already open.
    void updateGmTicket(const std::string& text);

    /// Release the lines held for `guid`, now that its name is known.
    void flushChatAwaitingName(uint64_t guid);
    /// Release anything whose wait has run out, named or not. A line nobody
    /// answers for is still a line the player should see.
    void expireChatAwaitingName();
    void handleMotd(network::Packet& packet);

    // --- State accessors ---
    std::deque<MessageChatData>& getChatHistory() { return chatHistory_; }
    [[nodiscard]] const std::deque<MessageChatData>& getChatHistory() const { return chatHistory_; }
    [[nodiscard]] size_t getMaxChatHistory() const { return maxChatHistory_; }
    void setMaxChatHistory(size_t n) { maxChatHistory_ = n; }

    // Chat auto-join settings (aliased from handler_types.hpp)
    using ChatAutoJoin = game::ChatAutoJoin;
    ChatAutoJoin chatAutoJoin;

private:
    /// A chat line waiting for a name: its sender's, or that of the player an
    /// NPC's line is written for. See deliverChatMessage.
    struct ChatAwaitingName {
        MessageChatData data;
        uint64_t guid = 0;
        double deadline = 0.0;
    };
    std::deque<ChatAwaitingName> chatAwaitingName_;
    /// Long enough for a name query to come back on a working connection,
    /// short enough that a line does not feel withheld.
    static constexpr double kNameWaitSeconds = 1.0;
    // --- Packet handlers ---
    void handleMessageChat(network::Packet& packet);
    /// Deliver a parsed chat line: resolve its sender, filter it, store it and
    /// tell the interface. Split from handleMessageChat so a line held back
    /// for its sender's name can be run through it again once the name lands.
    void deliverChatMessage(MessageChatData data, bool alreadyWaited);
    /// A player's name from whatever already holds it - the name cache, the
    /// player nearby, the party, the guild roster. Empty when none does.
    [[nodiscard]] std::string knownPlayerName(uint64_t guid) const;
    /// Who an NPC's line fills its $-tokens in for: the receiver the packet
    /// names, or the logged-in character when it names nobody.
    TextSubject monsterLineSubject(const MessageChatData& data);

    void handleTextEmote(network::Packet& packet);
    void handleChannelNotify(network::Packet& packet);
    /// Rebuild joinedChannels_ from the slots.
    void refreshJoinedChannels();
    /// Where the player is, for a zone channel's name (0x00507a80).
    [[nodiscard]] chat_channels::Place currentChannelPlace() const;
    /// Tell the interface a channel notice as 0x00509dd0 does: the word is
    /// arg1, the channel numbered in arg4, its id, number, name and instance
    /// in arg7 to arg10.
    void fireChannelNotice(ChatType type, const std::string& word, const std::string& channelName,
                           const std::string& player, const std::string& target, uint64_t senderGuid);
    void handleChannelList(network::Packet& packet);
    void handleUserlistAdd(network::Packet& packet);
    void handleUserlistRemove(network::Packet& packet);
    void applyUserlistChange(const std::string& chanName, uint64_t guid,
                             uint8_t memberFlags, bool removing);
    void initializeChatLog();
    void logChatMessage(const MessageChatData& msg, const char* source);

    GameHandler& owner_;

    // --- State ---
    std::deque<MessageChatData> chatHistory_;
    /// What has just been said, for the spam filter. Short on purpose: this
    /// is looking for a line pasted again a moment later, not keeping a record
    /// of the conversation. See repeatsRecentLine in chat_filters.hpp.
    std::deque<RecentChatLine> recentChatLines_;
    size_t maxChatHistory_ = 100;
    uint64_t chatUidCounter_ = 0;  // monotonic uid for MessageChatData::uid
    std::vector<std::string> joinedChannels_;
    chat_channels::Slots channelSlots_;
    mutable std::vector<chat_channels::Row> channelRows_;
    mutable bool channelRowsLoaded_ = false;
    /// The zone channels to join at login (0x00bcf010): a bit per row id,
    /// set when one is joined and cleared when the player leaves it.
    uint32_t zoneChannelMask_ = 0;
    /// Armed at world entry, done once the zone channels have been asked for
    /// (0x00bcefe4 and 0x00bcefe8).
    bool loginChannelJoinArmed_ = false;
    bool loginChannelJoinDone_ = false;
    std::set<std::string> ownedChannels_;
    /// Who sent the message a chat line id names, newest last.
    ///
    /// Every chat event carries a line id as its eleventh argument, and
    /// FrameXML builds it into the player link on the name - so a right-click
    /// on that name hands the id back and expects the client to know which
    /// message it was. This client sent zero for all of them, which is one id
    /// shared by every line ever printed and therefore no id at all.
    ///
    /// Bounded, because a session's chat is unbounded and only recent lines can
    /// be reported anyway.
    std::deque<std::pair<uint32_t, uint64_t>> chatLineSenders_;
    uint32_t nextChatLineId_ = 1;
    std::unordered_map<std::string, std::vector<ChannelMember>> channelRosters_;
    bool chatLogEnabled_ = false;
    bool chatLogInitialized_ = false;
    std::string chatLogPath_;
    std::ofstream chatLogStream_;

    // Track senders we've already auto-replied to (AFK/DND) this session
    // to prevent infinite reply loops. Cleared when AFK/DND is toggled off.
    std::unordered_set<std::string> afkAutoRepliedSenders_;
};

} // namespace game
} // namespace wowee
