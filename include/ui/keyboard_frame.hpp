#pragma once

#include "ui/widget_tree.hpp"

#include <cstdint>

namespace wowee::ui {

/// The frame a key goes to (0x004943c0): of those shown that listen for keys,
/// the topmost that takes it - one with a script for it (0x0048fb80,
/// 0x0048fb10). A frame that listens and has none is passed over: enableKeyboard
/// alone takes nothing, and StaticPopupTemplate has it. 0 for none.
template <class Takes>
[[nodiscard]] uint32_t topKeyboardFrame(const WidgetTree& tree, Takes&& takes) {
    const Widget* best = nullptr;
    for (uint32_t id = tree.nextMaybeVisible(1); id != 0; id = tree.nextMaybeVisible(id + 1)) {
        const Widget* w = tree.get(id);
        if (!w || !w->keyboardEnabled || !w->visible || !tree.shownNow(id)) continue;
        if (best && (w->effStrata < best->effStrata ||
                     (w->effStrata == best->effStrata && w->effLevel < best->effLevel)))
            continue;
        if (takes(w->id)) best = w;
    }
    return best ? best->id : 0;
}

}  // namespace wowee::ui
