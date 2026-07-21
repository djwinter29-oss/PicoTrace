#!/usr/bin/env bash
set -euo pipefail

board="pico"
firmware_build_dir=""
test_build_dir="build/tests"
skip_firmware_build=0

usage() {
	cat <<'EOF'
Usage:
  ./tools/linux/test.sh [firmware_build_dir] [test_build_dir] [board]
	./tools/linux/test.sh [--board BOARD] [--firmware-build-dir DIR] [--test-build-dir DIR] [--skip-firmware-build]
	./tools/linux/test.sh [-Board BOARD] [-board BOARD]
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
		--test-build-dir)
			test_build_dir="$2"
			shift 2
			;;
		--skip-firmware-build)
			skip_firmware_build=1
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

if [[ ${#positionals[@]} -ge 2 && "${test_build_dir}" == "build/tests" ]]; then
	test_build_dir="${positionals[1]}"
fi

if [[ ${#positionals[@]} -ge 3 && "${board}" == "pico" ]]; then
	board="${positionals[2]}"
fi

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck disable=SC1091
source "${script_dir}/common.sh"

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

if [[ "${skip_firmware_build}" -eq 0 ]]; then
	cmake -S firmware -B "${firmware_build_dir}" -DPICO_BOARD="${board}"
	cmake --build "${firmware_build_dir}"
fi

cmake -S firmware/tests -B "${test_build_dir}"
cmake --build "${test_build_dir}"

ctest --test-dir "${test_build_dir}" --output-on-failure