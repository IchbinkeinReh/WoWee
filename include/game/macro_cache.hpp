#pragma once

/// macros-cache.txt, the account's and the character's, as the client writes
/// and reads it (0x005668f0, 0x00567c10) and keeps it on the realm (account
/// data 4 and 5): MACRO id "name" icon, the body's lines, END.

#include "game/macro_list.hpp"

#include <cstdint>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

namespace wowee::game::macro_cache {

struct Macro {
    uint32_t id = 0;
    std::string name;
    std::string icon;   ///< the file's name alone
    std::string body;
};

[[nodiscard]] inline std::string write(const std::vector<Macro>& macros) {
    std::string out;
    for (const Macro& m : macros) {
        out += "MACRO " + std::to_string(m.id) + " \"" + m.name + "\" " +
               (m.icon.empty() ? std::string("INV_Misc_QuestionMark") : m.icon) + "\r\n";
        std::istringstream lines(m.body);
        for (std::string line; std::getline(lines, line);) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) out += line + "\r\n";
        }
        out += "END\r\n";
    }
    return out;
}

/// The macros of a file. A character's has ids over macro_list::kCharacterBase,
/// and one that is not is put there; an id read twice, or none, is no macro,
/// nor is anything past the thirty-six a half may have.
[[nodiscard]] inline std::vector<Macro> read(const std::string& text, bool character) {
    std::vector<Macro> out;
    std::istringstream in(text);
    const auto next = [&in](std::string& line) {
        if (!std::getline(in, line)) return false;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        return true;
    };
    for (std::string line; next(line);) {
        if (line.rfind("MACRO ", 0) != 0) continue;
        Macro m;
        char* rest = nullptr;
        m.id = static_cast<uint32_t>(std::strtoul(line.c_str() + 6, &rest, 10));
        const std::string after = rest ? rest : "";
        const size_t open = after.find('"');
        const size_t close = open == std::string::npos ? open : after.find('"', open + 1);
        if (close != std::string::npos) {
            m.name = after.substr(open + 1, close - open - 1);
            const size_t at = after.find_first_not_of(' ', close + 1);
            if (at != std::string::npos) m.icon = after.substr(at);
        }
        for (std::string body; next(body) && body != "END";) {
            if (!m.body.empty()) m.body += "\n";
            m.body += body;
        }
        if (m.id == 0 || close == std::string::npos || m.name.empty() || out.size() >= macro_list::kAccountMacros) continue;
        m.id = macro_list::fileId(m.id, character);
        bool there = m.id == 0;
        for (const Macro& other : out) there = there || other.id == m.id;
        if (!there) out.push_back(std::move(m));
    }
    return out;
}

}  // namespace wowee::game::macro_cache
