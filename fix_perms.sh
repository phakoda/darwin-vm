#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/dmgutil.sh"

fixup_perms() {
    local ramdisk="${1}"
    livemount="$(mktemp -d)"

    if [[ -z "${livemount}" || ! -d "${livemount}" ]]; then
        echo "something's wrong with the livemount, stopping here"
        exit 1
    fi

    if ! dmg_attach "${ramdisk}" "${livemount}" on; then
        echo "mount failed"
        rmdir "${livemount}"
        exit 1
    fi

    echo "mounted ${ramdisk} on ${livemount}"
    trap 'dmg_detach "${livemount}"; rmdir "${livemount}"' EXIT

    # macOS root:wheel and Linux 0:0 are both numeric uid/gid 0/0.
    local owner_group="root:wheel"
    [[ "$(uname -s)" == "Linux" ]] && owner_group="0:0"

    echo "This will run: sudo chown -R ${owner_group} ${livemount}/bin ${livemount}/System ${livemount}/libexec"
    read -r -p "Are you sure? (y/n) " response
    echo "${response}"

    case "${response}" in
        [Yy])
            sudo chown -R "${owner_group}" "${livemount}/bin" "${livemount}/System"

            if [[ -d "${livemount}/libexec" ]]; then
                sudo chown -R "${owner_group}" "${livemount}/libexec"
            fi
            echo "done!"
            ;;
        *)
            echo "skipping permission fixes"
            ;;
    esac
}

main() {
    if [[ -z "${1:-}" ]]; then
        echo "usage: fix_perms.sh [ramdisk.dmg]"
        exit 1
    fi

    case "$(uname -s)" in
        Darwin|Linux) ;;
        *) echo "unsupported host OS: $(uname -s)"; exit 1 ;;
    esac

    fixup_perms "${1}"
}

main "$@"
