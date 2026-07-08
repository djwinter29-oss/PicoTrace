#ifndef SYSTEM_H
#define SYSTEM_H

#if defined(PICO_RP2350A) || defined(PICO_RP2350B)
#define SYSTEM_BOARD_FAMILY "pico2"
#else
#define SYSTEM_BOARD_FAMILY "pico"
#endif

#if defined(PICO_RP2350A) || defined(PICO_RP2350B)
#define SYSTEM_CLOCK 150000u
#else
#define SYSTEM_CLOCK 150000u
#endif

void system_init_clock(void);
void system_reboot(void);

#endif