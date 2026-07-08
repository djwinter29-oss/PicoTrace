#include "driver/system.h"

#include "hardware/clocks.h"
#include "hardware/watchdog.h"

void system_init_clock(void) {
    hard_assert(set_sys_clock_khz(200000u, true));
}

void system_reboot(void) {
    watchdog_reboot(0u, 0u, 0u);
}