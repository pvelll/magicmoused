/*
 * Magic Mouse 2 HID report decoder (report ID 0x12).
 *
 * Layout, as used by the Linux hid-magicmouse driver:
 *
 *   byte 0       report id (0x12)
 *   byte 1       buttons: bit 0 left, bit 1 right (the mouse's own guess)
 *   bytes 2-3    relative X, signed 16-bit little endian
 *   bytes 4-5    relative Y, signed 16-bit little endian
 *   bytes 6-13   timestamp and unknown data
 *   bytes 14..   N touch records of 8 bytes
 *
 * A report is either exactly 8 bytes (no touch data) or 14 + 8*N bytes.
 *
 * Touch record:
 *
 *   X  = signed 12 bit: byte0 | (byte1 & 0x0f) << 8
 *   Y  = signed 12 bit: byte2 << 4 | byte1 >> 4, then negated
 *        (after negation Y grows towards the back of the mouse, i.e. the palm)
 *   major = byte3, minor = byte4, size = byte5 & 0x3f
 *   id    = (byte6 << 2 | byte5 >> 6) & 0x0f
 *   orientation = (byte6 >> 2) - 32
 *   state = byte7 & 0xf0
 *
 * The coordinates are signed values around the centre of the surface.
 * They never wrap, so deltas are plain differences.
 */
#include "mm.h"

#include <string.h>

static int sext12(int v)
{
    v &= 0x0fff;
    return (v & 0x0800) ? v - 0x1000 : v;
}

static double clamp01(double v)
{
    return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
}

static void decode_touch(const uint8_t *p, const mm_config *cfg, mm_touch *t)
{
    memset(t, 0, sizeof(*t));

    t->x = sext12(p[0] | ((p[1] & 0x0f) << 8));
    t->y = -sext12((p[2] << 4) | (p[1] >> 4));
    t->major = p[3];
    t->minor = p[4];
    t->size = p[5] & 0x3f;
    t->id = ((p[6] << 2) | (p[5] >> 6)) & 0x0f;
    t->orientation = (p[6] >> 2) - 32;
    t->state = p[7] & 0xf0;
    t->active = (t->state == MM_STATE_START || t->state == MM_STATE_DRAG);

    double x = t->x;
    double y = t->y;

    double xn = (x - MM_MIN_X) / (double)(MM_MAX_X - MM_MIN_X);
    double yn = (y - MM_MIN_Y) / (double)(MM_MAX_Y - MM_MIN_Y);

    if (cfg->invert_x) {
        x = -x;
        xn = 1.0 - xn;
    }
    if (cfg->invert_y) {
        y = -y;
        yn = 1.0 - yn;
    }

    t->xmm = x / cfg->units_per_mm_x;
    t->ymm = y / cfg->units_per_mm_y;
    t->xn = clamp01(xn);
    t->yn = clamp01(yn);
}

int mm_parse_report(const uint8_t *buf, size_t len, const mm_config *cfg, mm_report *r)
{
    memset(r, 0, sizeof(*r));

    if (len < 6 || buf[0] != MM_REPORT_ID)
        return 0;

    if (len != 8 && (len < MM_HEADER_SIZE || (len - MM_HEADER_SIZE) % MM_TOUCH_SIZE != 0))
        return 0;

    r->buttons = buf[1];
    r->dx = (int16_t)(buf[2] | (buf[3] << 8));
    r->dy = (int16_t)(buf[4] | (buf[5] << 8));

    if (len < MM_HEADER_SIZE)
        return 1;

    int n = (int)((len - MM_HEADER_SIZE) / MM_TOUCH_SIZE);
    if (n > MM_MAX_TOUCHES - 1)
        return 0;

    r->has_touch = 1;
    r->ntouch = n;

    for (int i = 0; i < n; i++)
        decode_touch(buf + MM_HEADER_SIZE + i * MM_TOUCH_SIZE, cfg, &r->touch[i]);

    return 1;
}
