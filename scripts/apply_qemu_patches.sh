#!/bin/bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
QEMU_DIR="${ROOT_DIR}/qemu-sptm"
PATCH_DIR="${ROOT_DIR}/patches/qemu-sptm"
OVERLAY_DIR="${ROOT_DIR}/qemu-overlays"
EXPECTED_QEMU_REV="006cc6b174e6177e64d06a6457e4125fd627649f"

die() {
    echo "error: $*" >&2
    exit 1
}

[[ -d "${QEMU_DIR}/.git" || -f "${QEMU_DIR}/.git" ]] ||
    die "qemu-sptm submodule is not initialized; run: git submodule update --init qemu-sptm"

actual_rev="$(git -C "${QEMU_DIR}" rev-parse HEAD)"
if [[ "${actual_rev}" != "${EXPECTED_QEMU_REV}" ]]; then
    die "qemu-sptm is at ${actual_rev}, expected ${EXPECTED_QEMU_REV}; update the patches before changing the submodule revision"
fi

# New source files are kept as ordinary files in darwin-vm so they can be
# reviewed and edited normally. Mirror them into the external QEMU submodule
# before applying the small integration patches.
if [[ -d "${OVERLAY_DIR}" ]]; then
    while IFS= read -r -d '' overlay; do
        rel="${overlay#${OVERLAY_DIR}/}"
        dest="${QEMU_DIR}/${rel}"
        mkdir -p "$(dirname "${dest}")"
        if ! cmp -s "${overlay}" "${dest}"; then
            echo "overlay: ${rel}"
            cp "${overlay}" "${dest}"
        fi
    done < <(find "${OVERLAY_DIR}" -type f -print0 | sort -z)
fi

shopt -s nullglob
patches=("${PATCH_DIR}"/*.patch)
((${#patches[@]} > 0)) || die "no QEMU patches found in ${PATCH_DIR}"

# Patches form an ordered series, so a later patch may intentionally change a
# line introduced by an earlier one. In that final state, reverse-checking the
# earlier patch by itself is expected to fail. If the final patch reverses
# cleanly, the series reached its final state and there is nothing to do.
last_patch="${patches[${#patches[@]} - 1]}"
if git -C "${QEMU_DIR}" apply --reverse --check "${last_patch}" >/dev/null 2>&1; then
    echo "QEMU patch series: already applied"
    exit 0
fi

for patch in "${patches[@]}"; do
    name="$(basename "${patch}")"

    # This also handles an interrupted run where an earlier patch was applied
    # but the rest of the series was not.
    if git -C "${QEMU_DIR}" apply --reverse --check "${patch}" >/dev/null 2>&1; then
        echo "${name}: already applied"
        continue
    fi

    git -C "${QEMU_DIR}" apply --check "${patch}" ||
        die "${name} does not apply cleanly to ${EXPECTED_QEMU_REV}"

    echo "${name}: applying"
    git -C "${QEMU_DIR}" apply "${patch}"
done
