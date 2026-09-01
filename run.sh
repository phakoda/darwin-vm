#!/bin/bash
set -euo pipefail

FIRMWARE_DIR="${FIRMWARE_DIR:-firmware}"
QEMU="${QEMU:-qemu-sptm/build/qemu-system-aarch64}"
GRAPHICS="${GRAPHICS:-0}"

if [[ -n "${BOOT_ARGS+x}" ]]; then
    BOOT_ARGS_EXPLICIT=1
else
    BOOT_ARGS_EXPLICIT=0
    BOOT_ARGS="rd=md0 serial=3 -v -noprogress wdt=-1 wlan-olyhal-abort"
fi

fix_tty() {
    stty sane 2>/dev/null || true
}

boot_qemu() {
    local machine="darwin"

    case "${GRAPHICS}" in
        0|off|false|no)
            ;;
        1|on|true|yes)
            machine+=",boot-fb=on"
            # serial=3 switches XNU's active console to the UART and
            # -noprogress suppresses framebuffer boot graphics. When the caller
            # did not supply BOOT_ARGS explicitly, prefer XNU's video console
            # so GRAPHICS=1 produces visible pixels.
            if ((BOOT_ARGS_EXPLICIT == 0)); then
                BOOT_ARGS="rd=md0 -v wdt=-1 wlan-olyhal-abort"
            fi
            ;;
        *)
            echo "error: GRAPHICS must be 0/1, off/on, false/true, or no/yes" >&2
            exit 1
            ;;
    esac

    args=(
        -M        "${machine}"
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
            args+=(-nographic)
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
