#include "game/interface_text.hpp"

#include "addons/addon_manager.hpp"
#include "addons/lua_engine.hpp"
#include "core/application.hpp"

namespace wowee {
namespace game {

std::string interfaceString(const char* key, const char* enUS) {
    if (auto* addons = core::Application::getInstance().getAddonManager()) {
        if (auto* lua = addons->getLuaEngine(); lua && lua->isInitialized()) {
            std::string text = lua->globalText(key);
            if (!text.empty()) return text;
        }
    }
    return enUS ? std::string(enUS) : std::string();
}

}  // namespace game
}  // namespace wowee
