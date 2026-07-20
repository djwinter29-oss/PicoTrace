cmake_minimum_required(VERSION 3.13)

include(pico_sdk_import.cmake)

set(PICO_BOARD "pico" CACHE STRING "Pico SDK board target")
set(PICOTRACE_FIRMWARE_VERSION "dev" CACHE STRING "Firmware version string exposed over CLI and HID")

project(PicoTrace C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(picotrace
    src/app_control.c
    src/cli/cli_shell.c
    src/cli/device_cli.c
    src/main.c
    src/driver/led.c
    src/driver/system.c
    src/trace/i2c/i2c_decoder.c
    src/trace/i2c/i2c_trace_packet.c
    src/trace/i2c/i2c_monitor.c
    src/trace/i2c/i2c_monitor_control.c
    src/trace/spi/spi_monitor.c
    src/trace/spi/spi_monitor_control.c
    src/trace/trace_ring.c
    src/usb/usb_bulk.c
    src/usb/usb_cdc.c
    src/usb/usb_hid.c
    src/usb/usb_descriptors.c
)

target_include_directories(picotrace
    PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/src
    ${CMAKE_CURRENT_LIST_DIR}/src/config
)

target_link_libraries(picotrace
    PRIVATE
    hardware_clocks
    hardware_dma
    hardware_gpio
    hardware_irq
    hardware_pio
    pico_multicore
    pico_stdlib
    tinyusb_device
    tinyusb_board
)

target_compile_definitions(picotrace PRIVATE
    PICOTRACE_FIRMWARE_VERSION="${PICOTRACE_FIRMWARE_VERSION}"
    PICO_HEAP_SIZE=0x1C00
)

pico_generate_pio_header(picotrace ${CMAKE_CURRENT_LIST_DIR}/src/trace/i2c/i2c_monitor.pio)
pico_generate_pio_header(picotrace ${CMAKE_CURRENT_LIST_DIR}/src/trace/spi/spi_monitor.pio)

if (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(picotrace PRIVATE
        $<$<COMPILE_LANGUAGE:C>:-O3>
        $<$<COMPILE_LANGUAGE:CXX>:-O3>
    )
endif ()

message(STATUS "Configuring ${PROJECT_NAME} for PICO_BOARD=${PICO_BOARD} (PICO_PLATFORM=${PICO_PLATFORM})")

pico_enable_stdio_uart(picotrace 0)
pico_enable_stdio_usb(picotrace 0)

pico_add_extra_outputs(picotrace)