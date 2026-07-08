#!/usr/bin/env bash
set -euo pipefail

board="pico"
firmware_build_dir=""
openocd_exe="${OPENOCD_EXE:-openocd}"
adapter_speed_khz="${PICO_DEBUG_PROBE_SPEED_KHZ:-5000}"
openocd_target="${PICO_OPENOCD_TARGET:-${OPENOCD_TARGET:-}}"
skip_build=0

usage() {
    cat <<'EOF'
Usage:
  ./tools/linux/load.sh [firmware_build_dir] [unused] [board]
  ./tools/linux/load.sh [--board pico|pico2] [--firmware-build-dir DIR] [--openocd-target FILE]
                        [--openocd-exe EXE] [--adapter-speed-khz KHZ] [--skip-build]
    ./tools/linux/load.sh [-Board pico|pico2] [-board pico|pico2]
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
    firmware_build_dir="build/firmware-${board}"
fi

if [[ -z "${openocd_target}" ]]; then
    if [[ "${board}" == pico2* ]]; then
        openocd_target="target/rp2350.cfg"
    else
        openocd_target="target/rp2040.cfg"
    fi
fi

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/../.." && pwd)"
elf_path="${repo_root}/${firmware_build_dir}/picotrace.elf"

cd "${repo_root}"

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

"${openocd_exe}" \
    -f interface/cmsis-dap.cfg \
    -f "${openocd_target}" \
    -c "adapter speed ${adapter_speed_khz}" \
    -c "program ${elf_path} verify reset exit"

printf 'Programmed %s over Debug Probe\n' "${elf_path}"