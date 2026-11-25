#pragma once

#include <cctype>
#include <cstdint>
#include <optional>
#include <string_view>

namespace wowee::ui {

/// How a texture is put on what is under it (CSimpleTexture +0xd8), by the client's numbers
/// and names (0x00814ff0).
enum class TextureBlend : uint8_t {
    Disable = 0,   // as it is, its alpha and its frame's ignored
    AlphaKey = 1,  // as it is where its alpha is 224/255 or more, nothing elsewhere
    Blend = 2,     // over it by its alpha
    Add = 3,       // added to it, by its alpha
    Mod = 4,       // what is under it times the texture
};

[[nodiscard]] inline const char* textureBlendName(TextureBlend blend) {
    constexpr const char* kNames[] = {"DISABLE", "ALPHAKEY", "BLEND", "ADD", "MOD"};
    return kNames[static_cast<uint8_t>(blend)];
}

/// By its name, in any case; none of another.
[[nodiscard]] inline std::optional<TextureBlend> textureBlendOf(std::string_view name) {
    for (uint8_t i = 0; i <= static_cast<uint8_t>(TextureBlend::Mod); ++i) {
        const auto blend = static_cast<TextureBlend>(i);
        const std::string_view known = textureBlendName(blend);
        if (known.size() != name.size()) continue;
        bool same = true;
        for (size_t c = 0; c < name.size() && same; ++c)
            same = std::toupper(static_cast<unsigned char>(name[c])) == known[c];
        if (same) return blend;
    }
    return std::nullopt;
}

}  // namespace wowee::ui
