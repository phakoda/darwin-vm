#!/bin/bash
set -euo pipefail

FIRMWARE_DIR="${FIRMWARE_DIR:-firmware}"
QEMU="${QEMU:-qemu-sptm/build/qemu-system-aarch64}"
BOOT_ARGS="${BOOT_ARGS:-rd=md0 serial=3 -v -noprogress wdt=-1 wlan-olyhal-abort}"
GRAPHICS="${GRAPHICS:-0}"

fix_tty() {
    stty sane 2>/dev/null || true
}

boot_qemu() {
    machine="darwin"

    args=(
        -bootkc   "${FIRMWARE_DIR}/bootkc"
        -dtree    "${FIRMWARE_DIR}/dtree"
        -tc       "${FIRMWARE_DIR}/ramdisk.tc"
        -ramdisk  "${FIRMWARE_DIR}/ramdisk.dmg"
        -args     "${BOOT_ARGS}"
        -serial   mon:stdio
        -m        8G
    )

    case "${GRAPHICS}" in
        0|off|false|no)
            args=(-M "${machine}" "${args[@]}" -nographic)
            ;;
        1|on|true|yes)
            machine+=",boot-fb=on"
            args=(-M "${machine}" "${args[@]}")
            ;;
        *)
            echo "error: GRAPHICS must be 0/1, off/on, false/true, or no/yes" >&2
            exit 1
            ;;
    esac

    if [[ -f "${FIRMWARE_DIR}/sptm" ]]; then
        args+=(
            -sptm "${FIRMWARE_DIR}/sptm"
            -txm  "${FIRMWARE_DIR}/txm"
        )
    fi

    "${QEMU}" "${args[@]}"
}

main() {
    trap 'fix_tty' EXIT
    boot_qemu
}

main "$@"
