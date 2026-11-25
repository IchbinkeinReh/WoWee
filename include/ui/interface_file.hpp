#pragma once

/// The files a texture's name stands for.

#include <string>

namespace wowee::ui {

/// A texture's name without its extension, whatever that is (0x004b51c0): what
/// stands behind the last point of the file's name.
inline std::string textureBase(const std::string& name) {
    size_t slash = name.rfind('\\');
    if (slash == std::string::npos) slash = name.rfind('/');
    const size_t point = name.rfind('.');
    if (point == std::string::npos || (slash != std::string::npos && point <= slash)) return name;
    return name.substr(0, point);
}

/// Its picture is the BLP of that name, or else the TGA (0x004b9760). Blizzard's
/// own markup names 33 of its BLPs .tga.
inline std::string textureFileOf(const std::string& name) { return textureBase(name) + ".blp"; }
inline std::string textureTgaOf(const std::string& name) { return textureBase(name) + ".tga"; }

}  // namespace wowee::ui
