#!/bin/bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
QEMU_DIR="${ROOT_DIR}/qemu-sptm"
BUILD_DIR="${QEMU_DIR}/build"

git -C "${ROOT_DIR}" submodule update --init --recursive
"${ROOT_DIR}/scripts/apply_qemu_patches.sh"

mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"

if [[ ! -f build.ninja ]]; then
    ../configure --target-list=aarch64-softmmu "$@"
elif (($# > 0)); then
    echo "error: QEMU is already configured; remove qemu-sptm/build before passing configure options" >&2
    exit 1
fi

if command -v nproc >/dev/null 2>&1; then
    jobs="${JOBS:-$(nproc)}"
else
    jobs="${JOBS:-4}"
fi

ninja -j"${jobs}" qemu-system-aarch64
