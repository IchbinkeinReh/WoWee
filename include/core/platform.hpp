#pragma once

// Which platform this is, once, for the code that differs by it.
//
//   WOWEE_MOBILE  a phone or tablet: Android or iOS. Touch controls, a
//                 fullscreen landscape window, the smaller memory budgets,
//                 the cheaper shading defaults - everything the Android port
//                 did for being a phone, which iOS wants the same.
//   WOWEE_IOS     iOS (and iPadOS) itself: MoltenVK linked into the app, the
//                 Documents folder for data, no JIT.
//   WOWEE_MACOS   macOS: AppKit, the bundled MoltenVK ICD, Homebrew paths.
//
// __APPLE__ alone is both macOS and iOS; __ANDROID__ alone is still the
// right test for what is Android's own (JNI, logcat, the activity).

#if defined(__APPLE__)
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
#define WOWEE_IOS 1
#else
#define WOWEE_MACOS 1
#endif
#endif

#if defined(__ANDROID__) || defined(WOWEE_IOS)
#define WOWEE_MOBILE 1
#endif
