#!/usr/bin/env bash
# Builds the Mali capability probe for arm64-v8a Android (bionic).
#
#   ANDROID_NDK_HOME=/path/to/android-ndk-r27c ./mali-poc/build.sh
#
# The resulting binary is position independent, links libc++ statically and
# only needs the platform libraries that ship on every Android device
# (libandroid.so, liblog.so, libvulkan.so, libEGL.so, libGLESv2.so).
set -euo pipefail

NDK="${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}"
if [[ -z "${NDK}" ]]; then
    echo "error: set ANDROID_NDK_HOME to an NDK r26/r27 install" >&2
    exit 1
fi

API="${API:-33}"
HOST_TAG="${HOST_TAG:-linux-x86_64}"
CXX="${NDK}/toolchains/llvm/prebuilt/${HOST_TAG}/bin/aarch64-linux-android${API}-clang++"

if [[ ! -x "${CXX}" ]]; then
    echo "error: ${CXX} not found" >&2
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

"${CXX}" \
    -O2 -g -std=c++17 -fPIE -pie -static-libstdc++ \
    -Wall -Wextra -Wno-unused-parameter \
    -o "${SCRIPT_DIR}/mali_probe" \
    "${SCRIPT_DIR}/probe.cpp" \
    -landroid -llog -lvulkan -lEGL -lGLESv2

echo "built: ${SCRIPT_DIR}/mali_probe"
"${NDK}/toolchains/llvm/prebuilt/${HOST_TAG}/bin/llvm-readelf" -d "${SCRIPT_DIR}/mali_probe" \
    | grep -E "NEEDED|SONAME" || true
