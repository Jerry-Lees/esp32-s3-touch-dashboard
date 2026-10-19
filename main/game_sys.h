#pragma once
/*
 * The few system services the game engines need, kept separate so the
 * engines also compile on a PC for testing (see test/host/).
 * Implemented in game_common.c on the device.
 */
#include <stdint.h>

int64_t  game_time_us(void);      /* microseconds since boot */
uint32_t game_random(void);       /* hardware random number */
/* Long computations call this often; it sleeps for 1 tick every ~50 ms so
 * the idle task (and its watchdog) still gets to run. */
void     game_worker_yield(void);
