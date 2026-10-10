/* Bring-up task: cycles the status LED through red, green and blue and logs
 * which colour it just set.
 *
 * Doubles as the check for the open byte-order question in
 * docs/hardware/hardware-info.md - if the LED shows a different colour than the log
 * announces, the WL-ICLED wants RGB instead of the GRB currently configured in
 * components/rgb/rgb.c.
 *
 * Expects rgb_init() to have run already. Never returns; remove the task from
 * main.c once the LED is verified.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void rgb_test_task(void *arg);

#ifdef __cplusplus
}
#endif
