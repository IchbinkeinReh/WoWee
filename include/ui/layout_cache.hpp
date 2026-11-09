#pragma once

// layout-local.txt: where the player has put the frames they moved or sized,
// kept per character (AccountData type 6) and put back when the interface
// loads (0x0052adbe hands 0x005183a0 to 0x006b9050).
//
// Written (0x005234a0) as "Version: 1" and then, for each named frame the
// player placed (flag 0x1000) that is movable or resizable (0x300) and not
// told SetDontSavePosition (0x80000), the lines 0x00517e00 prints:
//
//     Frame: <name>
//     FrameLevel: <level>
//     Anchor: <point>      the one of the nine points of the frame nearest the
//                          same point of its parent, first nearest wins
//     X: <x>  Y: <y>       that point less the parent's, in the frame's units
//     W: <w>  H: <h>       the frame's size, in its units
//
// Read back (0x005183a0) a line at a time, each key matched as a prefix with
// case ignored; a frame's lines are acted on when the next "Frame:" comes or
// the file ends (0x00518280): its level when one was given, then - if it is
// movable - one point to its parent at that offset, and - if it is
// resizable - that size, either way marking it placed by the player.

#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace wowee::ui::layout_cache {

/// The client's points in its own order (0x00512410 and the table at
/// 0x00a43f78).
inline constexpr std::array<const char*, 9> kPoints = {
    "TOPLEFT", "TOP", "TOPRIGHT", "LEFT", "CENTER", "RIGHT", "BOTTOMLEFT", "BOTTOM", "BOTTOMRIGHT"};

struct Rect {
    float left = 0.0f, bottom = 0.0f, right = 0.0f, top = 0.0f;
};

struct Placement {
    int point = 0;
    float dx = 0.0f, dy = 0.0f;
};

/// The nine points of a rect, x then y, in the client's order (0x00512410).
inline std::array<float, 18> pointsOf(const Rect& r) {
    const float cx = (r.right - r.left) * 0.5f + r.left;
    const float cy = (r.top - r.bottom) * 0.5f + r.bottom;
    return {r.left, r.top,    cx, r.top,    r.right, r.top,
            r.left, cy,       cx, cy,       r.right, cy,
            r.left, r.bottom, cx, r.bottom, r.right, r.bottom};
}
/// The point of `frame` nearest the same point of `parent` and how far it
/// sits from it (0x00517e00's search).
inline Placement nearestPoint(const Rect& frame, const Rect& parent) {
    const auto f = pointsOf(frame);
    const auto p = pointsOf(parent);
    Placement out;
    float best = 3.4028235e+38f;
    for (int i = 0; i < 9; ++i) {
        const float dx = f[i * 2] - p[i * 2];
        const float dy = f[i * 2 + 1] - p[i * 2 + 1];
        const float d = dy * dy + dx * dx;
        if (d < best) {
            best = d;
            out = {i, dx, dy};
        }
    }
    return out;
}

/// One frame's record, as written and as read.
struct Entry {
    std::string name;
    int level = -1;
    int point = 0;
    int x = 0, y = 0, w = 0, h = 0;
};

/// A frame to save: its rect and its parent's in screen units, the scale it is
/// drawn at, and its level.
struct Saved {
    std::string name;
    Rect rect, parentRect;
    float effectiveScale = 1.0f;
    int level = 0;
};

/// The record 0x00517e00 writes for it: offsets and size in the frame's own
/// units, rounded.
inline Entry entryFor(const Saved& s) {
    const Placement at = nearestPoint(s.rect, s.parentRect);
    const float scale = s.effectiveScale > 0.0f ? s.effectiveScale : 1.0f;
    Entry e;
    e.name = s.name;
    e.level = s.level;
    e.point = at.point;
    e.x = static_cast<int>(std::lround(at.dx / scale));
    e.y = static_cast<int>(std::lround(at.dy / scale));
    e.w = static_cast<int>(std::lround((s.rect.right - s.rect.left) / scale));
    e.h = static_cast<int>(std::lround((s.rect.top - s.rect.bottom) / scale));
    return e;
}

/// Whether `text` starts with the `n` characters of `key`, case ignored, as
/// the client's SStrCmpI (0x0076e780) asks it.
inline bool startsWithNoCase(const std::string& text, const char* key, size_t n) {
    if (text.size() < n) return false;
    for (size_t i = 0; i < n; ++i)
        if (std::tolower(static_cast<unsigned char>(text[i])) != std::tolower(static_cast<unsigned char>(key[i])))
            return false;
    return true;
}

/// The point's index, -1 for a name that is not one (0x00814ed0).
inline int pointIndex(const std::string& name) {
    for (size_t i = 0; i < kPoints.size(); ++i)
        if (const size_t n = std::char_traits<char>::length(kPoints[i]);
            name.size() == n && startsWithNoCase(name, kPoints[i], n))
            return static_cast<int>(i);
    return -1;
}

inline std::string serialize(const std::vector<Entry>& entries) {
    std::string out = "Version: 1\n";
    for (const Entry& e : entries) {
        out += "Frame: " + e.name + "\n";
        out += "FrameLevel: " + std::to_string(e.level) + "\n";
        out += std::string("Anchor: ") + kPoints[static_cast<size_t>(e.point) % kPoints.size()] + "\n";
        out += "X: " + std::to_string(e.x) + "\n";
        out += "Y: " + std::to_string(e.y) + "\n";
        out += "W: " + std::to_string(e.w) + "\n";
        out += "H: " + std::to_string(e.h) + "\n";
    }
    return out;
}

/// The records in order; one without a frame name (lines before the first
/// "Frame:") finds no frame and is left out. Reading stops at an empty line,
/// as 0x005183a0's does.
inline std::vector<Entry> parse(const std::string& text) {
    std::vector<Entry> out;
    Entry cur;
    bool haveFrame = false;
    auto flush = [&] {
        if (haveFrame) out.push_back(cur);
    };
    auto keyed = [](const std::string& line, const char* key, std::string& rest) {
        const size_t n = std::char_traits<char>::length(key);
        if (!startsWithNoCase(line, key, n)) return false;
        rest = line.substr(n);
        return true;
    };
    size_t at = 0;
    while (at <= text.size()) {
        size_t end = text.find_first_of("\r\n", at);
        if (end == std::string::npos) end = text.size();
        const std::string line = text.substr(at, end - at);
        at = text.find_first_not_of("\r\n", end);
        if (line.empty()) break;
        std::string rest;
        if (keyed(line, "Version: ", rest)) {
        } else if (keyed(line, "Frame: ", rest)) {
            flush();
            cur = Entry{};
            cur.name = rest;
            haveFrame = true;
        } else if (keyed(line, "FrameLevel: ", rest)) {
            cur.level = std::atoi(rest.c_str());
        } else if (keyed(line, "Anchor: ", rest)) {
            if (const int p = pointIndex(rest); p >= 0) cur.point = p;
        } else if (keyed(line, "X: ", rest)) {
            cur.x = std::atoi(rest.c_str());
        } else if (keyed(line, "Y: ", rest)) {
            cur.y = std::atoi(rest.c_str());
        } else if (keyed(line, "W: ", rest)) {
            cur.w = std::atoi(rest.c_str());
        } else if (keyed(line, "H: ", rest)) {
            cur.h = std::atoi(rest.c_str());
        }
        if (at == std::string::npos) break;
    }
    flush();
    return out;
}

}  // namespace wowee::ui::layout_cache
