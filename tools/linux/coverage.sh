#!/usr/bin/env bash
set -euo pipefail

coverage_build_dir="${1:-build/tests-coverage}"
coverage_output_dir="${2:-build/coverage}"

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/../.." && pwd)"

cd "${repo_root}"

if ! command -v gcovr >/dev/null 2>&1; then
    echo "gcovr is required for coverage reporting. Install it with: pip install gcovr" >&2
    exit 1
fi

cmake -S firmware/tests -B "${coverage_build_dir}" -DBRIDGE_ENABLE_COVERAGE=ON
cmake --build "${coverage_build_dir}"
ctest --test-dir "${coverage_build_dir}" --output-on-failure

mkdir -p "${coverage_output_dir}"

gcovr \
    --root "${repo_root}" \
    --filter "firmware/src" \
    --filter "firmware/tests" \
    --exclude "firmware/tests/.*/CMakeFiles" \
    --txt "${coverage_output_dir}/coverage.txt" \
    --html-details "${coverage_output_dir}/coverage.html" \
    "${coverage_build_dir}"