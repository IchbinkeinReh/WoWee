#pragma once

struct ImDrawList;

namespace wowee {
namespace rendering {

class VkContext;

/// Interface art the client draws added onto what is under it, blend mode 3
/// (GxBlend_Add: source by its alpha, plus the destination).
///
/// ImGui has one pipeline with one blend state, so the draw list is told to
/// switch: a callback binds a second pipeline - ImGui's own vertex layout,
/// shaders and pipeline layout, added rather than blended - and ImGui's reset
/// callback puts its own pipeline back. Everything drawn between
/// beginAdditive and endAdditive is added.
void initImGuiBlend(VkContext* ctx);
void shutdownImGuiBlend();
void beginAdditive(ImDrawList* list);
void endAdditive(ImDrawList* list);

}  // namespace rendering
}  // namespace wowee
