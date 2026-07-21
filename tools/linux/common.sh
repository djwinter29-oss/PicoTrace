#!/usr/bin/env bash

picotrace_default_firmware_build_dir() {
	local board="${1:-pico}"
	printf 'build/firmware-%s\n' "${board}"
}

picotrace_default_openocd_target() {
	local board="${1:-pico}"

	case "${board}" in
		pico|pico_w|rp2040*)
			printf 'target/rp2040.cfg\n'
			;;
		pico2|pico2_w|rp2350*)
			printf 'target/rp2350.cfg\n'
			;;
		*)
			printf 'No default OpenOCD target is defined for board "%s". Pass --openocd-target or set PICO_OPENOCD_TARGET.\n' "${board}" >&2
			return 1
			;;
	esac
}