#!/bin/bash
# Windows cross-compile entrypoint - runs INSIDE the windows container.
# Toolchain: LLVM-MinGW (Clang + LLD), target: x86_64-w64-mingw32-ucrt
# Bind-mounts:
#   /src  (ro) - project source
#   /out  (rw) - host ./build/windows

set -euo pipefail

SRC=/src
OUT=/out
NPROC=$(nproc)
TARGET=x86_64-w64-mingw32

# vcpkg's MinGW applocal hook always appends a powershell.exe post-build step to
# copy DLLs next to each binary, even when VCPKG_APPLOCAL_DEPS=OFF.  For the
# x64-mingw-static triplet the bin/ dir is empty (no DLLs) so the script does
# nothing - but it still needs to exit 0.  Provide a no-op stub if the real
# PowerShell isn't available.
if ! command -v powershell.exe &>/dev/null; then
    printf '#!/bin/sh\nexit 0\n' > /usr/local/bin/powershell.exe
    chmod +x /usr/local/bin/powershell.exe
fi

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

echo "==> [windows] Copying source tree..."
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

echo "==> [windows] Fetching external SDKs (if needed)..."
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

echo "==> [windows] Generating Vulkan import library for cross-compile..."
# Windows applications link against vulkan-1.dll (the Khronos Vulkan loader).
# The cross-compile toolchain only ships Vulkan *headers* (via vcpkg), not the
# import library.  Generate a minimal libvulkan-1.a from the header prototypes
# so the linker can resolve vk* symbols → vulkan-1.dll at runtime.
# We use the host libvulkan-dev header for function name extraction - the Vulkan
# API prototypes are platform-independent.
VULKAN_IMP_DIR="${OUT}/vulkan-import"
if [ ! -f "${VULKAN_IMP_DIR}/libvulkan-1.a" ]; then
    mkdir -p "${VULKAN_IMP_DIR}"
    # Try vcpkg-installed header first (available on incremental builds),
    # then fall back to the host libvulkan-dev header (always present in the image).
    VK_HEADER="${OUT}/vcpkg_installed/x64-mingw-static/include/vulkan/vulkan_core.h"
    if [ ! -f "${VK_HEADER}" ]; then
        VK_HEADER="/usr/include/vulkan/vulkan_core.h"
    fi
    {
        echo "LIBRARY vulkan-1.dll"
        echo "EXPORTS"
        grep -oP 'VKAPI_ATTR \S+ VKAPI_CALL \K(vk\w+)' "${VK_HEADER}" | sort -u | sed 's/^/    /'
    } > "${VULKAN_IMP_DIR}/vulkan-1.def"
    "${TARGET}-dlltool" -d "${VULKAN_IMP_DIR}/vulkan-1.def" \
        -l "${VULKAN_IMP_DIR}/libvulkan-1.a" -m i386:x86-64
    echo "   Generated $(wc -l < "${VULKAN_IMP_DIR}/vulkan-1.def") export entries"
fi

echo "==> [windows] Configuring with CMake (Release, Ninja, LLVM-MinGW cross)..."
# Lock pkg-config to the cross-compiled vcpkg packages only.
# Without this, CMake's Vulkan pkg-config fallback finds the *Linux* libvulkan-dev
# and injects /usr/include into every MinGW compile command, which then fails
# because the glibc-specific bits/libc-header-start.h is not in the MinGW sysroot.
export PKG_CONFIG_LIBDIR="${OUT}/vcpkg_installed/x64-mingw-static/lib/pkgconfig"
export PKG_CONFIG_PATH=""
cmake -S . -B "${OUT}" \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_SYSTEM_NAME=Windows \
    -DCMAKE_C_COMPILER="${TARGET}-clang" \
    -DCMAKE_CXX_COMPILER="${TARGET}-clang++" \
    -DCMAKE_RC_COMPILER="${TARGET}-windres" \
    -DCMAKE_AR="/opt/llvm-mingw/bin/llvm-ar" \
    -DCMAKE_RANLIB="/opt/llvm-mingw/bin/llvm-ranlib" \
    -DCMAKE_EXE_LINKER_FLAGS="-fuse-ld=lld" \
    -DCMAKE_SHARED_LINKER_FLAGS="-fuse-ld=lld" \
    -DCMAKE_TOOLCHAIN_FILE="${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" \
    -DVCPKG_TARGET_TRIPLET=x64-mingw-static \
    -DVCPKG_APPLOCAL_DEPS=OFF \
    -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF \
    -DWOWEE_ENABLE_ASAN=OFF

echo "==> [windows] Building with ${NPROC} cores..."
cmake --build "${OUT}" --parallel "${NPROC}"

echo ""
echo "==> [windows] Build complete. Artifacts in: ./build/windows/"
echo "    Binary: ./build/windows/bin/wowee.exe"
