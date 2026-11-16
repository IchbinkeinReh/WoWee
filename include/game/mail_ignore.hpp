#pragma once

// What becomes of a letter from someone the player ignores (0x00571c50, the
// SMSG_MAIL_LIST_RESULT reader): a player's letter (type 0) whose sender is on
// the ignore list (0x006b52e0), and whose flags carry none of 0x20a, is not
// listed at all. With no item attached and no money it is deleted
// (CMSG_MAIL_DELETE); otherwise it is sent back (CMSG_MAIL_RETURN_TO_SENDER).
// A COD charge alone does not save it from deletion.

#include <cstdint>

namespace wowee::game {

enum class IgnoredMailAction { Keep, Delete, Return };

inline IgnoredMailAction ignoredMailAction(uint8_t messageType, uint32_t flags, bool senderIgnored,
                                           int itemsAttached, uint64_t money) {
    if (messageType != 0 || !senderIgnored || (flags & 0x20au) != 0) return IgnoredMailAction::Keep;
    if (itemsAttached == 0 && money == 0) return IgnoredMailAction::Delete;
    return IgnoredMailAction::Return;
}

}  // namespace wowee::game
