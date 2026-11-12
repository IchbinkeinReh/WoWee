// quest_poi_layer.cpp - Quest objective markers on the world map.
// Extracted from WorldMap::renderImGuiOverlay (Phase 8 of refactoring plan).
#include "rendering/world_map/layers/quest_poi_layer.hpp"
#include "rendering/world_map/coordinate_projection.hpp"
#include "core/coordinates.hpp"
#include "rendering/polygon_triangulate.hpp"
#include "rendering/vk_context.hpp"
#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <vector>

namespace wowee {
namespace rendering {
namespace world_map {

namespace {

// The objective area, in the same teal the objective marker is drawn in: a
// wash the map reads through, with a firmer edge so the shape is legible on
// noisy ground.
constexpr ImU32 kBlobFill = IM_COL32(0, 210, 255, 40);
constexpr ImU32 kBlobEdge = IM_COL32(0, 210, 255, 150);

}  // namespace

using wowee::rendering::triangulateSimplePolygon;

QuestPOILayer::~QuestPOILayer() {
    if (!vkCtx_) return;
    fill_.texture.destroy(vkCtx_->getDevice(), vkCtx_->getAllocator());
    border_.texture.destroy(vkCtx_->getDevice(), vkCtx_->getAllocator());
}

void QuestPOILayer::initialize(VkContext* ctx, pipeline::AssetManager* am) {
    vkCtx_ = ctx;
    assetManager_ = am;
}

void QuestPOILayer::setBlobStyle(const std::string& fill, uint8_t fillAlpha, const std::string& border,
                                 uint8_t borderAlpha, float borderScalar) {
    fillPath_ = fill;
    borderPath_ = border;
    fillAlpha_ = fillAlpha;
    borderAlpha_ = borderAlpha;
    borderScalar_ = borderScalar;
}

ImTextureID QuestPOILayer::textureFor(Loaded& slot, const std::string& path) {
    if (path.empty() || !vkCtx_ || !assetManager_) return ImTextureID(0);
    if (slot.path != path) {
        slot.texture.destroy(vkCtx_->getDevice(), vkCtx_->getAllocator());
        slot.path = path;
        slot.attempted = false;
    }
    if (!slot.attempted) {
        slot.attempted = true;
        std::string file = path;
        if (file.size() < 4 || file.compare(file.size() - 4, 4, ".blp") != 0) file += ".blp";
        slot.texture = loadImGuiTexture(*assetManager_, *vkCtx_, file);
    }
    return slot.texture ? reinterpret_cast<ImTextureID>(slot.texture.descriptorSet) : ImTextureID(0);
}

void QuestPOILayer::render(const LayerContext& ctx) {
    if (!pois_ || pois_->empty()) return;
    const auto projection = currentProjection(ctx);
    if (!projection) return;

    ImVec2 mp = ImGui::GetMousePos();
    ImFont* qFont = ImGui::GetFont();

    // The shaded areas first, so every marker sits on top of every blob
    // rather than under the next one along.
    const ImVec2 imgMax(ctx.imgMin.x + ctx.displayW, ctx.imgMin.y + ctx.displayH);
    for (const auto& qp : *pois_) {
        if (qp.area.size() < 3) continue;
        std::vector<ImVec2> screen;
        std::vector<glm::vec2> flat;
        screen.reserve(qp.area.size());
        flat.reserve(qp.area.size());
        bool anyOnMap = false;
        for (const auto& pt : qp.area) {
            const glm::vec3 rPos = core::coords::canonicalToRender(glm::vec3(pt.x, pt.y, 0.0f));
            const glm::vec2 uv = renderPosToMapUV(rPos, projection->bounds, projection->isContinent);
            if (uv.x >= 0.0f && uv.x <= 1.0f && uv.y >= 0.0f && uv.y <= 1.0f) anyOnMap = true;
            const float sx = ctx.imgMin.x + uv.x * ctx.displayW;
            const float sy = ctx.imgMin.y + uv.y * ctx.displayH;
            screen.push_back(ImVec2(sx, sy));
            flat.push_back(glm::vec2(sx, sy));
        }
        // An area belonging to another part of the world is not drawn at all;
        // one that only crosses the edge is drawn and cut off there, since
        // clamping its points to the edge would change the shape.
        if (!anyOnMap) continue;
        ctx.drawList->PushClipRect(ctx.imgMin, imgMax, true);
        // With the interface's textures, the client's own mesh (0x0058f1a0):
        // a fan from the centroid, the fill texture's 0,0 there and 1,1 at
        // every point, at the fill alpha; then a strip along each edge, its
        // inner side on the points (0,0) and its outer side pushed out by
        // the border scalar in hundredths of the screen (1,1), mitred where
        // two edges' outer lines meet (0x0058e5c0), at the border alpha.
        const ImTextureID fillTex = textureFor(fill_, fillPath_);
        const ImTextureID borderTex = textureFor(border_, borderPath_);
        if (fillTex || borderTex) {
            const size_t n = screen.size();
            ImVec2 centre(0.0f, 0.0f);
            for (const auto& p : screen) { centre.x += p.x; centre.y += p.y; }
            centre.x /= static_cast<float>(n);
            centre.y /= static_cast<float>(n);
            ImDrawList* dl = ctx.drawList;
            if (fillTex) {
                const ImU32 col = IM_COL32(255, 255, 255, fillAlpha_);
                dl->PushTextureID(fillTex);
                dl->PrimReserve(static_cast<int>(n * 3), static_cast<int>(n + 1));
                const ImDrawIdx base = static_cast<ImDrawIdx>(dl->_VtxCurrentIdx);
                dl->PrimWriteVtx(centre, ImVec2(0.0f, 0.0f), col);
                for (const auto& p : screen) dl->PrimWriteVtx(p, ImVec2(1.0f, 1.0f), col);
                for (size_t i = 0; i < n; ++i) {
                    dl->PrimWriteIdx(base);
                    dl->PrimWriteIdx(static_cast<ImDrawIdx>(base + 1 + i));
                    dl->PrimWriteIdx(static_cast<ImDrawIdx>(base + 1 + (i + 1) % n));
                }
                dl->PopTextureID();
            }
            if (borderTex && borderScalar_ > 0.0f) {
                // A hundredth of the interface's height (0.75 of its width).
                const float width = borderScalar_ * 0.01f * ImGui::GetIO().DisplaySize.y / 0.75f;
                // Each edge's outward offset: away from the centroid.
                std::vector<ImVec2> offs(n);
                for (size_t i = 0; i < n; ++i) {
                    const ImVec2 a = screen[i], b = screen[(i + 1) % n];
                    float nx = -(b.y - a.y), ny = b.x - a.x;
                    const float len = std::sqrt(nx * nx + ny * ny);
                    if (len > 1e-4f) { nx /= len; ny /= len; } else { nx = ny = 0.0f; }
                    if ((centre.x - a.x) * nx + (centre.y - a.y) * ny > 0.0f) { nx = -nx; ny = -ny; }
                    offs[i] = ImVec2(nx * width, ny * width);
                }
                // The outer corner at point i: where edge i-1's and edge i's
                // outer lines cross, else edge i's own offset.
                std::vector<ImVec2> outer(n);
                for (size_t i = 0; i < n; ++i) {
                    const size_t prev = (i + n - 1) % n;
                    const ImVec2 p = screen[i];
                    const ImVec2 a1(screen[prev].x + offs[prev].x, screen[prev].y + offs[prev].y);
                    const ImVec2 d1(p.x - screen[prev].x, p.y - screen[prev].y);
                    const ImVec2 a2(p.x + offs[i].x, p.y + offs[i].y);
                    const ImVec2 b2 = screen[(i + 1) % n];
                    const ImVec2 d2(b2.x - p.x, b2.y - p.y);
                    const float den = d1.x * d2.y - d1.y * d2.x;
                    if (std::fabs(den) > 1e-4f) {
                        const float t = ((a2.x - a1.x) * d2.y - (a2.y - a1.y) * d2.x) / den;
                        outer[i] = ImVec2(a1.x + d1.x * t, a1.y + d1.y * t);
                    } else {
                        outer[i] = a2;
                    }
                }
                const ImU32 col = IM_COL32(255, 255, 255, borderAlpha_);
                dl->PushTextureID(borderTex);
                dl->PrimReserve(static_cast<int>(n * 6), static_cast<int>(n * 2));
                const ImDrawIdx base = static_cast<ImDrawIdx>(dl->_VtxCurrentIdx);
                for (size_t i = 0; i < n; ++i) {
                    dl->PrimWriteVtx(screen[i], ImVec2(0.0f, 0.0f), col);
                    dl->PrimWriteVtx(outer[i], ImVec2(1.0f, 1.0f), col);
                }
                for (size_t i = 0; i < n; ++i) {
                    const auto in0 = static_cast<ImDrawIdx>(base + 2 * i), out0 = static_cast<ImDrawIdx>(in0 + 1);
                    const auto in1 = static_cast<ImDrawIdx>(base + 2 * ((i + 1) % n));
                    const auto out1 = static_cast<ImDrawIdx>(in1 + 1);
                    dl->PrimWriteIdx(in0); dl->PrimWriteIdx(out0); dl->PrimWriteIdx(out1);
                    dl->PrimWriteIdx(in0); dl->PrimWriteIdx(out1); dl->PrimWriteIdx(in1);
                }
                dl->PopTextureID();
            }
            ctx.drawList->PopClipRect();
            continue;
        }
        // Without them, this client's own wash. Cut into triangles rather
        // than handed to AddConvexPolyFilled: a
        // blob that follows a valley or wraps a lake is concave, and a convex
        // fill of one paints over ground the objective is not on.
        const auto tris = triangulateSimplePolygon(flat);
        for (size_t i = 0; i + 2 < tris.size(); i += 3) {
            ctx.drawList->AddTriangleFilled(screen[tris[i]], screen[tris[i + 1]],
                                            screen[tris[i + 2]], kBlobFill);
        }
        ctx.drawList->AddPolyline(screen.data(), static_cast<int>(screen.size()),
                                  kBlobEdge, ImDrawFlags_Closed, 1.5f);
        ctx.drawList->PopClipRect();
    }
    // In FrameXML's frame the markers are its own QuestPOI buttons; the
    // shaded areas are WorldMapBlobFrame's, which the client draws itself.
    if (ctx.hostedInFrame) return;
    for (const auto& qp : *pois_) {
        glm::vec3 rPos = core::coords::canonicalToRender(
            glm::vec3(qp.wowX, qp.wowY, 0.0f));
        glm::vec2 uv = renderPosToMapUV(rPos, projection->bounds, projection->isContinent);
        if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f) continue;

        float px = ctx.imgMin.x + uv.x * ctx.displayW;
        float py = ctx.imgMin.y + uv.y * ctx.displayH;

        const char* marker = nullptr;
        ImU32 fill = IM_COL32(0, 210, 255, 220);
        ImU32 outline = IM_COL32(255, 215, 0, 220);
        const char* description = "Quest Objective";
        switch (qp.kind) {
            case QuestPOI::Kind::AVAILABLE:
                marker = "!";
                fill = IM_COL32(255, 210, 0, 255);
                outline = IM_COL32(80, 55, 0, 230);
                description = "Has a quest for you";
                break;
            case QuestPOI::Kind::AVAILABLE_LOW:
                marker = "!";
                fill = IM_COL32(160, 160, 160, 255);
                outline = IM_COL32(50, 50, 50, 230);
                description = "Has a low-level quest for you";
                break;
            case QuestPOI::Kind::REWARD:
                marker = "?";
                fill = IM_COL32(255, 210, 0, 255);
                outline = IM_COL32(80, 55, 0, 230);
                description = "Quest ready to turn in";
                break;
            case QuestPOI::Kind::INCOMPLETE:
                marker = "?";
                fill = IM_COL32(160, 160, 160, 255);
                outline = IM_COL32(50, 50, 50, 230);
                description = "Quest in progress";
                break;
            case QuestPOI::Kind::OBJECTIVE:
                break;
        }

        ctx.drawList->AddCircleFilled(ImVec2(px, py), 5.0f, fill);
        ctx.drawList->AddCircle(ImVec2(px, py), 5.0f, outline, 0, 1.5f);
        if (marker) {
            const float markerSize = ImGui::GetFontSize() * 0.8f;
            ImVec2 markerSz = qFont->CalcTextSizeA(markerSize, FLT_MAX, 0.0f, marker);
            ctx.drawList->AddText(qFont, markerSize,
                                  ImVec2(px - markerSz.x * 0.5f, py - markerSz.y * 0.5f),
                                  IM_COL32(0, 0, 0, 255), marker);
        }

        // The name goes under an objective and nowhere else.
        //
        // Every marker used to carry one, so a zone with a dozen quest givers
        // in it was a wall of yellow NPC names over the map - and the names
        // were the least of what the marker already said: the ! and the ? say
        // what the NPC is for, and hovering one still names it. What a marker
        // cannot say by its shape is which quest an objective circle belongs
        // to, so that is the one that keeps its label.
        if (qp.kind == QuestPOI::Kind::OBJECTIVE && !qp.name.empty()) {
            ImVec2 nameSz = qFont->CalcTextSizeA(ImGui::GetFontSize() * 0.85f, FLT_MAX, 0.0f, qp.name.c_str());
            float tx = px - nameSz.x * 0.5f;
            float ty = py - nameSz.y - 7.0f;
            ctx.drawList->AddText(qFont, ImGui::GetFontSize() * 0.85f,
                                  ImVec2(tx + 1.0f, ty + 1.0f), IM_COL32(0, 0, 0, 180), qp.name.c_str());
            ctx.drawList->AddText(qFont, ImGui::GetFontSize() * 0.85f,
                                  ImVec2(tx, ty), IM_COL32(255, 230, 100, 230), qp.name.c_str());
        }
        float mdx = mp.x - px, mdy = mp.y - py;
        if (mdx * mdx + mdy * mdy < 49.0f && !qp.name.empty()) {
            ImGui::SetTooltip("%s\n(%s)", qp.name.c_str(), description);
        }
    }
}

} // namespace world_map
} // namespace rendering
} // namespace wowee
