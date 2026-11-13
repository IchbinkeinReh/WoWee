#!/bin/bash
# Wowee Build Script - Ensures no stale binaries

set -e  # Exit on error

cd "$(dirname "$0")"

# Commit the superproject records for a submodule path; empty outside a git checkout.
recorded_commit() {
    git rev-parse -q --verify "HEAD:$1" 2>/dev/null || true
}

# Shallow-fetch one ref (branch, tag or commit SHA) of a repository into a directory.
fetch_at_ref() {
    local repo="$1" ref="$2" dir="$3"
    git init -q "$dir" &&
        git -C "$dir" fetch -q --depth 1 "$repo" "$ref" &&
        git -C "$dir" checkout -q --detach FETCH_HEAD
}

ensure_fsr2_sdk() {
    local sdk_dir="extern/FidelityFX-FSR2"
    local sdk_header="$sdk_dir/src/ffx-fsr2-api/ffx_fsr2.h"
    if [ -f "$sdk_header" ]; then
        return
    fi
    if ! command -v git >/dev/null 2>&1; then
        echo "Warning: git not found; cannot auto-fetch AMD FSR2 SDK."
        return
    fi
    local sdk_ref
    sdk_ref="$(recorded_commit "$sdk_dir")"
    sdk_ref="${sdk_ref:-master}"
    echo "Fetching AMD FidelityFX FSR2 SDK ($sdk_ref) into $sdk_dir ..."
    mkdir -p extern
    fetch_at_ref https://github.com/GPUOpen-Effects/FidelityFX-FSR2.git "$sdk_ref" "$sdk_dir" || {
        echo "Warning: failed to clone AMD FSR2 SDK. Build will use internal fallback path."
    }
}

ensure_fidelityfx_sdk() {
    local sdk_dir="extern/FidelityFX-SDK"
    local sdk_header="$sdk_dir/Kits/FidelityFX/framegeneration/fsr3/include/ffx_frameinterpolation.h"
    local sdk_repo="${WOWEE_FFX_SDK_REPO:-https://github.com/Kelsidavis/FidelityFX-SDK.git}"
    if [ -f "$sdk_header" ]; then
        return
    fi
    if ! command -v git >/dev/null 2>&1; then
        echo "Warning: git not found; cannot auto-fetch AMD FidelityFX SDK."
        return
    fi
    local sdk_ref="${WOWEE_FFX_SDK_REF:-$(recorded_commit "$sdk_dir")}"
    sdk_ref="${sdk_ref:-main}"
    echo "Fetching AMD FidelityFX SDK ($sdk_ref from $sdk_repo) into $sdk_dir ..."
    mkdir -p extern
    fetch_at_ref "$sdk_repo" "$sdk_ref" "$sdk_dir" || {
        echo "Warning: failed to clone AMD FidelityFX SDK. FSR3 framegen extern will be unavailable."
    }
}

echo "Building wowee..."
ensure_fsr2_sdk
ensure_fidelityfx_sdk

# Create build directory if it doesn't exist
mkdir -p build
cd build

# Configure with cmake
echo "Configuring with CMake..."
cmake .. -DCMAKE_BUILD_TYPE=Release

# Build with all cores
NPROC=$(nproc 2>/dev/null || sysctl -n hw.logicalcpu 2>/dev/null || echo 4)
echo "Building with $NPROC cores..."
cmake --build . --parallel "$NPROC"

# Ensure Data symlink exists in bin directory
cd bin
if [ ! -e Data ]; then
    ln -s ../../Data Data
fi
cd ..

echo ""
echo "Build complete! Binary: build/bin/wowee"
echo "Run with: cd build/bin && ./wowee"
