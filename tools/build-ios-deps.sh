#!/usr/bin/env bash
# Builds and fetches the iOS dependencies CMake cannot get for itself.
#
# The iOS counterpart of tools/build-android-deps.sh. SDL3 and glm come down
# through FetchContent in CMakeLists, as on Android; these two do not:
#
#   OpenSSL  built here once, static, for iOS arm64 (no CMake build of its own).
#   MoltenVK the release package from KhronosGroup/MoltenVK: the static library
#            for iOS and the Vulkan headers it was built against. An iOS app
#            links its Vulkan driver in; there is no loader to install.
#
#   tools/build-ios-deps.sh
#
# Needs a Mac with Xcode. The result lands in build-ios-deps/, gitignored, and
# the last lines say what to hand CMake.
set -euo pipefail

OPENSSL_VERSION="${OPENSSL_VERSION:-3.5.1}"
MOLTENVK_VERSION="${MOLTENVK_VERSION:-v1.3.0}"
IOS_MIN="${IOS_MIN:-15.0}"

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="$ROOT/build-ios-deps"
PREFIX="$WORK/openssl"
mkdir -p "$WORK"

if [ -f "$PREFIX/lib/libcrypto.a" ]; then
    echo "OpenSSL already built: $PREFIX"
else
    TARBALL="$WORK/openssl-$OPENSSL_VERSION.tar.gz"
    SRC="$WORK/openssl-$OPENSSL_VERSION"
    [ -f "$TARBALL" ] || curl -fL --retry 3 -o "$TARBALL" \
        "https://github.com/openssl/openssl/releases/download/openssl-$OPENSSL_VERSION/openssl-$OPENSSL_VERSION.tar.gz"
    [ -d "$SRC" ] || tar -xzf "$TARBALL" -C "$WORK"
    # ios64-xcrun: arm64 device, through xcrun's iphoneos SDK. Static, no
    # tests, no command-line tools, as on Android.
    (cd "$SRC" && ./Configure ios64-xcrun no-shared no-tests no-apps no-dso \
        "-mios-version-min=$IOS_MIN" --prefix="$PREFIX" --openssldir="$PREFIX/ssl")
    make -C "$SRC" -j"$(sysctl -n hw.logicalcpu)"
    make -C "$SRC" install_sw
fi

MVK_DIR="$WORK/moltenvk"
if [ ! -d "$MVK_DIR/MoltenVK" ]; then
    mkdir -p "$MVK_DIR"
    TAR="$WORK/MoltenVK-ios.tar"
    [ -f "$TAR" ] || curl -fL --retry 3 -o "$TAR" \
        "https://github.com/KhronosGroup/MoltenVK/releases/download/$MOLTENVK_VERSION/MoltenVK-ios.tar"
    tar -xf "$TAR" -C "$MVK_DIR"
fi
# Found rather than spelled out: the package's layout has moved between
# releases. The device slice, not the simulator's.
MVK_LIB="$(find "$MVK_DIR" -path '*MoltenVK.xcframework/ios-arm64/*' -name 'libMoltenVK.a' | head -1)"
VK_INCLUDE="$(dirname "$(dirname "$(find "$MVK_DIR" -path '*/include/vulkan/vulkan.h' | head -1)")")"
if [ -z "$MVK_LIB" ] || [ -z "$VK_INCLUDE" ]; then
    echo "MoltenVK package has no ios-arm64 libMoltenVK.a or Vulkan headers:" >&2
    find "$MVK_DIR" -maxdepth 4 >&2
    exit 1
fi

echo
echo "Configure the client with:"
echo "  -DOPENSSL_ROOT_DIR=$PREFIX -DCMAKE_FIND_ROOT_PATH=$PREFIX"
echo "  -DWOWEE_MOLTENVK_LIB=$MVK_LIB -DWOWEE_VULKAN_INCLUDE_DIR=$VK_INCLUDE"
# For a CI step to pick up.
{
    echo "OPENSSL_ROOT_DIR=$PREFIX"
    echo "WOWEE_MOLTENVK_LIB=$MVK_LIB"
    echo "WOWEE_VULKAN_INCLUDE_DIR=$VK_INCLUDE"
} > "$WORK/paths.env"
