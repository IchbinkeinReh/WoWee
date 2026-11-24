#pragma once

/// The file a texture's name stands for.

#include <cctype>
#include <string>

namespace wowee::ui {

/// "Interface\\Foo\\Bar" is the BLP of that name, with its extension or
/// without; .tga means .blp. Blizzard's own markup still names 33 files that
/// way - the world map's quest icons and small frame edges, the login screen's
/// rating art - and no .tga has shipped in the archives since long before
/// 3.3.5: the real client accepts the name and loads the BLP beside it.
inline std::string textureFileOf(const std::string& name) {
    const auto endsWith = [&name](const char* ext) {
        if (name.size() <= 4) return false;
        for (size_t i = 0; i < 4; ++i)
            if (std::tolower(static_cast<unsigned char>(name[name.size() - 4 + i])) != ext[i]) return false;
        return true;
    };
    if (endsWith(".tga")) return name.substr(0, name.size() - 4) + ".blp";
    return endsWith(".blp") ? name : name + ".blp";
}

}  // namespace wowee::ui
