#pragma once

#include <algorithm>
#include <cctype>
#include <string>

namespace wowee::game {

/// 0x0076ec80: two strings in order without case, a difference in case alone
/// deciding only when nothing else does. Negative, 0 or positive. The client
/// sorts names this way wherever a list is ordered by name - calendar invites,
/// a trainer's skill lines and services.
inline int compareCaseless(const std::string& a, const std::string& b) {
    int byCase = 0;
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        const auto ca = static_cast<unsigned char>(a[i]);
        const auto cb = static_cast<unsigned char>(b[i]);
        const int la = std::tolower(ca), lb = std::tolower(cb);
        if (la != lb) return la - lb;
        if (byCase == 0 && ca != cb) byCase = static_cast<int>(ca) - static_cast<int>(cb);
    }
    if (a.size() != b.size()) return a.size() < b.size() ? -1 : 1;
    return byCase;
}

}  // namespace wowee::game
