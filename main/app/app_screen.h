/* What this device's screen looks like.
 *
 * The layout lives here rather than in components/eink/ on purpose: a water
 * drop, a soil percentage and a cell-voltage-to-icon mapping are this product,
 * not e-paper. The component draws pixels and refreshes glass and is reusable
 * in the next project unchanged; everything that would have to be rewritten for
 * a different device is in this file.
 *
 * Draws into the canvas and returns. Pushing it to the panel is
 * eink_refresh()'s job, so the caller decides when the seconds get spent.
 */
#pragma once

#include <stdbool.h>

#include "app_sample.h"

void app_screen_draw(const app_sample_t *sample, bool battery_low);
