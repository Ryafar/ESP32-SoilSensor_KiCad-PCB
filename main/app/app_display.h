/* When the e-paper screen gets redrawn, and with what.
 *
 * The layout itself lives in the eink component - this file holds only the
 * policy, which is app business: a refresh costs 2.5 s on the black/white panel
 * and about 21 s on the four-colour one, with the rail up the whole time, so
 * "every measurement" is the wrong rule and "only when the picture would
 * actually differ" is the right one.
 *
 * The panel is powered up, drawn and powered down inside every call. E-paper
 * holds its image with no power, so there is nothing to keep alive in between;
 * anything else would leave a rail on for minutes to display a number that did
 * not change.
 */
#pragma once

#include <stdbool.h>

#include "app_sample.h"

/* Draws the sample if it would change what is on the glass, and does nothing
 * otherwise. `battery_low` comes from the application's own threshold decision,
 * not from a voltage comparison in here - the screen must agree with what the
 * firmware decided, or a device that is about to enter ship mode shows a
 * cheerful reading while it does.
 *
 * Blocks for the length of a refresh. Safe to call when nothing changed; that
 * is the common case and costs nothing.
 */
void app_display_update(const app_sample_t *sample, bool battery_low);
