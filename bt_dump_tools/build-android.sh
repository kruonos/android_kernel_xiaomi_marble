#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
OUT_DIR="${ROOT_DIR}/out/android-arm64"
API_LEVEL="${API_LEVEL:-24}"

find_ndk_root() {
  local base

  if [[ -n "${ANDROID_NDK_HOME:-}" && -d "${ANDROID_NDK_HOME}" ]]; then
    printf '%s\n' "${ANDROID_NDK_HOME}"
    return 0
  fi
  if [[ -n "${ANDROID_NDK_ROOT:-}" && -d "${ANDROID_NDK_ROOT}" ]]; then
    printf '%s\n' "${ANDROID_NDK_ROOT}"
    return 0
  fi
  for base in \
    "${ANDROID_HOME:-}/ndk" \
    "${ANDROID_SDK_ROOT:-}/ndk" \
    "${HOME}/Android/Sdk/ndk" \
    /opt/android-sdk/ndk \
    /home/ubuntu/Android/Sdk/ndk \
    /home/voiduser/Android/Sdk/ndk; do
    [[ -d "${base}" ]] || continue
    find "${base}" -mindepth 1 -maxdepth 1 -type d | sort -V | tail -n 1
    return 0
  done
  return 1
}

NDK_ROOT="${NDK_ROOT:-$(find_ndk_root || true)}"
if [[ -z "${NDK_ROOT}" || ! -d "${NDK_ROOT}" ]]; then
  printf 'Unable to find Android NDK. Set NDK_ROOT=/path/to/ndk.\n' >&2
  exit 1
fi

case "$(uname -m)" in
  x86_64) CANDIDATE_TAGS=(linux-x86_64) ;;
  aarch64|arm64) CANDIDATE_TAGS=(linux-aarch64 linux-arm64 linux-x86_64) ;;
  *) CANDIDATE_TAGS=(linux-x86_64) ;;
esac

if [[ -z "${NDK_HOST_TAG:-}" ]]; then
  for tag in "${CANDIDATE_TAGS[@]}"; do
    if [[ -d "${NDK_ROOT}/toolchains/llvm/prebuilt/${tag}/bin" ]]; then
      NDK_HOST_TAG="${tag}"
      break
    fi
  done
fi
if [[ -z "${NDK_HOST_TAG:-}" ]]; then
  printf 'Unable to find a compatible NDK host toolchain.\n' >&2
  exit 1
fi

CC="${NDK_ROOT}/toolchains/llvm/prebuilt/${NDK_HOST_TAG}/bin/aarch64-linux-android${API_LEVEL}-clang"
if [[ ! -x "${CC}" ]]; then
  printf 'Missing Android compiler: %s\n' "${CC}" >&2
  exit 1
fi

mkdir -p "${OUT_DIR}"
"${CC}" -std=c11 -O2 -Wall -Wextra -Werror \
  "${ROOT_DIR}/qca_bt_dump.c" -o "${OUT_DIR}/qca-bt-dump"
file "${OUT_DIR}/qca-bt-dump"
