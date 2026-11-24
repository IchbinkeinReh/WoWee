// What a letter in the mailbox shows (0x00571c50, 0x0056f520, 0x00570bd0) and
// which attachment a call names (0x0056d420).
#include <catch_amalgamated.hpp>

#include "core/application.hpp"
#include "game/mail_text.hpp"

namespace wowee::core {
Application* Application::instance = nullptr;
}

using namespace wowee::game;
namespace mt = wowee::game::mail_text;

namespace {

std::string german(const char* key, const char* enUS) {
    const std::string k = key;
    if (k == "AUCTION_WON_MAIL_SUBJECT") return "Auktion gewonnen: %s";
    if (k == "CALENDAR_EVENT_REMOVED_MAIL_SUBJECT") return "%s abgebrochen.";
    if (k == "CALENDAR_INVITE_REMOVED_MAIL_SUBJECT") return "Ihr wurdet aus \"%s\" entfernt.";
    return enUS;
}
std::string linen(const AuctionMailSubject&) { return "Leinenstoff"; }
std::string unknownItem(const AuctionMailSubject&) { return {}; }

MailMessage letter(uint8_t type, const std::string& subject, const std::string& body = {}) {
    MailMessage m;
    m.messageType = type;
    m.subject = subject;
    m.body = body;
    m.stationeryId = 41;
    return m;
}

MailAttachment item(uint8_t slot, uint32_t id) {
    MailAttachment a;
    a.slot = slot;
    a.itemId = id;
    a.itemGuidLow = 1000 + slot;
    return a;
}

}  // namespace

TEST_CASE("a letter made from a MailTemplate has the template's subject", "[mail]") {
    // Minigob Manaflausch's letters (264): the server sends no subject.
    MailMessage m = letter(mt::kCreature, "");
    m.mailTemplateId = 264;
    const MailTemplateText wool{"Man hat Euch wollig gemacht!", "..."}, empty;
    CHECK(mt::subject(m, &wool, linen, german) == wool.subject);
    // Whatever the server sent, and an empty one too (0x00571c50 copies it).
    m.subject = "from the server";
    CHECK(mt::subject(m, &wool, linen, german) == wool.subject);
    CHECK(mt::subject(m, &empty, linen, german).empty());
    // An auction's or a calendar's too.
    CHECK(mt::subject(letter(mt::kAuction, "2589:0:1:1:1"), &wool, linen, german) == wool.subject);
    CHECK(mt::subject(letter(mt::kCalendar, "4660:Raid"), &wool, linen, german) == wool.subject);
    // A template with no row leaves the server's.
    CHECK(mt::subject(m, nullptr, linen, german) == "from the server");
}

TEST_CASE("an auction's and a calendar's subject are worded for what they are", "[mail]") {
    CHECK(mt::subject(letter(mt::kAuction, "2589:0:1:1:1"), nullptr, linen, german) ==
          "Auktion gewonnen: Leinenstoff");
    // Nothing until the item is known (0x0056f520).
    CHECK(mt::subject(letter(mt::kAuction, "2589:0:1:1:1"), nullptr, unknownItem, german).empty());
    MailMessage m = letter(mt::kCalendar, "4660:Raid am Mittwoch");
    CHECK(mt::subject(m, nullptr, linen, german) == "Raid am Mittwoch abgebrochen.");
    m.flags = mt::kFlagCalendarInvite;
    CHECK(mt::subject(m, nullptr, linen, german) == "Ihr wurdet aus \"Raid am Mittwoch\" entfernt.");
    CHECK(mt::calendarSubject(m.subject).remover == 4660);
}

TEST_CASE("the text of a letter by its kind", "[mail]") {
    CHECK(mt::body(letter(mt::kPlayer, "s", "Hallo"), false) == mt::Body::Plain);
    CHECK(mt::body(letter(mt::kCreature, "s", "Hallo, $N"), false) == mt::Body::Tokens);
    CHECK(mt::body(letter(mt::kGameObject, "s", "Hallo, $N"), false) == mt::Body::Tokens);
    CHECK(mt::body(letter(mt::kAuction, "2589:0:1:1:1", "1a2b:500:0"), false) == mt::Body::None);
    CHECK(mt::body(letter(mt::kCalendar, "1:t", "123456"), false) == mt::Body::Calendar);
    MailMessage m = letter(mt::kCreature, "");
    CHECK(mt::body(m, false) == mt::Body::None);
    m.mailTemplateId = 264;
    CHECK(mt::body(m, true) == mt::Body::Template);
    CHECK(mt::body(m, false) == mt::Body::None);
}

TEST_CASE("the text that can be taken", "[mail]") {
    // Not what the letter carries: GetInboxText's third answer was the money
    // and the items.
    MailMessage m = letter(mt::kPlayer, "s");
    m.money = 100;
    m.attachments.push_back(item(0, 2589));
    CHECK_FALSE(mt::textTakeable(m, false));
    m = letter(mt::kPlayer, "s", "Hallo");
    CHECK(mt::textTakeable(m, false));
    m.flags = mt::kFlagTextTaken;
    CHECK_FALSE(mt::textTakeable(m, false));
    m.flags = 0;
    m.stationeryId = mt::kGmStationery;
    CHECK_FALSE(mt::textTakeable(m, false));
    CHECK_FALSE(mt::textTakeable(letter(mt::kCalendar, "1:t", "5"), false));
    CHECK_FALSE(mt::textTakeable(letter(mt::kAuction, "2589:0:0:1:1", "1a2b:500:0"), false));
    CHECK(mt::textTakeable(letter(mt::kAuction, "2589:0:1:1:1", "1a2b:500:0"), true));
    MailMessage made = letter(mt::kCreature, "");
    made.mailTemplateId = 264;
    CHECK(mt::textTakeable(made, false));
}

TEST_CASE("an invoice is the letter of a sale or a purchase", "[mail][auction]") {
    const bool invoiced[7] = {false, true, true, false, false, false, true};
    for (int response = 0; response < 7; ++response) {
        const auto m = letter(mt::kAuction, "2589:0:" + std::to_string(response) + ":1:1", "1a2b:500:0");
        CHECK(mt::isInvoice(m, true) == invoiced[response]);
        CHECK_FALSE(mt::isInvoice(m, false));
    }
    CHECK_FALSE(mt::isInvoice(letter(mt::kPlayer, "2589:0:1:1:1", "1a2b:500:0"), true));
    CHECK(std::string(mt::invoiceType(2)) == "seller");
    CHECK(std::string(mt::invoiceType(6)) == "seller_temp_invoice");
    CHECK(std::string(mt::invoiceType(1)) == "buyer");
    AuctionMailInvoice invoice;
    REQUIRE(parseAuctionMailBody("         1A2B3C4:6000:7000:10:350:3600:123", invoice));
    CHECK(invoice.ownerGuid == 0x1A2B3C4);
    CHECK(invoice.consignment == 350);
    CHECK(invoice.moneyDelay == 3600);
    CHECK(invoice.eta == 123);
}

TEST_CASE("an attachment is named by its place, or is the first", "[mail]") {
    MailMessage m = letter(mt::kPlayer, "s");
    m.attachments = {item(1, 2589), item(3, 2592)};   // the first of four has been taken
    REQUIRE(mt::attachment(m, std::nullopt));
    CHECK(mt::attachment(m, std::nullopt)->itemId == 2589);
    CHECK(mt::attachment(m, 0)->itemId == 2589);
    CHECK(mt::attachment(m, 1) == nullptr);
    CHECK(mt::attachment(m, 2)->itemId == 2589);
    CHECK(mt::attachment(m, 4)->itemId == 2592);
    CHECK(mt::attachment(m, 17) == nullptr);
    CHECK(mt::attachment(m, -1) == nullptr);
    m.attachments.clear();
    CHECK(mt::attachment(m, std::nullopt) == nullptr);
}

TEST_CASE("the reader gives each attachment a place of its own", "[mail]") {
    uint32_t taken = 0;
    CHECK(mt::attachmentPlace(0, taken) == 0);
    CHECK(mt::attachmentPlace(2, taken) == 2);
    CHECK(mt::attachmentPlace(2, taken) == 1);
    CHECK(mt::attachmentPlace(200, taken) == 3);
    taken = 0xffff;
    CHECK(mt::attachmentPlace(5, taken) == mt::kAttachments);
}

TEST_CASE("the clock of a calendar letter", "[mail]") {
    CHECK(mt::clockTime("%d:%02d", 9, 5) == "9:05");
    CHECK(mt::clockTime("%d:%02d PM", 12, 30) == "12:30 PM");
}
