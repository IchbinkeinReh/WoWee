#pragma once

#include "game/who_filter.hpp"
#include "game/guild_emblem.hpp"
#include <optional>
#include "game/world_packets.hpp"
#include "game/opcode_table.hpp"
#include "game/group_defines.hpp"
#include "game/handler_types.hpp"
#include "game/calendar_data.hpp"
#include "game/lfg_update.hpp"
#include "game/difficulty_change.hpp"
#include "network/packet.hpp"
#include <array>
#include <chrono>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace wowee {
namespace game {

class GameHandler;

class SocialHandler {
public:
    using PacketHandler = std::function<void(network::Packet&)>;
    using DispatchTable = std::unordered_map<LogicalOpcode, PacketHandler>;

    explicit SocialHandler(GameHandler& owner);

    void registerOpcodes(DispatchTable& table);

    // ---- Structs (aliased from handler_types.hpp) ----

    using InspectArenaTeam = game::InspectArenaTeam;

    using InspectResult = game::InspectResult;

    using WhoEntry = game::WhoEntry;

    using BgQueueSlot = game::BgQueueSlot;

    using AvailableBgInfo = game::AvailableBgInfo;

    using BgPlayerScore = game::BgPlayerScore;

    using ArenaTeamScore = game::ArenaTeamScore;

    using BgScoreboardData = game::BgScoreboardData;

    using BgPlayerPosition = game::BgPlayerPosition;

    using PetitionSignature = game::PetitionSignature;

    using PetitionInfo = game::PetitionInfo;

    using ReadyCheckResult = game::ReadyCheckResult;

    using InstanceLockout = game::InstanceLockout;

    using LfgState = game::LfgState;

    using ArenaTeamStats = game::ArenaTeamStats;

    using ArenaTeamMember = game::ArenaTeamMember;

    using ArenaTeamRoster = game::ArenaTeamRoster;

    // ---- Public API ----

    // Inspection
    void inspectTarget();
    /// Inspect any player by guid; inspectTarget is this with the current target.
    void inspectUnit(uint64_t guid);
    /// Ask for the honour figures behind the inspect window's PvP tab. A
    /// separate request from the inspect itself, and answered on its own
    /// opcode, which is why the tab asks for it when it opens.
    void requestInspectHonorData(uint64_t guid);
    [[nodiscard]] const InspectResult* getInspectResult() const {
        return inspectResult_.guid ? &inspectResult_ : nullptr;
    }

    // Server info / who
    /// Ask the server for the time. `announce` prints it to chat, which is
    /// what /time wants and what the login-time query must not do.
    void queryServerTime(bool announce = false);
    /// Seconds until the daily quest reset, or 0 if the server has not said.
    /// Counted down from when the answer arrived rather than stored raw, since
    /// the reply is asked for once and read for the rest of the session.
    [[nodiscard]] uint32_t getSecondsUntilDailyReset() const;
    void requestPlayedTime();
    void queryWho(const std::string& playerName = "");
    [[nodiscard]] uint32_t getTotalTimePlayed() const { return totalTimePlayed_; }
    [[nodiscard]] uint32_t getLevelTimePlayed() const { return levelTimePlayed_; }
    [[nodiscard]] const std::vector<WhoEntry>& getWhoResults() const { return whoResults_; }
    [[nodiscard]] uint32_t getWhoOnlineCount() const { return whoOnlineCount_; }

    /// Where a /who answer goes: the panel, or the chat.
    ///
    /// SetWhoToUI is how the interface says which. FriendsFrame turns it on
    /// while its Who tab is up and off again on the way out, so a search typed
    /// into chat with the panel closed is meant to print its results there -
    /// which is what a stock client does and what this one did not, having
    /// treated the call as a no-op and always kept the rows to itself.
    void setWhoToUI(bool toUI) { whoToUI_ = toUI; }
    [[nodiscard]] bool isWhoToUI() const { return whoToUI_; }
    [[nodiscard]] std::string getWhoAreaName(uint32_t zoneId) const;
    /// ChrRaces' and ChrClasses' localized names, as GetWhoInfo and the chat
    /// rows give them; empty where the row is not known.
    [[nodiscard]] std::string getWhoRaceName(uint32_t raceId) const;
    [[nodiscard]] std::string getWhoClassName(uint32_t classId) const;

    // Social commands
    void addFriend(const std::string& playerName, const std::string& note = "");
    void removeFriend(const std::string& playerName);
    void setFriendNote(const std::string& playerName, const std::string& note);
    void addIgnore(const std::string& playerName);
    void removeIgnore(const std::string& playerName);

    // Random roll
    void randomRoll(uint32_t minRoll = 1, uint32_t maxRoll = 100);

    // Battleground
    [[nodiscard]] bool hasPendingBgInvite() const;
    void acceptBattlefield(uint32_t queueSlot = 0xFFFFFFFF);
    void declineBattlefield(uint32_t queueSlot = 0xFFFFFFFF);
    void leaveBattlefield();
    /// Ask the server which instances of one battleground are running.
    void requestBattlefieldList(uint32_t bgTypeId);
    /// Report a player in this battleground as not participating.
    void reportPvpAfk(uint64_t playerGuid);
    void joinBattlefield(uint64_t battlemasterGuid, uint32_t bgTypeId,
                         uint32_t instanceId, bool asGroup);
    [[nodiscard]] const std::array<BgQueueSlot, 3>& getBgQueues() const { return bgQueues_; }
    [[nodiscard]] const std::vector<AvailableBgInfo>& getAvailableBgs() const { return availableBgs_; }
    void requestPvpLog();
    [[nodiscard]] const BgScoreboardData* getBgScoreboard() const {
        return bgScoreboard_.players.empty() ? nullptr : &bgScoreboard_;
    }
    [[nodiscard]] const std::vector<BgPlayerPosition>& getBgPlayerPositions() const { return bgPlayerPositions_; }

    // Logout. exitAfterLogout distinguishes /quit and /exit, which leave the game
    // entirely, from /logout and /camp, which drop back to character select.
    void requestLogout(bool exitAfterLogout = false);
    void cancelLogout();
    [[nodiscard]] bool  isLoggingOut()        const { return loggingOut_; }
    [[nodiscard]] float getLogoutCountdown()  const { return logoutCountdown_; }
    [[nodiscard]] bool  isLogoutToExit()      const { return exitAfterLogout_; }

    // Guild
    void requestGuildInfo();
    void requestGuildRoster();
    /// MSG_GUILD_PERMISSIONS, empty: the rights of the player's rank, asked
    /// for by 0x005caa40.
    void requestGuildPermissions();
    /// The guild half of the player entering the world (0x005cb900, from
    /// 0x00528010 as the active player is made): rank 0's six tab flags and
    /// items a day set to -1, and MSG_GUILD_PERMISSIONS - alone, without the
    /// money - when PLAYER_GUILDID names a guild.
    void onPlayerEnteredWorld();
    /// Ask for the guild's event log. The reply is the same opcode.
    void requestGuildEventLog();
    /// Ask for one bank tab's log. Tab six is the money log, which is what the
    /// server means by GUILD_BANK_MAX_TABS.
    void requestGuildBankLog(uint8_t tab);

    /// Ask the battleground for everyone's position.
    ///
    /// MSG_BATTLEGROUND_PLAYER_POSITIONS is a request the server answers with
    /// the same opcode - the reply was already parsed here and nothing ever
    /// asked, so the list it fills stayed empty for both maps. Throttled
    /// because the interface calls it from WorldMapFrame_OnUpdate, which is
    /// every frame the map is open.
    void requestBattlefieldPositions();
    /// Ask the server to resend the friend, ignore and mute lists.
    ///
    /// The server sends them once at login and pushes a status line whenever a
    /// friend comes or goes, so this is a refresh rather than the only way to
    /// have them - but the friends panel asks for one every time it redraws,
    /// which is what ShowFriends means, and a stale online column is what
    /// happens without it.
    void requestContactList();

    /// Give or take raid assistant. CMSG_GROUP_ASSISTANT_LEADER is a guid and
    /// a flag, and AzerothCore drops it unless the sender leads the group -
    /// which is the same test the unit menu makes before offering the entry.
    void setGroupAssistant(uint64_t guid, bool apply);
    static constexpr uint8_t kGuildBankMoneyTab = 6;
    [[nodiscard]] const std::vector<GuildBankLogEntry>& getGuildBankLog(uint8_t tab) const {
        static const std::vector<GuildBankLogEntry> empty;
        return (tab < guildBankLogs_.size()) ? guildBankLogs_[tab] : empty;
    }
    [[nodiscard]] const std::vector<GuildEventLogEntry>& getGuildEventLog() const { return guildEventLog_; }
    void setGuildInfoText(const std::string& text);
    void takeInboxTextItem(uint32_t mailId);
    void setGuildMotd(const std::string& motd);
    void promoteGuildMember(const std::string& playerName);
    void demoteGuildMember(const std::string& playerName);
    void leaveGuild();
    void inviteToGuild(const std::string& playerName);
    void kickGuildMember(const std::string& playerName);
    void disbandGuild();
    void setGuildLeader(const std::string& name);
    void setGuildPublicNote(const std::string& name, const std::string& note);
    void setGuildOfficerNote(const std::string& name, const std::string& note);
    void acceptGuildInvite();
    void declineGuildInvite();
    void queryGuildInfo(uint32_t guildId);
    void createGuild(const std::string& guildName);
    void addGuildRank(const std::string& rankName);
    void delGuildRank();
    void saveGuildRank(uint32_t rankId, uint32_t rights, const std::string& rankName,
                       uint32_t goldLimit, const uint32_t* tabRights, const uint32_t* tabSlots);
    void deleteGuildRank();
    void requestPetitionShowlist(uint64_t npcGuid);
    void buyPetition(uint64_t npcGuid, const std::string& guildName,
                     uint32_t clientIndex = 1);

    // Guild state accessors
    [[nodiscard]] bool isInGuild() const;
    [[nodiscard]] const std::string& getGuildName() const { return guildName_; }
    [[nodiscard]] const GuildRosterData& getGuildRoster() const { return guildRoster_; }
    [[nodiscard]] bool hasGuildRoster() const { return hasGuildRoster_; }

    /// Membership the server has confirmed this session, for a guild joined
    /// after login - which no record built at login can know about.
    bool inGuild_ = false;
    /// What the invite called it, kept until the guild names itself.
    std::string joinedGuildName_;

    /// The guild this character is in, by id, whichever record knows it.
    ///
    /// The character list is the record at login and goes stale the moment a
    /// guild is joined or left mid-session; the player's own PLAYER_GUILDID
    /// field is the one the server keeps current. Zero for a player in none.
    [[nodiscard]] uint32_t ownGuildId() const;

    /// Forget which guild the player is in, for a character that is not them.
    ///
    /// Called as a character enters the world: the name, the ranks and the
    /// roster all belong to whoever was played last, and isInGuild() reads the
    /// name.
    /// The world's setup (0x00405540 -> 0x0052b550) starts these empty: the
    /// party (0x0052d0e0), the raid and its ready check (0x005756c0), the
    /// battlefield queues (0x0054e720), the arena teams (0x005a40e0) and the
    /// dungeon finder (0x0055d440).
    void resetForWorldEntry();
    void clearGuildMembership() {
        inGuild_ = false;
        joinedGuildName_.clear();
        guildName_.clear();
        guildRankNames_.clear();
        guildRoster_ = GuildRosterData{};
        hasGuildRoster_ = false;
        guildQueryData_ = GuildQueryResponseData{};
    }
    [[nodiscard]] const std::vector<std::string>& getGuildRankNames() const { return guildRankNames_; }
    /// Where the player sits in the guild: PLAYER_GUILDRANK, which is what
    /// the client reads (descriptor +0x10, e.g. 0x005cbcf0), not the roster's
    /// row for them. 0xFFFFFFFF with no player or no guild (PLAYER_GUILDID
    /// 0) - distinct from rank zero, which is the guild master.
    /// The rights of that rank, off its row of the rank table; 0 when there is
    /// none.
    [[nodiscard]] uint32_t getPlayerGuildRankIndex() const;
    [[nodiscard]] uint32_t getPlayerGuildRankRights() const;
    /// PLAYER_GUILDRANK (UNIT_END + 4) as it stands, in a guild or not;
    /// 0xFFFFFFFF with no player.
    [[nodiscard]] uint32_t getPlayerGuildRankField() const;
    /// One row of the rank table (0x00c21e60, ten of 0x38 bytes), or null
    /// past the tenth.
    [[nodiscard]] const GuildRankInfo* getGuildRankRow(uint32_t rank) const {
        return rank < guildRankTable_.size() ? &guildRankTable_[rank] : nullptr;
    }
    [[nodiscard]] bool hasPendingGuildInvite() const { return pendingGuildInvite_; }
    [[nodiscard]] const std::string& getPendingGuildInviterName() const { return pendingGuildInviterName_; }
    [[nodiscard]] const std::string& getPendingGuildInviteGuildName() const { return pendingGuildInviteGuildName_; }
    [[nodiscard]] const GuildInfoData& getGuildInfoData() const { return guildInfoData_; }
    [[nodiscard]] const GuildQueryResponseData& getGuildQueryData() const { return guildQueryData_; }
    [[nodiscard]] bool hasGuildInfoData() const { return guildInfoData_.isValid(); }

    // Petition
    [[nodiscard]] bool hasPetitionShowlist() const { return showPetitionDialog_; }
    void clearPetitionDialog() { showPetitionDialog_ = false; }
    /// Shuts the vendor session and tells the interface. Both windows read a
    /// different thing, so closing one has to close the other.
    void closePetitionVendor();
    [[nodiscard]] bool isGuildRegistrar() const { return petitionIsGuildCharter_; }
    /// The charters on offer, in the order the vendor listed them - one from a
    /// guild registrar, three from an arena registrar for the two, three and
    /// five person teams.
    [[nodiscard]] const std::vector<PetitionShowlistData::Charter>& getPetitionCharters() const {
        return petitionCharters_;
    }
    [[nodiscard]] uint32_t getPetitionCost() const { return petitionCost_; }
    [[nodiscard]] uint64_t getPetitionNpcGuid() const { return petitionNpcGuid_; }
    [[nodiscard]] const PetitionInfo& getPetitionInfo() const { return petitionInfo_; }
    [[nodiscard]] bool hasPetitionSignaturesUI() const { return petitionInfo_.showUI; }
    void clearPetitionSignaturesUI() { petitionInfo_.showUI = false; }
    void signPetition(uint64_t petitionGuid);
    void turnInPetition(uint64_t petitionGuid, bool blankDesign = false);
    /// Offer the charter to another player so they can sign it.
    void offerPetition(uint64_t petitionGuid, uint64_t targetGuid);

    // Guild name lookup
    const std::string& lookupGuildName(uint32_t guildId);
    /// A guild's tabard design from its query answer, asking for it when
    /// it is not known yet (the guild cache, 0x0067d930).
    std::optional<GuildEmblem> lookupGuildEmblem(uint32_t guildId);
    /// Counts the designs that have arrived, for whoever waits on one.
    [[nodiscard]] uint32_t guildEmblemGeneration() const { return guildEmblemGeneration_; }
    /// A player's PLAYER_GUILD_TIMESTAMP, which the client keeps per guild
    /// (0x006e1c60): one it has not seen for the guild asks for the guild
    /// again (its cache record dropped, 0x0067a5f0), whose answer paints
    /// the guild's members again.
    void noteGuildTimestamp(uint32_t guildId, uint32_t timestamp);
    [[nodiscard]] uint32_t getEntityGuildId(uint64_t guid) const;

    // Ready check
    void initiateReadyCheck();
    void respondToReadyCheck(bool ready);
    [[nodiscard]] bool hasPendingReadyCheck() const { return pendingReadyCheck_; }
    void dismissReadyCheck() { pendingReadyCheck_ = false; }
    [[nodiscard]] const std::string& getReadyCheckInitiator() const { return readyCheckInitiator_; }
    [[nodiscard]] const std::vector<ReadyCheckResult>& getReadyCheckResults() const { return readyCheckResults_; }
    /// GetReadyCheckTimeLeft() (0x00572c80): whole seconds to the ready
    /// check's deadline (0xbeb61c, 30 s from MSG_RAID_READY_CHECK,
    /// 0x00574d50), 0 out of a group or with none running.
    [[nodiscard]] uint32_t readyCheckSecondsLeft() const;
    /// GetReadyCheckStatus(unit) (0x00574180): "ready", "notready" or
    /// "waiting" for a member while a check runs and the player may see it -
    /// the leader of a party, a leader or assistant in a raid - else null.
    [[nodiscard]] const char* readyCheckStatus(uint64_t guid) const;

    // Duel
    void acceptDuel();
    void forfeitDuel();
    void proposeDuel(uint64_t targetGuid);
    void reportPlayer(uint64_t targetGuid, const std::string& reason);
    [[nodiscard]] bool hasPendingDuelRequest() const { return pendingDuelRequest_; }
    [[nodiscard]] const std::string& getDuelChallengerName() const { return duelChallengerName_; }
    /// 0x00c22b60: the player challenged (0x005cf950) or challenging
    /// (0x005cfb20), until the duel completes (0x005cfa90).
    [[nodiscard]] uint64_t getDuelOpponentGuid() const { return duelOpponentGuid_; }
    [[nodiscard]] float getDuelCountdownRemaining() const {
        if (duelCountdownMs_ == 0) return 0.0f;
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - duelCountdownStartedAt_).count();
        float rem = (static_cast<float>(duelCountdownMs_) - static_cast<float>(elapsed)) / 1000.0f;
        return rem > 0.0f ? rem : 0.0f;
    }

    // Party/Raid
    void inviteToGroup(const std::string& playerName);
    void acceptGroupInvite();
    void declineGroupInvite();

    // ---- Arena team invitations ----
    // Accept and decline carry no payload: the server answers from the invite
    // it is already holding against this character.
    void acceptArenaTeamInvite();
    void declineArenaTeamInvite();
    void arenaTeamInvite(uint32_t teamId, const std::string& name);
    void arenaTeamLeave(uint32_t teamId);
    void arenaTeamRemove(uint32_t teamId, const std::string& name);
    void arenaTeamSetLeader(uint32_t teamId, const std::string& name);
    void disbandArenaTeam(uint32_t teamId);
    /// Report a mail as spam. Type 0 of CMSG_COMPLAIN; the three values after
    /// the sender are a zero, the mail's own id, and another zero.
    void reportMailSpam(uint64_t senderGuid, uint32_t mailId);
    void leaveGroup();
    /// A worldport to `mapId`: the party's finder dungeon is forgotten,
    /// LFG_UPDATE, if worldportForgetsLfgDungeon says so (0x0052cc30).
    void forgetLfgDungeonOnWorldport(uint32_t mapId);
    void convertToRaid();
    void sendSetLootMethod(uint32_t method, uint32_t threshold, uint64_t masterLooterGuid);
    [[nodiscard]] bool isInGroup() const { return !partyData.isEmpty(); }
    [[nodiscard]] const GroupListData& getPartyData() const { return partyData; }
    [[nodiscard]] bool hasPendingGroupInvite() const { return pendingGroupInvite; }
    [[nodiscard]] const std::string& getPendingInviterName() const { return pendingInviterName; }
    void setGuildBankTabText(uint8_t tab, const std::string& text);
    void channelModeration(Opcode op, const std::string& channelName,
                           const std::string& targetName,
                           bool allowEmptyTarget = false);
    void promoteToLeader(uint64_t guid);
    void uninvitePlayer(const std::string& playerName);
    void setLootMethod(uint8_t method, uint64_t masterGuid, uint8_t threshold);
    void setPartyAssignment(uint8_t assignment, uint64_t guid, bool apply);
    void setRaidSubgroup(const std::string& playerName, uint8_t group);
    void swapRaidSubgroup(const std::string& firstName, const std::string& secondName);
    void leaveParty();
    void setMainTank(uint64_t targetGuid);
    void setMainAssist(uint64_t targetGuid);
    void clearMainTank();
    void clearMainAssist();
    void setRaidMark(uint64_t guid, uint8_t icon);
    // Apply a mark without the server round-trip, used when the player has no
    // group (the server would drop the request). Grouped marking stays
    // server-authoritative so every member sees the same icons.
    void setRaidMarkLocally(uint64_t guid, uint8_t icon);
    void requestRaidInfo();
    /// CMSG_REQUEST_PARTY_MEMBER_STATS for a group member whose player the
    /// client does not have, so its frame is filled from
    /// SMSG_PARTY_MEMBER_STATS_FULL (0x0052c760, 0x0052d410, 0x00573070,
    /// 0x00574f10): its guid, u64.
    void requestPartyMemberStats(uint64_t guid);
    /// A player leaving the world here: a member of the group is asked about
    /// (0x0052c760 and 0x00573070 from the player object's going).
    void playerLeftSight(uint64_t guid);
    void setSavedInstanceExtend(uint32_t mapId, uint32_t difficulty, bool extend);
    /// The roles last offered to the dungeon finder. lfgSetRoles used to send
    /// them and keep nothing, so the panel reading its own checkboxes back, and
    /// the ready dialog naming the role it found you a group for, had no source.
    [[nodiscard]] uint8_t getLfgOfferedRoles() const { return lfgOfferedRoles_; }
    [[nodiscard]] int32_t  getLfgWaitTank() const   { return lfgWaitTank_; }
    [[nodiscard]] int32_t  getLfgWaitHealer() const { return lfgWaitHealer_; }
    [[nodiscard]] int32_t  getLfgWaitDps() const    { return lfgWaitDps_; }
    [[nodiscard]] uint8_t  getLfgNeedTank() const   { return lfgNeedTank_; }
    [[nodiscard]] uint8_t  getLfgNeedHealer() const { return lfgNeedHealer_; }
    [[nodiscard]] uint8_t  getLfgNeedDps() const    { return lfgNeedDps_; }
    [[nodiscard]] uint64_t getLfgBootVictimGuid() const { return lfgBootVictimGuid_; }
    [[nodiscard]] const std::unordered_map<uint32_t, uint32_t>& getLfgLocks() const { return lfgLocks_; }
    /// The party's locks, member by member in the order the server listed
    /// them, and whether any have arrived (0x00acdc08) - and the same for the
    /// player's own (0x00acdbe8).
    [[nodiscard]] const std::vector<LfgMemberLocks>& getLfgPartyLocks() const { return lfgPartyLocks_; }
    [[nodiscard]] bool hasLfgPartyLocks() const { return lfgPartyLocksReceived_; }
    [[nodiscard]] bool hasLfgPlayerLocks() const { return lfgPlayerLocksReceived_; }
    [[nodiscard]] const std::vector<LfgReward>& getLfgRewards() const { return lfgRewards_; }
    void handleLfgPlayerInfo(network::Packet& packet);
    [[nodiscard]] const std::vector<LfgProposalMember>& getLfgProposalMembers() const {
        return lfgProposalMembers_;
    }

    // Instance lockouts
    [[nodiscard]] const std::vector<InstanceLockout>& getInstanceLockouts() const { return instanceLockouts_; }

    // Instance difficulty
    [[nodiscard]] uint32_t getInstanceDifficulty() const { return instanceDifficulty_; }
    /// The raid difficulty MSG_SET_RAID_DIFFICULTY keeps (0xbd089c, 0xbd197c).
    [[nodiscard]] const difficulty_change::RaidDifficulty& getRaidDifficulty() const { return raidDifficulty_; }
    /// The dungeon difficulty MSG_SET_DUNGEON_DIFFICULTY keeps (0xbd0898, 0xbd1978).
    [[nodiscard]] const difficulty_change::DungeonDifficulty& getDungeonDifficulty() const { return dungeonDifficulty_; }
    /// The party's dungeon difficulty is the one in force: a party slot is
    /// filled and there is no raid (0x00515790's test).
    [[nodiscard]] bool inDungeonParty() const;
    /// SetDungeonDifficulty (0x00526050).
    bool setDungeonDifficulty(uint32_t difficulty);

    // Minimap ping
    void sendMinimapPing(float wowX, float wowY);

    // Summon request
    void handleSummonRequest(network::Packet& packet);
    /// CMSG_SUMMON_RESPONSE: the summoner's guid, then yes or no.
    void sendSummonResponse(bool accept);
    void acceptSummon();
    void declineSummon();

    // Battlefield Manager
    void acceptBfMgrInvite();
    void declineBfMgrInvite();
    void respondBfMgrQueueInvite(uint32_t battleId, bool accept);
    void requestBfMgrExit(uint32_t battleId);

    // Calendar
    void requestCalendar();
    /// Invite someone: CMSG_CALENDAR_EVENT_INVITE. A pre-invite belongs to an
    /// event that has not been created yet, and carries no event id.
    void inviteToCalendarEvent(uint64_t eventId, uint64_t inviteId,
                               const std::string& name, bool isPreInvite,
                               bool isGuildEvent);
    /// Set another invitee's status, or their moderator rank. Both act on
    /// someone else; the player's own answer is respondToCalendarInvite.
    void setCalendarInviteStatus(uint64_t inviteeGuid, uint64_t eventId,
                                 uint64_t inviteId, uint8_t status);
    void setCalendarInviteModerator(uint64_t inviteeGuid, uint64_t eventId,
                                    uint64_t inviteId, uint8_t rank);
    /// Edit an existing event: CMSG_CALENDAR_UPDATE_EVENT.
    void updateCalendarEvent(uint64_t eventId, uint64_t inviteId,
                             const CalendarEventDraft& draft);
    /// Delete one: CMSG_CALENDAR_REMOVE_EVENT.
    void removeCalendarEvent(uint64_t eventId, uint64_t inviteId);
    /// Invite the guild by filter: CMSG_CALENDAR_GUILD_FILTER.
    void massInviteGuildToCalendarEvent(uint32_t minLevel, uint32_t maxLevel,
                                        uint32_t minRank);
    /// Ask for one event's detail: CMSG_CALENDAR_GET_EVENT.
    void requestCalendarEvent(uint64_t eventId);
    /// Create an event: CMSG_CALENDAR_ADD_EVENT, with its flags and its
    /// invite list.
    void createCalendarEvent(const CalendarEventDraft& draft, uint32_t flags,
                             const std::vector<CalendarEventInvitee>& invitees);
    /// Paste a copied event onto a date: CMSG_CALENDAR_COPY_EVENT.
    void copyCalendarEvent(uint64_t eventId, uint64_t inviteId, uint32_t packedTime);
    /// Answer an invitation: CMSG_CALENDAR_EVENT_RSVP with a
    /// CalendarInviteStatus (1 accepted, 2 declined, 8 tentative, 9 removed).
    void respondToCalendarInvite(uint64_t eventId, uint64_t inviteId,
                                 uint32_t status);

    // ---- Methods moved from GameHandler ----
    void sendSetDifficulty(uint32_t difficulty, bool raid = false);
    void toggleHelm();
    void toggleCloak();
    void setStandState(uint8_t standState);
    void sendAlterAppearance(uint32_t hairStyleEntry, uint32_t hairColor,
                             uint32_t facialHairEntry, uint32_t skinColorEntry);
    void deleteGmTicket();
    void requestGmTicket();
    /// Ask whether the GM ticket queue is accepting tickets.
    void requestGmSystemStatus();

    // Utility methods for delegation from GameHandler
    void updateLogoutCountdown(float deltaTime);
    void resetTransferState();
    GroupListData& mutablePartyData() { return partyData; }
    /// The player's own group as it stands outside a battleground - the "real"
    /// group the client keeps beside the roster (0xbd1990 its leader, 0xbd1998
    /// the party count, 0xbeb60c the raid's): what IsRealPartyLeader,
    /// GetRealNumPartyMembers and GetRealNumRaidMembers read. The roster of a
    /// group that is not a battleground's writes it (0x006d8870 under
    /// `(type & 1) == 0`), and SMSG_REAL_GROUP_UPDATE while one is
    /// (0x006cc300) - not the roster itself, which stays the battleground's.
    struct RealGroup {
        uint64_t leader = 0;
        uint32_t partyCount = 0;
        uint32_t raidCount = 0;
    };
    [[nodiscard]] const RealGroup& realGroup() const { return realGroup_; }
    /// SMSG_REAL_GROUP_UPDATE's three fields, as 0x006cc300 stores them.
    void applyRealGroupUpdate(uint8_t groupType, uint32_t count, uint64_t leader);
    InspectResult& mutableInspectResult() { return inspectResult_; }
    void setRaidTargetGuid(uint8_t icon, uint64_t guid) {
        if (icon < kRaidMarkCount) raidTargetGuids_[icon] = guid;
    }
    void setEncounterUnitGuid(uint32_t slot, uint64_t guid) {
        if (slot < kMaxEncounterSlots) encounterUnitGuids_[slot] = guid;
    }

    // The bind-or-leave question the server asks on entering an instance that
    // is already under way. It carries a countdown, and the answer is the
    // player's: accept and be saved, decline and go to the graveyard. Nothing
    // happens until one of the two is sent, so this is held rather than
    // answered here.
    struct InstanceLockPrompt {
        bool active = false;
        float secondsLeft = 0.0f;
        bool previouslySaved = false;
        uint32_t completedEncounterMask = 0;
    };
    [[nodiscard]] const InstanceLockPrompt& getInstanceLockPrompt() const { return instanceLock_; }
    void respondInstanceLock(bool accept);

    // Encounter unit tracking
    static constexpr uint32_t kMaxEncounterSlots = 5;
    [[nodiscard]] uint64_t getEncounterUnitGuid(uint32_t slot) const {
        return (slot < kMaxEncounterSlots) ? encounterUnitGuids_[slot] : 0;
    }

    // Raid target markers (0-7: Star, Circle, Diamond, Triangle, Moon, Square, Cross, Skull)
    static constexpr uint32_t kRaidMarkCount = game::kRaidMarkCount;
    [[nodiscard]] uint64_t getRaidMarkGuid(uint32_t icon) const {
        return (icon < kRaidMarkCount) ? raidTargetGuids_[icon] : 0;
    }
    [[nodiscard]] uint8_t getEntityRaidMark(uint64_t guid) const {
        if (guid == 0) return 0xFF;
        for (uint32_t i = 0; i < kRaidMarkCount; ++i)
            if (raidTargetGuids_[i] == guid) return static_cast<uint8_t>(i);
        return 0xFF;
    }

    // LFG / Dungeon Finder
    /// Queue for every dungeon the player ticked. CMSG_LFG_JOIN carries a
    /// list, and the server queues for all of them at once.
    void lfgJoin(const std::vector<uint32_t>& dungeonIds, uint8_t roles);
    void lfgLeave();
    void lfgSetRoles(uint8_t roles);
    /// CompleteLFGRoleCheck (0x00553e20): the role check answered with the
    /// roles kept (none when declined) - refused without a tank, healer or
    /// damage role. -1 when the three-a-ten-seconds throttle holds it.
    int lfgCompleteRoleCheck(bool accept);

    /// The client's local join record (0x00acd880..0x00acd99c): the dungeon
    /// slots JoinLFG sends - each the dungeon id with its LFGDungeons type in
    /// the top byte - the comment, whether a join went out and whether any of
    /// it changed since. GetLFGInfoLocal reads it.
    [[nodiscard]] const LfgLocalJoin& getLfgLocalJoin() const { return lfgLocalJoin_; }
    /// SetLFGDungeon's slot (0x00559b70): random and specific dungeons (types
    /// 1 and 5) keep each other, raids (2) keep raids while ungrouped, and
    /// anything else replaces what was there.
    void lfgAddSlot(uint32_t slot, int partyMembers);
    void lfgRemoveSlot(uint32_t slot);   ///< 0x005593a0
    void lfgClearSlots();                ///< 0x00559340
    /// JoinLFG (0x00557660): refused with ERR_LFG_NO_SLOTS_SELECTED or
    /// ERR_LFG_NO_ROLES_SELECTED, else CMSG_LFG_JOIN with the record
    /// (0x005572d0) when nothing has gone out yet or it changed - three a
    /// ten seconds.
    void lfgJoinLocal(uint8_t roles);
    /// LeaveLFG (0x00553ab0): CMSG_LFG_LEAVE while joined or queued.
    void lfgLeaveLocal();
    /// SetLFGComment (0x005539a0): the comment kept, and sent at once
    /// (CMSG_SET_LFG_COMMENT, three a two seconds) while joined.
    void lfgSetComment(const std::string& comment);
    /// The dungeons the server says are queued, the party's in a group
    /// (0x00acdac0) and the player's otherwise (0x00acd9a0).
    [[nodiscard]] const std::vector<uint32_t>& getLfgQueuedSlots(bool inGroup) const {
        return inGroup ? lfgPartyUpdate_.dungeons : lfgPlayerUpdate_.dungeons;
    }
    void lfgAcceptProposal(uint32_t proposalId, bool accept);
    void lfgSetBootVote(bool vote);
    void lfgTeleport(bool toLfgDungeon = true);
    [[nodiscard]] LfgState getLfgState()           const { return lfgState_; }
    /// What the last finished dungeon paid out, for GetLFGCompletionReward.
    [[nodiscard]] const LfgCompletionReward& getLfgCompletionReward() const { return lfgCompletionReward_; }
    [[nodiscard]] bool isLfgQueued()               const { return lfgState_ == LfgState::Queued; }
    [[nodiscard]] bool isLfgInDungeon()            const { return lfgState_ == LfgState::InDungeon; }
    /// The raid browser's joined-list id, packed the way this client packs it:
    /// the type in the top eight bits, the LFGDungeonData id in the low
    /// twenty-four (its 0x00beaa14). Zero means not listed.
    [[nodiscard]] uint32_t getLfgJoinedListId()    const { return lfgJoinedListId_; }
    /// Ask to be listed in the raid browser for one raid, or stop being listed
    /// (SearchLFGJoin / SearchLFGLeave, 0x00559400 and 0x00558f90).
    void lfgSearchJoin(uint32_t typeId, uint32_t lfgId);
    void lfgSearchLeave();
    /// How many rows the raid browser's browse panel has, which is
    /// SearchLFGGetNumResults' first number (0x00553ba0) and answers zero while no
    /// list is held - the client's own guard on its held list id.
    [[nodiscard]] uint32_t getLfgListRowCount() const {
        return lfgListWindowId_ == 0 ? 0 : static_cast<uint32_t>(lfgListRows_.size());
    }
    /// One of those rows by index from zero, or null past the end.
    [[nodiscard]] const LfgListRow* getLfgListRow(uint32_t index) const {
        if (lfgListWindowId_ == 0 || index >= lfgListRows_.size()) return nullptr;
        return &lfgListRows_[index];
    }
    /// How many rows the browse panel can actually show, which is what
    /// SearchLFGGetNumResults answers: the parsed rows paired up the way the
    /// client resolves an index (0x00553530), so a player record with no listing
    /// attached is not counted at all.
    [[nodiscard]] uint32_t getLfgBrowserRowCount() const {
        return lfgListWindowId_ == 0 ? 0 : static_cast<uint32_t>(lfgBrowserRows_.size());
    }
    /// One of those pairs by index from zero, or null past the end. Its index
    /// fields name entries of getLfgListRow, and stay valid until the next list
    /// packet, which rebuilds both.
    [[nodiscard]] const LfgBrowserRow* getLfgBrowserRow(uint32_t index) const {
        if (lfgListWindowId_ == 0 || index >= lfgBrowserRows_.size()) return nullptr;
        return &lfgBrowserRows_[index];
    }
    /// Which list those rows are held for, packed type<<24 | id. The client masks
    /// the low twenty-four bits off its joined browser id (0x00beab20) to name the
    /// dungeon those rows' bosses belong to, at 0x00553c30:3228; the id here is the
    /// one a packet actually named (0x00beaa98), which is the one the rows read
    /// belong to and, whenever rows are held, the same id.
    [[nodiscard]] uint32_t getLfgSearchListId() const { return lfgListWindowId_; }
    /**
     * What one of the browse panel's column headers does (`SearchLFGSort`,
     * 0x0055d2b0), by the word that header carries: "group" swaps which array of rows
     * is walked first and sorts nothing, and any other word makes that key primary —
     * reversing it if it already was. The client's guard against no word at all (a
     * usage line and no change, :10638-10642) is the binding's, not this one's.
     *
     * Both paths end with the client raising UPDATE_LFG_LIST (0x0055b700:9654, and
     * :10710 for the swap), which is what the panel refills itself on; that is the one
     * thing here that a header click sends to the interface.
     */
    void sortLfgBrowser(std::string_view word);
    /**
     * A name answer landed, and one of the raid browser's rows may be waiting on it.
     *
     * The client's callback matches no guid at all — it is a bare "an answer arrived"
     * notice closing one request off a counter (0x005528a0) — so this is called for
     * every answer and closes one only while a row is outstanding. The list is
     * re-announced when the last request closes, which is what fills a row's name in
     * and, off the same answer, its class: `SearchLFGGetResults` reads both from the
     * record its row's guid resolves to (0x00555fd0:4856-4873).
     */
    void onLfgNameAnswered(uint64_t guid);
    [[nodiscard]] uint32_t getLfgDungeonId()       const { return lfgDungeonId_; }
    [[nodiscard]] std::string getCurrentLfgDungeonName() const;
    [[nodiscard]] uint32_t getLfgProposalId()      const { return lfgProposalId_; }
    [[nodiscard]] int32_t  getLfgAvgWaitSec()      const { return lfgAvgWaitSec_; }
    /// This player's own estimate, in seconds, or -1 while the server has none.
    [[nodiscard]] int32_t  getLfgMyWaitSec()       const { return lfgMyWaitSec_; }
    /// How long this player has been in the queue, in seconds.
    [[nodiscard]] uint32_t getLfgQueuedSeconds()   const { return lfgQueuedSeconds_; }
    [[nodiscard]] uint32_t getLfgBootVotes()       const { return lfgBootVotes_; }
    [[nodiscard]] uint32_t getLfgBootTotal()       const { return lfgBootTotal_; }
    [[nodiscard]] uint32_t getLfgBootTimeLeft()    const { return lfgBootTimeLeft_; }
    [[nodiscard]] uint32_t getLfgBootNeeded()      const { return lfgBootNeeded_; }
    [[nodiscard]] const std::string& getLfgBootTargetName() const { return lfgBootTargetName_; }
    [[nodiscard]] const std::string& getLfgBootReason()     const { return lfgBootReason_; }
    [[nodiscard]] const std::vector<LfgRoleCheckDungeon>& getLfgRoleCheckDungeons() const {
        return lfgRoleCheckDungeons_;
    }
    [[nodiscard]] uint8_t getLfgRoleCheckMembers() const { return lfgRoleCheckMembers_; }
    [[nodiscard]] const std::vector<LfgRoleCheckMember>& getLfgRoleCheckMemberList() const {
        return lfgRoleCheckMemberList_;
    }
    /// SilenceMember / UnSilenceMember (0x0052d9c0, 0x0052daf0): a group
    /// member's voice, on CMSG_PARTY_SILENCE or _UNSILENCE, for the party's
    /// leader or a raid's leader or assistant.
    void silenceGroupMember(const std::string& name, bool silence);
    /// CMSG_BATTLEFIELD_MGR_QUEUE_REQUEST for a battlefield (0x0054d590).
    void requestBfMgrQueue(uint32_t battleId);
    [[nodiscard]] bool isLfgBootInProgress() const { return lfgBootInProgress_; }
    [[nodiscard]] bool hasLfgBootVoted()     const { return lfgBootDidVote_; }
    [[nodiscard]] bool getLfgBootMyVote()    const { return lfgBootMyVote_; }

    // Arena
    [[nodiscard]] const std::vector<ArenaTeamStats>& getArenaTeamStats() const { return arenaTeamStats_; }
    void requestArenaTeamRoster(uint32_t teamId);
    [[nodiscard]] const ArenaTeamRoster* getArenaTeamRoster(uint32_t teamId) const {
        for (const auto& r : arenaTeamRosters_)
            if (r.teamId == teamId) return &r;
        return nullptr;
    }
    /// Sort every roster by a column of the team details frame; the same
    /// column again reverses it. Online members stay first while offline ones
    /// are hidden (0x005a2bd0).
    void sortArenaTeamRosters(const std::string& key);
    [[nodiscard]] bool arenaRosterShowOffline() const { return arenaRosterShowOffline_; }
    /// The arena side byte of the last in-progress SMSG_BATTLEFIELD_STATUS.
    [[nodiscard]] uint8_t battlefieldArenaFaction() const { return battlefieldArenaFaction_; }
    /// SetArenaTeamRosterShowOffline (0x005a2e10): on a change every roster is
    /// sorted again and ARENA_TEAM_ROSTER_UPDATE raised for each that has
    /// members.
    void setArenaRosterShowOffline(bool show);

private:
    // ---- Packet handlers ----
    void handleInspectResults(network::Packet& packet);
    void handleGuildBankLog(network::Packet& packet);
    void sendBfMgrResponse(Opcode op, uint32_t battleId, bool accept, bool withFlag);
    void handleQueryTimeResponse(network::Packet& packet);
    void handlePlayedTime(network::Packet& packet);
    void handleWho(network::Packet& packet);
    void handleFriendList(network::Packet& packet);
    void handleContactList(network::Packet& packet);
    void handleFriendStatus(network::Packet& packet);
    void handleRandomRoll(network::Packet& packet);
    void handleLogoutResponse(network::Packet& packet);
    void handleLogoutComplete(network::Packet& packet);
    void handleGroupInvite(network::Packet& packet);
    void handleGroupDecline(network::Packet& packet);
    void handleGroupList(network::Packet& packet);
    void handleGroupUninvite(network::Packet& packet);
    /// The events that go out when the group stops existing: PARTY_MEMBERS-
    /// _CHANGED and PLAYER_ROLES_ASSIGNED, plus RAID_ROSTER_UPDATE for a raid.
    /// The client's own emptier 0x0052d6e0 fires the first two; the note on the
    /// definition gives each one's site, and why none of them is the
    /// GROUP_ROSTER_UPDATE that these sites used to fire instead.
    void firePartyClearedEvents(bool wasRaid);
    void handlePartyCommandResult(network::Packet& packet);
    void handlePartyMemberStats(network::Packet& packet, bool isFull);
    void handleGuildInfo(network::Packet& packet);
    void handleGuildRoster(network::Packet& packet);
    void handleGuildPermissions(network::Packet& packet);
    void handleGuildQueryResponse(network::Packet& packet);
    void handleGuildEvent(network::Packet& packet);
    // Updates a roster member's cached online flag; returns true only on a real change so
    // the login-time SIGNED_ON flood (state unchanged) is suppressed from guild chat.
    bool guildMemberOnlineTransition(const std::string& name, bool nowOnline);
    void handleGuildInvite(network::Packet& packet);
    void handleGuildCommandResult(network::Packet& packet);
    void handlePetitionShowlist(network::Packet& packet);
    void handlePetitionQueryResponse(network::Packet& packet);
    void handleGuildEventLog(network::Packet& packet);
    void handlePetitionShowSignatures(network::Packet& packet);
    void handlePetitionSignResults(network::Packet& packet);
    void handleTurnInPetitionResults(network::Packet& packet);
    void handleBattlefieldStatus(network::Packet& packet);
    void handleBattlefieldList(network::Packet& packet);
    void handleRaidInstanceInfo(network::Packet& packet);
    void handleSetDungeonDifficulty(network::Packet& packet);
    /// 0x00524720: ERR_DUNGEON_DIFFICULTY_CHANGED_S with the one in force.
    void sayDungeonDifficulty();
    void handleInstanceDifficulty(network::Packet& packet);
    void handleSetRaidDifficulty(network::Packet& packet);
    void handleDuelRequested(network::Packet& packet);
    void handleDuelComplete(network::Packet& packet);
    void handleDuelWinner(network::Packet& packet);
    void handleLfgJoinResult(network::Packet& packet);
    void playLfgSound(uint32_t soundId);
    void handleLfgQueueStatus(network::Packet& packet);
    void handleLfgProposalUpdate(network::Packet& packet);
    void handleLfgRoleCheckUpdate(network::Packet& packet);
    void handleLfgUpdatePlayer(network::Packet& packet);
    void handleLfgUpdateParty(network::Packet& packet);
    /// SMSG_LFG_UPDATE_SEARCH, one byte: the server saying this player is no
    /// longer listed drops the joined raid-browser list (case 0x369 of 0x0055bdc0).
    void handleLfgUpdateSearch(network::Packet& packet);
    void handleUpdateLfgList(network::Packet& packet);
    /// Re-pair the held rows for the browse panel (0x00553530's index resolution) and
    /// order them as the client orders its two arrays before the getter walks them
    /// (0x0055b700:9646-9653, which resorts and then rebuilds the same way).
    void rebuildLfgBrowserRows();
    /// Ask for the names the held rows have none for, as the client does for each
    /// listing record it has to allocate (0x0055b770:9931-9937), and count the
    /// requests it made: the last answer to arrive re-announces the list.
    void requestLfgRowNames();
    /// The window the list is held in, dropped: its rows, its paired rows and the
    /// names those rows waited on. That is what 0x00558af0 does, unregistering the
    /// name callback of every row it drops on the way (:6967) — which is also why it
    /// closes no request and the count outlives the rows it belongs to.
    void clearLfgListWindow();
    /**
     * Raise UPDATE_LFG_LIST: script event 469, named by the client's own id-to-name
     * table (`kLfgListChangedEvent`). Six sites fire it and every one of them is the
     * raid browser's — when the joined list id changes (0x00558ed0:7231) and when it is
     * cleared (0x00558f90:7278), from the one branch of the SMSG_LFG_UPDATE_SEARCH case
     * (through 0x00558c30:7019), at the tail of the SMSG_UPDATE_LFG_LIST handler (:9980),
     * after a resort (:9654) and after the "group" swap (:10710). It carries no arguments,
     * and its only consumer in FrameXML is lfrframe.lua:31-34, which refills the browse
     * list while that frame is visible: this is the event the panel's rows ride on, and
     * `LFG_UPDATE` — which those lines use for the comment box alone, and which this file
     * used to raise from all of these sites — is event 501 and something else.
     */
    void fireLfgListChanged();
    void applyLfgUpdate(uint8_t updateType, const LfgUpdateBody& body, bool party);
    /// Whether any of the body's dungeons is a raid, which turns the queue's
    /// lines into the raid browser's (0x00557260).
    bool lfgBodyHasRaid(const LfgUpdateBody& body) const;
    /// Drop the proposal the way 0x00552850 does.
    void clearLfgProposal();
    /// The player has left the group: the party's locks, a role check still
    /// beginning and a proposal held go (0x0055b120).
    void leaveLfgGroup();
    void handleLfgPartyInfo(network::Packet& packet);
    void handleLfgPlayerReward(network::Packet& packet);
    void handleLfgBootProposalUpdate(network::Packet& packet);
    void handleLfgTeleportDenied(network::Packet& packet);
    void handleArenaTeamCommandResult(network::Packet& packet);
    void handleArenaTeamQueryResponse(network::Packet& packet);
    void handleArenaTeamRoster(network::Packet& packet);
    void handleArenaTeamInvite(network::Packet& packet);
    void handleArenaTeamEvent(network::Packet& packet);
    void handleArenaTeamStats(network::Packet& packet);
    void handleArenaError(network::Packet& packet);
    void handlePvpLogData(network::Packet& packet);
    void handleInitializeFactions(network::Packet& packet);
    void handleSetFactionStanding(network::Packet& packet);
    void handleSetFactionAtWar(network::Packet& packet);
    void handleSetFactionVisible(network::Packet& packet);
    void handleGroupSetLeader(network::Packet& packet);
    void handleTalentsInfo(network::Packet& packet);
    void loadGuildNameCache();
    void saveGuildNameCache() const;
    void rememberGuildName(uint32_t guildId, const std::string& guildName);

    GameHandler& owner_;

    // ---- State ----

    // Inspect
    InspectResult inspectResult_;

    // Logout
    bool  loggingOut_        = false;
    bool  exitAfterLogout_   = false;
    float logoutCountdown_   = 0.0f;

    // Time played
    uint32_t totalTimePlayed_ = 0;
    /// SMSG_QUERY_TIME_RESPONSE's second word and when it landed. Zero means
    /// no answer yet, which is different from "the reset is now".
    uint32_t dailyResetOffset_ = 0;
    time_t   dailyResetReceivedAt_ = 0;
    bool     announceServerTime_ = false;
    uint32_t levelTimePlayed_ = 0;

    // Who results
    std::vector<WhoEntry> whoResults_;
    uint32_t whoOnlineCount_ = 0;
    /// The names a /who filter is read against (who_filter::parse): the
    /// zones (AreaTable rows with no parent), races and classes, localized.
    /// Read once, when the assets are there.
    struct WhoNames {
        bool loaded = false;
        std::vector<who_filter::Named> zones;
        std::vector<who_filter::Named> races;
        std::vector<who_filter::Named> classes;
    };
    mutable WhoNames whoNames_;
    const WhoNames& whoNames() const;
    /// Off until the interface asks otherwise, which is a stock client's
    /// default: with no panel open the results belong in the chat.
    bool whoToUI_ = false;

    // Duel
    bool pendingDuelRequest_    = false;
    uint64_t duelChallengerGuid_= 0;
    uint64_t duelOpponentGuid_ = 0;
    uint64_t duelArbiterGuid_   = 0;   // the duel flag; both answers name it

    // A join the server has not answered yet, and how long it has left.
    uint32_t bgJoinPendingType_ = 0;
    float bgJoinWaitSec_        = 0.0f;
    std::string duelChallengerName_;
    uint32_t duelCountdownMs_   = 0;
    std::chrono::steady_clock::time_point duelCountdownStartedAt_{};

    // Guild
    std::string guildName_;
    std::vector<std::string> guildRankNames_;
    GuildRosterData guildRoster_;
    /// The rank table (0x00c21e60): ten rows of rights, gold a day, six tab
    /// flags and six items a day. The roster fills its ranks' rows and zeroes
    /// the rest (0x005cc5d0); MSG_GUILD_PERMISSIONS writes the row of the
    /// rank it names (0x005cb9f0). Every reader of a rank's rights reads it.
    std::array<GuildRankInfo, 10> guildRankTable_{};
    GuildInfoData guildInfoData_;
    GuildQueryResponseData guildQueryData_;
    bool hasGuildRoster_ = false;
    std::unordered_map<uint32_t, std::string> guildNameCache_;
    std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> pendingGuildNameQueries_;
    std::unordered_map<uint32_t, GuildEmblem> guildEmblems_;
    uint32_t guildEmblemGeneration_ = 0;
    std::unordered_map<uint32_t, uint32_t> guildTimestamps_;
    bool pendingGuildInvite_ = false;
    std::string pendingGuildInviterName_;
    std::string pendingGuildInviteGuildName_;
    bool showPetitionDialog_ = false;
    /// Which of the two registrar panels is open - the offer says, not the
    /// opcode, and closing has to name the same one that opened.
    bool petitionIsGuildCharter_ = false;
    std::vector<PetitionShowlistData::Charter> petitionCharters_;
    uint32_t petitionCost_ = 0;
    uint64_t petitionNpcGuid_ = 0;
    PetitionInfo petitionInfo_;

    // Group
    GroupListData partyData;
    bool pendingGroupInvite = false;
    std::string pendingInviterName;

    // Ready check
    bool        pendingReadyCheck_       = false;
    uint32_t    readyCheckReadyCount_    = 0;
    uint32_t    readyCheckNotReadyCount_ = 0;
    std::string readyCheckInitiator_;
    std::vector<ReadyCheckResult> readyCheckResults_;
    /// The deadline (0xbeb61c): unset when no check runs. A check every
    /// member has answered is ended by putting it a moment in the past
    /// (0x005744f0).
    std::optional<std::chrono::steady_clock::time_point> readyCheckDeadline_;
    uint64_t readyCheckInitiatorGuid_ = 0;  ///< 0xbeb620
    /// Each member's answer: still waiting (party record / raid +800), and
    /// ready (raid +0x324).
    struct ReadyCheckAnswer { bool waiting = false; bool ready = false; };
    std::unordered_map<uint64_t, ReadyCheckAnswer> readyCheckAnswers_;
    [[nodiscard]] bool readyCheckRunning() const;

    // Instance
    std::vector<InstanceLockout> instanceLockouts_;
    std::vector<GuildEventLogEntry> guildEventLog_;
    /// Six item tabs and the money log after them.
    std::array<std::vector<GuildBankLogEntry>, 7> guildBankLogs_;
    uint32_t instanceDifficulty_ = 0;
    difficulty_change::RaidDifficulty raidDifficulty_;
    difficulty_change::DungeonDifficulty dungeonDifficulty_;

    // Raid marks
    std::array<uint64_t, kRaidMarkCount> raidTargetGuids_ = {};

    // Encounter units
    std::array<uint64_t, kMaxEncounterSlots> encounterUnitGuids_ = {};
    InstanceLockPrompt instanceLock_;

    // Arena
    std::vector<ArenaTeamStats>  arenaTeamStats_;
    std::vector<ArenaTeamRoster> arenaTeamRosters_;
    /// The column the arena rosters are sorted by, so clicking it again knows
    /// to reverse rather than re-sort the same way.
    std::string arenaSortKey_;
    bool arenaSortAscending_ = true;
    /// 0x00acf834, which the client starts at 1.
    bool arenaRosterShowOffline_ = true;
    /// 0x00bea54a.
    uint8_t battlefieldArenaFaction_ = 0;
    /// Sort one roster the way the rosters stand: online first while offline
    /// members are hidden, then by the column last chosen.
    void sortArenaRoster(ArenaTeamRoster& roster) const;

    // Battleground
    std::array<BgQueueSlot, 3> bgQueues_{};
    std::vector<AvailableBgInfo> availableBgs_;
    BgScoreboardData bgScoreboard_;
    std::vector<BgPlayerPosition> bgPlayerPositions_;
    std::chrono::steady_clock::time_point lastBgPositionRequest_{};
    std::chrono::steady_clock::time_point lastContactListRequest_{};

    // LFG / Dungeon Finder
    LfgState lfgState_        = LfgState::None;
    /// The raid browser list this player joined, packed type<<24 | id; zero when
    /// not listed. This client's own 0x00beaa14 - it is set by the request, not
    /// by any packet, which is why IsListedInLFR can only be as right as this.
    uint32_t lfgJoinedListId_ = 0;
    /// The rows of the list held, in the order the packet brought them: the
    /// player rows first, then the listings. The client shows them in neither of
    /// those orders — it qsorts its two arrays separately (0x0055b700) and then
    /// walks the array of listings that belong to no one before the player array
    /// unless the swap byte says otherwise (0x00553530:2831-2864).
    std::vector<LfgListRow> lfgListRows_;
    /// Those rows paired into the rows the browse panel can show, rebuilt whenever
    /// the rows are (buildLfgBrowserRows, then sortLfgBrowserRows). Held beside them
    /// rather than derived at query time so that the index fields of one never
    /// outlive the vector they name indices into.
    std::vector<LfgBrowserRow> lfgBrowserRows_;
    /// The browse panel's sort state: the client's seven {key, direction} slots at
    /// 0x00bea820, and the byte at 0x00acd660 that says which of the two arrays of
    /// rows is walked first. The LFG module's init fills the list with all seven keys
    /// ascending and sets that byte to listings-first (0x0055d440:10766-10772).
    RealGroup realGroup_;
    std::vector<LfgSortEntry> lfgSortKeys_ = defaultLfgSortKeys();
    bool lfgListingsFirst_ = true;
    /// Which list those rows belong to, the id of the last packet actually read -
    /// the client's 0x00beaa98, which is also what it tests to decide whether the
    /// rows changed enough to report. Zero when none is held.
    uint32_t lfgListWindowId_ = 0;
    /// The names the browser's rows are waiting for. A listing row that had to be
    /// allocated asks for its guid's name and registers a callback with the request
    /// (0x0055b770:9931-9937); a row whose name record already exists does not ask
    /// again, which is what `lfgNamesAsked_` stands in for, and `lfgPendingNames_` is
    /// the client's 0x00beaaac. The answer that closes the last of them re-announces
    /// the list (0x005528a0, `lfgNameAnswerClosesGate`), which is how a row the panel
    /// had already drawn gets its name - and with it the class the same answer
    /// carries, since the row getter reads both off the record the guid resolves to.
    std::unordered_set<uint64_t> lfgNamesAsked_;
    uint32_t lfgPendingNames_ = 0;
    LfgCompletionReward lfgCompletionReward_;
    uint32_t lfgDungeonId_    = 0;
    uint32_t lfgProposalId_   = 0;
    /// The proposal the ready dialog has already been opened for. The server
    /// resends the update as each member answers, and opening again would
    /// reset the countdown and the ticks beside every name.
    uint32_t shownProposalId_ = 0;
    /// A proposal is held (its dungeon is non-zero) until a decline or a
    /// queue update ends it - the condition 0x0055bdc0 tests before firing
    /// LFG_PROPOSAL_SUCCEEDED or _FAILED.
    bool lfgProposalValid_ = false;
    /// The last player and party queue bodies (0x00acd9a0, 0x00acdac0).
    LfgUpdateBody lfgPlayerUpdate_;
    LfgUpdateBody lfgPartyUpdate_;
    /// The party's locks from SMSG_LFG_PARTY_INFO or a join refused with
    /// result 6, in the server's order.
    std::vector<LfgMemberLocks> lfgPartyLocks_;
    bool lfgPartyLocksReceived_ = false;
    bool lfgPlayerLocksReceived_ = false;
    uint8_t  lfgOfferedRoles_ = 0;
    LfgLocalJoin lfgLocalJoin_;
    /// The two throttles: joins (0x00acdd6c, ten seconds) and comments
    /// (0x00acdd50, two).
    int lfgJoinSends_ = 0;
    double lfgJoinWindowStart_ = 0.0;
    int lfgCommentSends_ = 0;
    double lfgCommentWindowStart_ = 0.0;
    int lfgRoleSends_ = 0;
    double lfgRoleWindowStart_ = 0.0;
    // The queue's own numbers, which never arrived while the length check was
    // longer than the packet.
    int32_t  lfgWaitTank_ = -1;
    int32_t  lfgWaitHealer_ = -1;
    int32_t  lfgWaitDps_ = -1;
    uint8_t  lfgNeedTank_ = 0;
    uint8_t  lfgNeedHealer_ = 0;
    uint8_t  lfgNeedDps_ = 0;
    uint64_t lfgBootVictimGuid_ = 0;
    /// Why the server will not let this character queue for a dungeon, by
    /// dungeon id. Empty means nothing is locked, which is also what it meant
    /// while SMSG_LFG_PLAYER_INFO was being skipped - the difference is that
    /// now it is an answer rather than an absence.
    std::unordered_map<uint32_t, uint32_t> lfgLocks_;
    std::vector<LfgReward> lfgRewards_;
    /// The group a proposal is offering, in the order the server lists it.
    std::vector<LfgProposalMember> lfgProposalMembers_;
    int32_t  lfgAvgWaitSec_   = -1;
    int32_t  lfgMyWaitSec_    = -1;
    uint32_t lfgQueuedSeconds_= 0;
    uint32_t lfgBootVotes_    = 0;
    uint32_t lfgBootTotal_    = 0;
    uint32_t lfgBootTimeLeft_ = 0;
    uint32_t lfgBootNeeded_   = 0;
    /// The three flags the proposal opens with. Read from the packet and
    /// dropped until 2026-08-06; the vote dialog branches on all three.
    /// What the role check is for, and how many are answering it. Read off
    /// SMSG_LFG_ROLE_CHECK_UPDATE, which used to stop at the count.
    std::vector<LfgRoleCheckDungeon> lfgRoleCheckDungeons_;
    uint8_t lfgRoleCheckMembers_ = 0;
    std::vector<LfgRoleCheckMember> lfgRoleCheckMemberList_;
    bool lfgBootInProgress_ = false;
    bool lfgBootDidVote_    = false;
    bool lfgBootMyVote_     = false;
    std::string lfgBootTargetName_;
    std::string lfgBootReason_;
};

} // namespace game
} // namespace wowee
