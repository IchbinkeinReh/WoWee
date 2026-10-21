#pragma once

/// SMSG_AUCTION_LIST_PENDING_SALES (0x0059e880): the sales still to pay out,
/// each a mail's subject and body, and how many days the money is from coming.
/// The subject is the auction mail's "itemId:randomPropertyId:2:auctionId",
/// read only with all four and the third 2 (0x0059a4a0); the body the buyer's
/// guid in hexadecimal, the bid, the buyout, the deposit and the cut
/// ("%16I64X:%d:%d:%d:%d"). Each becomes a row of the owner's list marked as
/// a pending sale (+0x9c), owned by the player and with no count.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>

#include "game/world_packets.hpp"

namespace wowee::game::auction_pending {

struct Mail {
    std::string subject;
    std::string body;
    float daysLeft = 0.0f;
};

/// 0x0059a4a0: up to four numbers, each after the ':' found from just past
/// the one before (the first from the second character on), read as atoi
/// reads them; how many there were.
inline int subjectFields(const std::string& subject, int32_t (&out)[4]) {
    for (int32_t& v : out) v = 0;
    const char* p = subject.c_str();
    out[0] = std::atoi(p);
    if (subject.empty()) return 1;
    for (int n = 1; n < 4; ++n) {
        p = std::strchr(p + 1, ':');
        if (!p) return n;
        out[n] = std::atoi(p + 1);
    }
    return 4;
}

inline std::optional<AuctionEntry> row(const Mail& mail, uint64_t player) {
    int32_t f[4];
    if (subjectFields(mail.subject, f) != 4 || f[2] != 2) return std::nullopt;
    unsigned long long buyer = 0;
    int bid = 0, buyout = 0, deposit = 0, cut = 0;
    std::sscanf(mail.body.c_str(), "%16llX:%d:%d:%d:%d", &buyer, &bid, &buyout, &deposit, &cut);
    AuctionEntry e;
    e.auctionId = static_cast<uint32_t>(f[3]);
    e.itemEntry = static_cast<uint32_t>(f[0]);
    e.randomPropertyId = static_cast<uint32_t>(f[1]);
    e.stackCount = 0;
    e.ownerGuid = player;
    e.buyoutPrice = static_cast<uint32_t>(buyout);
    e.bidderGuid = buyer;
    e.currentBid = static_cast<uint32_t>(bid);
    e.timeLeftMs = static_cast<uint32_t>(std::lround(mail.daysLeft * 86400000.0f));
    e.pendingSale = true;
    return e;
}

}  // namespace wowee::game::auction_pending
