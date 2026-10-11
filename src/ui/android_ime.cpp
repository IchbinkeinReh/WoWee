#include "ui/android_ime.hpp"

#include "core/logger.hpp"

#include <atomic>

#ifdef __ANDROID__
#include <jni.h>
#endif

namespace wowee {
namespace ui {

namespace {
std::atomic<int> gImeVisibility{-1};
}

int reportedImeVisibility() {
    return gImeVisibility.load(std::memory_order_acquire);
}

void noteImeVisibility(bool visible) {
    const int now = visible ? 1 : 0;
    const int before = gImeVisibility.exchange(now, std::memory_order_acq_rel);
    if (before != now) {
        LOG_INFO("On-screen keyboard ", visible ? "shown" : "hidden",
                 " (window IME inset)");
    }
}

} // namespace ui
} // namespace wowee

#ifdef __ANDROID__
// com.wowee.client.WoweeActivity.nativeImeVisibilityChanged(boolean). Called
// from the activity's window-insets listener on the UI thread.
extern "C" JNIEXPORT void JNICALL
Java_com_wowee_client_WoweeActivity_nativeImeVisibilityChanged(JNIEnv*, jclass,
                                                               jboolean visible) {
    wowee::ui::noteImeVisibility(visible == JNI_TRUE);
}
#endif
