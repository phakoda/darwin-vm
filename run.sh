#!/bin/bash
set -euo pipefail

FIRMWARE_DIR="${FIRMWARE_DIR:-firmware}"
QEMU="${QEMU:-qemu-sptm/build/qemu-system-aarch64}"
BOOT_ARGS="${BOOT_ARGS:-rd=md0 serial=3 -v -noprogress wdt=-1 wlan-olyhal-abort}"
GRAPHICS="${GRAPHICS:-0}"
AIC_V1="${AIC_V1:-0}"

fix_tty() {
    stty sane 2>/dev/null || true
}

enabled() {
    case "$1" in
        1|on|true|yes) return 0 ;;
        0|off|false|no) return 1 ;;
        *)
            echo "error: expected 0/1, off/on, false/true, or no/yes; got '$1'" >&2
            exit 1
            ;;
    esac
}

boot_qemu() {
    machine="darwin"

    if enabled "${AIC_V1}"; then
        machine+=",aic-v1=on"
    fi

    args=(
        -bootkc   "${FIRMWARE_DIR}/bootkc"
        -dtree    "${FIRMWARE_DIR}/dtree"
        -tc       "${FIRMWARE_DIR}/ramdisk.tc"
        -ramdisk  "${FIRMWARE_DIR}/ramdisk.dmg"
        -args     "${BOOT_ARGS}"
        -serial   mon:stdio
        -m        8G
    )

    if enabled "${GRAPHICS}"; then
        machine+=",boot-fb=on"
        args=(-M "${machine}" "${args[@]}")
    else
        args=(-M "${machine}" "${args[@]}" -nographic)
    fi

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
