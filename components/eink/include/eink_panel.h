/* Which of the two WeAct Studio 1.54" modules is fitted on CON1.
 *
 * Set one of these to 1 and rebuild. That is the whole knob: it decides which
 * controller driver is compiled, how deep the framebuffer is, and which way
 * round BUSY is read. The other panel's code is not in the binary at all.
 *
 * It lives in this component rather than in main/config.h because components/
 * may not include that file - board values reach a component through its init
 * struct, and a build-time constant cannot travel that way. main/config.h
 * points here so it is still findable from the board side.
 */
#pragma once

/* Exactly one of these is 1. */
#define EINK_PANEL_SSD1681_BW    0 // 1.54" black/white, GDEH0154D67, 1 bpp, ~2.5 s full refresh
#define EINK_PANEL_JD79660_COLOR 1 // 1.54" four colour, GDEM0154F51H, 2 bpp, ~21 s, no partial

#if (EINK_PANEL_SSD1681_BW + EINK_PANEL_JD79660_COLOR) != 1
#error "Set exactly one of EINK_PANEL_SSD1681_BW / EINK_PANEL_JD79660_COLOR to 1"
#endif
