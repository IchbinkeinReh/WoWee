#!/bin/bash
# Builds the two self-contained Windows executables a release offers on their
# own: wowee.exe and wowee_vr.exe, each to be copied alone into an original
# 3.3.5a folder beside Wow.exe (drop-in mode, include/core/dropin.hpp).
#
# Both are linked statically (WOWEE_STATIC_RUNTIME) with the minimal LGPL
# FFmpeg of tools/build-ffmpeg-minimal.sh, and carry this client's own files
# (assets, addons, Data) appended by wowee_pack. They import only Windows' own
# DLLs and vulkan-1.dll, the system's Vulkan loader; this script fails if
# either imports anything else.
#
# Usage (MSYS2 UCRT64 or MINGW64 shell, x86-64, the client's build
# dependencies, StormLib and GNU make installed):
#   tools/ci/build_windows_dropin.sh <output dir>
set -euo pipefail

out=$1
src=$(cd "$(dirname "$0")/../.." && pwd)
work=${WOWEE_DROPIN_WORK:-/tmp/wowee-dropin}
mkdir -p "$work" "$out"

# FFmpeg, from the official release tarball at a pinned hash.
FFMPEG_VERSION=9.0.2
FFMPEG_SHA256=8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e
tarball="$work/ffmpeg-${FFMPEG_VERSION}.tar.xz"
if [ ! -f "$tarball" ]; then
    curl -fsSL --retry 3 -o "$tarball" "https://ffmpeg.org/releases/ffmpeg-${FFMPEG_VERSION}.tar.xz"
fi
echo "${FFMPEG_SHA256}  ${tarball}" | sha256sum -c -
tar -xf "$tarball" -C "$work"
bash "$src/tools/build-ffmpeg-minimal.sh" "$work/ffmpeg-${FFMPEG_VERSION}" "$work/ffmpeg" "$work/ffmpeg-build"

cmake -S "$src" -B "$work/build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DWOWEE_STATIC_RUNTIME=ON \
    -DWOWEE_BUILD_TESTS=OFF \
    -DWOWEE_BUILD_VR=ON \
    -DWOWEE_FFMPEG_ROOT="$work/ffmpeg"
cmake --build "$work/build" --parallel "$(nproc)" --target wowee_dropin wowee_vr_dropin

# Only Windows' own DLLs and the Vulkan loader. Anything else would have to
# sit beside the exe, which is what these two are not to need.
allowed='^(advapi32|bcrypt|crypt32|gdi32|imm32|kernel32|msvcrt|ole32|oleaut32|setupapi|shell32|user32|version|winmm|ws2_32|vulkan-1|api-ms-win-[a-z0-9-]+|ucrtbase|dwmapi|shlwapi|hid|cfgmgr32|mfplat|mfuuid|uuid|dxgi|d3d11|dbghelp|psapi|iphlpapi|userenv|ntdll|secur32|comdlg32|winhttp|wininet|dnsapi|rpcrt4|powrprof|mf|mfreadwrite|evr|strmiids)\.dll$'
for exe in wowee.exe wowee_vr.exe; do
    f="$work/build/dropin/$exe"
    test -s "$f"
    bad=$(objdump -p "$f" | awk '/DLL Name:/ { print tolower($3) }' | sort -u | grep -Ev "$allowed" || true)
    if [ -n "$bad" ]; then
        echo "$exe imports DLLs that are not Windows' own:" >&2
        echo "$bad" >&2
        exit 1
    fi
    echo "$exe imports: $(objdump -p "$f" | awk '/DLL Name:/ { print $3 }' | sort -u | tr '\n' ' ')"
    cp "$f" "$out/$exe"
done
ls -l "$out"
