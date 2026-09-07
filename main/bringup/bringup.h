/* Entry point of the bring-up build.
 *
 * Owns everything the hardware checks need: the shared I2C bus, the shared ADC
 * unit, and the switches that say which check actually runs. main.c only
 * decides whether this or app_run() is the firmware for this flash - so the
 * throwaway half can be edited, wired up and ripped out again without ever
 * touching the application.
 *
 * Called once from app_main and returns; the tasks it created keep running.
 */
#pragma once

void bringup_run(void);
