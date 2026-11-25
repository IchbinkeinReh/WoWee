#pragma once

#include <cstdint>
#include <string>

#include <glm/glm.hpp>

namespace wowee::ui {

/// What is to be seen on a unit's nameplate (addons/nameplate_lua.hpp draws it).
struct NamePlateView {
    uint64_t guid = 0;
    /// The middle of its top, in units of a 768th of the screen's height from the bottom left.
    float x = 0.0f, y = 0.0f;
    bool target = false;
    float alpha = 1.0f;
    std::string name;
    glm::vec3 nameColor{1.0f};
    int level = 0;   ///< 0: the skull
    glm::vec3 levelColor{1.0f};
    bool elite = false;
    int raidIcon = -1;
    uint32_t health = 0, maxHealth = 0;
    glm::vec3 healthColor{1.0f};
    bool threat = false;
    glm::vec3 threatColor{1.0f};
    bool glowing = false;
    /// The cast bar; no spell, none.
    uint32_t castSpell = 0;
    float castMin = 0.0f, castMax = 0.0f, castValue = 0.0f;
    glm::vec3 castColor{1.0f};
    float castAlpha = 1.0f;
    bool castShield = false;
};

}  // namespace wowee::ui
