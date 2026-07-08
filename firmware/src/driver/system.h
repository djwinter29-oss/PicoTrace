/**
 * @file system.h
 * @brief Board-level clock and reboot helpers used by the PicoTrace firmware.
 */

#ifndef SYSTEM_H
#define SYSTEM_H

/** @brief Board-family string exposed to host-side status and test code. */
#if defined(PICO_RP2350A) || defined(PICO_RP2350B)
#define SYSTEM_BOARD_FAMILY "pico2"
#else
#define SYSTEM_BOARD_FAMILY "pico"
#endif

/** @brief Nominal system clock target in kHz for the current board family. */
#if defined(PICO_RP2350A) || defined(PICO_RP2350B)
#define SYSTEM_CLOCK 150000u
#else
#define SYSTEM_CLOCK 150000u
#endif

/** @brief Configure the board system clock for the PicoTrace firmware runtime. */
void system_init_clock(void);

/** @brief Reboot the board through the watchdog reset path. */
void system_reboot(void);

#endif