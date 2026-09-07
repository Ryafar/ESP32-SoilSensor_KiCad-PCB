/* Entry point of the application build - the firmware that has to survive.
 *
 * The counterpart to bringup_run(): it owns the shared resources, the drivers
 * and the tasks of the real device. Nothing is written yet, so for now it only
 * says so in the log instead of pretending to be a device.
 *
 * Called once from app_main and returns; the tasks it created keep running.
 */
#pragma once

void app_run(void);
