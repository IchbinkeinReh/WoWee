// SMSG_CALENDAR_SEND_CALENDAR, read against the server that writes it.
//
// Six lists back to back, no length prefix on any of them, and four of the six
// rows carry a packed guid or a string - so a row read one field wrong does
// not fail. It slides everything after it, and the next count is read out of
// the middle of a string. Nothing in the packet says so, and nothing in the
// client would either: the calendar would simply be full of plausible nonsense.
//
// The bytes below are laid out exactly as WorldSession::HandleCalendarGetCalendar
// writes them (CalendarHandler.cpp:53), so a change on either side shows up
// here rather than on screen.

#include <catch_amalgamated.hpp>

#include "core/application.hpp"
#include "game/calendar_data.hpp"
#include "network/packet.hpp"

#include <ctime>
#include "core/local_time.hpp"

namespace wowee::core {
Application* Application::instance = nullptr;
}

using namespace wowee::game;
using wowee::network::Packet;

namespace {

/// A holiday row: five fields, then 26 dates, 10 durations, 10 flags, then the
/// texture name. The three array lengths are the server's own constants and
/// getting any of them wrong misreads every holiday after the first.
void writeHoliday(Packet& p, uint32_t id, const std::string& texture,
                  uint32_t firstDate) {
    p.writeUInt32(id);
    p.writeUInt32(2);    // region
    p.writeUInt32(1);    // looping
    p.writeUInt32(7);    // priority
    p.writeUInt32(3);    // calendarFilterType
    for (int i = 0; i < CalendarHoliday::kNumDates; ++i) {
        p.writeUInt32(i == 0 ? firstDate : 0);
    }
    for (int i = 0; i < CalendarHoliday::kNumDurations; ++i) {
        p.writeUInt32(i == 0 ? 24u : 0u);
    }
    for (int i = 0; i < CalendarHoliday::kNumFlags; ++i) {
        p.writeUInt32(i == 0 ? 0x10u : 0u);
    }
    p.writeString(texture);
}

/// One packet carrying something in every list, so no list can be skipped
/// without the ones after it landing wrong.
Packet fullCalendar() {
    Packet p(0x436);

    p.writeUInt32(2);                 // invites
    p.writeUInt64(1001); p.writeUInt64(5001);
    p.writeUInt8(1); p.writeUInt8(2); p.writeUInt8(1);
    p.writePackedGuid(0x070000000000ABCDull);
    p.writeUInt64(1002); p.writeUInt64(5002);
    p.writeUInt8(0); p.writeUInt8(0); p.writeUInt8(0);
    p.writePackedGuid(0x42ull);

    p.writeUInt32(2);                 // events
    p.writeUInt64(1001);
    p.writeString("Naxxramas");
    p.writeUInt32(1);
    p.writeUInt32(0x09E1C000u);       // packed time
    p.writeUInt32(0x0100u);
    p.writeUInt32(533);
    p.writePackedGuid(0x070000000000ABCDull);
    p.writeUInt64(1002);
    p.writeString("");                // an untitled event is still a row
    p.writeUInt32(4);
    p.writeUInt32(0);
    p.writeUInt32(0);
    p.writeUInt32(-1);
    p.writePackedGuid(0x42ull);

    p.writeUInt32(1300000000u);       // server time
    p.writeUInt32(0x09E1C000u);       // zone time

    p.writeUInt32(1);                 // lockouts
    p.writeUInt32(533); p.writeUInt32(1); p.writeUInt32(86400);
    p.writeUInt64(0x0010000000000007ull);

    p.writeUInt32(1135814400u);       // reset relation time
    p.writeUInt32(2);                 // resets
    p.writeUInt32(533); p.writeUInt32(604800); p.writeUInt32(0);
    p.writeUInt32(615); p.writeUInt32(604800); p.writeUInt32(3600);

    p.writeUInt32(2);                 // holidays
    writeHoliday(p, 141, "Calendar_HallowsEnd", 0x09E1C000u);
    writeHoliday(p, 181, "Calendar_WinterVeil", 0x0A21C000u);

    return p;
}

}  // namespace

TEST_CASE("A whole calendar reads back field for field", "[calendar]") {
    Packet p = fullCalendar();
    CalendarData cal;
    REQUIRE(parseCalendarSendCalendar(p, cal));

    // Nothing left over. A parser that stopped a field early would still fill
    // everything above and only show itself here.
    CHECK(p.getRemainingSize() == 0);

    REQUIRE(cal.invites.size() == 2);
    CHECK(cal.invites[0].eventId == 1001);
    CHECK(cal.invites[0].inviteId == 5001);
    CHECK(cal.invites[0].status == 1);
    CHECK(cal.invites[0].rank == 2);
    CHECK(cal.invites[0].isGuildEvent);
    CHECK(cal.invites[0].creatorGuid == 0x070000000000ABCDull);
    CHECK_FALSE(cal.invites[1].isGuildEvent);
    CHECK(cal.invites[1].creatorGuid == 0x42ull);

    REQUIRE(cal.events.size() == 2);
    CHECK(cal.events[0].title == "Naxxramas");
    CHECK(cal.events[0].type == 1);
    CHECK(cal.events[0].flags == 0x0100u);
    CHECK(cal.events[0].dungeonId == 533);
    CHECK(cal.events[0].creatorGuid == 0x070000000000ABCDull);
    // Kept raw as well as unpacked: the interface sorts events on the packed
    // value and re-sending it is how an edit keeps the same time.
    CHECK(cal.events[0].eventTimePacked == 0x09E1C000u);
    // A dungeon id of -1 is "no dungeon", which only reads correctly as
    // signed - as a uint32 it is four billion and every guard on it inverts.
    CHECK(cal.events[1].dungeonId == -1);
    CHECK(cal.events[1].title.empty());

    CHECK(cal.serverTimeUnix == 1300000000u);

    REQUIRE(cal.lockouts.size() == 1);
    CHECK(cal.lockouts[0].mapId == 533);
    CHECK(cal.lockouts[0].difficulty == 1);
    CHECK(cal.lockouts[0].secondsRemaining == 86400);
    // A full guid, not a packed one - the server writes the instance guid
    // straight into the buffer while the two creator fields above are packed.
    CHECK(cal.lockouts[0].instanceGuid == 0x0010000000000007ull);

    CHECK(cal.resetRelationTime == 1135814400u);
    REQUIRE(cal.resets.size() == 2);
    CHECK(cal.resets[1].mapId == 615);
    CHECK(cal.resets[1].periodSeconds == 604800);
    CHECK(cal.resets[1].offsetSeconds == 3600);

    REQUIRE(cal.holidays.size() == 2);
    CHECK(cal.holidays[0].id == 141);
    CHECK(cal.holidays[0].textureFilename == "Calendar_HallowsEnd");
    CHECK(cal.holidays[0].dates[0] == 0x09E1C000u);
    CHECK(cal.holidays[0].durations[0] == 24);
    CHECK(cal.holidays[0].calendarFlags[0] == 0x10u);
    // The second holiday is the one that proves the array lengths. Read 25
    // dates instead of 26 and this row is four bytes out, so its id is the
    // tail of the previous row's texture name rather than 181.
    CHECK(cal.holidays[1].id == 181);
    CHECK(cal.holidays[1].textureFilename == "Calendar_WinterVeil");
    CHECK(cal.holidays[1].dates[0] == 0x0A21C000u);
}

TEST_CASE("The event time unpacks as a date", "[calendar]") {
    Packet p = fullCalendar();
    CalendarData cal;
    REQUIRE(parseCalendarSendCalendar(p, cal));

    // Same helper the achievement and guild parsers use, so the calendar
    // cannot drift into a fifth reading of the same field.
    const WowDate expected = unpackWowPackedTime(0x09E1C000u);
    CHECK(cal.events[0].eventTime.fullYear() == expected.fullYear());
    CHECK(cal.events[0].eventTime.month == expected.month);
    CHECK(cal.events[0].eventTime.day == expected.day);
    CHECK(cal.zoneTime.fullYear() == expected.fullYear());
}

TEST_CASE("An empty calendar is not an error", "[calendar]") {
    // What a new character gets. Every count zero, and the two times still
    // present between the lists - a parser that skipped the times when the
    // lists were empty would read the trailing counts as clock values.
    Packet p(0x436);
    p.writeUInt32(0);
    p.writeUInt32(0);
    p.writeUInt32(1300000000u);
    p.writeUInt32(0x09E1C000u);
    p.writeUInt32(0);
    p.writeUInt32(1135814400u);
    p.writeUInt32(0);
    p.writeUInt32(0);

    CalendarData cal;
    REQUIRE(parseCalendarSendCalendar(p, cal));
    CHECK(cal.invites.empty());
    CHECK(cal.events.empty());
    CHECK(cal.holidays.empty());
    CHECK(cal.serverTimeUnix == 1300000000u);
    CHECK(cal.resetRelationTime == 1135814400u);
    CHECK(p.getRemainingSize() == 0);
}

TEST_CASE("A packet cut short is refused rather than half-believed",
          "[calendar]") {
    // Truncation at every length, because the interesting ones are not at the
    // edges: a cut inside a holiday's 46 array words leaves five plausible
    // fields already read, and a cut inside a title leaves the string reader
    // running to the end of the buffer.
    const Packet whole = fullCalendar();
    const std::vector<uint8_t>& bytes = whole.getData();
    for (size_t len = 0; len < bytes.size(); ++len) {
        Packet cut(0x436, std::vector<uint8_t>(bytes.begin(), bytes.begin() + len));
        CalendarData cal;
        INFO("truncated to " << len << " of " << bytes.size());
        CHECK_FALSE(parseCalendarSendCalendar(cut, cal));
    }
    // And the whole thing still parses, so the loop above is not passing
    // because the parser refuses everything.
    Packet full = fullCalendar();
    CalendarData cal;
    CHECK(parseCalendarSendCalendar(full, cal));
}

TEST_CASE("A count larger than the packet reserves nothing", "[calendar]") {
    // The counts are server-controlled uint32s. Believed literally, the first
    // of these asks for four billion invites before a single read fails.
    for (uint32_t count : {0xFFFFFFFFu, 0x10000000u, 1000u}) {
        Packet p(0x436);
        p.writeUInt32(count);
        CalendarData cal;
        INFO("claimed invites: " << count);
        CHECK_FALSE(parseCalendarSendCalendar(p, cal));
    }
}

TEST_CASE("A day's rows are holidays first, then events by time", "[calendar]") {
    // The interface reads a day through one index space: CalendarGetNumDayEvents
    // counts the rows and three separate getters index into the same list. So
    // the order is decided once, and a change to it moves every row the player
    // clicks on.
    CalendarData cal;

    CalendarEvent late;
    late.title = "Late";
    late.eventTime.month = 8; late.eventTime.day = 12;
    late.eventTime.yearSince2000 = 26;
    late.eventTime.hour = 20; late.eventTime.minute = 30;
    CalendarEvent early = late;
    early.title = "Early";
    early.eventTime.hour = 9; early.eventTime.minute = 0;
    CalendarEvent otherDay = late;
    otherDay.title = "Tomorrow";
    otherDay.eventTime.day = 13;
    // Pushed out of order on purpose: the wire order is the server's, not the
    // player's, and the grid has to sort it.
    cal.events = {late, early, otherDay};

    CalendarHoliday festival;
    festival.id = 141;
    festival.textureFilename = "Calendar_HallowsEnd";
    // Three days from 11 August 2026, 00:00.
    festival.dates[0] = (26u << 24) | (7u << 20) | (10u << 14);
    festival.durations[0] = 72;
    cal.holidays = {festival};

    const auto rows = calendarEntriesForDay(cal, 8, 12, 2026);
    REQUIRE(rows.size() == 3);
    CHECK(rows[0].kind == CalendarEntryKind::Holiday);
    // The 12th is the holiday's second day, so its name is drawn once on the
    // 11th and this row is skipped - FrameXML tests sequenceType ~= "ONGOING".
    CHECK(rows[0].ongoing);
    CHECK(rows[1].kind == CalendarEntryKind::Event);
    CHECK(cal.events[rows[1].index].title == "Early");
    CHECK(cal.events[rows[2].index].title == "Late");

    // The first day of the holiday is not ongoing, and has no events.
    const auto first = calendarEntriesForDay(cal, 8, 11, 2026);
    REQUIRE(first.size() == 1);
    CHECK_FALSE(first[0].ongoing);

    // The day after it ends carries only its own event.
    const auto after = calendarEntriesForDay(cal, 8, 14, 2026);
    CHECK(after.empty());
    const auto tomorrow = calendarEntriesForDay(cal, 8, 13, 2026);
    REQUIRE(tomorrow.size() == 2);   // holiday's third day, plus the event
    CHECK(cal.events[tomorrow[1].index].title == "Tomorrow");

    // A different year is a different day, even on the same date.
    CHECK(calendarEntriesForDay(cal, 8, 12, 2027).empty());
}

TEST_CASE("A holiday with no duration still happens on its day", "[calendar]") {
    // Zero hours is what the server sends for a one-off, and rounding it to
    // zero days would drop the holiday entirely rather than show it once.
    CalendarData cal;
    CalendarHoliday oneOff;
    oneOff.dates[0] = (26u << 24) | (7u << 20) | (10u << 14);
    oneOff.durations[0] = 0;
    cal.holidays = {oneOff};

    CHECK(calendarEntriesForDay(cal, 8, 11, 2026).size() == 1);
    CHECK(calendarEntriesForDay(cal, 8, 12, 2026).empty());
}

namespace {

/// One SMSG_CALENDAR_SEND_EVENT, laid out as CalendarMgr::SendCalendarEvent
/// writes it (CalendarMgr.cpp:627).
Packet oneEvent() {
    Packet p(0x043D);
    p.writeUInt8(0);                          // sendType
    p.writePackedGuid(0x070000000000ABCDull); // creator
    p.writeUInt64(4242);                      // eventId
    p.writeString("Raid Night");
    p.writeString("Bring flasks");
    p.writeUInt8(1);                          // type
    p.writeUInt8(0);                          // repeatable
    p.writeUInt32(100);                       // maxInvites
    p.writeUInt32(static_cast<uint32_t>(-1)); // dungeonId
    p.writeUInt32(0x0400);                    // flags
    p.writeUInt32(0x09E1C000u);               // eventTime
    p.writeUInt32(0x09E1C000u);               // timeZoneTime
    p.writeUInt32(77);                        // guildId

    p.writeUInt32(2);                         // invitees
    p.writePackedGuid(0x11ull);
    p.writeUInt8(80); p.writeUInt8(1); p.writeUInt8(2); p.writeUInt8(1);
    p.writeUInt64(9001);
    p.writeUInt32(0x09E1C000u);
    p.writeString("see you there");
    p.writePackedGuid(0x070000000000BEEFull);
    p.writeUInt8(72); p.writeUInt8(0); p.writeUInt8(0); p.writeUInt8(0);
    p.writeUInt64(9002);
    p.writeUInt32(0);
    p.writeString("");                        // an empty note is still a row
    return p;
}

}  // namespace

TEST_CASE("One event reads back with its invite list", "[calendar]") {
    Packet p = oneEvent();
    CalendarEventDetail ev;
    REQUIRE(parseCalendarSendEvent(p, ev));
    CHECK(p.getRemainingSize() == 0);

    CHECK(ev.creatorGuid == 0x070000000000ABCDull);
    CHECK(ev.eventId == 4242);
    CHECK(ev.title == "Raid Night");
    CHECK(ev.description == "Bring flasks");
    CHECK(ev.maxInvites == 100);
    // -1 is "no dungeon" and only reads that way signed; as a uint32 it is
    // four billion and every guard on it inverts.
    CHECK(ev.dungeonId == -1);
    CHECK(ev.flags == 0x0400u);
    CHECK(ev.guildId == 77);

    REQUIRE(ev.invitees.size() == 2);
    CHECK(ev.invitees[0].guid == 0x11ull);
    CHECK(ev.invitees[0].level == 80);
    CHECK(ev.invitees[0].status == 1);
    CHECK(ev.invitees[0].rank == 2);
    CHECK(ev.invitees[0].isGuildMember);
    CHECK(ev.invitees[0].inviteId == 9001);
    CHECK(ev.invitees[0].note == "see you there");
    // The second row is what proves the first was read to its exact end: a
    // packed guid, a packed time and a string all vary in length, so one field
    // misread slides this row rather than failing.
    CHECK(ev.invitees[1].guid == 0x070000000000BEEFull);
    CHECK(ev.invitees[1].level == 72);
    CHECK(ev.invitees[1].inviteId == 9002);
    CHECK(ev.invitees[1].note.empty());
}

TEST_CASE("An event packet cut short is refused", "[calendar]") {
    const Packet whole = oneEvent();
    const std::vector<uint8_t>& bytes = whole.getData();
    for (size_t len = 0; len < bytes.size(); ++len) {
        Packet cut(0x043D, std::vector<uint8_t>(bytes.begin(), bytes.begin() + len));
        CalendarEventDetail ev;
        INFO("truncated to " << len << " of " << bytes.size());
        CHECK_FALSE(parseCalendarSendEvent(cut, ev));
    }
    Packet full = oneEvent();
    CalendarEventDetail ev;
    CHECK(parseCalendarSendEvent(full, ev));
}

TEST_CASE("A raid lockout lands on the day it expires", "[calendar]") {
    // The packet dates a lockout only by how long it has left, and the client
    // places it at the calendar's zone time plus that, in whole minutes
    // (0x005c29c0) - the realm's clock, not this machine's.
    Packet p = fullCalendar();
    CalendarData parsed;
    REQUIRE(parseCalendarSendCalendar(p, parsed));
    REQUIRE(parsed.lockouts.size() == 1);
    const WowDate want = addMinutes(parsed.zoneTime, 86400 / 60);
    CHECK(parsed.lockouts[0].expires.day == want.day);
    CHECK(parsed.lockouts[0].expires.month == want.month);

    CalendarData cal;
    CalendarRaidLockout lock;
    lock.mapId = 533;
    lock.expires = unpackWowPackedTime(0x09927000u + (21u << 6) + 30u);  // 10 Oct 2009 21:30
    cal.lockouts.push_back(lock);
    const auto onDay = calendarEntriesForDay(cal, 10, 10, 2009);
    REQUIRE(onDay.size() == 1);
    CHECK(onDay[0].kind == CalendarEntryKind::RaidLockout);
    CHECK(onDay[0].hour == 21);
    CHECK(onDay[0].minute == 30);
    CHECK(calendarEntriesForDay(cal, 10, 9, 2009).empty());
}

TEST_CASE("Adding minutes to a packed date crosses months, years and weekdays", "[calendar]") {
    WowDate d;
    d.yearSince2000 = 9; d.month = 12; d.day = 31; d.hour = 23; d.minute = 30;
    const WowDate n = addMinutes(d, 45);
    CHECK(n.fullYear() == 2010);
    CHECK(n.month == 1);
    CHECK(n.day == 1);
    CHECK(n.hour == 0);
    CHECK(n.minute == 15);
    CHECK(n.weekday == 5);  // 1 January 2010 was a Friday
    const WowDate leap = addMinutes(WowDate{0, 0, 0, 28, 2, 12}, 24 * 60);
    CHECK(leap.month == 2);
    CHECK(leap.day == 29);
}

// ---- The calendar's incremental packets, applied as wow.exe applies them ----
//
// Each packet below is laid out as AzerothCore's CalendarMgr writes it, and
// the events checked are the ones 0x005c3fe0's handlers raise, in order.

#include "game/handler_types.hpp"

namespace {

constexpr uint64_t kMe = 0x11ull;
constexpr uint32_t kOct1 = 0x09902000u;   // Thursday 1 October 2009, 00:00
constexpr uint32_t kOct10 = 0x09927000u;  // Saturday 10 October 2009, 00:00

std::vector<std::string> names(const CalendarContext& ctx) {
    std::vector<std::string> out;
    for (const auto& s : ctx.signals) out.push_back(s.event);
    return out;
}

/// A calendar with one event on it, the player invited and not yet answered.
CalendarData calendarWithInvite(uint64_t eventId, uint32_t flags = 0) {
    CalendarData cal;
    cal.received = true;
    cal.zoneTime = unpackWowPackedTime(kOct1);
    CalendarEvent ev;
    ev.eventId = eventId;
    ev.title = "Raid";
    ev.eventTimePacked = kOct10;
    ev.eventTime = unpackWowPackedTime(kOct10);
    ev.flags = flags;
    cal.events.push_back(ev);
    CalendarInvite inv;
    inv.eventId = eventId;
    inv.inviteId = 77;
    inv.status = calendar_status::kInvited;
    inv.creatorGuid = 0x42;
    cal.invites.push_back(inv);
    cal.pendingInvites.push_back({eventId, 0x42});
    return cal;
}

}  // namespace

TEST_CASE("The whole calendar keeps its unanswered invites pending", "[calendar]") {
    // fullCalendar's second invite is unanswered, but its event is dated
    // before the zone time, so it is not ahead: nothing is pending.
    Packet p = fullCalendar();
    CalendarData cal;
    cal.serverPendingCount = 5;
    CalendarEventDetail open;
    CalendarContext ctx{cal, open, kMe};
    REQUIRE(applyCalendarPacket(CalendarPacket::SendCalendar, p, ctx));
    CHECK(cal.received);
    CHECK(cal.serverPendingCount == 0);
    CHECK(cal.pendingInvites.empty());
    CHECK(names(ctx) == std::vector<std::string>{"CALENDAR_UPDATE_EVENT_LIST",
                                                 "CALENDAR_ACTION_PENDING"});
    CHECK(ctx.signals.back().args == std::vector<std::string>{kEventNil});
}

TEST_CASE("An opened event says so with its calendar type", "[calendar]") {
    Packet p = oneEvent();   // sendType 0, a guild event
    CalendarData cal;
    CalendarEventDetail open;
    CalendarContext ctx{cal, open, kMe};
    REQUIRE(applyCalendarPacket(CalendarPacket::SendEvent, p, ctx));
    CHECK(open.eventId == 4242);
    // The player (0x11) is on the list as accepted, so nothing is pending.
    REQUIRE(ctx.signals.size() == 2);
    CHECK(ctx.signals[0].event == "CALENDAR_OPEN_EVENT");
    CHECK(ctx.signals[0].args == std::vector<std::string>{"GUILD_EVENT"});
    CHECK(ctx.signals[1].event == "CALENDAR_ACTION_PENDING");
}

TEST_CASE("A copied event is a new event that says it was copied", "[calendar]") {
    Packet src = oneEvent();
    std::vector<uint8_t> bytes = src.getData();
    bytes[0] = 2;   // CALENDAR_SENDTYPE_COPY
    Packet p(0x437, bytes);
    CalendarData cal;
    CalendarEventDetail open;
    CalendarContext ctx{cal, open, kMe};
    REQUIRE(applyCalendarPacket(CalendarPacket::SendEvent, p, ctx));
    CHECK(ctx.signals[0].event == "CALENDAR_NEW_EVENT");
    CHECK(ctx.signals[0].args == std::vector<std::string>{"1"});
}

TEST_CASE("The guild filter fills the new event's invite list", "[calendar]") {
    // Guild::MassInviteToEvent: a count, then a packed guid and a byte each.
    Packet p(0x438);
    p.writeUInt32(2);
    p.writePackedGuid(0x21); p.writeUInt8(70);
    p.writePackedGuid(0x22); p.writeUInt8(80);
    CalendarData cal;
    CalendarEventDetail open;
    CalendarContext ctx{cal, open, kMe, 80};
    REQUIRE(applyCalendarPacket(CalendarPacket::FilterGuild, p, ctx));
    CHECK(p.getRemainingSize() == 0);
    REQUIRE(open.invitees.size() == 3);
    CHECK(open.invitees[0].guid == 0x21);
    CHECK(open.invitees[0].level == 70);
    CHECK(open.invitees[1].level == 80);
    // The player is added as the confirmed owner.
    CHECK(open.invitees[2].guid == kMe);
    CHECK(open.invitees[2].status == calendar_status::kConfirmed);
    CHECK(open.invitees[2].rank == 2);
    CHECK(names(ctx) == std::vector<std::string>{"CALENDAR_UPDATE_INVITE_LIST",
                                                 "CALENDAR_ACTION_PENDING"});
    CHECK(ctx.signals[0].args == std::vector<std::string>{"1"});
}

TEST_CASE("An invite to the open event joins its list", "[calendar]") {
    // CalendarMgr::SendCalendarEventInvite: invitee, event, invite, level,
    // status, whether a status time follows, it, and the sign-up byte.
    Packet p(0x43A);
    p.writePackedGuid(0x33);
    p.writeUInt64(9);
    p.writeUInt64(500);
    p.writeUInt8(80);
    p.writeUInt8(calendar_status::kInvited);
    p.writeUInt8(1);
    p.writeUInt32(kOct1);
    p.writeUInt8(1);
    CalendarData cal = calendarWithInvite(9);
    CalendarEventDetail open;
    open.eventId = 9;
    CalendarContext ctx{cal, open, kMe};
    REQUIRE(applyCalendarPacket(CalendarPacket::EventInvite, p, ctx));
    CHECK(p.getRemainingSize() == 0);
    REQUIRE(open.invitees.size() == 1);
    CHECK(open.invitees[0].guid == 0x33);
    CHECK(open.invitees[0].inviteId == 500);
    CHECK(open.invitees[0].level == 80);
    CHECK(names(ctx) == std::vector<std::string>{"CALENDAR_UPDATE_EVENT_LIST",
                                                 "CALENDAR_UPDATE_INVITE_LIST",
                                                 "CALENDAR_ACTION_PENDING"});
}

TEST_CASE("A sign-up invite leaves the pending action alone", "[calendar]") {
    Packet p(0x43A);
    p.writePackedGuid(kMe);
    p.writeUInt64(9); p.writeUInt64(500);
    p.writeUInt8(80); p.writeUInt8(calendar_status::kSignedUp);
    p.writeUInt8(0);      // no status time
    p.writeUInt8(0);      // the invitee sent it themself
    CalendarData cal;
    CalendarEventDetail open;
    CalendarContext ctx{cal, open, kMe};
    REQUIRE(applyCalendarPacket(CalendarPacket::EventInvite, p, ctx));
    CHECK(p.getRemainingSize() == 0);
    CHECK(ctx.signals.empty());
}

TEST_CASE("An invite removed takes the row off the open event", "[calendar]") {
    // SendCalendarEventInviteRemove: invitee, event, flags, a byte.
    Packet p(0x43B);
    p.writePackedGuid(0x33);
    p.writeUInt64(9);
    p.writeUInt32(0);
    p.writeUInt8(1);
    CalendarData cal = calendarWithInvite(9);
    CalendarEventDetail open;
    open.eventId = 9;
    CalendarEventInvitee row; row.guid = 0x33;
    open.invitees.push_back(row);
    CalendarContext ctx{cal, open, kMe};
    REQUIRE(applyCalendarPacket(CalendarPacket::InviteRemoved, p, ctx));
    CHECK(open.invitees.empty());
    CHECK(names(ctx) == std::vector<std::string>{"CALENDAR_UPDATE_INVITE_LIST",
                                                 "CALENDAR_ACTION_PENDING"});
}

TEST_CASE("The player's own status answers the pending invite", "[calendar]") {
    // SendCalendarEventStatus: invitee packed, event, its time, its flags,
    // status, a byte, the status time - where this used to read two ids.
    Packet p(0x43C);
    p.writePackedGuid(kMe);
    p.writeUInt64(9);
    p.writeUInt32(kOct10);
    p.writeUInt32(0);
    p.writeUInt8(calendar_status::kAccepted);
    p.writeUInt8(1);
    p.writeUInt32(kOct1);
    CalendarData cal = calendarWithInvite(9);
    CalendarEventDetail open;
    open.eventId = 9;
    CalendarEventInvitee row; row.guid = kMe;
    open.invitees.push_back(row);
    CalendarContext ctx{cal, open, kMe};
    REQUIRE(applyCalendarPacket(CalendarPacket::EventStatus, p, ctx));
    CHECK(p.getRemainingSize() == 0);
    CHECK(cal.invites[0].status == calendar_status::kAccepted);
    CHECK(cal.pendingInvites.empty());
    CHECK(open.invitees[0].status == calendar_status::kAccepted);
    CHECK(names(ctx) == std::vector<std::string>{"CALENDAR_UPDATE_PENDING_INVITES",
                                                 "CALENDAR_UPDATE_EVENT_LIST",
                                                 "CALENDAR_UPDATE_INVITE_LIST",
                                                 "CALENDAR_ACTION_PENDING"});
}

TEST_CASE("Raid lockouts are added, moved and removed by their packets", "[calendar]") {
    CalendarData cal;
    CalendarEventDetail open;
    {
        // SendCalendarRaidLockout(add): now, map, difficulty, seconds, id.
        Packet p(0x43E);
        p.writeUInt32(kOct1);
        p.writeUInt32(533); p.writeUInt32(1); p.writeUInt32(2 * 86400 + 90);
        p.writeUInt64(0x1234);
        CalendarContext ctx{cal, open, kMe};
        REQUIRE(applyCalendarPacket(CalendarPacket::LockoutAdded, p, ctx));
        CHECK(p.getRemainingSize() == 0);
        REQUIRE(cal.lockouts.size() == 1);
        CHECK(cal.lockouts[0].instanceGuid == 0x1234);
        CHECK(cal.lockouts[0].expires.day == 3);
        CHECK(cal.lockouts[0].expires.minute == 1);
        CHECK(names(ctx) == std::vector<std::string>{"CALENDAR_UPDATE_EVENT_LIST",
                                                     "CALENDAR_ACTION_PENDING"});
    }
    {
        // SendCalendarRaidLockoutUpdated: now, map, difficulty, 0, seconds.
        Packet p(0x471);
        p.writeUInt32(kOct1);
        p.writeUInt32(533); p.writeUInt32(1); p.writeUInt32(0); p.writeUInt32(86400);
        CalendarContext ctx{cal, open, kMe};
        REQUIRE(applyCalendarPacket(CalendarPacket::LockoutUpdated, p, ctx));
        CHECK(cal.lockouts[0].expires.day == 2);
    }
    {
        // SendCalendarRaidLockout(remove): map, difficulty, 0, id.
        Packet p(0x43F);
        p.writeUInt32(533); p.writeUInt32(1); p.writeUInt32(0);
        p.writeUInt64(0x1234);
        CalendarContext ctx{cal, open, kMe};
        REQUIRE(applyCalendarPacket(CalendarPacket::LockoutRemoved, p, ctx));
        CHECK(p.getRemainingSize() == 0);
        CHECK(cal.lockouts.empty());
    }
}

TEST_CASE("An invite alert puts the event on the calendar and pends it", "[calendar]") {
    // SendCalendarEventInviteAlert.
    Packet p(0x440);
    p.writeUInt64(31);
    p.writeString("Ulduar");
    p.writeUInt32(kOct10);
    p.writeUInt32(0);
    p.writeUInt32(0);            // type
    p.writeUInt32(603);          // dungeon
    p.writeUInt64(600);          // invite
    p.writeUInt8(calendar_status::kInvited);
    p.writeUInt8(0);             // rank
    p.writePackedGuid(0x42);     // creator
    p.writePackedGuid(0x43);     // sender
    CalendarData cal;
    CalendarEventDetail open;
    CalendarContext ctx{cal, open, kMe};
    REQUIRE(applyCalendarPacket(CalendarPacket::InviteAlert, p, ctx));
    CHECK(p.getRemainingSize() == 0);
    REQUIRE(cal.events.size() == 1);
    CHECK(cal.events[0].title == "Ulduar");
    CHECK(cal.events[0].dungeonId == 603);
    CHECK(cal.events[0].creatorGuid == 0x42);
    REQUIRE(cal.invites.size() == 1);
    CHECK(cal.invites[0].inviteId == 600);
    CHECK(cal.invites[0].creatorGuid == 0x43);
    CHECK(cal.pendingInviteCount() == 1);
    // No CALENDAR_ACTION_PENDING: the dispatcher returns before it.
    CHECK(names(ctx) == std::vector<std::string>{"CALENDAR_UPDATE_PENDING_INVITES",
                                                 "CALENDAR_UPDATE_EVENT_LIST"});
}

TEST_CASE("An invite removed alert takes the event off the calendar", "[calendar]") {
    // SendCalendarEventInviteRemoveAlert: event, time, flags, status.
    Packet p(0x441);
    p.writeUInt64(9); p.writeUInt32(kOct10); p.writeUInt32(0);
    p.writeUInt8(calendar_status::kRemoved);
    CalendarData cal = calendarWithInvite(9);
    CalendarEventDetail open;
    open.eventId = 9;
    CalendarContext ctx{cal, open, kMe};
    REQUIRE(applyCalendarPacket(CalendarPacket::InviteRemovedAlert, p, ctx));
    CHECK(cal.events.empty());
    CHECK(cal.pendingInvites.empty());
    CHECK(open.eventId == 0);
    CHECK(names(ctx) == std::vector<std::string>{"CALENDAR_CLOSE_EVENT",
                                                 "CALENDAR_UPDATE_PENDING_INVITES",
                                                 "CALENDAR_UPDATE_EVENT_LIST"});
}

TEST_CASE("A status alert for an unknown event asks for the pending count", "[calendar]") {
    // SendCalendarEventStatusAlert: event, time, flags, status.
    Packet p(0x442);
    p.writeUInt64(99); p.writeUInt32(kOct10); p.writeUInt32(0); p.writeUInt8(1);
    CalendarData cal;
    CalendarEventDetail open;
    CalendarContext ctx{cal, open, kMe};
    REQUIRE(applyCalendarPacket(CalendarPacket::InviteStatusAlert, p, ctx));
    CHECK(ctx.askNumPending);
    CHECK(ctx.signals.empty());
}

TEST_CASE("An event removed alert closes it and takes it off", "[calendar]") {
    // SendCalendarEventRemovedAlert: a byte, event, time.
    Packet p(0x443);
    p.writeUInt8(1); p.writeUInt64(9); p.writeUInt32(kOct10);
    CalendarData cal = calendarWithInvite(9);
    CalendarEventDetail open;
    open.eventId = 9;
    CalendarContext ctx{cal, open, kMe};
    REQUIRE(applyCalendarPacket(CalendarPacket::EventRemovedAlert, p, ctx));
    CHECK(cal.events.empty());
    CHECK(names(ctx) == std::vector<std::string>{"CALENDAR_CLOSE_EVENT",
                                                 "CALENDAR_UPDATE_PENDING_INVITES",
                                                 "CALENDAR_UPDATE_EVENT_LIST",
                                                 "CALENDAR_ACTION_PENDING"});
}

TEST_CASE("An event updated alert rewrites the row and the open event", "[calendar]") {
    // SendCalendarEventUpdateAlert.
    Packet p(0x444);
    p.writeUInt8(1);
    p.writeUInt64(9);
    p.writeUInt32(kOct10);                       // old time
    p.writeUInt32(calendar_flag::kInvitesLocked);
    p.writeUInt32(kOct1);                        // new time
    p.writeUInt8(2);                             // type
    p.writeUInt32(615);
    p.writeString("Moved raid");
    p.writeString("New plan");
    p.writeUInt8(0);
    p.writeUInt32(100);
    p.writeUInt32(0);
    CalendarData cal = calendarWithInvite(9);
    CalendarEventDetail open;
    open.eventId = 9;
    CalendarContext ctx{cal, open, kMe};
    REQUIRE(applyCalendarPacket(CalendarPacket::EventUpdatedAlert, p, ctx));
    CHECK(p.getRemainingSize() == 0);
    CHECK(cal.events[0].title == "Moved raid");
    CHECK(cal.events[0].dungeonId == 615);
    CHECK(cal.events[0].eventTime.day == 1);
    // Locked now, so no longer waiting on the player.
    CHECK(cal.pendingInvites.empty());
    CHECK(open.description == "New plan");
    CHECK(open.maxInvites == 100);
    CHECK(names(ctx) == std::vector<std::string>{"CALENDAR_UPDATE_PENDING_INVITES",
                                                 "CALENDAR_UPDATE_EVENT_LIST",
                                                 "CALENDAR_UPDATE_EVENT",
                                                 "CALENDAR_ACTION_PENDING"});
}

TEST_CASE("Moderator status and notes reach the open event's rows", "[calendar]") {
    CalendarData cal = calendarWithInvite(9);
    CalendarEventDetail open;
    open.eventId = 9;
    CalendarEventInvitee row; row.guid = kMe;
    open.invitees.push_back(row);
    {
        // SendCalendarEventModeratorStatusAlert: invitee, event, rank, a byte.
        Packet p(0x445);
        p.writePackedGuid(kMe); p.writeUInt64(9); p.writeUInt8(1); p.writeUInt8(1);
        CalendarContext ctx{cal, open, kMe};
        REQUIRE(applyCalendarPacket(CalendarPacket::ModeratorStatusAlert, p, ctx));
        CHECK(open.invitees[0].rank == 1);
        CHECK(cal.invites[0].rank == 1);
        CHECK(names(ctx) == std::vector<std::string>{"CALENDAR_UPDATE_INVITE_LIST",
                                                     "CALENDAR_UPDATE_EVENT_LIST",
                                                     "CALENDAR_ACTION_PENDING"});
    }
    {
        // The invitee, the event, the note and a byte - the event's id where
        // the core names it an invite id, since that is what the client
        // compares it against.
        Packet p(0x460);
        p.writePackedGuid(kMe); p.writeUInt64(9); p.writeString("late"); p.writeUInt8(0);
        CalendarContext ctx{cal, open, kMe};
        REQUIRE(applyCalendarPacket(CalendarPacket::InviteNotes, p, ctx));
        CHECK(open.invitees[0].note == "late");
        CHECK(names(ctx) == std::vector<std::string>{"CALENDAR_UPDATE_INVITE_LIST"});
    }
    {
        Packet p(0x461);
        p.writeUInt64(9); p.writeString("on time");
        CalendarContext ctx{cal, open, kMe};
        REQUIRE(applyCalendarPacket(CalendarPacket::InviteNotesAlert, p, ctx));
        CHECK(open.invitees[0].note == "on time");
        CHECK(names(ctx) == std::vector<std::string>{"CALENDAR_UPDATE_INVITE_LIST"});
    }
}

TEST_CASE("The pending count and the cleared action", "[calendar]") {
    CalendarData cal;
    CalendarEventDetail open;
    {
        Packet p(0x448);
        p.writeUInt32(3);
        CalendarContext ctx{cal, open, kMe};
        REQUIRE(applyCalendarPacket(CalendarPacket::SendNumPending, p, ctx));
        CHECK(cal.pendingInviteCount() == 3);
        CHECK(names(ctx) == std::vector<std::string>{"CALENDAR_UPDATE_PENDING_INVITES",
                                                     "CALENDAR_ACTION_PENDING"});
    }
    {
        Packet p(0x4BB);
        CalendarContext ctx{cal, open, kMe};
        REQUIRE(applyCalendarPacket(CalendarPacket::ClearPendingAction, p, ctx));
        CHECK(names(ctx) == std::vector<std::string>{"CALENDAR_ACTION_PENDING"});
    }
}

// ---- What the handlers hold back, drop and leave for others ----

TEST_CASE("An opened event waits for its invitees' names", "[calendar]") {
    // 0x005bed20: while a name on the list is being asked for, the event is
    // not announced; the last answer announces it (0x005f1a40, case 4).
    Packet p = oneEvent();
    CalendarData cal;
    CalendarEventDetail open;
    CalendarContext ctx{cal, open, kMe};
    ctx.nameKnown = [](uint64_t guid) { return guid == kMe; };
    REQUIRE(applyCalendarPacket(CalendarPacket::SendEvent, p, ctx));
    CHECK(ctx.nameQueries == std::vector<uint64_t>{0x070000000000BEEFull});
    CHECK(names(ctx) == std::vector<std::string>{"CALENDAR_ACTION_PENDING"});

    std::vector<CalendarSignal> said;
    calendarNameArrived(open, 0x99, said);   // not one it waits on
    CHECK(said.empty());
    calendarNameArrived(open, 0x070000000000BEEFull, said);
    REQUIRE(said.size() == 1);
    CHECK(said[0].event == "CALENDAR_OPEN_EVENT");
    CHECK(said[0].args == std::vector<std::string>{"GUILD_EVENT"});
    // Said once: the count is spent.
    said.clear();
    calendarNameArrived(open, 0x070000000000BEEFull, said);
    CHECK(said.empty());
}

TEST_CASE("A new event and an invite to it wait for names the same way", "[calendar]") {
    Packet src = oneEvent();
    std::vector<uint8_t> bytes = src.getData();
    bytes[0] = 1;   // CALENDAR_SENDTYPE_ADD
    Packet p(0x437, bytes);
    CalendarData cal;
    CalendarEventDetail open;
    CalendarContext ctx{cal, open, kMe};
    ctx.nameKnown = [](uint64_t guid) { return guid == kMe; };
    REQUIRE(applyCalendarPacket(CalendarPacket::SendEvent, p, ctx));
    CHECK(names(ctx) == std::vector<std::string>{"CALENDAR_ACTION_PENDING"});

    // An invite to it while that name is still out: its own name is known,
    // but the list is held back all the same - and now says only the invite
    // list once the names are in (0x005f32b0 sets case 0).
    Packet inv(0x43A);
    inv.writePackedGuid(kMe);
    inv.writeUInt64(4242); inv.writeUInt64(9100);
    inv.writeUInt8(80); inv.writeUInt8(calendar_status::kInvited);
    inv.writeUInt8(0); inv.writeUInt8(1);
    CalendarContext ctx2{cal, open, kMe};
    ctx2.nameKnown = ctx.nameKnown;
    REQUIRE(applyCalendarPacket(CalendarPacket::EventInvite, inv, ctx2));
    CHECK(ctx2.nameQueries.empty());
    CHECK(names(ctx2) == std::vector<std::string>{"CALENDAR_ACTION_PENDING"});

    std::vector<CalendarSignal> said;
    calendarNameArrived(open, 0x070000000000BEEFull, said);
    REQUIRE(said.size() == 1);
    CHECK(said[0].event == "CALENDAR_UPDATE_INVITE_LIST");
    CHECK(said[0].args.empty());
}

TEST_CASE("The guild filter's list waits for the names it lists", "[calendar]") {
    Packet p(0x438);
    p.writeUInt32(1);
    p.writePackedGuid(0x21); p.writeUInt8(70);
    CalendarData cal;
    CalendarEventDetail open;
    CalendarContext ctx{cal, open, kMe, 80};
    ctx.nameKnown = [](uint64_t) { return false; };
    REQUIRE(applyCalendarPacket(CalendarPacket::FilterGuild, p, ctx));
    // The player's own row, added after, is not asked for.
    CHECK(ctx.nameQueries == std::vector<uint64_t>{0x21});
    CHECK(names(ctx) == std::vector<std::string>{"CALENDAR_ACTION_PENDING"});
    std::vector<CalendarSignal> said;
    calendarNameArrived(open, 0x21, said);
    REQUIRE(said.size() == 1);
    CHECK(said[0].event == "CALENDAR_UPDATE_INVITE_LIST");
    CHECK(said[0].args == std::vector<std::string>{"1"});
}

TEST_CASE("The calendar drops events the player has no invite to", "[calendar]") {
    // 0x005c29c0: a guild announcement needs no invite and a guild event
    // without one is listed not signed up; any other event is dropped.
    Packet p(0x436);
    p.writeUInt32(1);                       // invites: event 3 only
    p.writeUInt64(3); p.writeUInt64(503);
    p.writeUInt8(calendar_status::kInvited); p.writeUInt8(0); p.writeUInt8(0);
    p.writePackedGuid(0x42);
    p.writeUInt32(4);                       // events
    const std::pair<uint64_t, uint32_t> rows[] = {
        {1, 0},                             // no invite: dropped
        {2, calendar_flag::kGuildEvent},    // no invite: not signed up
        {3, 0},                             // invited
        {4, calendar_flag::kAnnouncement},  // needs none
    };
    for (const auto& [id, flags] : rows) {
        p.writeUInt64(id);
        p.writeString("e");
        p.writeUInt32(0);
        p.writeUInt32(kOct10);
        p.writeUInt32(flags);
        p.writeUInt32(static_cast<uint32_t>(-1));
        p.writePackedGuid(0x55);
    }
    p.writeUInt32(1300000000u);
    p.writeUInt32(kOct1);
    p.writeUInt32(0);                       // lockouts
    p.writeUInt32(0); p.writeUInt32(0);     // reset time, resets
    p.writeUInt32(0);                       // holidays
    CalendarData cal;
    CalendarEventDetail open;
    CalendarContext ctx{cal, open, kMe};
    REQUIRE(applyCalendarPacket(CalendarPacket::SendCalendar, p, ctx));
    CHECK(p.getRemainingSize() == 0);
    REQUIRE(cal.events.size() == 3);
    CHECK(cal.events[0].eventId == 2);
    CHECK(cal.events[1].eventId == 3);
    CHECK(cal.events[2].eventId == 4);
    bool guildRow = false;
    for (const auto& inv : cal.invites) {
        if (inv.eventId != 2) continue;
        guildRow = true;
        CHECK(inv.status == calendar_status::kNotSignedUp);
        CHECK(inv.isGuildEvent);
        CHECK(inv.creatorGuid == 0x55);
    }
    CHECK(guildRow);
    REQUIRE(cal.pendingInvites.size() == 1);
    CHECK(cal.pendingInvites[0].eventId == 3);
    CHECK(cal.pendingInvites[0].sender == 0x42);
    CHECK(names(ctx) == std::vector<std::string>{"CALENDAR_UPDATE_EVENT_LIST",
                                                 "CALENDAR_UPDATE_PENDING_INVITES",
                                                 "CALENDAR_ACTION_PENDING"});
}

TEST_CASE("An invite from someone ignored pends without being counted", "[calendar]") {
    // 0x005c3ab0 keeps the node but says nothing when 0x006b52e0 finds the
    // sender ignored, and 0x005bec70 leaves it out of the count.
    Packet p(0x440);
    p.writeUInt64(31);
    p.writeString("Ulduar");
    p.writeUInt32(kOct10);
    p.writeUInt32(0); p.writeUInt32(0); p.writeUInt32(603);
    p.writeUInt64(600);
    p.writeUInt8(calendar_status::kInvited);
    p.writeUInt8(0);
    p.writePackedGuid(0x42);
    p.writePackedGuid(0x43);
    CalendarData cal;
    CalendarEventDetail open;
    CalendarContext ctx{cal, open, kMe};
    const auto ignoring = [](uint64_t guid) { return guid == 0x43; };
    ctx.isIgnored = ignoring;
    REQUIRE(applyCalendarPacket(CalendarPacket::InviteAlert, p, ctx));
    CHECK(names(ctx) == std::vector<std::string>{"CALENDAR_UPDATE_EVENT_LIST"});
    REQUIRE(cal.pendingInvites.size() == 1);
    CHECK(cal.pendingInvites[0].sender == 0x43);
    CHECK(cal.pendingInviteCount(ignoring) == 0);
    CHECK(cal.pendingInviteCount() == 1);
}

TEST_CASE("A removed invite that leaves a mail looks for it", "[calendar]") {
    // 0x005c0ae0 calls 0x005704d0 unless the status was invited or removed,
    // after the open event closes and before the pending invite goes.
    const auto signalsFor = [](uint8_t status) {
        Packet p(0x441);
        p.writeUInt64(9); p.writeUInt32(kOct10); p.writeUInt32(0);
        p.writeUInt8(status);
        CalendarData cal = calendarWithInvite(9);
        CalendarEventDetail open;
        open.eventId = 9;
        CalendarContext ctx{cal, open, kMe};
        REQUIRE(applyCalendarPacket(CalendarPacket::InviteRemovedAlert, p, ctx));
        return names(ctx);
    };
    const std::vector<std::string> withMail{"CALENDAR_CLOSE_EVENT", kCalendarCheckMail,
                                            "CALENDAR_UPDATE_PENDING_INVITES",
                                            "CALENDAR_UPDATE_EVENT_LIST"};
    const std::vector<std::string> withoutMail{"CALENDAR_CLOSE_EVENT",
                                               "CALENDAR_UPDATE_PENDING_INVITES",
                                               "CALENDAR_UPDATE_EVENT_LIST"};
    CHECK(signalsFor(calendar_status::kAccepted) == withMail);
    CHECK(signalsFor(calendar_status::kConfirmed) == withMail);
    CHECK(signalsFor(calendar_status::kInvited) == withoutMail);
    CHECK(signalsFor(calendar_status::kRemoved) == withoutMail);
}

TEST_CASE("Invitations are two seconds apart", "[calendar]") {
    // 0x005bc360 / 0x005b8b30 against 0x00c207f8: none sent yet never holds,
    // and the wait is measured as the client's signed difference.
    CHECK_FALSE(calendarInviteThrottled(0, 500));
    CHECK(calendarInviteThrottled(10000, 10000));
    CHECK(calendarInviteThrottled(10000, 11999));
    CHECK_FALSE(calendarInviteThrottled(10000, 12000));
    CHECK(calendarInviteThrottled(0xFFFFFF00u, 0x00000100u));   // across the wrap
    CHECK_FALSE(calendarInviteThrottled(0xFFFFFF00u, 0x00000800u));
}

TEST_CASE("Events added are five seconds apart", "[calendar]") {
    // 0x005bbd60 / 0x005b8ba0 against 0x00c207fc.
    CHECK_FALSE(calendarAddThrottled(0, 500));
    CHECK(calendarAddThrottled(10000, 10000));
    CHECK(calendarAddThrottled(10000, 14999));
    CHECK_FALSE(calendarAddThrottled(10000, 15000));
    CHECK(calendarAddThrottled(0xFFFFFF00u, 0x00000100u));   // across the wrap
}

TEST_CASE("Who may invite to an opened event", "[calendar]") {
    // 0x005f2c00 keeps the player's own invite id only when their row carries
    // a rank; 0x005f1b30 lets them invite with one, or as the event's maker.
    constexpr uint64_t me = 0x42, maker = 0x77;
    CalendarEventDetail open;
    open.eventId = 900;
    open.creatorGuid = maker;
    CalendarEventInvitee mine;
    mine.guid = me;
    mine.inviteId = 31;
    open.invitees.push_back(mine);
    CHECK(calendarOwnInviteId(open, me) == 0u);   // a plain invitee
    CHECK_FALSE(calendarMayInvite(calendarOwnInviteId(open, me), maker, me));
    open.invitees[0].rank = 1;                    // moderator
    CHECK(calendarOwnInviteId(open, me) == 31u);
    CHECK(calendarMayInvite(calendarOwnInviteId(open, me), maker, me));
    // Not on the list at all, but its maker.
    CHECK(calendarOwnInviteId(open, maker) == 0u);
    CHECK(calendarMayInvite(0, maker, maker));
    // A new event keeps invite id 1 (0x005f2a20).
    CHECK(calendarMayInvite(1, 0, me));
}

TEST_CASE("A new event lists its maker, and an announcement nobody", "[calendar]") {
    // 0x005f2a20 with 0, 1, 2 (0x005bbe50, 0x005bbf20, 0x005bc030).
    const CalendarEventDetail mine = calendarNewEvent(CalendarNewKind::Player, kMe, 80);
    CHECK(mine.eventId == 0u);
    CHECK(mine.creatorGuid == kMe);
    CHECK(mine.flags == calendar_flag::kPlayer);
    CHECK(mine.maxInvites == 100u);
    CHECK(mine.dungeonId == -1);
    REQUIRE(mine.invitees.size() == 1);
    CHECK(mine.invitees[0].guid == kMe);
    CHECK(mine.invitees[0].level == 80);
    CHECK(mine.invitees[0].status == calendar_status::kConfirmed);
    CHECK(mine.invitees[0].rank == 2);
    CHECK_FALSE(mine.invitees[0].isGuildMember);

    const CalendarEventDetail guild = calendarNewEvent(CalendarNewKind::GuildEvent, kMe, 80);
    CHECK(guild.flags == calendar_flag::kGuildEvent);
    REQUIRE(guild.invitees.size() == 1);
    CHECK(guild.invitees[0].isGuildMember);

    const CalendarEventDetail news = calendarNewEvent(CalendarNewKind::Announcement, kMe, 80);
    CHECK(news.flags == calendar_flag::kAnnouncement);
    CHECK(news.invitees.empty());
}

TEST_CASE("A pre-invite joins the new event's list", "[calendar]") {
    // The server answers an invitation to an event not yet made with event
    // id 0, which is the new event's: 0x005bf030 adds it (0x005f32b0).
    auto preInvite = [] {
        Packet inv(0x43A);
        inv.writePackedGuid(0x21);
        inv.writeUInt64(0); inv.writeUInt64(0);
        inv.writeUInt8(70); inv.writeUInt8(calendar_status::kInvited);
        inv.writeUInt8(0); inv.writeUInt8(1);
        return inv;
    };
    CalendarData cal;
    CalendarEventDetail open = calendarNewEvent(CalendarNewKind::Player, kMe, 80);
    {
        Packet p = preInvite();
        CalendarContext ctx{cal, open, kMe};
        ctx.newEventOpen = true;
        REQUIRE(applyCalendarPacket(CalendarPacket::EventInvite, p, ctx));
        CHECK(names(ctx) == std::vector<std::string>{"CALENDAR_UPDATE_INVITE_LIST",
                                                     "CALENDAR_ACTION_PENDING"});
    }
    REQUIRE(open.invitees.size() == 2);
    CHECK(open.invitees[1].guid == 0x21u);
    CHECK(open.invitees[1].level == 70);
    CHECK(open.invitees[1].rank == 0);
    // With no new event open, an id of 0 is nobody's.
    CalendarEventDetail none;
    Packet p = preInvite();
    CalendarContext ctx{cal, none, kMe};
    REQUIRE(applyCalendarPacket(CalendarPacket::EventInvite, p, ctx));
    CHECK(none.invitees.empty());
}

TEST_CASE("The dates an event may be added on", "[calendar]") {
    auto date = [](int y, int m, int d, int h = 0, int mi = 0) {
        WowDate w;
        w.yearSince2000 = y - 2000; w.month = m; w.day = d; w.hour = h; w.minute = mi;
        return w;
    };
    // 0x005b7b30: from 24 November 2004 to 1 January 2031, with no clock.
    CHECK_FALSE(calendarDateAddable(date(2004, 11, 23), nullptr));
    CHECK(calendarDateAddable(date(2004, 11, 24), nullptr));
    CHECK(calendarDateAddable(date(2031, 1, 1, 23, 59), nullptr));
    CHECK_FALSE(calendarDateAddable(date(2031, 1, 2), nullptr));
    // 0x005b7c00: today at the earliest, whatever the hour, and the first of
    // the month thirteen on (0x005b7a80) at the latest.
    const WowDate now = date(2026, 10, 15, 18, 30);
    CHECK_FALSE(calendarDateAddable(date(2026, 10, 14, 23, 59), &now));
    CHECK(calendarDateAddable(date(2026, 10, 15, 0, 0), &now));
    CHECK(calendarDateAddable(date(2027, 11, 1, 23, 59), &now));
    CHECK_FALSE(calendarDateAddable(date(2027, 11, 2), &now));
    // From December the month thirteen on is January two years ahead.
    const WowDate december = date(2026, 12, 31);
    CHECK(calendarDateAddable(date(2028, 1, 1), &december));
    CHECK_FALSE(calendarDateAddable(date(2028, 1, 2), &december));
    // 0x0076ccc0: now, or before, has passed.
    CHECK(calendarTimePassed(now, &now));
    CHECK(calendarTimePassed(date(2026, 10, 15, 18, 29), &now));
    CHECK_FALSE(calendarTimePassed(date(2026, 10, 15, 18, 31), &now));
    CHECK_FALSE(calendarTimePassed(now, nullptr));
}

TEST_CASE("CMSG_CALENDAR_ADD_EVENT carries the invite list", "[calendar]") {
    // 0x005f1b90: the body, then the count and each row's packed guid,
    // status and rank - none for an announcement.
    CalendarEventDraft d;
    d.title = "Raid";
    d.description = "";
    d.type = 1;
    d.repeatOption = 0;
    d.maxInvites = 25;
    d.dungeonId = 533;
    d.eventTime.yearSince2000 = 26; d.eventTime.month = 10; d.eventTime.day = 20;
    d.eventTime.hour = 20;
    CalendarEventDetail ev = calendarNewEvent(CalendarNewKind::Player, kMe, 80);
    CalendarEventInvitee other;
    other.guid = 0x21;
    ev.invitees.push_back(other);

    Packet p(0x42D);
    writeCalendarAddEvent(p, d, ev.flags, ev.invitees);
    CHECK(p.readString() == "Raid");
    CHECK(p.readString().empty());
    CHECK(p.readUInt8() == 1);
    CHECK(p.readUInt8() == 0);
    CHECK(p.readUInt32() == 25u);
    CHECK(p.readUInt32() == 533u);
    CHECK(p.readUInt32() == packWowPackedTime(d.eventTime));
    // No lockout set: every field of it -1, all ones (0x0076ca50).
    CHECK(p.readUInt32() == 0x1FFFFFFFu);
    CHECK(p.readUInt32() == calendar_flag::kPlayer);
    REQUIRE(p.readUInt32() == 2u);
    CHECK(p.readPackedGuid() == kMe);
    CHECK(p.readUInt8() == calendar_status::kConfirmed);
    CHECK(p.readUInt8() == 2);
    CHECK(p.readPackedGuid() == 0x21u);
    CHECK(p.readUInt8() == 0);
    CHECK(p.readUInt8() == 0);
    CHECK_FALSE(p.hasRemaining(1));

    Packet a(0x42D);
    writeCalendarAddEvent(a, d, calendar_flag::kAnnouncement, ev.invitees);
    a.readString(); a.readString(); a.readUInt8(); a.readUInt8();
    a.readUInt32(); a.readUInt32(); a.readUInt32(); a.readUInt32();
    CHECK(a.readUInt32() == calendar_flag::kAnnouncement);
    CHECK_FALSE(a.hasRemaining(1));
}

TEST_CASE("The lockout time packs its unset parts as all ones", "[calendar]") {
    CalendarEventDraft d;
    CHECK(calendarLockoutPacked(d) == 0x1FFFFFFFu);
    d.lockoutTime.hour = 23;
    d.lockoutTime.minute = 59;
    d.hasLockoutTime = true;
    CHECK(calendarLockoutPacked(d) == (0x1FFFF800u | (23u << 6) | 59u));
    d.lockoutTime.yearSince2000 = 10;
    d.lockoutTime.month = 3;
    d.lockoutTime.day = 4;
    d.lockoutTime.weekday = 4;
    d.hasLockoutDate = true;
    CHECK(calendarLockoutPacked(d) == packWowPackedTime(d.lockoutTime));
}

TEST_CASE("An event's invite flags and status as the calendar keeps them", "[calendar]") {
    constexpr uint64_t kMe = 0x11;
    CalendarData cal;
    CalendarEvent ev;
    ev.eventId = 7;
    ev.creatorGuid = 0x22;
    ev.eventTime.yearSince2000 = 10;
    ev.eventTime.month = 5;
    ev.eventTime.day = 2;
    WowDate before = ev.eventTime;
    before.day = 1;
    WowDate after = ev.eventTime;
    after.day = 3;

    // No invitation: dropped, unless a guild event.
    CHECK_FALSE(calendarInviteState(cal, ev, kMe, &before).kept);
    ev.flags = calendar_flag::kGuildEvent;
    auto s = calendarInviteState(cal, ev, kMe, &before);
    CHECK(s.kept);
    CHECK(s.flags == 8u);
    CHECK(s.status == calendar_status::kNotSignedUp);

    ev.flags = 0;
    CalendarInvite inv;
    inv.eventId = 7;
    inv.status = calendar_status::kInvited;
    cal.invites.push_back(inv);
    CHECK(calendarInviteState(cal, ev, kMe, &before).flags == 1u);
    CHECK(calendarInviteState(cal, ev, kMe, &after).flags == 0u);
    CHECK(calendarInviteState(cal, ev, kMe, nullptr).flags == 0u);
    cal.invites[0].rank = 1;
    CHECK(calendarInviteState(cal, ev, kMe, &before).flags == 3u);
    cal.invites[0].rank = 2;
    cal.invites[0].status = calendar_status::kAccepted;
    CHECK(calendarInviteState(cal, ev, kMe, &before).flags == 6u);
    ev.creatorGuid = kMe;
    cal.invites[0].rank = 0;
    s = calendarInviteState(cal, ev, kMe, &before);
    CHECK(s.flags == 4u);
    CHECK(s.status == calendar_status::kAccepted);
}
