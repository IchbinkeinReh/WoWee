#include "game/text_tokens.hpp"

#include "game/chr_names.hpp"
#include "game/game_handler.hpp"
#include "game/interface_text.hpp"

#include <ctime>

namespace wowee {
namespace game {

// The half that needs a client. Kept out of text_tokens.cpp so the substitution
// itself links against nothing and can be exercised on its own.

TextSubject textSubjectFor(GameHandler& gameHandler) {
    TextSubject subject;
    if (const auto* character = gameHandler.getActiveCharacter()) {
        subject.gender = character->gender;
        if (!character->name.empty()) subject.name = character->name;
        nameForSubject(gameHandler, subject, static_cast<uint8_t>(character->characterClass),
                       static_cast<uint8_t>(character->race));
    }
    subject.worldState = [&gameHandler](int32_t id) -> uint32_t {
        const auto& states = gameHandler.getWorldStates();
        const auto it = states.find(static_cast<uint32_t>(id));
        return it != states.end() ? it->second : 0u;
    };
    subject.duration = [](char unit, int count) {
        const std::string format = unit == 'D'   ? interfaceString("INT_GENERAL_DURATION_DAYS", "%d |4Day:Days;")
                                   : unit == 'H' ? interfaceString("INT_GENERAL_DURATION_HOURS", "%d |4Hour:Hours;")
                                                 : interfaceString("INT_GENERAL_DURATION_MIN", "%d |4Minute:Minutes;");
        std::string out = format;
        if (const size_t at = out.find("%d"); at != std::string::npos) out.replace(at, 2, std::to_string(count));
        return out;
    };
    subject.serverTime = static_cast<int64_t>(std::time(nullptr));
    return subject;
}

// 0x0071a590 and 0x0071a4b0: the row's word in the reader's gender, and the
// gender that word is in.
void nameForSubject(GameHandler& gameHandler, TextSubject& subject, uint8_t classId, uint8_t raceId) {
    auto* assets = gameHandler.services().assetManager;
    const int gender = subject.gender == Gender::MALE ? 0 : subject.gender == Gender::FEMALE ? 1 : 2;
    const auto asGender = [](int g) {
        return g == 0 ? Gender::MALE : g == 1 ? Gender::FEMALE : Gender::NONBINARY;
    };
    if (classId != 0) {
        const GenderedName c = chrClassName(assets, classId, gender);
        subject.className = !c.name.empty() ? c.name : getClassName(static_cast<Class>(classId));
        if (!c.name.empty()) subject.classGender = asGender(c.gender);
    }
    if (raceId != 0) {
        const GenderedName r = chrRaceName(assets, raceId, gender);
        subject.raceName = !r.name.empty() ? r.name : getRaceName(static_cast<Race>(raceId));
        if (!r.name.empty()) subject.raceGender = asGender(r.gender);
    }
}

std::string resolveTextTokens(const std::string& text, GameHandler& gameHandler) {
    return resolveTextTokens(text, textSubjectFor(gameHandler));
}

}  // namespace game
}  // namespace wowee
