#pragma once

/// What a letter in the mailbox shows: its subject and text by its kind
/// (0x00571c50, 0x0056f520, 0x00570bd0), whether its text can be taken and
/// whether it is an auction's invoice, and which attachment a call names.

#include "game/mail_template.hpp"
#include "game/world_packets.hpp"

#include <cstdint>
#include <cstdlib>
#include <functional>
#include <string>

namespace wowee::game::mail_text {

inline constexpr uint32_t kFlagTextTaken = 0x4;           ///< a copy of the text was made
inline constexpr uint32_t kFlagCalendarInvite = 0x100;    ///< removed from an event, not the event itself
inline constexpr uint32_t kGmStationery = 0x3d;
inline constexpr int kAttachments = 16;

/// Letter kinds (0x00571c50's +4).
inline constexpr uint8_t kPlayer = 0, kAuction = 2, kCreature = 3, kGameObject = 4, kCalendar = 5;

/// A global string by its key, with the English one to fall back on.
using Text = std::function<std::string(const char* key, const char* enUS)>;

inline std::string fillIn(const std::string& fmt, const std::string& arg) {
    const size_t at = fmt.find("%s");
    return at == std::string::npos ? fmt : std::string(fmt).replace(at, 2, arg);
}

/// "%d:%02d" with the hour and the minute in, TIME_TWENTYFOURHOURS and its
/// two siblings (0x005b7e50).
inline std::string clockTime(std::string fmt, int hour, int minute) {
    const auto put = [&fmt](const char* spec, int value, bool pad) {
        const size_t at = fmt.find(spec);
        if (at == std::string::npos) return;
        std::string digits = std::to_string(value);
        if (pad && digits.size() < 2) digits = "0" + digits;
        fmt.replace(at, std::char_traits<char>::length(spec), digits);
    };
    put("%02d", minute, true);
    put("%d", hour, false);
    return fmt;
}

/// The attachment a call names by its place, counted from one; with none named
/// (or 0), the first there is (0x0056d420). Places count what the server put
/// in them, so one taken leaves its place empty.
inline const MailAttachment* attachment(const MailMessage& mail, std::optional<int> place) {
    const MailAttachment* found = nullptr;
    if (!place || *place == 0) {
        for (const auto& att : mail.attachments)
            if (att.itemId != 0 && (!found || att.slot < found->slot)) found = &att;
        return found;
    }
    if (*place < 1 || *place > kAttachments) return nullptr;
    for (const auto& att : mail.attachments)
        if (att.itemId != 0 && att.slot == *place - 1) return &att;
    return nullptr;
}

/// Where the reader puts an attachment (0x00571c50): past the sixteen at the
/// first place, and at the first free one where its own is taken. kAttachments
/// where none is free. `taken` is a bit per place.
inline uint8_t attachmentPlace(uint8_t slot, uint32_t& taken) {
    if (slot >= kAttachments) slot = 0;
    if ((taken & (1u << slot)) != 0) {
        slot = 0;
        while (slot < kAttachments && (taken & (1u << slot)) != 0) ++slot;
        if (slot == kAttachments) return slot;
    }
    taken |= 1u << slot;
    return slot;
}

/// A calendar letter's subject: who removed the player or the event, and the
/// event's title (0x00571c50: the number before the colon, what is after it).
struct CalendarSubject {
    uint64_t remover = 0;
    std::string title;
};
inline CalendarSubject calendarSubject(const std::string& subject) {
    CalendarSubject out;
    out.remover = std::strtoull(subject.c_str(), nullptr, 10);
    if (const size_t at = subject.find(':'); at != std::string::npos) out.title = subject.substr(at + 1);
    return out;
}

/// The subject a letter shows: an auction's worded for its answer, with the
/// item's name - nothing while the item is not known - a calendar letter's
/// for its event, and the MailTemplate row's in place of either or of the
/// server's, whatever it says. `itemName` answers "" for an item not known.
inline std::string subject(const MailMessage& mail, const MailTemplateText* row,
                           const std::function<std::string(const AuctionMailSubject&)>& itemName,
                           const Text& text) {
    if (!row && mail.messageType == kAuction) {
        AuctionMailSubject auction;
        if (!parseAuctionMailSubject(mail.subject, auction)) return mail.subject;
        const std::string item = itemName ? itemName(auction) : std::string();
        return item.empty() ? std::string() : formatAuctionMailSubject(auction, item, text);
    }
    if (!row && mail.messageType == kCalendar) {
        const std::string fmt = (mail.flags & kFlagCalendarInvite) != 0
                                    ? text("CALENDAR_INVITE_REMOVED_MAIL_SUBJECT", "Removed from \"%s\".")
                                    : text("CALENDAR_EVENT_REMOVED_MAIL_SUBJECT", "%s cancelled.");
        return fillIn(fmt, calendarSubject(mail.subject).title);
    }
    return mailSubject(mail.subject, row);
}

/// An auction's letter about a sale or a purchase is an invoice, once its item
/// is known (0x0056f520): won 1, sold 2, sale pending 6.
inline bool isInvoice(const MailMessage& mail, bool itemKnown) {
    AuctionMailSubject auction;
    return mail.messageType == kAuction && itemKnown && parseAuctionMailSubject(mail.subject, auction) &&
           (auction.response == 1 || auction.response == 2 || auction.response == 6);
}

/// Which side of the sale the invoice is (0x0056e9b0).
inline const char* invoiceType(uint32_t response) {
    return response == 2 ? "seller" : response == 6 ? "seller_temp_invoice" : "buyer";
}

/// Where the text of an opened letter comes from (0x00570bd0).
enum class Body : uint8_t {
    None,       ///< nil: an auction's, or nothing to show
    Template,   ///< MailTemplate's, its $-tokens filled
    Plain,      ///< a player's, as written but for the profanity filter
    Calendar,   ///< CALENDAR_*_REMOVED_MAIL_BODY
    Tokens,     ///< the letter's, its $-tokens filled
};
inline Body body(const MailMessage& mail, bool templateThere) {
    if (mail.body.empty()) return mail.mailTemplateId != 0 && templateThere ? Body::Template : Body::None;
    switch (mail.messageType) {
        case kAuction: return Body::None;
        case kPlayer: return Body::Plain;
        case kCalendar: return Body::Calendar;
        default: return Body::Tokens;
    }
}

/// Whether the letter's text can be taken as a letter of one's own
/// (0x00570bd0's third answer): there is one, not taken yet, and it is no
/// game master's, no calendar's and - of the auction's - an invoice alone.
inline bool textTakeable(const MailMessage& mail, bool invoice) {
    if (mail.body.empty() && mail.mailTemplateId == 0) return false;
    if ((mail.flags & kFlagTextTaken) != 0) return false;
    if (mail.stationeryId == kGmStationery || mail.messageType == kCalendar) return false;
    return mail.messageType != kAuction || invoice;
}

}  // namespace wowee::game::mail_text
