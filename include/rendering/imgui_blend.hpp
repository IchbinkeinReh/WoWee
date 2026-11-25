#pragma once

#include <cstdint>

struct ImDrawList;

namespace wowee {
namespace rendering {

class VkContext;

/// How what is drawn next is put on what is under it. Interface art has five ways
/// (ui/texture_blend.hpp: GxBlend's); Premultiplied is a picture whose colours have their
/// alpha in them already, a model frame's (its tint likewise): what is added in it is added
/// to what is under it.
///
/// ImGui has one pipeline with one blend state, so the draw list is told to
/// switch: a callback binds another pipeline - ImGui's own vertex layout,
/// shaders and pipeline layout, blended otherwise - and for Blend ImGui's reset
/// callback puts its own pipeline back.
enum class ImGuiBlend : uint8_t { Blend, Add, Mod, Opaque, AlphaKey, Premultiplied };
void initImGuiBlend(VkContext* ctx);
void shutdownImGuiBlend();
void setImGuiBlend(ImDrawList* list, ImGuiBlend blend);

}  // namespace rendering
}  // namespace wowee
