#!/bin/bash
# Shared helpers for mounting/unmounting firmware/ramdisk.dmg (an APFS
# container image), used by get_files.sh and fix_perms.sh.

host_os() {
    uname -s
}

require_linux_apfs() {
    if [[ "$(host_os)" != "Linux" ]]; then
        return 0
    fi

    if [[ ! -r /proc/filesystems ]] || ! grep -qw apfs /proc/filesystems; then
        echo "error: Linux APFS read/write support is not available." 1>&2
        echo "  darwin-vm needs the linux-apfs-rw kernel module to patch ramdisk.dmg." 1>&2
        echo "  See LINUX.md for setup instructions." 1>&2
        return 1
    fi
}

dmg_attach() {
    local dmg="$1" mountpoint="$2" owners="$3"

    case "$(host_os)" in
        Darwin)
            hdiutil attach -owners "${owners}" -mountpoint "${mountpoint}" "${dmg}"
            ;;
        Linux)
            require_linux_apfs || return 1
            local opts="loop,rw"
            if [[ "${owners}" == "off" ]]; then
                opts+=",uid=$(id -u),gid=$(id -g)"
            fi
            sudo mount -t apfs -o "${opts}" "${dmg}" "${mountpoint}"
            ;;
        *)
            echo "error: unsupported host OS: $(host_os)" 1>&2
            return 1
            ;;
    esac
}

dmg_detach() {
    local mountpoint="$1"

    case "$(host_os)" in
        Darwin)
            hdiutil detach "${mountpoint}"
            ;;
        Linux)
            sudo umount "${mountpoint}"
            ;;
        *)
            echo "error: unsupported host OS: $(host_os)" 1>&2
            return 1
            ;;
    esac
}

# Recursively copy the contents of $1 into $2. On Linux, --remove-destination
# avoids O_TRUNC, which linux-apfs-rw does not currently implement reliably.
copy_tree() {
    local src="$1" dst="$2"

    case "$(host_os)" in
        Darwin)
            ditto "${src}" "${dst}"
            ;;
        Linux)
            mkdir -p "${dst}"
            cp -a --remove-destination "${src}/." "${dst}/"
            ;;
        *)
            echo "error: unsupported host OS: $(host_os)" 1>&2
            return 1
            ;;
    esac
}
