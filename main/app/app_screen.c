#include "app_screen.h"

#include "config.h"
#include "eink_canvas.h"

/* Screen layout, in pixels on the 200x200 glass. Collected here rather than
 * spread through the drawing code: this is the part that gets nudged when the
 * result is looked at on real hardware, and nudging it should not mean reading
 * the renderer. */
#define MARGIN 8

#define HEADER_Y 10
#define DROP_RADIUS 11
#define DROP_CX (MARGIN + DROP_RADIUS + 4)
/* The taper is two radii tall and ends at the disc centre, so the centre sits
 * two radii below the top of the icon and the icon is three radii tall. */
#define DROP_CY (HEADER_Y + 2 * DROP_RADIUS)

#define BATTERY_W 46
#define BATTERY_H 24
#define BATTERY_X (EINK_CANVAS_WIDTH - MARGIN - BATTERY_W - 5) // room for the nub
#define BATTERY_Y HEADER_Y
#define BATTERY_WALL 3
#define BATTERY_NUB_W 5
#define BATTERY_NUB_H 10

/* Sized so the widest case - "100%" - still clears the margins, since that is
 * the one that decides the digit width and every narrower reading is centred
 * inside it anyway. */
#define VALUE_Y 62
#define VALUE_DIGIT_W 40
#define VALUE_DIGIT_H 78
#define VALUE_STROKE 10
#define VALUE_GAP 9
#define PERCENT_W 28
#define PERCENT_H 38
#define PERCENT_STROKE 6
#define PERCENT_GAP 8

#define BAR_X MARGIN
#define BAR_Y 162
#define BAR_W (EINK_CANVAS_WIDTH - 2 * MARGIN)
#define BAR_H 26
#define BAR_WALL 3

/* Drawn around everything when the cell is nearly empty. Deliberately loud:
 * this is the one state where the reading on the screen matters less than the
 * fact that the device is about to shut itself off. */
#define ALERT_FRAME_THICKNESS 5

/* Coarse state-of-charge for the battery icon, linear between these two. It is
 * an indicator, not a fuel gauge - a real one needs the discharge curve and a
 * load model, and the icon is five pixels wide per 20 %. The low end is the
 * firmware's ship-mode threshold, so a visibly empty icon and a shutdown mean
 * the same thing. */
#define BATTERY_ICON_EMPTY_MV BATTERY_SHIP_MODE_MV
#define BATTERY_ICON_FULL_MV 4200

static int battery_percent(int millivolts)
{
    if (millivolts <= BATTERY_ICON_EMPTY_MV) {
        return 0;
    }
    if (millivolts >= BATTERY_ICON_FULL_MV) {
        return 100;
    }
    return (millivolts - BATTERY_ICON_EMPTY_MV) * 100 /
           (BATTERY_ICON_FULL_MV - BATTERY_ICON_EMPTY_MV);
}

/* A reading that failed is not dry soil, so it draws in the neutral colour - a
 * broken probe must never show a red alert nobody can act on. */
static eink_color_t moisture_color(int soil_percent)
{
    if (soil_percent < 0) {
        return EINK_BLACK;
    }
    if (soil_percent <= SOIL_ALERT_PERCENT) {
        return EINK_RED;
    }
    if (soil_percent <= SOIL_WARN_PERCENT) {
        return EINK_YELLOW;
    }
    return EINK_BLACK;
    /* On the black/white panel the canvas maps all three to black by itself, so
     * there is no #if here: this file says what it means, and the component
     * decides what the fitted glass can show. */
}

/* A water drop: a disc with a tapered tip, drawn as one span per row so it
 * needs no polygon fill.
 *
 * The taper runs all the way down to the centre of the disc, not to the top of
 * it. Stopping at the top leaves the widest part of the cone sitting on the
 * narrowest part of the sphere, and the icon reads as a triangle balanced on a
 * ball; carrying it to the centre makes the two shapes share a tangent and the
 * outline comes out as one teardrop. */
static void draw_drop(int cx, int cy, int radius, eink_color_t color)
{
    eink_canvas_disc(cx, cy, radius, color);

    const int tip_height = radius * 2;
    for (int row = 0; row < tip_height; row++) {
        const int half = (radius * row) / tip_height;
        eink_canvas_fill_rect(cx - half, cy - tip_height + row, 2 * half + 1, 1, color);
    }
}

static void draw_battery(int x, int y, int percent, bool charging, eink_color_t color)
{
    eink_canvas_rect(x, y, BATTERY_W, BATTERY_H, BATTERY_WALL, color);
    eink_canvas_fill_rect(x + BATTERY_W, y + (BATTERY_H - BATTERY_NUB_H) / 2, BATTERY_NUB_W,
                          BATTERY_NUB_H, color);

    /* The fill sits inside the wall with one pixel of clearance, so a full
     * battery still reads as a filled outline rather than a solid block. */
    const int inset = BATTERY_WALL + 1;
    const int track_w = BATTERY_W - 2 * inset;
    eink_canvas_fill_rect(x + inset, y + inset, track_w * percent / 100, BATTERY_H - 2 * inset,
                          color);

    if (charging) {
        /* A bolt across the icon. Drawn in the accent colour so it stays
         * visible over the fill on the four-colour panel; on the black/white
         * one the fill is what it is drawn over, which is why it gets a white
         * outline stroke first. */
        const int cx = x + BATTERY_W / 2;
        const int top = y + 2;
        const int bottom = y + BATTERY_H - 2;
        const int middle = y + BATTERY_H / 2;
        eink_canvas_line(cx + 6, top, cx - 3, middle, 7, EINK_WHITE);
        eink_canvas_line(cx - 3, middle, cx + 4, middle, 7, EINK_WHITE);
        eink_canvas_line(cx + 4, middle, cx - 5, bottom, 7, EINK_WHITE);
        eink_canvas_line(cx + 6, top, cx - 3, middle, 3, EINK_ACCENT);
        eink_canvas_line(cx - 3, middle, cx + 4, middle, 3, EINK_ACCENT);
        eink_canvas_line(cx + 4, middle, cx - 5, bottom, 3, EINK_ACCENT);
    }
}

static void draw_bar(int percent, eink_color_t color)
{
    eink_canvas_rect(BAR_X, BAR_Y, BAR_W, BAR_H, BAR_WALL, color);
    if (percent <= 0) {
        return;
    }

    const int inset = BAR_WALL + 2;
    const int track_w = BAR_W - 2 * inset;
    eink_canvas_fill_rect(BAR_X + inset, BAR_Y + inset, track_w * percent / 100,
                          BAR_H - 2 * inset, color);
}

void app_screen_draw(const app_sample_t *sample, bool battery_low)
{
    if (sample == NULL) {
        return;
    }

    eink_canvas_clear(EINK_WHITE);

    /* The drop stays black: it is the label for the reading, not part of it.
     * Colouring both the icon and the number would leave nothing to compare
     * the colour against. */
    draw_drop(DROP_CX, DROP_CY, DROP_RADIUS, EINK_BLACK);
    draw_battery(BATTERY_X, BATTERY_Y, battery_percent(sample->battery_mv), sample->charging,
                 battery_low ? EINK_ACCENT : EINK_BLACK);

    /* The number and the percent sign are centred as one block, so 8 % and
     * 100 % both sit in the middle instead of the number wandering. */
    const int number_w = eink_canvas_number_width(sample->soil_percent, VALUE_DIGIT_W, VALUE_GAP);
    const int block_w = number_w + PERCENT_GAP + PERCENT_W;
    const int block_x = (EINK_CANVAS_WIDTH - block_w) / 2;

    /* Number and bar share the colour, so the reading says "water me" from
     * across the room without anyone reading the digits. */
    const eink_color_t value_color = moisture_color(sample->soil_percent);

    eink_canvas_number(block_x, VALUE_Y, VALUE_DIGIT_W, VALUE_DIGIT_H, VALUE_STROKE, VALUE_GAP,
                       sample->soil_percent, value_color);
    eink_canvas_percent(block_x + number_w + PERCENT_GAP, VALUE_Y, PERCENT_W, PERCENT_H,
                        PERCENT_STROKE, value_color);

    /* An empty outline for "no reading" rather than a bar at zero, which would
     * claim bone-dry soil the sensor never measured. */
    draw_bar(sample->soil_percent < 0 ? 0 : sample->soil_percent, value_color);

    if (battery_low) {
        eink_canvas_rect(0, 0, EINK_CANVAS_WIDTH, EINK_CANVAS_HEIGHT, ALERT_FRAME_THICKNESS,
                         EINK_ACCENT);
    }
}
