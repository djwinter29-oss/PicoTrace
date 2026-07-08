cmake_minimum_required(VERSION 3.13)

project(PicoTraceTests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(BRIDGE_ENABLE_COVERAGE "Enable host test coverage instrumentation" OFF)

enable_testing()

add_executable(usb_app_test
    ../src/app_control.c
    app_control_test.c
    ../src/cli/cli_shell.c
    ../src/cli/device_cli.c
    trace/decode/i2c_decoder_test.c
    trace/decode/i2c_trace_packet_test.c
    usb_app_test.c
    ../src/driver/system.c
    ../src/trace/capture/i2c_monitor_control.c
    ../src/trace/capture/spi_monitor.c
    ../src/trace/capture/spi_monitor_control.c
    ../src/trace/decode/i2c_decoder.c
    ../src/trace/decode/i2c_trace_packet.c
    ../src/trace/trace_ring.c
    ../src/usb/usb_bulk.c
    ../src/usb/usb_cdc.c
    ../src/usb/usb_hid.c
)

target_include_directories(usb_app_test
    PRIVATE
        .
    stubs
        ../src
        ../src/config
)

if (BRIDGE_ENABLE_COVERAGE)
    if (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(usb_app_test PRIVATE -O0 -g --coverage)
        target_link_options(usb_app_test PRIVATE --coverage)
    else ()
        message(FATAL_ERROR "Coverage is only supported with GCC or Clang compilers.")
    endif ()
endif ()

add_test(NAME usb_app_test COMMAND usb_app_test)
