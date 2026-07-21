cmake_minimum_required(VERSION 3.13)

project(PicoTraceTests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(BRIDGE_ENABLE_COVERAGE "Enable host test coverage instrumentation" OFF)
set(PICOTRACE_FIRMWARE_VERSION "test" CACHE STRING "Firmware version string exposed over CLI and HID in host tests")

enable_testing()

add_executable(usb_app_test
    ../src/app_control.c
    ../src/device_control.c
    integration/app_control_test.c
    ../src/cli/cli_shell.c
    ../src/cli/device_cli.c
    integration/spi_monitor_test_hooks.c
    integration/trace/decode/i2c_decoder_test.c
    integration/trace/decode/i2c_trace_packet_test.c
    integration/usb_app_test.c
    ../src/driver/system.c
    ../src/trace/capture/i2c/i2c_monitor_control.c
    ../src/trace/capture/spi/spi_monitor.c
    ../src/trace/capture/spi/spi_monitor_control.c
    ../src/trace/decode/i2c/i2c_decoder.c
    ../src/trace/decode/i2c/i2c_trace_packet.c
    ../src/trace/trace_ring.c
    ../src/usb/usb_bulk.c
    ../src/usb/usb_cdc.c
    ../src/usb/usb_hid.c
)

add_executable(i2c_monitor_runtime_test
    ../src/app_control.c
    integration/i2c_monitor_runtime_test.c
    integration/i2c_monitor_test_bridge.c
    ../src/trace/decode/i2c/i2c_decoder.c
    ../src/trace/decode/i2c/i2c_trace_packet.c
    ../src/trace/trace_ring.c
)

target_include_directories(usb_app_test
    PRIVATE
        integration
        integration/stubs
        ../src
        ../src/config
)

    target_compile_definitions(usb_app_test PRIVATE PICOTRACE_FIRMWARE_VERSION="${PICOTRACE_FIRMWARE_VERSION}")

target_include_directories(i2c_monitor_runtime_test
    PRIVATE
        integration
        integration/stubs
        ../src
        ../src/config
)

    target_compile_definitions(i2c_monitor_runtime_test PRIVATE PICOTRACE_FIRMWARE_VERSION="${PICOTRACE_FIRMWARE_VERSION}")

if (BRIDGE_ENABLE_COVERAGE)
    if (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(usb_app_test PRIVATE -O0 -g --coverage)
        target_link_options(usb_app_test PRIVATE --coverage)
    else ()
        message(FATAL_ERROR "Coverage is only supported with GCC or Clang compilers.")
    endif ()
endif ()

add_test(NAME usb_app_test COMMAND usb_app_test)
add_test(NAME i2c_monitor_runtime_test COMMAND i2c_monitor_runtime_test)
