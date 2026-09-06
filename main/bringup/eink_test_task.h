/* Bring-up check for the e-ink panel.
 *
 * Walks a sequence of screens so the panel can be judged by eye: whether the
 * image is the right way up, whether partial refreshes leave ghosting, and
 * whether the low-battery and charging states are distinguishable across the
 * room. Runs once and then puts the panel to sleep - an e-paper panel that is
 * refreshed in a loop wears out, and unlike the RGB test there is nothing to
 * gain from repeating it.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* FreeRTOS task entry point. Deletes itself when the sequence is done. */
void eink_test_task(void *arg);

#ifdef __cplusplus
}
#endif
