#pragma once

#include <cmath>
#include <cstdint>

namespace wowee::game::auction_deposit {

/// CalculateAuctionDeposit's run time (0x0059d270): the argument, rounded
/// less one, indexes 720, 1440, 2880 minutes (0x00a15248); anything else is
/// 1440.
inline uint32_t minutesForRunTime(double runTime) {
    static constexpr uint32_t kMinutes[3] = {720, 1440, 2880};
    const auto index = static_cast<uint32_t>(static_cast<int64_t>(std::nearbyint(runTime - 1.0)));
    return index < 3 ? kMinutes[index] : 1440;
}

/// 0x0076dde0: the deposit, in copper, for an AuctionHouse.dbc DepositRate
/// percent and the item's sell price times the count. A length other than
/// 720, 1440 or 2880 minutes asks 100. Otherwise the percentage of the
/// price is taken in whole copper (unsigned 32-bit, as the client
/// multiplies), times the minutes and 1/240 (the float 0.004166667), cut
/// toward zero, and never under 100.
inline uint32_t deposit(uint32_t depositPercent, uint32_t priceTimesCount, uint32_t minutes) {
    if (minutes != 720 && minutes != 1440 && minutes != 2880) return 100;
    const uint32_t part = (depositPercent * priceTimesCount) / 100u;
    const double v = static_cast<double>(minutes) * static_cast<double>(part) *
                     static_cast<double>(0.004166667f);
    const auto out = static_cast<uint32_t>(static_cast<int64_t>(v));
    return out < 100 ? 100 : out;
}

}  // namespace wowee::game::auction_deposit
