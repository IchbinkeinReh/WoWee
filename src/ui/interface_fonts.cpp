#include "ui/interface_fonts.hpp"
#include "ui/edit_lines.hpp"
#include "ui/widget_tree.hpp"

#include <imgui.h>
#include <cfloat>

#include "ui/text_markup.hpp"
#include "ui/text_wrap.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <unordered_map>
#include <vector>

namespace wowee::ui {

namespace {

/// The lower-case stem of a path, with any directory and extension removed.
/// "Fonts\\MORPHEUS.ttf" and "morpheus.ttf" both come out as "morpheus", which
/// is what lets a font object written one way find a file named the other.
std::string faceKey(const std::string& pathOrName) {
    const size_t slash = pathOrName.find_last_of("\\/");
    std::string stem = (slash == std::string::npos) ? pathOrName
                                                    : pathOrName.substr(slash + 1);
    const size_t dot = stem.find_last_of('.');
    if (dot != std::string::npos) stem = stem.substr(0, dot);
    std::transform(stem.begin(), stem.end(), stem.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return stem;
}

std::unordered_map<std::string, ImFont*>& faces() {
    static std::unordered_map<std::string, ImFont*> map;
    return map;
}

uint64_t& faceGeneration() {
    static uint64_t generation = 0;
    return generation;
}

/// A big-endian field out of a font file, or zero past the end.
///
/// Bounds-checked on every read rather than once at the top: a truncated font
/// is a file this client did not write, and the tables it names are at offsets
/// the file itself supplies.
uint32_t beAt(const uint8_t* d, size_t n, size_t at, int bytes) {
    if (at + static_cast<size_t>(bytes) > n) return 0;
    uint32_t v = 0;
    for (int i = 0; i < bytes; ++i) v = (v << 8) | d[at + static_cast<size_t>(i)];
    return v;
}

} // namespace

void registerInterfaceFace(const std::string& pathOrName, ImFont* font) {
    if (!font) return;
    faces()[faceKey(pathOrName)] = font;
    ++faceGeneration();
}

uint64_t interfaceFaceGeneration() {
    return faceGeneration();
}

ImFont* interfaceFace(const std::string& pathOrName) {
    if (pathOrName.empty()) return nullptr;
    const auto it = faces().find(faceKey(pathOrName));
    return (it == faces().end()) ? nullptr : it->second;
}

ImFont* interfaceFaceOrDefault(const std::string& fontFace) {
    // The order the renderer uses. The interface's own default rather than this
    // client's, because ImGui draws with whatever face was added first and that
    // is deliberately the built-in one.
    if (ImFont* f = interfaceFace(fontFace)) return f;
    if (ImFont* f = interfaceFace("frizqt__")) return f;
    return (ImGui::GetCurrentContext() != nullptr) ? ImGui::GetFont() : nullptr;
}

float interfaceFontSize(float fontHeight) {
    if (fontHeight > 0.0f) return fontHeight;
    // What the renderer falls back to: the current size, not a flat twelve.
    return (ImGui::GetCurrentContext() != nullptr) ? ImGui::GetFontSize() : 12.0f;
}

float interfaceTextWidth(const std::string& text, const std::string& fontFace,
                         float fontHeight) {
    if (text.empty()) return 0.0f;
    const float size = interfaceFontSize(fontHeight);
    if (ImFont* font = interfaceFaceOrDefault(fontFace)) {
        return font->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str()).x;
    }
    // No context yet - during the FrameXML load there may be no frame in
    // flight to ask. An estimate is far better than nothing: the alternative
    // is answering zero, and MoneyFrame does SetWidth(GetTextWidth() +
    // iconWidth), which then places three buttons on top of each other.
    return static_cast<float>(text.size()) * size * 0.5f;
}

int interfaceTextLines(const std::string& text, const std::string& fontFace,
                       float fontHeight, float wrapWidth, bool nonSpaceWrap) {
    if (text.empty()) return 0;
    const float size = interfaceFontSize(fontHeight);
    ImFont* font = interfaceFaceOrDefault(fontFace);
    const auto measure = [&](const std::string& piece) {
        if (font) return font->CalcTextSizeA(size, FLT_MAX, 0.0f, piece.c_str()).x;
        // The same estimate interfaceTextWidth falls back on when there is no
        // context yet, so a measure taken during the load does not answer that
        // everything fits on one line.
        return static_cast<float>(piece.size()) * size * 0.5f;
    };
    const auto lines = wrapText(parseMarkup(text), wrapWidth, nonSpaceWrap, measure);
    return lines.empty() ? 1 : static_cast<int>(lines.size());
}

std::string interfaceFittedText(const std::string& text, ImFont* font, float size, float width, int lines,
                                bool wordWrap, bool nonSpaceWrap, float indent) {
    // 0x00483a40: one without a height is not shortened - its lines are broken
    // at its width, and end where a word is too long for one.
    if (text.empty() || width <= 0.0f || (lines <= 0 && !wordWrap)) return text;
    const auto measure = [&](const std::string& piece) {
        if (font) return font->CalcTextSizeA(size, FLT_MAX, 0.0f, piece.c_str()).x;
        return static_cast<float>(piece.size()) * size * 0.5f;
    };
    return fitText(text, [&](const std::string& t) {
        bool stopped = false;
        const auto wrapped = wrapText(parseMarkup(t), wordWrap ? width : 0.0f, nonSpaceWrap, measure, indent, &stopped);
        if (stopped || (lines > 0 && wrapped.size() > static_cast<size_t>(lines))) return false;
        if (wordWrap) return true;
        float wide = 0.0f;
        if (!wrapped.empty()) {
            for (const WrapRun& run : wrapped.front()) {
                wide += run.texture.empty()      ? measure(run.text)
                        : run.texWidth > 0.0f    ? run.texWidth
                        : run.texHeight > 0.0f   ? run.texHeight
                                                 : size;
            }
        }
        return wide <= width + 0.5f;
    }, lines > 0 ? "..." : "");
}

std::string interfaceFittedText(const std::string& text, const std::string& fontFace, float fontHeight, float width,
                                int lines, bool wordWrap, bool nonSpaceWrap) {
    return interfaceFittedText(text, interfaceFaceOrDefault(fontFace), interfaceFontSize(fontHeight), width, lines,
                               wordWrap, nonSpaceWrap);
}

float editTextWidth(const Widget& box, const std::string& piece) {
    std::string letters;
    for (const auto& run : parseMarkup(piece)) letters += run.text;
    return interfaceTextWidth(letters, box.fontFace, box.fontHeight);
}

const std::vector<size_t>& editBoxLines(const Widget& box) {
    if (!box.editMultiLine) {
        box.editStarts.assign(1, 0);
        box.editStartsWidth = -1.0f;
        return box.editStarts;
    }
    const float scale = box.effScale > 0.0f ? box.effScale : 1.0f;
    const float width =
        (box.rectW > 0.0f ? box.rectW / scale : box.width) - box.textInsetLeft - box.textInsetRight;
    const float size = interfaceFontSize(box.fontHeight);
    if (box.editStartsText != box.editText || box.editStartsWidth != width || box.editStartsSize != size ||
        box.editStartsFace != box.fontFace) {
        box.editStarts = editLines(box.editText, std::max(width, 0.0f),
                                   [&box](const std::string& piece) { return editTextWidth(box, piece); });
        box.editStartsText = box.editText;
        box.editStartsWidth = width;
        box.editStartsSize = size;
        box.editStartsFace = box.fontFace;
    }
    return box.editStarts;
}

float editBoxHeight(const Widget& box) {
    return box.textInsetTop + box.textInsetBottom +
           textHeight(editBoxLines(box).size(), interfaceFontSize(box.fontHeight), box.lineSpacing);
}

size_t editBoxPlaceAt(const Widget& box, float x, float y) {
    const auto& starts = editBoxLines(box);
    const float scale = box.effScale > 0.0f ? box.effScale : 1.0f;
    const float down = (box.bottom + box.rectH - y) / scale - box.textInsetTop;
    const float pitch = interfaceFontSize(box.fontHeight) + box.lineSpacing;
    const size_t line = !box.editMultiLine || down <= 0.0f || pitch <= 0.0f
                            ? 0
                            : std::min(starts.size() - 1, static_cast<size_t>(down / pitch));
    const std::string shown = box.editPassword ? std::string(box.editText.size(), '*') : box.editText;
    return editPlaceIn(shown, starts, line, (x - box.left) / scale - box.textInsetLeft,
                       [&box](const std::string& piece) { return editTextWidth(box, piece); });
}

float fontEmSizeScale(const void* ttfData, size_t byteCount) {
    // The OpenType offsets this reads, all big-endian.
    constexpr uint32_t kTagCollection = 0x74746366u;      // 'ttcf'
    constexpr uint32_t kTagHead = 0x68656164u;            // 'head'
    constexpr uint32_t kTagHhea = 0x68686561u;            // 'hhea'
    constexpr size_t kCollectionFirstFaceOffset = 12;     // TTC header: tag, version, numFonts, offsets[]
    constexpr size_t kOffsetTableBytes = 12;              // sfntVersion, numTables, search fields
    constexpr size_t kNumTablesOffset = 4;
    constexpr size_t kTableRecordBytes = 16;              // tag, checksum, offset, length
    constexpr size_t kTableRecordOffsetField = 8;
    constexpr size_t kHeadUnitsPerEmOffset = 18;
    constexpr size_t kHheaAscenderOffset = 4;
    constexpr size_t kHheaDescenderOffset = 6;

    const auto* d = static_cast<const uint8_t*>(ttfData);
    if (!d || byteCount < kOffsetTableBytes) return 1.0f;

    // A collection points at its first face; a plain font is already there.
    size_t base = 0;
    if (beAt(d, byteCount, 0, 4) == kTagCollection) {
        base = beAt(d, byteCount, kCollectionFirstFaceOffset, 4);
        if (base + kOffsetTableBytes > byteCount) return 1.0f;
    }

    const uint32_t tables = beAt(d, byteCount, base + kNumTablesOffset, 2);
    size_t head = 0, hhea = 0;
    for (uint32_t i = 0; i < tables; ++i) {
        const size_t record = base + kOffsetTableBytes + static_cast<size_t>(i) * kTableRecordBytes;
        const uint32_t tag = beAt(d, byteCount, record, 4);
        if (tag == kTagHead) head = beAt(d, byteCount, record + kTableRecordOffsetField, 4);
        else if (tag == kTagHhea) hhea = beAt(d, byteCount, record + kTableRecordOffsetField, 4);
    }
    if (head == 0 || hhea == 0) return 1.0f;

    const float upem = static_cast<float>(beAt(d, byteCount, head + kHeadUnitsPerEmOffset, 2));
    // Signed, and the descender is negative in every font that has one.
    const auto s16 = [&](size_t at) {
        return static_cast<float>(static_cast<int16_t>(beAt(d, byteCount, at, 2)));
    };
    const float span = s16(hhea + kHheaAscenderOffset) - s16(hhea + kHheaDescenderOffset);
    if (upem <= 0.0f || span <= 0.0f) return 1.0f;

    // A face whose span is wildly different from its em is more likely a
    // misread than a real design, and scaling by it would ruin the text rather
    // than improve it. FRIZQT__ is 1.215 and the other four are near it.
    const float scale = span / upem;
    return (scale >= 0.5f && scale <= 3.0f) ? scale : 1.0f;
}

float fontEmSizeScaleOfFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return 1.0f;
    // The tables this reads are in the first few hundred bytes of every font,
    // but the directory can name them in any order, so the file is read whole
    // rather than guessed at. These are a few hundred kilobytes each.
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
    return fontEmSizeScale(bytes.data(), bytes.size());
}

} // namespace wowee::ui
