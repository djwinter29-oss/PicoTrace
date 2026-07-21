#!/usr/bin/env bash
set -euo pipefail

board="pico"
firmware_build_dir=""
openocd_exe="${OPENOCD_EXE:-openocd}"
adapter_speed_khz="${PICO_DEBUG_PROBE_SPEED_KHZ:-5000}"
openocd_target="${PICO_OPENOCD_TARGET:-${OPENOCD_TARGET:-}}"
skip_build=0

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck disable=SC1091
source "${script_dir}/common.sh"

usage() {
    cat <<'EOF'
Usage:
  ./tools/linux/load.sh [firmware_build_dir] [unused] [board]
    ./tools/linux/load.sh [--board BOARD] [--firmware-build-dir DIR] [--openocd-target FILE]
                        [--openocd-exe EXE] [--adapter-speed-khz KHZ] [--skip-build]
        ./tools/linux/load.sh [-Board BOARD] [-board BOARD]
EOF
}

positionals=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --board|--Board|-board|-Board)
            board="$2"
            shift 2
            ;;
        --firmware-build-dir)
            firmware_build_dir="$2"
            shift 2
            ;;
        --openocd-target)
            openocd_target="$2"
            shift 2
            ;;
        --openocd-exe)
            openocd_exe="$2"
            shift 2
            ;;
        --adapter-speed-khz)
            adapter_speed_khz="$2"
            shift 2
            ;;
        --skip-build)
            skip_build=1
            shift
            ;;
        --help|-h)
            usage
            exit 0
            ;;
        --*)
            printf 'Unknown option: %s\n' "$1" >&2
            usage >&2
            exit 1
            ;;
        *)
            positionals+=("$1")
            shift
            ;;
    esac
done

if [[ ${#positionals[@]} -ge 1 && -z "${firmware_build_dir}" ]]; then
    firmware_build_dir="${positionals[0]}"
fi

if [[ ${#positionals[@]} -ge 3 && "${board}" == "pico" ]]; then
    board="${positionals[2]}"
fi

if [[ -z "${firmware_build_dir}" ]]; then
    firmware_build_dir="$(picotrace_default_firmware_build_dir "${board}")"
fi

if [[ -z "${openocd_target}" ]]; then
    openocd_target="$(picotrace_default_openocd_target "${board}")"
fi

repo_root="$(cd "${script_dir}/../.." && pwd)"
linux_env_file="${script_dir}/.env.sh"
elf_path="${repo_root}/${firmware_build_dir}/picotrace.elf"

if [[ -f "${linux_env_file}" ]]; then
    # shellcheck disable=SC1090
    source "${linux_env_file}"
fi

cd "${repo_root}"

print_openocd_log() {
    local log_path="$1"

    if grep -q '\*\* Verified OK \*\*' "${log_path}" \
        && grep -q 'Error: Failed to select multidrop rp2040\.dap1' "${log_path}"; then
        grep -v 'Error: Failed to select multidrop rp2040\.dap1' "${log_path}"
        printf 'Note: ignoring OpenOCD post-reset rp2040.dap1 multidrop selection noise after successful verify.\n'
        return 0
    fi

    cat "${log_path}"
}

run_openocd() {
    local openocd_log
    openocd_log="$(mktemp)"
    local openocd_command=(
        "${openocd_exe}"
        -f interface/cmsis-dap.cfg
        -f "${openocd_target}"
        -c "adapter speed ${adapter_speed_khz}"
        -c "program ${elf_path} verify reset exit"
    )

    if "${openocd_command[@]}" >"${openocd_log}" 2>&1; then
        print_openocd_log "${openocd_log}"
        rm -f "${openocd_log}"
        return 0
    fi

    print_openocd_log "${openocd_log}"

    if grep -q 'Access denied (insufficient permissions)' "${openocd_log}" && command -v sudo >/dev/null 2>&1 && sudo -n true >/dev/null 2>&1; then
        printf 'Retrying OpenOCD with sudo because the Debug Probe USB device is not writable by the current user.\n' >&2
        if sudo -n "${openocd_command[@]}"; then
            rm -f "${openocd_log}"
            return 0
        fi
    fi

    if grep -q 'ADIv6 requires DPv3' "${openocd_log}"; then
        printf 'OpenOCD can open the Debug Probe, but the Pico 2 target is not answering as a DPv3 RP2350.\n' >&2
        printf 'Check that the target board is powered, the SWD wires and ground are correct, and the probe firmware is current.\n' >&2
    fi

    rm -f "${openocd_log}"
    return 1
}

best_effort_picotrace_reboot() {
    python3 - <<'PY' || true
import glob
import errno
import os
import select
import termios
import time

PORT_GLOB = '/dev/serial/by-id/usb-PicoTrace_*if00'

def find_port(timeout_s):
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        matches = glob.glob(PORT_GLOB)
        if matches:
            return matches[0]
        time.sleep(0.1)
    return None

def open_port(path, timeout_s):
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        try:
            fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
            attrs = termios.tcgetattr(fd)
            attrs[0] = 0
            attrs[1] = 0
            attrs[3] = 0
            attrs[2] = attrs[2] | termios.CREAD | termios.CLOCAL
            attrs[6][termios.VMIN] = 0
            attrs[6][termios.VTIME] = 1
            termios.tcsetattr(fd, termios.TCSANOW, attrs)
            termios.tcflush(fd, termios.TCIOFLUSH)
            return fd
        except OSError:
            time.sleep(0.1)
    return None

def wait_for_disconnect(path, timeout_s):
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        if not os.path.exists(path):
            return True
        time.sleep(0.1)
    return False

port = find_port(8.0)
if port is None:
    raise SystemExit(0)

reboot_requested = False
attempt_deadline = time.time() + 8.0
while time.time() < attempt_deadline:
    fd = open_port(port, 1.5)
    if fd is None:
        time.sleep(0.1)
        continue

    try:
        time.sleep(0.3)
        try:
            os.write(fd, b'reboot\r\n')
            reboot_requested = True
        except OSError as exc:
            if exc.errno not in (errno.EIO, errno.ENODEV, errno.ENOENT, errno.ENXIO):
                raise

        end = time.time() + 0.5
        while time.time() < end:
            ready, _, _ = select.select([fd], [], [], 0.1)
            if ready:
                try:
                    os.read(fd, 4096)
                except OSError as exc:
                    if exc.errno in (errno.EIO, errno.ENODEV, errno.ENOENT, errno.ENXIO):
                        break
                    raise
    finally:
        os.close(fd)

    if reboot_requested and wait_for_disconnect(port, 2.0):
        find_port(8.0)
        break

    time.sleep(0.2)
PY
}

if [[ "${skip_build}" -eq 0 ]]; then
    cmake -S firmware -B "${firmware_build_dir}" -DPICO_BOARD="${board}"
    cmake --build "${firmware_build_dir}"
fi

if ! command -v "${openocd_exe}" >/dev/null 2>&1; then
    printf 'OpenOCD executable not found: %s\n' "${openocd_exe}" >&2
    exit 1
fi

if [[ ! -f "${elf_path}" ]]; then
    printf 'ELF not found at %s\n' "${elf_path}" >&2
    exit 1
fi

run_openocd
best_effort_picotrace_reboot

printf 'Programmed %s over Debug Probe\n' "${elf_path}"