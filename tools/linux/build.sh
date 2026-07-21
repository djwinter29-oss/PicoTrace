#!/usr/bin/env bash
set -euo pipefail

board="pico"
firmware_build_dir=""

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck disable=SC1091
source "${script_dir}/common.sh"

usage() {
	cat <<'EOF'
Usage:
  ./tools/linux/build.sh [firmware_build_dir] [board]
	./tools/linux/build.sh [--board BOARD] [--firmware-build-dir DIR]
	./tools/linux/build.sh [-Board BOARD] [-board BOARD]
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

if [[ ${#positionals[@]} -ge 2 && "${board}" == "pico" ]]; then
	board="${positionals[1]}"
fi

if [[ -z "${firmware_build_dir}" ]]; then
	firmware_build_dir="$(picotrace_default_firmware_build_dir "${board}")"
fi

repo_root="$(cd "${script_dir}/../.." && pwd)"
linux_env_file="${script_dir}/.env.sh"

if [[ -f "${linux_env_file}" ]]; then
	# shellcheck disable=SC1090
	source "${linux_env_file}"
fi

cd "${repo_root}"

cmake -S firmware -B "${firmware_build_dir}" -DPICO_BOARD="${board}"
cmake --build "${firmware_build_dir}"