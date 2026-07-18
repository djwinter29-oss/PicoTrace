#!/usr/bin/env bash
set -euo pipefail

firmware_build_dir="${1:-build/firmware}"
test_build_dir="${2:-build/tests}"

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/../.." && pwd)"
linux_env_file="${script_dir}/.env.sh"

if [[ -f "${linux_env_file}" ]]; then
	# shellcheck disable=SC1090
	source "${linux_env_file}"
fi

cd "${repo_root}"

cmake -S firmware -B "${firmware_build_dir}"
cmake --build "${firmware_build_dir}"

cmake -S firmware/tests -B "${test_build_dir}"
cmake --build "${test_build_dir}"

ctest --test-dir "${test_build_dir}" --output-on-failure