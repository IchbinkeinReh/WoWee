#!/bin/bash
# Builds the minimal, LGPL-only, static FFmpeg the self-contained drop-in
# executable links (cmake -DWOWEE_STATIC_RUNTIME=ON -DWOWEE_FFMPEG_ROOT=<prefix>).
#
# Usage (MSYS2, MINGW64 shell, GNU make installed):
#   tools/build-ffmpeg-minimal.sh <ffmpeg source dir> <install prefix>
# The source is an official release tarball from ffmpeg.org/releases, checked
# against its .asc signature (key ffmpeg.org/ffmpeg-devel.asc,
# FCF9 86EA 15E6 E293 A564 4F10 B432 2F04 D676 58D8). It is not part of WoWee.
#
# What is in it, and for what:
#   the screen recorder (src/core/screen_recorder.cpp): encoders mpeg4, aac and
#     h264_mf (Media Foundation, part of Windows; it needs d3d11va), the mp4
#     muxer, swscale;
#   the movies (SMSG_TRIGGER_MOVIE, Interface\Cinematics\*.avi: MPEG-4 'DXGM'
#     video, MP3 sound): decoders mpeg4 and mp3float, the avi demuxer, their
#     parsers, swresample.
# No --enable-gpl, no external codec libraries, no assembly (no nasm needed).
set -euo pipefail
src=$(cd "$1" && pwd)
prefix=$2
build=${3:-$src/../build-minimal}
mkdir -p "$build" "$prefix"
cd "$build"
"$src/configure" \
    --prefix="$prefix" \
    --enable-static --disable-shared \
    --disable-everything --disable-autodetect \
    --disable-programs --disable-doc --disable-network --disable-debug \
    --disable-x86asm \
    --disable-avdevice --disable-avfilter \
    --enable-avcodec --enable-avformat --enable-avutil --enable-swscale --enable-swresample \
    --enable-mediafoundation --enable-d3d11va \
    --enable-encoder=mpeg4,aac,h264_mf \
    --enable-muxer=mp4 \
    --enable-decoder=mpeg4,mp3float,mp3 \
    --enable-demuxer=avi \
    --enable-parser=mpeg4video,mpegaudio \
    --enable-protocol=file
make -j"$(nproc)"
make install
