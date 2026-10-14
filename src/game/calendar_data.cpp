#include "game/calendar_data.hpp"

#include <algorithm>
#include <ctime>

#include "game/handler_types.hpp"
#include "network/packet.hpp"
#include "core/local_time.hpp"

namespace wowee {
namespace game {

namespace {

/// The smallest a row of each list can be, used to reject a count before any
/// of it is reserved. A packed guid is one byte at its shortest and a string
/// is one byte when empty, so these are lower bounds rather than sizes.
constexpr size_t kMinInviteBytes  = 8 + 8 + 1 + 1 + 1 + 1;
constexpr size_t kMinEventBytes   = 8 + 1 + 4 + 4 + 4 + 4 + 1;
constexpr size_t kLockoutBytes    = 4 + 4 + 4 + 8;
constexpr size_t kResetBytes      = 4 + 4 + 4;
constexpr size_t kMinHolidayBytes =
    5 * 4 + (CalendarHoliday::kNumDates + CalendarHoliday::kNumDurations +
             CalendarHoliday::kNumFlags) * 4 + 1;

/// A count is only believed if the packet is long enough to hold that many
/// rows at their smallest. Without this a corrupt uint32 reserves billions of
/// rows before the first read fails.
bool countFits(const network::Packet& packet, uint32_t count, size_t minRow) {
    return count == 0 || (minRow != 0 && count <= packet.getRemainingSize() / minRow);
}

/// A string, but only if its terminator is actually in the buffer.
///
/// Packet::readString stops at the end of the data as well as at a NUL, so a
/// name whose last bytes were cut off comes back looking like a complete one
/// and the parse reports success on a packet that is missing its tail. Every
/// truncation inside the final holiday's texture name read that way - 19 of
/// them, all reported clean.
bool readTerminatedString(network::Packet& packet, std::string& out) {
    const std::vector<uint8_t>& bytes = packet.getData();
    size_t at = packet.getReadPos();
    while (at < bytes.size() && bytes[at] != 0) ++at;
    if (at >= bytes.size()) return false;
    out = packet.readString();
    return true;
}

}  // namespace

bool parseCalendarSendCalendar(network::Packet& packet, CalendarData& out) {
    out = CalendarData{};

    if (!packet.hasRemaining(4)) return false;
    const uint32_t numInvites = packet.readUInt32();
    if (!countFits(packet, numInvites, kMinInviteBytes)) return false;
    out.invites.reserve(numInvites);
    for (uint32_t i = 0; i < numInvites; ++i) {
        if (!packet.hasRemaining(8 + 8 + 1 + 1 + 1)) return false;
        CalendarInvite inv;
        inv.eventId = packet.readUInt64();
        inv.inviteId = packet.readUInt64();
        inv.status = packet.readUInt8();
        inv.rank = packet.readUInt8();
        inv.isGuildEvent = packet.readUInt8() != 0;
        // The creator when the event still exists, the sender when it does
        // not - the server writes one field for both cases and there is
        // nothing in the packet that says which it chose.
        if (!packet.hasFullPackedGuid()) return false;
        inv.creatorGuid = packet.readPackedGuid();
        out.invites.push_back(inv);
    }

    if (!packet.hasRemaining(4)) return false;
    const uint32_t numEvents = packet.readUInt32();
    if (!countFits(packet, numEvents, kMinEventBytes)) return false;
    out.events.reserve(numEvents);
    for (uint32_t i = 0; i < numEvents; ++i) {
        if (!packet.hasRemaining(8 + 1)) return false;
        CalendarEvent ev;
        ev.eventId = packet.readUInt64();
        if (!readTerminatedString(packet, ev.title)) return false;
        if (!packet.hasRemaining(4 + 4 + 4 + 4)) return false;
        ev.type = packet.readUInt32();
        ev.eventTimePacked = packet.readUInt32();
        ev.eventTime = unpackWowPackedTime(ev.eventTimePacked);
        ev.flags = packet.readUInt32();
        ev.dungeonId = static_cast<int32_t>(packet.readUInt32());
        if (!packet.hasFullPackedGuid()) return false;
        ev.creatorGuid = packet.readPackedGuid();
        out.events.push_back(ev);
    }

    if (!packet.hasRemaining(4 + 4)) return false;
    out.serverTimeUnix = packet.readUInt32();
    out.zoneTime = unpackWowPackedTime(packet.readUInt32());

    if (!packet.hasRemaining(4)) return false;
    const uint32_t numLockouts = packet.readUInt32();
    if (!countFits(packet, numLockouts, kLockoutBytes)) return false;
    out.lockouts.reserve(numLockouts);
    for (uint32_t i = 0; i < numLockouts; ++i) {
        if (!packet.hasRemaining(kLockoutBytes)) return false;
        CalendarRaidLockout lock;
        lock.mapId = packet.readUInt32();
        lock.difficulty = packet.readUInt32();
        lock.secondsRemaining = packet.readUInt32();
        // A full guid rather than a packed one. The server writes this as an
        // ObjectGuid straight into the buffer, not WriteAsPacked like the two
        // creator fields above it.
        lock.instanceGuid = packet.readUInt64();
        // From the zone time just read, in whole minutes (0x005c29c0).
        lock.expires = addMinutes(out.zoneTime, lock.secondsRemaining / 60);
        out.lockouts.push_back(lock);
    }

    if (!packet.hasRemaining(4 + 4)) return false;
    out.resetRelationTime = packet.readUInt32();
    const uint32_t numResets = packet.readUInt32();
    if (!countFits(packet, numResets, kResetBytes)) return false;
    out.resets.reserve(numResets);
    for (uint32_t i = 0; i < numResets; ++i) {
        if (!packet.hasRemaining(kResetBytes)) return false;
        CalendarRaidReset reset;
        reset.mapId = static_cast<int32_t>(packet.readUInt32());
        reset.periodSeconds = static_cast<int32_t>(packet.readUInt32());
        reset.offsetSeconds = static_cast<int32_t>(packet.readUInt32());
        out.resets.push_back(reset);
    }

    if (!packet.hasRemaining(4)) return false;
    const uint32_t numHolidays = packet.readUInt32();
    if (!countFits(packet, numHolidays, kMinHolidayBytes)) return false;
    out.holidays.reserve(numHolidays);
    for (uint32_t i = 0; i < numHolidays; ++i) {
        if (!packet.hasRemaining(kMinHolidayBytes - 1)) return false;
        CalendarHoliday holiday;
        holiday.id = packet.readUInt32();
        holiday.region = packet.readUInt32();
        holiday.looping = packet.readUInt32();
        holiday.priority = packet.readUInt32();
        holiday.calendarFilterType = packet.readUInt32();
        for (uint32_t& date : holiday.dates) date = packet.readUInt32();
        for (uint32_t& dur : holiday.durations) dur = packet.readUInt32();
        for (uint32_t& flag : holiday.calendarFlags) flag = packet.readUInt32();
        if (!readTerminatedString(packet, holiday.textureFilename)) return false;
        out.holidays.push_back(std::move(holiday));
    }

    return true;
}


namespace {

/// A day number that can be compared and subtracted across month ends, so
/// "does this holiday still run on the 3rd" does not need a calendar walk.
/// Any consistent epoch will do - only differences are used.
int64_t dayNumber(int year, int month, int day) {
    // Howard Hinnant's days_from_civil, which is exact for the proleptic
    // Gregorian calendar the server's clock uses.
    year -= month <= 2;
    const int64_t era = (year >= 0 ? year : year - 399) / 400;
    const int64_t yoe = year - era * 400;
    const int64_t doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

/// The civil date of a day number, the inverse of dayNumber (Hinnant's
/// civil_from_days).
void civilFromDays(int64_t z, int& year, int& month, int& day) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const int64_t doe = z - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp = (5 * doy + 2) / 153;
    day = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    month = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
    year = static_cast<int>(yoe + era * 400 + (month <= 2 ? 1 : 0));
}

}  // namespace

WowDate addMinutes(const WowDate& date, int64_t minutes) {
    const int64_t start = dayNumber(date.fullYear(), date.month, date.day) * 1440 +
                          date.hour * 60 + date.minute;
    const int64_t total = start + minutes;
    int64_t days = total / 1440;
    int64_t rest = total % 1440;
    if (rest < 0) { rest += 1440; --days; }
    WowDate out;
    int y = 0, m = 0, d = 0;
    civilFromDays(days, y, m, d);
    out.yearSince2000 = y - 2000;
    out.month = m;
    out.day = d;
    out.hour = static_cast<int>(rest / 60);
    out.minute = static_cast<int>(rest % 60);
    // 1970-01-01 was a Thursday, day number 0.
    out.weekday = static_cast<int>(((days % 7) + 7 + 4) % 7);
    return out;
}

std::vector<CalendarDayEntry> calendarEntriesForDay(const CalendarData& data,
                                                    int month, int day,
                                                    int year) {
    std::vector<CalendarDayEntry> out;
    const int64_t wanted = dayNumber(year, month, day);

    for (size_t h = 0; h < data.holidays.size(); ++h) {
        const CalendarHoliday& holiday = data.holidays[h];
        for (int i = 0; i < CalendarHoliday::kNumDates; ++i) {
            if (holiday.dates[i] == 0) continue;
            const WowDate start = unpackWowPackedTime(holiday.dates[i]);
            const int64_t first = dayNumber(start.fullYear(), start.month, start.day);
            if (wanted < first) continue;
            // Hours, rounded up to whole days and never less than one: a
            // holiday sent with a duration of zero still happens on its day.
            const uint32_t hours = (i < CalendarHoliday::kNumDurations)
                                       ? holiday.durations[i] : 0;
            const int64_t days = hours > 0 ? static_cast<int64_t>((hours + 23) / 24) : 1;
            if (wanted >= first + days) continue;
            CalendarDayEntry entry;
            entry.kind = CalendarEntryKind::Holiday;
            entry.index = h;
            entry.ongoing = wanted > first;
            out.push_back(entry);
            break;  // one row per holiday per day, whichever run covers it
        }
    }

    // Raid lockouts, on the day and at the time they run out. The packets
    // date a lockout only by how long it has left, and the client adds that
    // to the zone time the same packet carries (0x005c29c0) - the server's
    // own wall clock in the realm's zone, not this machine's.
    for (size_t l = 0; l < data.lockouts.size(); ++l) {
        const WowDate& when = data.lockouts[l].expires;
        if (dayNumber(when.fullYear(), when.month, when.day) != wanted) continue;
        CalendarDayEntry entry;
        entry.kind = CalendarEntryKind::RaidLockout;
        entry.index = l;
        entry.hour = when.hour;
        entry.minute = when.minute;
        out.push_back(entry);
    }

    for (size_t e = 0; e < data.events.size(); ++e) {
        const WowDate when = data.events[e].eventTime;
        if (when.month != month || when.day != day || when.fullYear() != year) {
            continue;
        }
        CalendarDayEntry entry;
        entry.kind = CalendarEntryKind::Event;
        entry.index = e;
        out.push_back(entry);
    }

    // Events by time of day, holidays left where they are at the front.
    std::stable_sort(out.begin(), out.end(),
                     [&data](const CalendarDayEntry& a, const CalendarDayEntry& b) {
                         if (a.kind != b.kind) {
                             // The enum is in drawing order: holidays head the
                             // day, then raid lockouts, then the player's own
                             // events.
                             return a.kind < b.kind;
                         }
                         if (a.kind != CalendarEntryKind::Event) return false;
                         const WowDate& ta = data.events[a.index].eventTime;
                         const WowDate& tb = data.events[b.index].eventTime;
                         if (ta.hour != tb.hour) return ta.hour < tb.hour;
                         return ta.minute < tb.minute;
                     });
    return out;
}

bool parseCalendarSendEvent(network::Packet& packet, CalendarEventDetail& out) {
    out = CalendarEventDetail{};

    if (!packet.hasRemaining(1)) return false;
    out.sendType = packet.readUInt8();
    if (!packet.hasFullPackedGuid()) return false;
    out.creatorGuid = packet.readPackedGuid();
    if (!packet.hasRemaining(8 + 1)) return false;
    out.eventId = packet.readUInt64();
    if (!readTerminatedString(packet, out.title)) return false;
    if (!readTerminatedString(packet, out.description)) return false;
    if (!packet.hasRemaining(1 + 1 + 4 + 4 + 4 + 4 + 4 + 4)) return false;
    out.type = packet.readUInt8();
    out.repeatOption = packet.readUInt8();
    out.maxInvites = packet.readUInt32();
    out.dungeonId = static_cast<int32_t>(packet.readUInt32());
    out.flags = packet.readUInt32();
    out.eventTimePacked = packet.readUInt32();
    out.eventTime = unpackWowPackedTime(out.eventTimePacked);
    out.timeZoneTime = unpackWowPackedTime(packet.readUInt32());
    out.guildId = packet.readUInt32();

    if (!packet.hasRemaining(4)) return false;
    const uint32_t numInvitees = packet.readUInt32();
    // A guid is one byte at its shortest and the note one when empty, so this
    // is a lower bound rather than a row size.
    constexpr size_t kMinInviteeBytes = 1 + 1 + 1 + 1 + 1 + 8 + 4 + 1;
    if (!countFits(packet, numInvitees, kMinInviteeBytes)) return false;
    out.invitees.reserve(numInvitees);
    for (uint32_t i = 0; i < numInvitees; ++i) {
        CalendarEventInvitee inv;
        if (!packet.hasFullPackedGuid()) return false;
        inv.guid = packet.readPackedGuid();
        if (!packet.hasRemaining(1 + 1 + 1 + 1 + 8 + 4)) return false;
        inv.level = packet.readUInt8();
        inv.status = packet.readUInt8();
        inv.rank = packet.readUInt8();
        inv.isGuildMember = packet.readUInt8() != 0;
        inv.inviteId = packet.readUInt64();
        inv.statusTime = unpackWowPackedTime(packet.readUInt32());
        if (!readTerminatedString(packet, inv.note)) return false;
        out.invitees.push_back(std::move(inv));
    }
    return true;
}


const char* calendarTypeForFlags(uint32_t flags) {
    using namespace calendar_flag;
    // 0x005b7fd0, in its order: the player bit wins outright, then the guild
    // announcement, the guild event, system, holiday, raid lockout and raid
    // reset; anything left with 0x2 or 0x100 is the player's own.
    if (flags & kPlayer) return "PLAYER";
    if (flags & kAnnouncement) return "GUILD_ANNOUNCEMENT";
    if (flags & kGuildEvent) return "GUILD_EVENT";
    if (flags & kSystem) return "SYSTEM";
    if (flags & kHoliday) return "HOLIDAY";
    if (flags & kRaidLockout) return "RAID_LOCKOUT";
    if (flags & kRaidReset) return "RAID_RESET";
    if ((flags & 0x2u) == 0 && (flags & 0x100u) == 0) return "";
    return "PLAYER";
}

namespace {

/// Minutes since an arbitrary epoch, for ordering two dates.
int64_t minuteKey(const WowDate& d) {
    return dayNumber(d.fullYear(), d.month, d.day) * 1440 + d.hour * 60 + d.minute;
}

CalendarEvent* findEvent(CalendarData& cal, uint64_t eventId) {
    for (auto& ev : cal.events)
        if (ev.eventId == eventId) return &ev;
    return nullptr;
}

CalendarInvite* findInvite(CalendarData& cal, uint64_t eventId) {
    for (auto& inv : cal.invites)
        if (inv.eventId == eventId) return &inv;
    return nullptr;
}

/// The invite half of an event's row - the client keeps the invite id, the
/// status and the sender on the event itself, so a row the calendar holds
/// always has one to write to.
CalendarInvite& inviteFor(CalendarData& cal, uint64_t eventId) {
    if (CalendarInvite* inv = findInvite(cal, eventId)) return *inv;
    CalendarInvite inv;
    inv.eventId = eventId;
    cal.invites.push_back(inv);
    return cal.invites.back();
}

/// 0x005becd0: the first pending entry for the event, if there is one.
bool removeFirstPending(CalendarData& cal, uint64_t eventId) {
    auto it = std::find_if(cal.pendingInvites.begin(), cal.pendingInvites.end(),
                           [eventId](const CalendarData::PendingInvite& p) {
                               return p.eventId == eventId;
                           });
    if (it == cal.pendingInvites.end()) return false;
    cal.pendingInvites.erase(it);
    return true;
}

/// Who an event's row says sent it (the row's 0xb8): the creator for a guild
/// sign-up, otherwise whoever the invite names. Nobody for an event the
/// calendar does not hold.
uint64_t rowSender(CalendarData& cal, uint64_t eventId) {
    const CalendarEvent* ev = findEvent(cal, eventId);
    if (!ev) return 0;
    const CalendarInvite* inv = findInvite(cal, eventId);
    if (!inv) return 0;
    return inv->isGuildEvent ? ev->creatorGuid : inv->creatorGuid;
}

/// 0x005be590: the event's row joins the pending list, with its sender.
void addPending(CalendarData& cal, uint64_t eventId) {
    cal.pendingInvites.push_back({eventId, rowSender(cal, eventId)});
}

bool ignored(const CalendarContext& ctx, uint64_t guid) {
    return ctx.isIgnored && ctx.isIgnored(guid);
}

/// 0x0067d770 with the event's callback (0x005f1b10): a name not yet known is
/// counted, and asked for. A zero guid is never known and never asked.
void awaitName(CalendarContext& ctx, uint64_t guid) {
    if (guid != 0 && (!ctx.nameKnown || ctx.nameKnown(guid))) return;
    ctx.open.namesAwaited.push_back(guid);
    if (guid != 0) ctx.nameQueries.push_back(guid);
}

/// 0x005f1a40's switch: what the open event says once its names are in.
void sayOnceNamed(const CalendarEventDetail& open, uint8_t what,
                  std::vector<CalendarSignal>& out) {
    switch (what) {
    case 0: out.push_back({"CALENDAR_UPDATE_INVITE_LIST", {}}); break;
    case 1: out.push_back({"CALENDAR_UPDATE_INVITE_LIST", {eventBool(true)}}); break;
    case 2: out.push_back({"CALENDAR_NEW_EVENT", {eventBool(false)}}); break;
    case 3: out.push_back({"CALENDAR_NEW_EVENT", {eventBool(true)}}); break;
    case 4: out.push_back({"CALENDAR_OPEN_EVENT", {calendarTypeForFlags(open.flags)}}); break;
    default: break;
    }
}

/// 0x005be950: the event out of the list, and its invite with it.
bool removeEvent(CalendarData& cal, uint64_t eventId) {
    auto it = std::find_if(cal.events.begin(), cal.events.end(),
                           [eventId](const CalendarEvent& ev) { return ev.eventId == eventId; });
    if (it == cal.events.end()) return false;
    cal.events.erase(it);
    cal.invites.erase(std::remove_if(cal.invites.begin(), cal.invites.end(),
                                     [eventId](const CalendarInvite& inv) {
                                         return inv.eventId == eventId;
                                     }),
                      cal.invites.end());
    return true;
}

CalendarEventInvitee* findInvitee(CalendarEventDetail& open, uint64_t guid) {
    for (auto& inv : open.invitees)
        if (inv.guid == guid) return &inv;
    return nullptr;
}

bool isOpen(const CalendarEventDetail& open, uint64_t eventId) {
    return open.eventId != 0 && open.eventId == eventId;
}

void say(CalendarContext& ctx, const char* event, std::vector<std::string> args = {}) {
    ctx.signals.push_back({event, std::move(args)});
}

bool readGuid(network::Packet& packet, uint64_t& out) {
    if (!packet.hasFullPackedGuid()) return false;
    out = packet.readPackedGuid();
    return true;
}

/// The "signed up through the guild" mark (the row's 0x9c bit 8): set for a
/// guild invite and for a guild event the player was never invited to.
bool guildSignupRow(const CalendarData& cal, uint64_t eventId) {
    for (const auto& inv : cal.invites)
        if (inv.eventId == eventId) return inv.isGuildEvent;
    return true;
}

}  // namespace

bool applyCalendarPacket(CalendarPacket which, network::Packet& packet,
                         CalendarContext& ctx) {
    using namespace calendar_flag;
    using namespace calendar_status;
    CalendarData& cal = ctx.cal;
    CalendarEventDetail& open = ctx.open;
    // The dispatcher's ending (0x005c3fe0): CALENDAR_ACTION_PENDING false,
    // unless the handler returned straight out or the packet's own byte said
    // to leave the pending action alone.
    bool clearsAction = true;

    switch (which) {
    case CalendarPacket::SendCalendar: {
        CalendarData parsed;
        if (!parseCalendarSendCalendar(packet, parsed)) return false;
        // 0x005c29c0 puts an event on the calendar only with its invite - a
        // guild announcement needs none, and a guild event without one is
        // there not signed up, sent by its creator. Any other event the
        // player has no invite to is dropped.
        std::vector<CalendarEvent> listed;
        listed.reserve(parsed.events.size());
        for (auto& ev : parsed.events) {
            if (!(ev.flags & kAnnouncement) && !findInvite(parsed, ev.eventId)) {
                if (!(ev.flags & kGuildEvent)) continue;
                CalendarInvite inv;
                inv.eventId = ev.eventId;
                inv.status = kNotSignedUp;
                inv.isGuildEvent = true;
                inv.creatorGuid = ev.creatorGuid;
                parsed.invites.push_back(inv);
            }
            listed.push_back(std::move(ev));
        }
        parsed.events = std::move(listed);
        // It zeroes the server's count first, then keeps a pending entry for
        // each invite still unanswered whose event is listed - not as an
        // announcement - still ahead and not locked, and whose invite names
        // a sender. One from a sender on the ignore list stays in the list
        // but is not counted (0x006b52e0).
        uint32_t counted = 0;
        for (const auto& inv : parsed.invites) {
            if (inv.status != kInvited) continue;
            const CalendarEvent* ev = findEvent(parsed, inv.eventId);
            if (!ev || (ev->flags & (kInvitesLocked | kAnnouncement))) continue;
            if (minuteKey(ev->eventTime) <= minuteKey(parsed.zoneTime)) continue;
            if (inv.creatorGuid == 0) continue;
            parsed.pendingInvites.push_back({inv.eventId, inv.creatorGuid});
            if (!ignored(ctx, inv.creatorGuid)) ++counted;
        }
        parsed.serverPendingCount = 0;
        parsed.received = true;
        cal = std::move(parsed);
        say(ctx, "CALENDAR_UPDATE_EVENT_LIST");
        if (counted != 0) say(ctx, "CALENDAR_UPDATE_PENDING_INVITES");
        break;
    }

    case CalendarPacket::SendEvent: {
        CalendarEventDetail parsed;
        if (!parseCalendarSendEvent(packet, parsed)) return false;
        // The player's own status off the invite list - a guild event they
        // are not on is "not signed up" (0x005f2c00).
        int myStatus = -1;
        for (const auto& inv : parsed.invitees)
            if (ctx.player != 0 && inv.guid == ctx.player) { myStatus = inv.status; break; }
        if (myStatus < 0 && (parsed.flags & kGuildEvent)) myStatus = kNotSignedUp;
        open = std::move(parsed);
        // Every invitee's name is looked up as the list is read (0x005f2c00).
        for (const auto& inv : open.invitees) awaitName(ctx, inv.guid);
        // An event that turns out to be waiting on the player's answer joins
        // the pending list, once (0x005bed20).
        const uint64_t eventId = open.eventId;
        if (myStatus == kInvited && minuteKey(open.eventTime) > minuteKey(cal.zoneTime) &&
            std::none_of(cal.pendingInvites.begin(), cal.pendingInvites.end(),
                         [eventId](const CalendarData::PendingInvite& p) {
                             return p.eventId == eventId;
                         })) {
            cal.pendingInvites.push_back({eventId, rowSender(cal, eventId)});
            say(ctx, "CALENDAR_UPDATE_PENDING_INVITES");
        }
        // Opened, or just made: CALENDAR_OPEN_EVENT with the calendar type,
        // CALENDAR_NEW_EVENT with whether it was a copy - held back while any
        // invitee's name is still being asked for, and said by the last
        // answer instead (0x005f1410, 0x005f1a40).
        const uint8_t what = open.sendType == 1 ? 2 : open.sendType == 2 ? 3 : 4;
        if (open.namesAwaited.empty()) sayOnceNamed(open, what, ctx.signals);
        else open.onceNamed = what;
        break;
    }

    case CalendarPacket::FilterGuild:
    case CalendarPacket::ArenaTeam: {
        // The invite list for the event being made (0x005f2f90): a count, then
        // a guid and a level each - in place of whatever list it had.
        if (!packet.hasRemaining(4)) return false;
        const uint32_t count = packet.readUInt32();
        if (count > packet.getRemainingSize() / 2) return false;
        std::vector<CalendarEventInvitee> rows;
        rows.reserve(count);
        bool listed = false;
        for (uint32_t i = 0; i < count; ++i) {
            CalendarEventInvitee row;
            if (!readGuid(packet, row.guid)) return false;
            if (!packet.hasRemaining(1)) return false;
            row.level = packet.readUInt8();
            if (ctx.player != 0 && row.guid == ctx.player) {
                row.status = kConfirmed;
                row.rank = 2;
                listed = true;
            }
            rows.push_back(std::move(row));
        }
        // The player is on it whatever the list said, confirmed and its owner.
        if (!listed) {
            CalendarEventInvitee me;
            me.guid = ctx.player;
            me.level = ctx.playerLevel;
            me.status = kConfirmed;
            me.rank = 2;
            rows.push_back(std::move(me));
        }
        // Each listed name is looked up, the player's own row added after
        // them is not; said now when every name is known, else by the last
        // answer (0x005f2f90).
        for (uint32_t i = 0; i < count; ++i) awaitName(ctx, rows[i].guid);
        open.invitees = std::move(rows);
        if (open.namesAwaited.empty()) say(ctx, "CALENDAR_UPDATE_INVITE_LIST", {eventBool(true)});
        else open.onceNamed = 1;
        break;
    }

    case CalendarPacket::EventInvite: {
        // 0x005bf030: invitee, event, invite, level, status, whether a status
        // time follows, the time, and the clear-pending byte.
        uint64_t guid = 0;
        if (!readGuid(packet, guid)) return false;
        if (!packet.hasRemaining(8 + 8 + 1 + 1 + 1)) return false;
        const uint64_t eventId = packet.readUInt64();
        const uint64_t inviteId = packet.readUInt64();
        const uint8_t level = packet.readUInt8();
        const uint8_t status = packet.readUInt8();
        const uint8_t hasTime = packet.readUInt8();
        uint32_t statusTime = 0;
        if (hasTime == 1) {
            if (!packet.hasRemaining(4)) return false;
            statusTime = packet.readUInt32();
        }
        if (!packet.hasRemaining(1)) return false;
        if (packet.readUInt8() == 0) clearsAction = false;
        if (hasTime == 1 && findEvent(cal, eventId)) {
            CalendarInvite& inv = inviteFor(cal, eventId);
            inv.inviteId = inviteId;
            inv.status = status;
            say(ctx, "CALENDAR_UPDATE_EVENT_LIST");
        }
        if (isOpen(open, eventId)) {
            CalendarEventInvitee row;
            row.guid = guid;
            row.inviteId = inviteId;
            row.level = level;
            row.status = status;
            // The player's own row is the owner's unless it came timed
            // (0x005f32b0).
            row.rank = (ctx.player != 0 && guid == ctx.player && hasTime != 1) ? 2 : 0;
            row.isGuildMember = hasTime != 0;
            row.statusTime = unpackWowPackedTime(statusTime);
            open.invitees.push_back(std::move(row));
            // Held back while any name on the list is still being asked for,
            // this one or an earlier one.
            awaitName(ctx, guid);
            if (open.namesAwaited.empty()) say(ctx, "CALENDAR_UPDATE_INVITE_LIST");
            else open.onceNamed = 0;
        }
        break;
    }

    case CalendarPacket::InviteRemoved: {
        // 0x005bf250: invitee, event, the event's flags, clear-pending.
        uint64_t guid = 0;
        if (!readGuid(packet, guid)) return false;
        if (!packet.hasRemaining(8 + 4 + 1)) return false;
        const uint64_t eventId = packet.readUInt64();
        const uint32_t flags = packet.readUInt32();
        if (packet.readUInt8() == 0) clearsAction = false;
        // A guild event stays on the calendar, not signed up.
        if ((flags & kGuildEvent) && findEvent(cal, eventId)) {
            CalendarInvite& inv = inviteFor(cal, eventId);
            inv.inviteId = 0;
            inv.status = kNotSignedUp;
            say(ctx, "CALENDAR_UPDATE_EVENT_LIST");
        }
        if (isOpen(open, eventId)) {
            open.invitees.erase(std::remove_if(open.invitees.begin(), open.invitees.end(),
                                               [guid](const CalendarEventInvitee& r) {
                                                   return r.guid == guid;
                                               }),
                                open.invitees.end());
            say(ctx, "CALENDAR_UPDATE_INVITE_LIST");
        }
        break;
    }

    case CalendarPacket::EventStatus: {
        // 0x005bf420: invitee, event, its time, its flags, the status,
        // clear-pending, the status time.
        uint64_t guid = 0;
        if (!readGuid(packet, guid)) return false;
        if (!packet.hasRemaining(8 + 4 + 4 + 1 + 1 + 4)) return false;
        const uint64_t eventId = packet.readUInt64();
        packet.readUInt32();  // event time
        packet.readUInt32();  // flags
        const uint8_t status = packet.readUInt8();
        if (packet.readUInt8() == 0) clearsAction = false;
        const uint32_t statusTime = packet.readUInt32();
        if (!findEvent(cal, eventId)) break;
        if (ctx.player != 0 && guid == ctx.player) {
            inviteFor(cal, eventId).status = status;
            if (status == kInvited) {
                addPending(cal, eventId);
                say(ctx, "CALENDAR_UPDATE_PENDING_INVITES");
            } else if (removeFirstPending(cal, eventId)) {
                say(ctx, "CALENDAR_UPDATE_PENDING_INVITES");
            }
            say(ctx, "CALENDAR_UPDATE_EVENT_LIST");
        }
        if (isOpen(open, eventId)) {
            if (CalendarEventInvitee* row = findInvitee(open, guid)) {
                row->status = status;
                if (statusTime != 0) row->statusTime = unpackWowPackedTime(statusTime);
                say(ctx, "CALENDAR_UPDATE_INVITE_LIST");
            }
        }
        break;
    }

    case CalendarPacket::LockoutAdded: {
        // 0x005c37d0: now, map, difficulty, seconds left, instance.
        if (!packet.hasRemaining(4 + 4 + 4 + 4 + 8)) return false;
        const WowDate now = unpackWowPackedTime(packet.readUInt32());
        CalendarRaidLockout lock;
        lock.mapId = packet.readUInt32();
        lock.difficulty = packet.readUInt32();
        lock.secondsRemaining = packet.readUInt32();
        lock.instanceGuid = packet.readUInt64();
        lock.expires = addMinutes(now, static_cast<int32_t>(lock.secondsRemaining) / 60);
        cal.lockouts.push_back(lock);
        say(ctx, "CALENDAR_UPDATE_EVENT_LIST");
        break;
    }

    case CalendarPacket::LockoutRemoved: {
        // 0x005c07e0: a map of -1 clears every lockout and is all there is;
        // otherwise difficulty, seconds and instance follow, -1 in either of
        // the first and last matching any.
        if (!packet.hasRemaining(4)) return false;
        const uint32_t mapId = packet.readUInt32();
        if (static_cast<int32_t>(mapId) < 0) {
            cal.lockouts.clear();
        } else {
            if (!packet.hasRemaining(4 + 4 + 8)) return false;
            const uint32_t difficulty = packet.readUInt32();
            packet.readUInt32();  // seconds
            const uint64_t instance = packet.readUInt64();
            cal.lockouts.erase(
                std::remove_if(cal.lockouts.begin(), cal.lockouts.end(),
                               [&](const CalendarRaidLockout& l) {
                                   if (l.mapId != mapId) return false;
                                   if (difficulty != 0xFFFFFFFFu && l.difficulty != difficulty) return false;
                                   if (instance != ~0ull && l.instanceGuid != instance) return false;
                                   return true;
                               }),
                cal.lockouts.end());
        }
        say(ctx, "CALENDAR_UPDATE_EVENT_LIST");
        break;
    }

    case CalendarPacket::LockoutUpdated: {
        // 0x005c3880: now, map, difficulty, the old seconds and the new.
        if (!packet.hasRemaining(4 * 5)) return false;
        const WowDate now = unpackWowPackedTime(packet.readUInt32());
        const uint32_t mapId = packet.readUInt32();
        const uint32_t difficulty = packet.readUInt32();
        packet.readUInt32();  // the old seconds
        const uint32_t seconds = packet.readUInt32();
        const WowDate expires = addMinutes(now, static_cast<int32_t>(seconds) / 60);
        for (auto& l : cal.lockouts) {
            if (l.mapId != mapId || l.difficulty != difficulty) continue;
            if (minuteKey(l.expires) == minuteKey(expires)) continue;
            l.expires = expires;
            l.secondsRemaining = seconds;
        }
        say(ctx, "CALENDAR_UPDATE_EVENT_LIST");
        break;
    }

    case CalendarPacket::InviteAlert: {
        // 0x005c3ab0: the event's row and the invite, then creator and sender.
        clearsAction = false;
        if (!packet.hasRemaining(8 + 1)) return false;
        CalendarEvent ev;
        ev.eventId = packet.readUInt64();
        if (!readTerminatedString(packet, ev.title)) return false;
        if (!packet.hasRemaining(4 * 4 + 8 + 1 + 1)) return false;
        ev.eventTimePacked = packet.readUInt32();
        ev.eventTime = unpackWowPackedTime(ev.eventTimePacked);
        ev.flags = packet.readUInt32();
        ev.type = packet.readUInt32();
        ev.dungeonId = static_cast<int32_t>(packet.readUInt32());
        CalendarInvite inv;
        inv.eventId = ev.eventId;
        inv.inviteId = packet.readUInt64();
        inv.status = packet.readUInt8();
        inv.rank = packet.readUInt8();
        uint64_t creator = 0, sender = 0;
        if (!readGuid(packet, creator) || !readGuid(packet, sender)) return false;
        ev.creatorGuid = creator;
        // A sign-up row names the creator and carries the guild mark; any
        // other names who sent it.
        const bool signup = inv.status == kNotSignedUp || inv.status == kSignedUp;
        inv.isGuildEvent = signup;
        inv.creatorGuid = signup ? creator : sender;
        removeEvent(cal, ev.eventId);
        cal.events.push_back(ev);
        cal.invites.push_back(inv);
        // Pending, with its sender - and said only when the sender is not
        // on the ignore list (0x006b52e0).
        if (inv.status == kInvited && !(ev.flags & kAnnouncement)) {
            cal.pendingInvites.push_back({ev.eventId, inv.creatorGuid});
            if (!ignored(ctx, inv.creatorGuid)) say(ctx, "CALENDAR_UPDATE_PENDING_INVITES");
        }
        say(ctx, "CALENDAR_UPDATE_EVENT_LIST");
        break;
    }

    case CalendarPacket::InviteRemovedAlert: {
        // 0x005c0ae0: event, its time, its flags, the status it was left at.
        clearsAction = false;
        if (!packet.hasRemaining(8 + 4 + 4 + 1)) return false;
        const uint64_t eventId = packet.readUInt64();
        packet.readUInt32();  // event time
        packet.readUInt32();  // flags
        const uint8_t status = packet.readUInt8();
        const CalendarEvent* ev = findEvent(cal, eventId);
        bool dropEvent = true;
        if (ev && (ev->flags & kGuildEvent) && guildSignupRow(cal, eventId)) {
            // A guild event the player signed up to stays, not signed up.
            inviteFor(cal, eventId).status = kNotSignedUp;
            if (isOpen(open, eventId) && ctx.player != 0) {
                open.invitees.erase(std::remove_if(open.invitees.begin(), open.invitees.end(),
                                                   [&](const CalendarEventInvitee& r) {
                                                       return r.guid == ctx.player;
                                                   }),
                                    open.invitees.end());
                say(ctx, "CALENDAR_UPDATE_INVITE_LIST");
            }
            say(ctx, "CALENDAR_UPDATE_EVENT_LIST");
            dropEvent = false;
        } else if (isOpen(open, eventId)) {
            open = CalendarEventDetail{};
            say(ctx, "CALENDAR_CLOSE_EVENT");
        }
        // Left at anything but invited or removed, the server mails the
        // player about it, and the client looks for mail as it does on
        // SMSG_RECEIVED_MAIL (0x005704d0) - before the pending invite goes.
        if (status != kInvited && status != kRemoved) say(ctx, kCalendarCheckMail);
        removeFirstPending(cal, eventId);
        say(ctx, "CALENDAR_UPDATE_PENDING_INVITES");
        if (dropEvent && removeEvent(cal, eventId)) say(ctx, "CALENDAR_UPDATE_EVENT_LIST");
        break;
    }

    case CalendarPacket::InviteStatusAlert: {
        // 0x005bf590: event, its time, its flags, the player's new status.
        clearsAction = false;
        if (!packet.hasRemaining(8 + 4 + 4 + 1)) return false;
        const uint64_t eventId = packet.readUInt64();
        packet.readUInt32();  // event time
        packet.readUInt32();  // flags
        const uint8_t status = packet.readUInt8();
        CalendarEvent* ev = findEvent(cal, eventId);
        if (!ev) {
            // An event this calendar has never seen: ask how many are waiting
            // instead, until the calendar itself has arrived.
            if (!cal.received) ctx.askNumPending = true;
            break;
        }
        inviteFor(cal, eventId).status = status;
        if (!(ev->flags & kInvitesLocked)) {
            if (status == kInvited) {
                addPending(cal, eventId);
                say(ctx, "CALENDAR_UPDATE_PENDING_INVITES");
            } else if (removeFirstPending(cal, eventId)) {
                say(ctx, "CALENDAR_UPDATE_PENDING_INVITES");
            }
        }
        if (isOpen(open, eventId) && ctx.player != 0) {
            if (CalendarEventInvitee* row = findInvitee(open, ctx.player)) {
                row->status = status;
                say(ctx, "CALENDAR_UPDATE_INVITE_LIST");
            }
        }
        say(ctx, "CALENDAR_UPDATE_EVENT_LIST");
        break;
    }

    case CalendarPacket::EventRemovedAlert: {
        // 0x005c0cf0: clear-pending, event, its time.
        if (!packet.hasRemaining(1 + 8 + 4)) return false;
        if (packet.readUInt8() == 0) clearsAction = false;
        const uint64_t eventId = packet.readUInt64();
        packet.readUInt32();
        if (isOpen(open, eventId)) {
            open = CalendarEventDetail{};
            say(ctx, "CALENDAR_CLOSE_EVENT");
        }
        if (removeEvent(cal, eventId)) {
            removeFirstPending(cal, eventId);
            say(ctx, "CALENDAR_UPDATE_PENDING_INVITES");
            say(ctx, "CALENDAR_UPDATE_EVENT_LIST");
        }
        break;
    }

    case CalendarPacket::EventUpdatedAlert: {
        // 0x005c3d30: clear-pending, event, its old time, then the row as it
        // now is - flags, time, type, dungeon, title - and, for the event that
        // is open, from the flags again with the description, repeat, the
        // invite limit and the lockout time (0x005f14b0).
        if (!packet.hasRemaining(1 + 8 + 4)) return false;
        if (packet.readUInt8() == 0) clearsAction = false;
        const uint64_t eventId = packet.readUInt64();
        packet.readUInt32();  // the old time
        const size_t rest = packet.getReadPos();
        if (!packet.hasRemaining(4 + 4 + 1 + 4)) return false;
        const uint32_t flags = packet.readUInt32();
        const uint32_t timePacked = packet.readUInt32();
        const uint8_t type = packet.readUInt8();
        const int32_t dungeonId = static_cast<int32_t>(packet.readUInt32());
        std::string title;
        if (!readTerminatedString(packet, title)) return false;
        if (CalendarEvent* ev = findEvent(cal, eventId)) {
            const bool wasLocked = (ev->flags & kInvitesLocked) != 0;
            const bool nowLocked = (flags & kInvitesLocked) != 0;
            ev->title = title;
            ev->flags = flags;
            ev->type = type;
            ev->dungeonId = dungeonId;
            ev->eventTimePacked = timePacked;
            ev->eventTime = unpackWowPackedTime(timePacked);
            const CalendarInvite* inv = findInvite(cal, eventId);
            if (wasLocked != nowLocked && inv && inv->status == kInvited) {
                if (!nowLocked) {
                    addPending(cal, eventId);
                    say(ctx, "CALENDAR_UPDATE_PENDING_INVITES");
                } else if (removeFirstPending(cal, eventId)) {
                    say(ctx, "CALENDAR_UPDATE_PENDING_INVITES");
                }
            }
            say(ctx, "CALENDAR_UPDATE_EVENT_LIST");
        } else if (!cal.received) {
            ctx.askNumPending = true;
        }
        if (isOpen(open, eventId)) {
            packet.setReadPos(rest);
            if (!packet.hasRemaining(4 + 4 + 1 + 4)) return false;
            const uint32_t oFlags = packet.readUInt32();
            const uint32_t oTime = packet.readUInt32();
            const uint8_t oType = packet.readUInt8();
            const int32_t oDungeon = static_cast<int32_t>(packet.readUInt32());
            std::string oTitle, oDesc;
            if (!readTerminatedString(packet, oTitle)) return false;
            if (!readTerminatedString(packet, oDesc)) return false;
            if (!packet.hasRemaining(1 + 4 + 4)) return false;
            open.flags = oFlags;
            open.eventTimePacked = oTime;
            open.eventTime = unpackWowPackedTime(oTime);
            open.type = oType;
            open.dungeonId = oDungeon;
            open.title = std::move(oTitle);
            open.description = std::move(oDesc);
            open.repeatOption = packet.readUInt8();
            open.maxInvites = packet.readUInt32();
            open.timeZoneTime = unpackWowPackedTime(packet.readUInt32());
            say(ctx, "CALENDAR_UPDATE_EVENT");
        } else {
            packet.skipAll();
        }
        break;
    }

    case CalendarPacket::ModeratorStatusAlert: {
        // 0x005c0e70: invitee, event, rank, clear-pending.
        uint64_t guid = 0;
        if (!readGuid(packet, guid)) return false;
        if (!packet.hasRemaining(8 + 1 + 1)) return false;
        const uint64_t eventId = packet.readUInt64();
        const uint8_t rank = packet.readUInt8();
        if (packet.readUInt8() == 0) clearsAction = false;
        if (isOpen(open, eventId)) {
            if (CalendarEventInvitee* row = findInvitee(open, guid)) {
                row->rank = rank;
                say(ctx, "CALENDAR_UPDATE_INVITE_LIST");
            }
        }
        if (ctx.player != 0 && guid == ctx.player) {
            if (CalendarInvite* inv = findInvite(cal, eventId)) inv->rank = rank;
            say(ctx, "CALENDAR_UPDATE_EVENT_LIST");
        }
        break;
    }

    case CalendarPacket::SendNumPending: {
        // 0x005b7ce0: the count, announced when it moves.
        if (!packet.hasRemaining(4)) return false;
        const uint32_t count = packet.readUInt32();
        const bool moved = count != cal.serverPendingCount;
        cal.serverPendingCount = count;
        if (moved) say(ctx, "CALENDAR_UPDATE_PENDING_INVITES");
        break;
    }

    case CalendarPacket::InviteNotes: {
        // 0x005bbc20: invitee, event, the note, clear-pending.
        uint64_t guid = 0;
        if (!readGuid(packet, guid)) return false;
        if (!packet.hasRemaining(8)) return false;
        const uint64_t eventId = packet.readUInt64();
        std::string note;
        if (!readTerminatedString(packet, note)) return false;
        if (!packet.hasRemaining(1)) return false;
        if (packet.readUInt8() == 0) clearsAction = false;
        if (isOpen(open, eventId)) {
            if (CalendarEventInvitee* row = findInvitee(open, guid)) {
                row->note = std::move(note);
                say(ctx, "CALENDAR_UPDATE_INVITE_LIST");
            }
        }
        break;
    }

    case CalendarPacket::InviteNotesAlert: {
        // 0x005bbcd0: event and the note, for the player's own row.
        clearsAction = false;
        if (!packet.hasRemaining(8)) return false;
        const uint64_t eventId = packet.readUInt64();
        std::string note;
        if (!readTerminatedString(packet, note)) return false;
        if (isOpen(open, eventId) && ctx.player != 0) {
            if (CalendarEventInvitee* row = findInvitee(open, ctx.player)) {
                row->note = std::move(note);
                say(ctx, "CALENDAR_UPDATE_INVITE_LIST");
            }
        }
        break;
    }

    case CalendarPacket::ClearPendingAction:
        // No body: the dispatcher's default ending is the whole of it.
        break;
    }

    if (clearsAction) say(ctx, "CALENDAR_ACTION_PENDING", {eventBool(false)});
    return true;
}

void calendarNameArrived(CalendarEventDetail& open, uint64_t guid,
                         std::vector<CalendarSignal>& out) {
    // One callback per lookup counted, each taking one off the count and the
    // one that empties it saying what was held back (0x005f1a40).
    auto& awaited = open.namesAwaited;
    for (auto it = std::find(awaited.begin(), awaited.end(), guid); it != awaited.end();
         it = std::find(awaited.begin(), awaited.end(), guid)) {
        awaited.erase(it);
        if (awaited.empty()) sayOnceNamed(open, open.onceNamed, out);
    }
}

}  // namespace game
}  // namespace wowee
