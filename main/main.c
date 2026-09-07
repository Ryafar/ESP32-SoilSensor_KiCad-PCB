/* Soil sensor firmware - the one decision this file makes is which half runs.
 *
 * Two entry points, both under main/, neither one including the other:
 *   bringup/  hardware checks, read in a serial log, deleted once answered
 *   app/      the real firmware, has to survive
 *
 * Each half owns its own buses, drivers, stack sizes and priorities, so a
 * bring-up task can be rewired without touching the application and the
 * application can be flashed without dragging the checks along. The switch is
 * here rather than in either half because a file cannot sensibly decide
 * whether it itself should run.
 */
#include "app_run.h"
#include "bringup.h"

/* 1 = flash the hardware checks, 0 = flash the application. */
#define RUN_BRINGUP 1

void app_main(void)
{
#if RUN_BRINGUP
    bringup_run();
#else
    app_run();
#endif

    /* app_main returns; FreeRTOS deletes the main task and the tasks created by
     * the half that ran keep running. */
}
