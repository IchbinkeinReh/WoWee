#pragma once

#include <cstddef>
#include <string_view>

namespace wowee::addons {

/// Whether an event's argument, from 0, is text whatever it looks like: a line of chat that is a
/// number, what an addon sends of one, a CVar's value. The client says so with each event it
/// raises (0x0081b530): of a chat line "%s%s%s%s%s%s%d%d%s%d%d%s%u" (0x004fdbc0).
[[nodiscard]] inline bool eventArgIsText(std::string_view event, std::size_t index) {
    if (event == "CHAT_MSG_ADDON" || event == "CVAR_UPDATE" || event == "EXECUTE_CHAT_LINE" ||
        event == "UI_INFO_MESSAGE" || event == "UI_ERROR_MESSAGE" || event == "GUILD_MOTD")
        return true;
    return event.starts_with("CHAT_MSG_") && (index < 6 || index == 8 || index == 11);
}

}  // namespace wowee::addons
