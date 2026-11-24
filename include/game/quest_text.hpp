#pragma once

// Which of a quest's strings the interface is answered with.

#include <cstdint>
#include <string>

namespace wowee::game {

/// The quest whose objectives sentence is its completed line as well.
constexpr uint32_t kQuestFlagObjectivesAsCompletedText = 0x40000;

/// GetQuestLogCompletionText (0x005e06d0): the CompletedText (+0x20a4), or the
/// objectives sentence (+0x2b4) where there is none or the quest's flags (+0x50)
/// have 0x40000. Whether the quest is complete is not asked, and the EndText
/// (+0x1a24) is never the answer: it is an objective's name, not this line.
inline const std::string& questCompletionText(uint32_t questFlags,
                                              const std::string& completedText,
                                              const std::string& objectives) {
    if ((questFlags & kQuestFlagObjectivesAsCompletedText) == 0 && !completedText.empty())
        return completedText;
    return objectives;
}

} // namespace wowee::game
