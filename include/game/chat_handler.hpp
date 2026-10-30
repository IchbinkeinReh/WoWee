#pragma once

#include <deque>

#include "game/chat_channels.hpp"
#include "game/chat_filters.hpp"
#include "game/chat_spam_filter.hpp"

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
    /// What a channel argument names (0x004fe160): a number, the joined
    /// channel with that number - none when no channel has it; anything else,
    /// itself. Empty for an empty argument.
    [[nodiscard]] std::string resolveChannelArg(const std::string& channel) const;
    /// CMSG_SET_CHANNEL_WATCH and CMSG_CLEAR_CHANNEL_WATCH (0x004ffc30,
    /// 0x004ffc50).
    void setChannelWatch(const std::string& channel);
    void clearChannelWatch();
    /// ChannelSilenceAll / ChannelUnSilenceAll (0x00501250, 0x00501400): the
    /// channel and the member, on CMSG_CHANNEL_SILENCE_ALL or _UNSILENCE_ALL -
    /// for a chat channel. The group's own voice channels, "Party", "Raid" and
    /// "Battleground", are silenced on the client's voice chat instead, which
    /// this client does not have.
    void channelSilenceAll(const std::string& channel, const std::string& member, bool silence);
    /// EnumerateServerChannels (0x004ff200): the shortcut of every
    /// ChatChannels.dbc row this zone offers - a city-only one only in a zone
    /// whose AreaTable row is flagged 0x8.
    [[nodiscard]] std::vector<std::string> serverChannelNames() const;
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
    /// Fire the channel invite held back for `guid`'s name (0x0050e120 case 0x18
    /// requests the inviter's name and lets 0x004fb5c0 raise the popup with it).
    void onChannelInviteNameKnown(uint64_t guid);
    /// Release anything whose wait has run out, named or not. A line nobody
    /// answers for is still a line the player should see.
    void expireChatAwaitingName();
    /// A name query answered for a channel roster that was holding its events
    /// (0x004ffe30, the callback the roster fill and the member add register on
    /// the request itself). The row is filled in, and the last answer for that
    /// channel re-sorts the roster and raises the two channel events.
    void onChannelRosterNameKnown(uint64_t guid);
    /// Raise the two channel events for a roster whose names never came, so a
    /// panel is not held shut by a query the realm never answers.
    void expireChannelRosterWaits();
    void handleMotd(network::Packet& packet);

    // --- State accessors ---
    std::deque<MessageChatData>& getChatHistory() { return chatHistory_; }
    [[nodiscard]] const std::deque<MessageChatData>& getChatHistory() const { return chatHistory_; }
    [[nodiscard]] size_t getMaxChatHistory() const { return maxChatHistory_; }
    void setMaxChatHistory(size_t n) { maxChatHistory_ = n; }

    // Chat auto-join settings (aliased from handler_types.hpp)
    using ChatAutoJoin = game::ChatAutoJoin;
    ChatAutoJoin chatAutoJoin;

    /// LoggingChat([on]) (0x004fbc60 with index 0): with an argument, the
    /// chat lines written to Logs\WoWChatLog.txt or not; true while they are.
    bool loggingChat(std::optional<bool> enable);
    /// The chat log file closed and its logging off, on leaving the world
    /// (0x005072b0).
    void closeChatLog();

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
    /// gmName: SMSG_GM_MESSAGECHAT's sender name, kept as the line's sender.
    void handleMessageChat(network::Packet& packet, const std::string& gmName = {});
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
    /// Which of the three member packets arrived. They are three packets rather
    /// than one with a flag because the client answers them differently: ADD
    /// (0x00504130) appends a row, UPDATE (0x00500380) scans the rows and
    /// changes one already listed, REMOVE (0x005042f0) moves one out.
    enum class UserlistChange : uint8_t { add, update, remove };
    void handleUserlistAdd(network::Packet& packet, UserlistChange change);
    void handleUserlistRemove(network::Packet& packet);
    void applyUserlistChange(const std::string& chanName, uint64_t guid,
                             uint8_t memberFlags, UserlistChange change);
    /// The channel's own flag byte, as every packet that carries one writes it
    /// (the slot's +0x90), and CHANNEL_FLAGS_UPDATED (552) for it - raised only
    /// when the byte moved and the channel is in the list to be told about.
    void noteChannelFlags(const std::string& chanName, uint8_t chanFlags);
    /// Ask for the names the roster rows are missing, and hold the two channel
    /// events for that channel until they answer. Nothing is held when every row
    /// already has a name. See rosterNameWaits_ for the rule this mirrors.
    void holdRosterForNames(const std::string& chanName,
                            const std::vector<ChannelMember>& roster);
    /// CHANNEL_ROSTER_UPDATE and CHANNEL_COUNT_UPDATE for one channel, both by
    /// display index and both carrying the row count as it stands.
    void fireChannelRosterEvents(const std::string& chanName);
    void initializeChatLog();
    void logChatMessage(const MessageChatData& msg, const char* source);
    /// SMSG_EXPECTED_SPAM_RECORDS (0x00501c70): the server's spam regexes.
    void handleExpectedSpamRecords(network::Packet& packet);
    /// Whether a line is spam to be dropped (0x00509dd0 -> 0x00509bd0),
    /// reporting a spam whisper with CMSG_CHAT_FILTERED.
    bool dropAsSpam(const MessageChatData& data);

    GameHandler& owner_;

    // --- State ---
    std::deque<MessageChatData> chatHistory_;
    /// What has just been said, for the spam filter. Short on purpose: this
    /// is looking for a line pasted again a moment later, not keeping a record
    /// of the conversation. See repeatsRecentLine in chat_filters.hpp.
    std::deque<RecentChatLine> recentChatLines_;
    /// The spam filter: SpamMessages.dbc's patterns, read on first use, and
    /// the server's (SMSG_EXPECTED_SPAM_RECORDS).
    ChatSpamFilter spamFilter_;
    bool spamMessagesLoaded_ = false;
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
    /// An invite waiting on its inviter's name, as 0x0050e120 case 0x18 leaves it
    /// in the 0x80 bytes at DAT_00b75950. One at a time: a second invite while
    /// this is set is dropped by the case that reads it.
    uint64_t pendingInviteGuid_ = 0;
    std::string pendingInviteChannel_;
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
    /// The channel rosters waiting on names, and the guids still outstanding for
    /// each.
    ///
    /// The client counts the roster rows whose name it does not have
    /// (`DAT_00b743d8`) and raises CHANNEL_ROSTER_UPDATE and CHANNEL_COUNT_UPDATE
    /// only when that count is zero - either at the end of the fill (`0x00503990`)
    /// or from the callback the request carries (`0x004ffe30`), which re-sorts
    /// first. So the panel is never shown a row named after nobody and then made
    /// to move it when the name lands.
    ///
    /// Kept per channel here, where the client has one counter and one roster
    /// store for the whole client: its next fill resets that counter, which drops
    /// the events the rows it replaced were still owed. And a wait past its
    /// deadline fires anyway (expireChannelRosterWaits) - the client waits for an
    /// answer that may never come, and a panel that never updates is worse than
    /// one that updates twice.
    struct RosterNameWait {
        std::unordered_set<uint64_t> guids;
        double deadline = 0.0;
    };
    std::unordered_map<std::string, RosterNameWait> rosterNameWaits_;
    /// Long enough for a name query to come back, and longer than the chat line
    /// wait: a roster is not being read this second, and re-asking the panel on
    /// the way costs less than drawing it twice.
    static constexpr double kRosterNameWaitSeconds = 3.0;
    bool chatLogEnabled_ = false;
    bool chatLogInitialized_ = false;
    std::string chatLogPath_;
    std::ofstream chatLogStream_;
    /// The chat line as 0x00509dd0 writes it to WoWChatLog.txt, while
    /// loggingChat_ (0xb759d0) is on: the sender is the name the line shows.
    void writeChatLogLine(ChatType type, const std::string& text, const std::string& sender,
                          const std::string& channelName, uint32_t language, const std::string& target);
    bool loggingChat_ = false;
    std::ofstream chatLogFile_;

    // Track senders we've already auto-replied to (AFK/DND) this session
    // to prevent infinite reply loops. Cleared when AFK/DND is toggled off.
    std::unordered_set<std::string> afkAutoRepliedSenders_;
};

} // namespace game
} // namespace wowee
