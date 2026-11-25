#pragma once

namespace wowee::rendering {

/// Whether a model's triangles are drawn. A unit's twin, made for its particles and ribbons alone
/// (M2Renderer::setModelParticlesOnly), never has them drawn; a trap's are not to be seen in the
/// world, but are in a view of one model, which is asked for by its file (setForceNoCull).
constexpr bool m2MeshDrawn(bool valid, bool invisibleTrap, bool particlesOnly, bool forceNoCull) {
    return valid && !particlesOnly && (forceNoCull || !invisibleTrap);
}

}  // namespace wowee::rendering
