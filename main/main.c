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
 *
 * CMakeLists.txt reads the switch below out of this file and puts only the
 * selected half into SRC_DIRS, so the other one is not compiled and cannot
 * reach the image. The shipped firmware therefore contains no bring-up code at
 * all - which is the whole point of calling it throwaway.
 */

/* 1 = flash the hardware checks, 0 = flash the application.
 *
 * Before the includes, because it decides which header even exists as far as
 * this build is concerned. Keep it as a literal `#define X 0|1` on one line:
 * main/CMakeLists.txt greps for exactly that and fails the build if it is
 * gone. */
#define RUN_BRINGUP 0

#if RUN_BRINGUP
#include "bringup.h"
#else
#include "app_run.h"
#endif

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
