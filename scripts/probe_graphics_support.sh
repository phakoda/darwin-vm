#!/bin/bash
set -euo pipefail

BOOTKC="${1:-firmware/bootkc}"

if [[ ! -f "${BOOTKC}" ]]; then
    echo "error: BootKC not found: ${BOOTKC}" >&2
    exit 1
fi

if ! command -v strings >/dev/null 2>&1; then
    echo "error: 'strings' is required (binutils on Linux)" >&2
    exit 1
fi

tmp="$(mktemp)"
trap 'rm -f "${tmp}"' EXIT
LC_ALL=C strings -a "${BOOTKC}" > "${tmp}"

probe() {
    local label="$1"
    shift
    local found=1
    local marker

    for marker in "$@"; do
        if grep -F -m1 -q -- "${marker}" "${tmp}"; then
            found=0
            break
        fi
    done

    if ((found == 0)); then
        printf '%-24s %s\n' "${label}:" "yes"
        return 0
    fi

    printf '%-24s %s\n' "${label}:" "no"
    return 1
}

pvg=1
pvd=1
agx=1
iosurface=1

probe "AppleParavirtGPU" \
    "AppleParavirtGPU" \
    "com.apple.driver.AppleParavirtGPU" && pvg=0 || true
probe "AppleParavirtDisplay" \
    "AppleParavirtDisplay" \
    "com.apple.driver.AppleParavirtDisplay" && pvd=0 || true
probe "AGX family" \
    "AGXAccelerator" \
    "AGXG" \
    "com.apple.AGX" && agx=0 || true
probe "IOSurface" \
    "IOSurface" \
    "com.apple.iokit.IOSurface" && iosurface=0 || true

printf '\n'
if ((pvg == 0 && pvd == 0)); then
    echo "Reims candidate: yes"
    echo "The kernel collection contains stock Apple paravirtual GPU/display markers."
    echo "Next step: expose a compatible MMIO/IRQ device in the Darwin device tree."
elif ((pvg == 0)); then
    echo "Reims candidate: partial"
    echo "AppleParavirtGPU markers are present but AppleParavirtDisplay was not found."
    echo "The display/presentation path needs additional inspection before attaching Reims."
else
    echo "Reims candidate: no stock PVG marker found"
    if ((agx == 0)); then
        echo "AGX markers are present; this target currently points toward AGX emulation or a guest-side shim."
    else
        echo "No obvious Apple PVG or AGX marker was found by this heuristic scan."
    fi
fi

if ((iosurface != 0)); then
    echo "warning: no IOSurface marker found; GUI presentation support may be absent from this BootKC."
fi

cat <<'EOF'

Note: this is a heuristic string scan, not a definitive kext inventory. A negative result can be
caused by kernel-collection packing or stripped strings. Positive AppleParavirtGPU/Display markers
are useful evidence for choosing the Reims path; negative results should be confirmed with a KC
inspection tool before ruling it out.
EOF
