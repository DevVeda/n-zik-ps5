#!/usr/bin/env bash
# Builds a small static FFmpeg for the PS5: demux + decode of what N-Zik serves
# (YouTube Music webm/opus and mp4/aac, plus local mp3/flac/ogg/wav) and swresample.
# No network, no programs, no x86 asm (no nasm needed).
#   FFMPEG_SRC  FFmpeg 7.1 checkout (default ../n-zik-ps5-refs/ffmpeg)
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
src=${FFMPEG_SRC:-$(cd "$root/../n-zik-ps5-refs/ffmpeg" && pwd)}
sdk=${PS5_PAYLOAD_SDK:-$(cd "$root/../ps5-payload-sdk" && pwd)}
[[ -d /opt/homebrew/opt/llvm@18/bin ]] && export PATH=/opt/homebrew/opt/llvm@18/bin:$PATH
work=$root/build/ffmpeg
prefix=$root/build/ffmpeg-install
mkdir -p "$work"
cd "$work"
bash "$src/configure" --prefix="$prefix" \
    --enable-cross-compile --arch=x86_64 --target-os=freebsd \
    --cc="$sdk/bin/prospero-clang" --cxx="$sdk/bin/prospero-clang++" \
    --ar="$sdk/bin/prospero-ar" --ranlib="$sdk/bin/prospero-ranlib" --nm="$sdk/bin/prospero-nm" \
    --strip="$sdk/bin/prospero-strip" --pkg-config=false \
    --disable-autodetect --disable-everything --disable-programs --disable-doc --disable-debug \
    --disable-avdevice --disable-avfilter --disable-swscale --disable-network --disable-x86asm \
    --disable-pthreads --disable-runtime-cpudetect \
    --enable-demuxer=matroska,mov,ogg,mp3,flac,wav,aac \
    --enable-decoder=opus,aac,aac_latm,vorbis,mp3,mp3float,flac,pcm_s16le,pcm_s24le,pcm_f32le,alac \
    --enable-parser=opus,aac,vorbis,mpegaudio,flac \
    --enable-swresample --enable-small --enable-pic --disable-shared --enable-static \
    --extra-cflags="-Os -ffunction-sections -fdata-sections"
make -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc)"
make install
