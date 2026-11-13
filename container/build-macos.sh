#!/bin/bash
# macOS cross-compile entrypoint - runs INSIDE the macos container.
# Toolchain: osxcross + Apple Clang, target: arm64-apple-darwin (default) or
#            x86_64-apple-darwin when MACOS_ARCH=x86_64.
# Bind-mounts:
#   /src  (ro) - project source
#   /out  (rw) - host ./build/macos

set -euo pipefail

SRC=/src
OUT=/out
NPROC=$(nproc)

# Arch selection: arm64 (Apple Silicon) is the default primary target.
ARCH="${MACOS_ARCH:-arm64}"
case "${ARCH}" in
    arm64)   VCPKG_TRIPLET=arm64-osx-cross  ;;
    x86_64)  VCPKG_TRIPLET=x64-osx-cross    ;;
    *)  echo "ERROR: unsupported MACOS_ARCH '${ARCH}'. Use arm64 or x86_64." ; exit 1 ;;
esac

# Auto-detect darwin target from osxcross binaries (e.g. arm64-apple-darwin24.5).
OSXCROSS_BIN=/opt/osxcross/target/bin
TARGET=$(basename "$(ls "${OSXCROSS_BIN}/${ARCH}-apple-darwin"*-clang 2>/dev/null | head -1)" | sed 's/-clang$//')
if [[ -z "${TARGET}" ]]; then
    echo "ERROR: could not find osxcross ${ARCH} compiler in ${OSXCROSS_BIN}" >&2
    exit 1
fi
echo "==> Detected osxcross target: ${TARGET}"

# Commit the source checkout records for a submodule path; empty when /src is not a
# git checkout. The copied tree drops .git, so ask the bind-mounted source.
recorded_commit() {
    git -c safe.directory='*' -C "${SRC}" rev-parse -q --verify "HEAD:$1" 2>/dev/null || true
}

# Shallow-fetch one ref (branch, tag or commit SHA) of a repository into a directory.
fetch_at_ref() {
    local repo="$1" ref="$2" dir="$3"
    git init -q "$dir" &&
        git -C "$dir" fetch -q --depth 1 "$repo" "$ref" &&
        git -C "$dir" checkout -q --detach FETCH_HEAD
}

echo "==> [macos/${ARCH}] Copying source tree..."
mkdir -p /wowee-build-src
tar -C "${SRC}" \
    --exclude='./build' --exclude='./logs' --exclude='./cache' \
    --exclude='./container' --exclude='./.git' \
    --exclude='./Data/character' --exclude='./Data/creature' \
    --exclude='./Data/db' --exclude='./Data/environment' \
    --exclude='./Data/interface' --exclude='./Data/item' \
    --exclude='./Data/misc' --exclude='./Data/sound' \
    --exclude='./Data/spell' --exclude='./Data/terrain' \
    --exclude='./Data/world' \
    -cf - . | tar -C /wowee-build-src -xf -

cd /wowee-build-src

echo "==> [macos/${ARCH}] Fetching external SDKs (if needed)..."
if [ ! -f extern/FidelityFX-FSR2/src/ffx-fsr2-api/ffx_fsr2.h ]; then
    FSR2_REF="$(recorded_commit extern/FidelityFX-FSR2)"
    fetch_at_ref https://github.com/GPUOpen-Effects/FidelityFX-FSR2.git \
        "${FSR2_REF:-master}" extern/FidelityFX-FSR2 || echo "Warning: FSR2 clone failed"
fi

SDK_REPO="${WOWEE_FFX_SDK_REPO:-https://github.com/Kelsidavis/FidelityFX-SDK.git}"
SDK_REF="${WOWEE_FFX_SDK_REF:-$(recorded_commit extern/FidelityFX-SDK)}"
SDK_REF="${SDK_REF:-main}"
if [ ! -f "extern/FidelityFX-SDK/Kits/FidelityFX/framegeneration/fsr3/include/ffx_frameinterpolation.h" ]; then
    fetch_at_ref "${SDK_REPO}" "${SDK_REF}" extern/FidelityFX-SDK \
        || echo "Warning: FidelityFX-SDK clone failed"
fi

echo "==> [macos/${ARCH}] Configuring with CMake (Release, Ninja, osxcross ${TARGET})..."
cmake -S . -B "${OUT}" \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_SYSTEM_NAME=Darwin \
    -DCMAKE_OSX_ARCHITECTURES="${ARCH}" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-13.0}" \
    -DCMAKE_C_COMPILER="${OSXCROSS_BIN}/${TARGET}-clang" \
    -DCMAKE_CXX_COMPILER="${OSXCROSS_BIN}/${TARGET}-clang++" \
    -DCMAKE_AR="${OSXCROSS_BIN}/${TARGET}-ar" \
    -DCMAKE_RANLIB="${OSXCROSS_BIN}/${TARGET}-ranlib" \
    -DCMAKE_TOOLCHAIN_FILE="${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" \
    -DVCPKG_TARGET_TRIPLET="${VCPKG_TRIPLET}" \
    -DVCPKG_OVERLAY_TRIPLETS=/opt/vcpkg-triplets \
    -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF \
    -DWOWEE_ENABLE_ASAN=OFF

echo "==> [macos/${ARCH}] Building with ${NPROC} cores..."
cmake --build "${OUT}" --parallel "${NPROC}"

echo ""
echo "==> [macos/${ARCH}] Build complete. Artifacts in: ./build/macos/"
echo "    Binary: ./build/macos/bin/wowee"
