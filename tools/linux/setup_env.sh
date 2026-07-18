#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/../.." && pwd)"

sdk_path="${PICO_SDK_PATH:-${repo_root}/.pico-sdk}"
sdk_ref=""
update_sdk=0
skip_system_packages=0
apt_exe="apt-get"
udev_rule_file="/etc/udev/rules.d/70-picotrace-debugprobe.rules"

usage() {
	cat <<'EOF'
Usage:
  ./tools/linux/setup_env.sh [options]

Options:
  --sdk-path DIR            Pico SDK directory (default: $PICO_SDK_PATH or <repo>/.pico-sdk)
  --sdk-ref REF             Git branch/tag/commit to checkout after clone/update
  --update-sdk              Fetch latest refs when SDK already exists
  --skip-system-packages    Skip apt dependency install
  --apt-exe EXE             apt executable to use (default: apt-get)
  --help, -h                Show this help
EOF
}

positionals=()
while [[ $# -gt 0 ]]; do
	case "$1" in
		--sdk-path)
			sdk_path="$2"
			shift 2
			;;
		--sdk-ref)
			sdk_ref="$2"
			shift 2
			;;
		--update-sdk)
			update_sdk=1
			shift
			;;
		--skip-system-packages)
			skip_system_packages=1
			shift
			;;
		--apt-exe)
			apt_exe="$2"
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

if [[ ${#positionals[@]} -gt 0 ]]; then
	printf 'Unexpected positional arguments: %s\n' "${positionals[*]}" >&2
	usage >&2
	exit 1
fi

need_cmd() {
	local cmd="$1"
	if ! command -v "$cmd" >/dev/null 2>&1; then
		printf 'Missing required command: %s\n' "$cmd" >&2
		return 1
	fi
}

if [[ "$skip_system_packages" -eq 0 ]]; then
	if ! command -v "$apt_exe" >/dev/null 2>&1; then
		printf '%s not found. Re-run with --skip-system-packages or install dependencies manually.\n' "$apt_exe" >&2
		exit 1
	fi

	if ! command -v sudo >/dev/null 2>&1; then
		printf 'sudo is required for system package installation. Re-run with --skip-system-packages if packages are already installed.\n' >&2
		exit 1
	fi

	sudo "$apt_exe" update
	sudo "$apt_exe" install -y \
		build-essential \
		cmake \
		gcc-arm-none-eabi \
		git \
		libnewlib-arm-none-eabi \
		ninja-build \
		openocd \
		python3 \
		python3-venv
fi

if command -v sudo >/dev/null 2>&1; then
	sudo tee "$udev_rule_file" >/dev/null <<'EOF'
# Allow local access to Raspberry Pi Debug Probe devices for OpenOCD.
SUBSYSTEM=="usb", ATTR{idVendor}=="2e8a", ATTR{idProduct}=="000c", MODE:="0666"
EOF
	sudo udevadm control --reload-rules
	sudo udevadm trigger --subsystem-match=usb
	printf 'Installed udev rule at %s\n' "$udev_rule_file"
else
	printf 'sudo not found; skipping udev rule installation.\n' >&2
fi

need_cmd git
need_cmd cmake
need_cmd python3

mkdir -p "$(dirname "${sdk_path}")"

if [[ ! -d "${sdk_path}/.git" ]]; then
	git clone https://github.com/raspberrypi/pico-sdk.git "${sdk_path}"
elif [[ "$update_sdk" -eq 1 ]]; then
	git -C "${sdk_path}" fetch --all --tags --prune
fi

if [[ -n "$sdk_ref" ]]; then
	git -C "${sdk_path}" checkout "$sdk_ref"
fi

git -C "${sdk_path}" submodule update --init

linux_env_file="${script_dir}/.env.sh"
if [[ ! -f "$linux_env_file" ]]; then
	cat >"$linux_env_file" <<EOF
#!/usr/bin/env bash
export PICO_SDK_PATH="${sdk_path}"
EOF
	chmod +x "$linux_env_file"
	printf 'Created %s\n' "$linux_env_file"
else
	printf 'Keeping existing %s (not overwritten).\n' "$linux_env_file"
fi

printf '\nSetup complete.\n'
printf 'PICO_SDK_PATH=%s\n' "$sdk_path"
printf 'Load in current shell: source %s\n' "$linux_env_file"
printf 'Then build with: ./tools/linux/build.sh\n'
