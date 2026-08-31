#!/bin/bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "${TMP_DIR}"' EXIT

FAKE_QEMU="${TMP_DIR}/qemu"
cat > "${FAKE_QEMU}" <<'EOF'
#!/bin/sh
printf '%s\n' "$@"
EOF
chmod +x "${FAKE_QEMU}"

fail() {
    echo "error: $*" >&2
    exit 1
}

assert_has_line() {
    local output="$1" expected="$2"
    grep -Fx -- "${expected}" <<<"${output}" >/dev/null ||
        fail "missing argument: ${expected}"
}

assert_lacks_text() {
    local output="$1" unexpected="$2"
    if grep -F -- "${unexpected}" <<<"${output}" >/dev/null; then
        fail "unexpected text in arguments: ${unexpected}"
    fi
}

serial_output="$(
    cd "${ROOT_DIR}"
    QEMU="${FAKE_QEMU}" GRAPHICS=0 AIC_V1=0 ./run.sh
)"
assert_has_line "${serial_output}" "darwin"
assert_has_line "${serial_output}" "-nographic"
assert_has_line "${serial_output}" "rd=md0 serial=3 -v -noprogress wdt=-1 wlan-olyhal-abort"

graphics_output="$(
    cd "${ROOT_DIR}"
    QEMU="${FAKE_QEMU}" GRAPHICS=1 AIC_V1=0 ./run.sh
)"
assert_has_line "${graphics_output}" "darwin,boot-fb=on"
assert_has_line "${graphics_output}" "rd=md0 -v wdt=-1 wlan-olyhal-abort"
assert_lacks_text "${graphics_output}" "serial=3"
assert_lacks_text "${graphics_output}" "-noprogress"
assert_lacks_text "${graphics_output}" "-nographic"

aic_output="$(
    cd "${ROOT_DIR}"
    QEMU="${FAKE_QEMU}" GRAPHICS=0 AIC_V1=1 ./run.sh
)"
assert_has_line "${aic_output}" "darwin,aic-v1=on"
assert_has_line "${aic_output}" "-nographic"

combined_output="$(
    cd "${ROOT_DIR}"
    QEMU="${FAKE_QEMU}" GRAPHICS=1 AIC_V1=1 ./run.sh
)"
assert_has_line "${combined_output}" "darwin,aic-v1=on,boot-fb=on"
assert_lacks_text "${combined_output}" "-nographic"

custom_output="$(
    cd "${ROOT_DIR}"
    QEMU="${FAKE_QEMU}" GRAPHICS=1 BOOT_ARGS="rd=md0 serial=3 -v custom-flag" ./run.sh
)"
assert_has_line "${custom_output}" "rd=md0 serial=3 -v custom-flag"

echo "run.sh argument tests passed"
