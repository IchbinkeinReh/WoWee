#include "core/pointer_speed.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace wowee::core::pointer_speed {

#ifdef _WIN32
namespace {

// The desktop's speed, read once before anything is set (0x00d4154c), and
// the speed last put on the system (0x00d41548's counterpart).
int g_desktop = -1;
int g_applied = -1;
float g_mouseSpeed = -1.0f;  // none set yet: the desktop's own
bool g_active = false;

int desktopSpeed() {
    if (g_desktop < 0) {
        int speed = 10;
        if (!SystemParametersInfoA(SPI_GETMOUSESPEED, 0, &speed, 0)) speed = 10;
        g_desktop = speed;
    }
    return g_desktop;
}

void setSpeed(int speed) {
    if (speed == g_applied) return;
    // SPI_SETMOUSESPEED takes the speed itself as the pointer argument, and
    // no fWinIni: the setting holds for the session, not the user's profile.
    SystemParametersInfoA(SPI_SETMOUSESPEED, 0,
                          reinterpret_cast<void*>(static_cast<intptr_t>(speed)), 0);
    g_applied = speed;
}

}  // namespace

const char* desktopDefault() {
    static char text[16] = {};
    if (!text[0]) std::snprintf(text, sizeof(text), "%1.1f", static_cast<double>(desktopSpeed()) * 0.1);
    return text;
}

static void reapply() {
    const int desktop = desktopSpeed();
    if (!g_active || g_mouseSpeed < 0.0f) {
        if (g_applied >= 0) setSpeed(desktop);
        return;
    }
    // 0x0086a070: clamped to 0.1 - 2.0, times ten, to the nearest whole speed.
    const float clamped = std::clamp(g_mouseSpeed, 0.1f, 2.0f);
    setSpeed(static_cast<int>(std::lrint(clamped * 10.0f)));
}

void setMouseSpeed(float mouseSpeed) {
    g_mouseSpeed = mouseSpeed;
    reapply();
}

void setActive(bool active) {
    g_active = active;
    reapply();
}

void restore() {
    if (g_desktop >= 0 && g_applied >= 0 && g_applied != g_desktop) setSpeed(g_desktop);
}

#else

const char* desktopDefault() { return "1.0"; }
void setMouseSpeed(float) {}
void setActive(bool) {}
void restore() {}

#endif

}  // namespace wowee::core::pointer_speed
