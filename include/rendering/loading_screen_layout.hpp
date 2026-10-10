#pragma once

// The world loading screen as LoadingScreen.cpp draws it: the map's picture
// kept to its own shape in the middle of a black screen (0x0040a270), and on
// it the bar, Loading-BarFill under Loading-BarBorder (0x004090c0).

#include <array>
#include <string>

namespace wowee::rendering::loading_screen {

/// A picture's shape: 4:3, or 16:10 for a "Wide" one (0x00ab63b4, 0x00ab63b8).
inline constexpr float kPictureAspect = 4.0f / 3.0f;
inline constexpr float kWidePictureAspect = 1.6f;

/// Shown when the map names no picture or it cannot be read (0x00409ed0).
inline constexpr const char* kDefaultPicture = "Interface\\Glues\\loading";

/// 0x00409ed0: a screen wider than 4:3 takes the picture's "Wide" version
/// where LoadingScreens.dbc says it has one (+0xc).
inline bool wantsWidePicture(float screenAspect, bool hasWide) {
    return hasWide && screenAspect > kPictureAspect + 0.001f;
}

/// The "Wide" version's name: "Wide" before the first '.' (0x00409ed0).
inline std::string widePicturePath(const std::string& fileName) {
    const size_t dot = fileName.find('.');
    if (dot == std::string::npos) return fileName + "Wide";
    return fileName.substr(0, dot) + "Wide" + fileName.substr(dot);
}

/// A rectangle in the screen's 0..1, y up.
struct Rect {
    float x0 = 0.0f, x1 = 1.0f, y0 = 0.0f, y1 = 1.0f;
};

/// 0x0040a270: the viewport the picture and the bar are drawn in - the
/// picture's shape, centred, the rest of the screen left black.
inline Rect pictureViewport(float screenAspect, bool wide) {
    Rect r;
    const float ratio = screenAspect / (wide ? kWidePictureAspect : kPictureAspect);
    if (!(ratio > 0.0f)) return r;
    if (ratio > 1.0f) {
        const float w = 1.0f / ratio;
        r.x0 = (1.0f - w) * 0.5f;
        r.x1 = r.x0 + w;
    } else if (ratio < 1.0f) {
        r.y0 = (1.0f - ratio) * 0.5f;
        r.y1 = r.y0 + ratio;
    }
    return r;
}

/// One of the bar's two pieces (0x009e2dfc): its texture, whether it is the
/// fill (drawn as far as the progress), its centre and size in the
/// picture's 0..1, y up.
struct BarPiece {
    const char* texture;
    bool fill;
    float cx, cy, w, h;
};
inline constexpr std::array<BarPiece, 2> kBar{{
    {"Interface\\Glues\\LoadingBar\\Loading-BarFill", true, 0.5f, 0.075f, 0.525f, 0.025f},
    {"Interface\\Glues\\LoadingBar\\Loading-BarBorder", false, 0.5f, 0.075f, 0.6f, 0.05f},
}};

/// 0x004090c0: the piece's rectangle in the picture; the fill runs from its
/// left edge for the progress' share of its width, its texture squeezed
/// into that.
inline Rect barPieceRect(const BarPiece& piece, float progress) {
    Rect r;
    r.x0 = piece.cx - piece.w * 0.5f;
    r.x1 = piece.cx + piece.w * 0.5f;
    r.y0 = piece.cy - piece.h * 0.5f;
    r.y1 = piece.cy + piece.h * 0.5f;
    if (piece.fill) r.x1 = r.x0 + piece.w * progress;
    return r;
}

}  // namespace wowee::rendering::loading_screen
