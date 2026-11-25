#pragma once

#include <cctype>
#include <string>

namespace wowee::addons {

/// A file's chunk as the client names it (0x00818f60): "@" and the path it is
/// asked for by, "Interface\\AddOns\\TstA\\TstA.lua" or
/// "Interface\\FrameXML\\UIParent.lua", with its ".." folded. What debugstack and
/// errors show, and what libraries find their addon by. `path` is the file's on
/// the disk, `addon` the one whose it is, as it is called.
inline std::string chunkNameOf(const std::string& path, const std::string& addon = {}) {
    std::string p;
    for (const char c : path) p += c == '/' ? '\\' : c;
    for (size_t at; (at = p.find("\\..\\")) != std::string::npos;) {
        const size_t from = at == 0 ? std::string::npos : p.rfind('\\', at - 1);
        p = p.substr(0, from == std::string::npos ? 0 : from) + p.substr(at + 3);
    }
    const auto lowered = [](std::string s) {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    const std::string low = lowered(p);
    static const std::string kAddOns = "\\addons\\", kFrameXml = "\\framexml\\", kGlueXml = "\\gluexml\\";
    if (const size_t at = low.find(kAddOns); at != std::string::npos) {
        std::string rest = p.substr(at + kAddOns.size());
        if (!addon.empty() && lowered(rest.substr(0, rest.find('\\'))) == lowered(addon))
            rest = addon + rest.substr(addon.size());
        return "@Interface\\AddOns\\" + rest;
    }
    if (const size_t at = low.rfind(kFrameXml); at != std::string::npos)
        return "@Interface\\FrameXML\\" + p.substr(at + kFrameXml.size());
    if (const size_t at = low.rfind(kGlueXml); at != std::string::npos)
        return "@Interface\\GlueXML\\" + p.substr(at + kGlueXml.size());
    return "@" + path;
}

}  // namespace wowee::addons
