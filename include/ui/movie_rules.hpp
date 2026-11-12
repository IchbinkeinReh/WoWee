#pragma once

// The movie rules that need no decoder: the subtitle file and which
// resolution of a movie the client takes. Header-only for the test.

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace wowee::ui::movie {

struct Subtitle {
    uint32_t startMs = 0;
    uint32_t endMs = 0;
    std::string text;
};

/// The subtitle file's lines, as 0x0095e880 reads them: a line starting
/// with '0' holds "HH:MM:SS:FF - HH:MM:SS:FF", the next line the text;
/// times are ((h*60+m)*60+s)*1000 + frames*41 ms. Exposed for the test.
inline std::vector<Subtitle> parseSubtitles(const std::string& textIn) {
    std::vector<Subtitle> out;
    std::string text = textIn;
    // A UTF-8 byte order mark is skipped (0x0095e880: first byte 0xEF).
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF) text.erase(0, 3);
    std::vector<std::string> lines;
    size_t at = 0;
    while (at <= text.size()) {
        size_t nl = text.find('\n', at);
        if (nl == std::string::npos) nl = text.size();
        std::string line = text.substr(at, nl - at);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        // The client's line reader skips blank lines.
        if (!line.empty()) lines.push_back(line);
        at = nl + 1;
    }
    auto num = [](const std::string& s, size_t pos) {
        return pos + 2 <= s.size() ? std::atoi(s.substr(pos, 2).c_str()) : 0;
    };
    // "HH:MM:SS:FF - HH:MM:SS:FF": fields at 0,3,6,9 and 14,17,20,23.
    auto ms = [&](const std::string& s, size_t base) {
        return static_cast<uint32_t>(((num(s, base) * 60 + num(s, base + 3)) * 60 + num(s, base + 6)) * 1000 +
                                     num(s, base + 9) * 41);
    };
    for (size_t i = 0; i < lines.size(); ++i) {
        if (lines[i][0] != '0') continue;
        Subtitle sub;
        sub.startMs = ms(lines[i], 0);
        sub.endMs = ms(lines[i], 14);
        if (i + 1 < lines.size()) sub.text = lines[++i];
        out.push_back(std::move(sub));
    }
    return out;
}

/// Which MovieFileData resolution the client picks for a screen this wide
/// (0x00518700): the exact width, else the largest below it, else the
/// smallest. -1 when there is none.
inline int pickResolution(const std::vector<int>& resolutions, int screenWidth) {
    int best = -1;
    int bestRes = 0;
    for (size_t i = 0; i < resolutions.size(); ++i) {
        const int res = resolutions[i];
        if (res == screenWidth) return static_cast<int>(i);
        bool take = best < 0;
        if (!take) {
            if (screenWidth <= bestRes) take = res < bestRes;
            else take = bestRes < res && res < screenWidth;
        }
        if (take) {
            best = static_cast<int>(i);
            bestRes = res;
        }
    }
    return best;
}

} // namespace wowee::ui::movie
