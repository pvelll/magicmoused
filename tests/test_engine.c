/*
 * Engine tests: feed synthetic Magic Mouse 2 reports (encoded as real HID
 * bytes, so the parser is exercised too) and check what comes out.
 *
 *   make test
 */
#include "mm.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- */
/* Capturing output                                                  */
/* ---------------------------------------------------------------- */

typedef struct {
    int     btn_code[32];
    int     btn_down[32];
    int     nbtn;

    int     key_first[16];
    int     key_n[16];
    int     nkeys;

    int     tp_n;               /* fingers currently down */
    int     tp_max_n;
    int     tp_frames;
    int     tp_downs;
    int     tp_ups;
    int64_t tp_t_down;
    int64_t tp_t_up;
    double  tp_x0, tp_y0;       /* slot 0 when the contact started */
    double  tp_x, tp_y;         /* slot 0 now */
    int     tp_n3_frames;
    double  spread_first, spread_last;
    int     pinch_frames;
    int     pinch_lifts;
    int     tp_n2_frames;
    int64_t tp_last_frame;
    int64_t tp_min_gap;          /* smallest interval between two motion frames */
    int64_t tp_last_motion;
    int64_t tp_lift_gap;         /* time from the last motion frame to the lift */
    int64_t tp_first3;           /* when three fingers first landed */
    double  tp_path;             /* total distance slot 0 travelled while down */

    long    wheel_v, wheel_h;
    int     wheel_events;

    long    ptr_x, ptr_y;
} capture;

static capture cap;
static int64_t g_now;
static int failures;
static int checks;

static void c_pointer(void *ctx, int dx, int dy) { cap.ptr_x += dx; cap.ptr_y += dy; }

static void c_button(void *ctx, int code, int down)
{
    if (cap.nbtn < (int)(sizeof(cap.btn_code) / sizeof(cap.btn_code[0]))) {
        cap.btn_code[cap.nbtn] = code;
        cap.btn_down[cap.nbtn] = down;
    }
    cap.nbtn++;
}

static void c_wheel(void *ctx, int v, int h)
{
    cap.wheel_v += v;
    cap.wheel_h += h;
    cap.wheel_events++;
}

static void c_touchpad(void *ctx, int n, const mm_point *p)
{
    cap.tp_frames++;

    if (n > 0 && n == cap.tp_n) {
        int64_t gap = g_now - cap.tp_last_frame;
        if (cap.tp_min_gap == 0 || gap < cap.tp_min_gap)
            cap.tp_min_gap = gap;
        cap.tp_last_motion = g_now;
    }
    if (n == 0 && cap.tp_n > 0)
        cap.tp_lift_gap = g_now - cap.tp_last_motion;
    if (n == 3 && cap.tp_n != 3 && !cap.tp_first3)
        cap.tp_first3 = g_now;
    cap.tp_last_frame = g_now;
    if (n == 2)
        cap.tp_n2_frames++;

    if (n > 0 && cap.tp_n == 0) {
        cap.tp_downs++;
        cap.tp_t_down = g_now;
        cap.tp_x0 = p[0].x;
        cap.tp_y0 = p[0].y;
    }
    if (n == 0 && cap.tp_n > 0) {
        cap.tp_ups++;
        cap.tp_t_up = g_now;
    }
    if (n > cap.tp_max_n)
        cap.tp_max_n = n;
    if (n == 3)
        cap.tp_n3_frames++;
    if (n > 0) {
        if (n == cap.tp_n)
            cap.tp_path += hypot(p[0].x - cap.tp_x, p[0].y - cap.tp_y);
        cap.tp_x = p[0].x;
        cap.tp_y = p[0].y;
    }
    if (n == 2) {
        double spread = p[1].x - p[0].x;
        if (fabs(spread - 16.0) > 0.01) {
            if (cap.pinch_frames == 0)
                cap.spread_first = spread;
            cap.spread_last = spread;
            cap.pinch_frames++;
        }
    }
    if (n == 0 && cap.pinch_frames > 0)
        cap.pinch_lifts++;

    cap.tp_n = n;
}

static void c_keys(void *ctx, const int *codes, int n)
{
    if (cap.nkeys < (int)(sizeof(cap.key_first) / sizeof(cap.key_first[0]))) {
        cap.key_first[cap.nkeys] = codes[0];
        cap.key_n[cap.nkeys] = n;
    }
    cap.nkeys++;
}

/* ---------------------------------------------------------------- */
/* Helpers                                                           */
/* ---------------------------------------------------------------- */

static void check(int ok, const char *fmt, ...)
{
    checks++;
    if (ok)
        return;

    failures++;
    va_list ap;
    va_start(ap, fmt);
    printf("    FAIL: ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}

typedef struct { int id; double x, y; } finger;   /* mm, x right, y towards the palm */

static size_t encode(uint8_t *buf, int dx, int dy, int buttons, int n, const finger *f, const mm_config *cfg)
{
    memset(buf, 0, 14 + 8 * (size_t)n);
    buf[0] = MM_REPORT_ID;
    buf[1] = (uint8_t)buttons;
    buf[2] = (uint8_t)(dx & 0xff);
    buf[3] = (uint8_t)((dx >> 8) & 0xff);
    buf[4] = (uint8_t)(dy & 0xff);
    buf[5] = (uint8_t)((dy >> 8) & 0xff);

    for (int i = 0; i < n; i++) {
        uint8_t *p = buf + 14 + 8 * i;
        int x = (int)lround(f[i].x * cfg->units_per_mm_x);
        int y = (int)lround(f[i].y * cfg->units_per_mm_y);
        int ry = (-y) & 0x0fff;
        int id = f[i].id;

        p[0] = (uint8_t)(x & 0xff);
        p[1] = (uint8_t)(((x >> 8) & 0x0f) | ((ry & 0x0f) << 4));
        p[2] = (uint8_t)(ry >> 4);
        p[3] = 40;
        p[4] = 30;
        p[5] = (uint8_t)(20 | ((id & 3) << 6));
        p[6] = (uint8_t)((id >> 2) | (32 << 2));
        p[7] = MM_STATE_DRAG;
    }

    return 14 + 8 * (size_t)n;
}

static mm_config cfg;
static mm_engine eng;

static void run_until(int64_t t)
{
    while (g_now < t) {
        int64_t d = mm_engine_next_deadline(&eng);
        if (d && d <= t) {
            if (d > g_now)
                g_now = d;
            mm_engine_tick(&eng, g_now);
            if (mm_engine_next_deadline(&eng) == d)   /* not consumed: avoid spinning */
                g_now++;
        } else {
            g_now = t;
        }
    }
    mm_engine_tick(&eng, g_now);
}

static void send_dt(int dx, int dy, int buttons, int n, const finger *f, int64_t advance_us);

static void send(int dx, int dy, int buttons, int n, const finger *f)
{
    send_dt(dx, dy, buttons, n, f, 11000);
}

static void send_dt(int dx, int dy, int buttons, int n, const finger *f, int64_t advance_us)
{
    uint8_t buf[256];
    size_t len = encode(buf, dx, dy, buttons, n, f, &cfg);
    mm_report r;

    if (!mm_parse_report(buf, len, &cfg, &r)) {
        check(0, "report did not parse");
        return;
    }

    mm_engine_report(&eng, &r, g_now);
    run_until(g_now + advance_us);
}

static void idle(int ms)
{
    run_until(g_now + (int64_t)ms * 1000);
}

static void setup(void)
{
    mm_output out = { NULL, c_pointer, c_button, c_wheel, c_touchpad, c_keys };

    memset(&cap, 0, sizeof(cap));
    mm_engine_init(&eng, &cfg, &out);
    g_now += 5000000;   /* fresh time base, far from any previous test */
}

#define TEST(name) static void name(void)

/* ---------------------------------------------------------------- */
/* Tests                                                             */
/* ---------------------------------------------------------------- */

TEST(test_parser)
{
    finger f = { 5, -12.3, 20.0 };
    uint8_t buf[64];
    size_t len = encode(buf, -7, 300, 2, 1, &f, &cfg);
    mm_report r;

    check(mm_parse_report(buf, len, &cfg, &r) == 1, "parse ok");
    check(r.dx == -7 && r.dy == 300, "motion %d,%d", r.dx, r.dy);
    check(r.buttons == 2, "buttons %d", r.buttons);
    check(r.has_touch && r.ntouch == 1, "one touch");
    check(r.touch[0].id == 5, "id %d", r.touch[0].id);
    check(fabs(r.touch[0].xmm + 12.3) < 0.05 && fabs(r.touch[0].ymm - 20.0) < 0.05,
          "position %.2f,%.2f", r.touch[0].xmm, r.touch[0].ymm);
    check(r.touch[0].active, "active");

    uint8_t shortrep[8] = { 0x12, 1, 5, 0, 0xfb, 0xff, 0, 0 };
    check(mm_parse_report(shortrep, 8, &cfg, &r) == 1 && !r.has_touch && r.dx == 5 && r.dy == -5,
          "8-byte report");

    uint8_t bad[13] = { 0x12 };
    check(mm_parse_report(bad, 13, &cfg, &r) == 0, "bad length rejected");
    uint8_t other[14] = { 0x90 };
    check(mm_parse_report(other, 14, &cfg, &r) == 0, "other report id ignored");

    finger l = { 1, -24.0, 0.0 }, rr = { 1, 27.0, 0.0 };   /* edge to edge */
    mm_report a, b;
    len = encode(buf, 0, 0, 0, 1, &l, &cfg);
    mm_parse_report(buf, len, &cfg, &a);
    len = encode(buf, 0, 0, 0, 1, &rr, &cfg);
    mm_parse_report(buf, len, &cfg, &b);
    check(fabs((b.touch[0].xmm - a.touch[0].xmm) - 51.0) < 0.1, "no wrap across the surface (%.1f)",
          b.touch[0].xmm - a.touch[0].xmm);
}

TEST(test_vertical_scroll)
{
    setup();
    finger f = { 3, 0.0, 0.0 };

    send(0, 0, 0, 1, &f);
    check(cap.tp_frames == 0, "nothing on the virtual pad until the finger moves");

    for (int i = 0; i < 25; i++) {
        f.y -= 1.0;
        send(0, 0, 0, 1, &f);
    }

    check(cap.tp_y < cap.tp_y0 - 15.0, "virtual fingers moved up: %.1f -> %.1f", cap.tp_y0, cap.tp_y);
    check(fabs(cap.tp_x - cap.tp_x0) < 0.01, "axis lock keeps x still (%.2f)", cap.tp_x - cap.tp_x0);

    double at_lift = cap.tp_y;
    send(0, 0, 0, 0, NULL);
    check(cap.tp_n == 2, "virtual fingers keep gliding after the finger lifts");
    idle(300);
    check(cap.tp_y < at_lift - 10.0, "inertia continued the scroll (%.1f mm)", at_lift - cap.tp_y);
    idle(4000);
    check(cap.tp_n == 0, "inertia ended and the virtual fingers lifted");
    check(cap.nbtn == 0 && cap.nkeys == 0, "no clicks or keys");
}

TEST(test_touch_stops_inertia)
{
    setup();
    finger f = { 3, 0.0, 10.0 };

    send(0, 0, 0, 1, &f);
    for (int i = 0; i < 15; i++) {
        f.y -= 2.0;
        send(0, 0, 0, 1, &f);
    }
    send(0, 0, 0, 0, NULL);
    idle(100);

    f.y = 0.0;
    send(0, 0, 0, 1, &f);
    double y = cap.tp_y;
    idle(300);
    check(fabs(cap.tp_y - y) < 0.5, "no motion while the finger rests (%.2f mm)", cap.tp_y - y);
    check(cap.tp_n == 2 && cap.tp_downs == 1, "same virtual contact continues (no lift/relanding)");

    send(0, 0, 0, 0, NULL);
    idle(400);
    check(cap.tp_n == 0, "released afterwards");
    check(cap.nkeys == 0 && cap.pinch_frames == 0, "stopping inertia is not a tap");
}

TEST(test_app_inertia_mode)
{
    cfg.momentum = 0;
    setup();
    finger f = { 3, 0.0, 10.0 };

    send(0, 0, 0, 1, &f);
    for (int i = 0; i < 15; i++) {
        f.y -= 2.0;
        send(0, 0, 0, 1, &f);
    }
    send(0, 0, 0, 0, NULL);
    check(cap.tp_n == 0, "momentum = 0: released immediately so the app coasts");

    idle(500);
    memset(&cap, 0, sizeof(cap));
    send(0, 0, 0, 1, &f);
    check(cap.tp_n == 2, "momentum = 0: resting finger puts virtual fingers down");
    send(0, 0, 0, 0, NULL);
    idle(500);
    check(cap.tp_ups == 1 && cap.tp_t_up - cap.tp_t_down >= 300000,
          "short touch held >= 300 ms on the virtual pad (no libinput tap)");
    cfg.momentum = 1;
}

TEST(test_pointer_motion_blocks_scroll)
{
    setup();
    finger f = { 3, 0.0, 0.0 };

    send(0, 0, 0, 1, &f);
    for (int i = 0; i < 12; i++) {
        f.y -= 0.4;
        f.x += 0.4;
        send(30, -15, 0, 1, &f);
    }
    send(0, 0, 0, 0, NULL);
    idle(500);

    check(cap.tp_frames == 0, "moving the mouse does not scroll (%d frames)", cap.tp_frames);
    check(cap.ptr_x == 360 && cap.ptr_y == -180, "pointer moved (%ld,%ld)", cap.ptr_x, cap.ptr_y);
}

TEST(test_sideways_roll_no_scroll)
{
    setup();
    finger f = { 3, 0.0, 0.0 };

    send(0, 0, 0, 1, &f);
    for (int i = 0; i < 5; i++) {
        f.x += 0.5;
        send(0, 0, 0, 1, &f);
    }
    send(0, 0, 0, 0, NULL);
    idle(500);

    check(cap.tp_frames == 0, "a small sideways roll does not scroll");
}

TEST(test_scroll_acceleration)
{
    setup();
    finger f = { 1, 0.0, 0.0 };
    send(0, 0, 0, 1, &f);
    for (int i = 0; i < 15; i++) {
        f.y -= 0.3;
        send(0, 0, 0, 1, &f);
    }
    double slow = cap.tp_y0 - cap.tp_y;
    send(0, 0, 0, 0, NULL);
    idle(400);

    setup();
    f.y = 0.0;
    send(0, 0, 0, 1, &f);
    for (int i = 0; i < 10; i++) {
        f.y -= 4.0;
        send(0, 0, 0, 1, &f);
    }
    idle(30);
    double fast = cap.tp_y0 - cap.tp_y;
    send(0, 0, 0, 0, NULL);

    check(slow > 2.4 && slow < 3.5, "slow stroke is about 1:1 (%.2f mm for 4.5 mm minus the 1.5 mm threshold)", slow);
    check(fast > 60.0, "fast stroke is accelerated (%.1f mm for 40 mm)", fast);
}

TEST(test_click_is_not_tap)
{
    setup();
    finger f = { 2, -10.0, -5.0 };

    for (int k = 0; k < 2; k++) {
        send(0, 0, 0, 1, &f);
        send(0, 0, 1, 1, &f);
        send(0, 0, 1, 1, &f);
        send(0, 0, 0, 1, &f);
        send(0, 0, 0, 0, NULL);
        idle(40);
    }
    idle(800);

    check(cap.nbtn == 4, "4 button events, got %d", cap.nbtn);
    check(cap.btn_code[0] == MM_BTN_LEFT && cap.btn_down[0] == 1 && cap.btn_down[1] == 0, "left click");
    check(cap.nkeys == 0 && cap.pinch_frames == 0, "double click does not trigger double-tap actions");
    check(cap.tp_n == 0, "virtual touchpad released");
}

TEST(test_secondary_click)
{
    setup();
    finger r = { 1, 15.0, -5.0 };

    send(0, 0, 0, 1, &r);
    send(0, 0, 1, 1, &r);
    send(0, 0, 0, 1, &r);
    send(0, 0, 0, 0, NULL);
    check(cap.nbtn == 2 && cap.btn_code[0] == MM_BTN_RIGHT, "finger on the right half -> right click");

    idle(500);
    setup();
    finger two[2] = { { 1, 15.0, -5.0 }, { 2, -15.0, -5.0 } };
    send(0, 0, 0, 2, two);
    send(0, 0, 1, 2, two);
    send(0, 0, 0, 2, two);
    send(0, 0, 0, 0, NULL);
    check(cap.nbtn == 2 && cap.btn_code[0] == MM_BTN_LEFT, "a finger resting on the left -> left click");

    idle(500);
    setup();
    send(0, 0, 2, 0, NULL);
    send(0, 0, 0, 0, NULL);
    check(cap.nbtn == 2 && cap.btn_code[0] == MM_BTN_RIGHT, "no touches -> mouse's own guess");

    idle(500);
    setup();
    finger edge[2] = { { 1, 15.0, -5.0 }, { 2, -44.0, 0.0 } };
    send(0, 0, 0, 2, edge);
    send(0, 0, 1, 2, edge);
    send(0, 0, 0, 2, edge);
    send(0, 0, 0, 0, NULL);
    check(cap.nbtn == 2 && cap.btn_code[0] == MM_BTN_RIGHT, "gripping thumb does not block right click");
}

TEST(test_two_finger_double_tap)
{
    setup();
    finger two[2] = { { 1, -8.0, -5.0 }, { 2, 8.0, -5.0 } };

    for (int k = 0; k < 2; k++) {
        send(0, 0, 0, 1, two);
        send(0, 0, 0, 2, two);
        send(0, 0, 0, 2, two);
        send(0, 0, 0, 0, NULL);
        idle(60);
    }
    idle(600);

    check(cap.nkeys == 1 && cap.key_first[0] == 125 && cap.key_n[0] == 1, "SUPER pressed once");
    check(cap.nbtn == 0, "no clicks");
    check(cap.tp_frames == 0, "taps never touch the virtual pad (libinput cannot see a tap)");
}

TEST(test_two_finger_double_tap_lost_finger)
{
    setup();
    finger two[2] = { { 4, 19.0, -6.0 }, { 7, -8.5, -6.0 } };

    send(0, 0, 0, 2, two);
    send(0, 0, 0, 2, two);
    send(0, 0, 0, 0, NULL);
    idle(90);
    send(0, 0, 0, 1, &two[1]);
    send(0, 0, 0, 0, NULL);
    idle(600);

    check(cap.nkeys == 1 && cap.key_first[0] == 125, "still a two-finger double tap -> SUPER");
    check(cap.pinch_frames == 0, "not a smart zoom");
}

TEST(test_single_tap_is_harmless)
{
    setup();
    finger f = { 4, 0.0, -5.0 };
    send(0, 0, 0, 1, &f);
    send(0, 0, 0, 0, NULL);
    idle(1000);

    check(cap.nkeys == 0 && cap.nbtn == 0 && cap.pinch_frames == 0, "a single tap does nothing");
    check(cap.tp_frames == 0, "a tap does not touch the virtual pad");
}

TEST(test_smart_zoom)
{
    setup();
    finger f = { 4, 0.0, -5.0 };

    for (int round = 0; round < 2; round++) {
        memset(&cap.pinch_frames, 0, sizeof(cap.pinch_frames));
        cap.pinch_frames = 0;
        cap.pinch_lifts = 0;

        for (int k = 0; k < 2; k++) {
            send(0, 0, 0, 1, &f);
            send(0, 0, 0, 0, NULL);
            idle(80);
        }
        idle(800);

        check(cap.pinch_frames >= 10, "round %d: pinch frames %d", round, cap.pinch_frames);
        check(cap.pinch_lifts == 1, "round %d: pinch released", round);
        if (round == 0)
            check(cap.spread_first < cap.spread_last, "first double tap zooms in (%.1f -> %.1f)",
                  cap.spread_first, cap.spread_last);
        else
            check(cap.spread_first > cap.spread_last, "second double tap zooms out (%.1f -> %.1f)",
                  cap.spread_first, cap.spread_last);
    }
    check(cap.tp_n == 0, "nothing left on the virtual pad");
}

TEST(test_resting_finger_scroll)
{
    setup();
    finger f[2] = { { 1, 10.0, 0.0 }, { 2, -12.0, 2.0 } };

    send(0, 0, 0, 2, f);
    for (int i = 0; i < 15; i++) {
        f[0].y -= 1.0;
        send(0, 0, 0, 2, f);
    }
    send(0, 0, 0, 0, NULL);

    check(cap.tp_n3_frames == 0, "not a swipe");
    check(cap.tp_y0 - cap.tp_y > 8.0, "scrolled with the moving finger (%.1f mm)", cap.tp_y0 - cap.tp_y);
}

TEST(test_two_finger_swipe)
{
    setup();
    finger f[2] = { { 1, -8.0, -4.0 }, { 2, 8.0, -4.0 } };

    send(0, 0, 0, 2, f);
    for (int i = 0; i < 15; i++) {
        f[0].x -= 1.5;
        f[1].x -= 1.5;
        send(0, 0, 0, 2, f);
    }
    send(0, 0, 0, 1, f);
    send(0, 0, 0, 0, NULL);
    idle(200);

    check(cap.tp_max_n == 3, "three virtual fingers (GNOME workspace gesture)");
    check(cap.tp_n2_frames == 0 && cap.tp_downs == 1, "three fingers land at once, no two-finger phase");
    check(cap.tp_x < cap.tp_x0 - 30.0, "moved left %.1f mm (22.5 mm x swipe_speed, nothing lost)",
          cap.tp_x - cap.tp_x0);
    check(cap.tp_n == 0, "released");
}

TEST(test_swipe_with_drift)
{
    setup();
    finger f[2] = { { 1, -8.0, -4.0 }, { 2, 8.0, -4.0 } };

    send(0, 0, 0, 2, f);
    for (int i = 0; i < 15; i++) {
        f[0].x += 1.5; f[1].x += 1.5;
        f[0].y += 0.7; f[1].y += 0.7;
        send(0, 0, 0, 2, f);
    }
    send(0, 0, 0, 0, NULL);
    idle(200);

    check(cap.tp_max_n == 3 && cap.tp_x > cap.tp_x0 + 30.0, "workspace swipe to the right (%.1f)",
          cap.tp_x - cap.tp_x0);
    check(fabs(cap.tp_y - cap.tp_y0) < 0.01, "vertical drift removed (%.2f)", cap.tp_y - cap.tp_y0);
}

TEST(test_swipe_flick)
{
    setup();
    finger f[2] = { { 1, -8.0, -4.0 }, { 2, 8.0, -4.0 } };
    int64_t t0 = g_now;

    send(0, 0, 0, 2, f);
    for (int i = 0; i < 3; i++) {
        f[0].x -= 3.0;
        f[1].x -= 3.0;
        send(0, 0, 0, 2, f);
    }
    int64_t lifted = g_now;
    send(0, 0, 0, 0, NULL);
    idle(300);

    check(cap.tp_first3 && cap.tp_first3 - t0 <= 12000,
          "swipe recognised on the first moving report (%lld ms)", (long long)((cap.tp_first3 - t0) / 1000));
    check(cap.tp_x0 - cap.tp_x > 9.0 * 1.6 + 5.0, "flick glides on after the lift (%.1f mm)", cap.tp_x0 - cap.tp_x);
    check(cap.tp_n == 0 && cap.tp_t_up - lifted <= 100000, "then lifts quickly (%lld ms)",
          (long long)((cap.tp_t_up - lifted) / 1000));
    check(cap.tp_lift_gap <= 8000, "lift right after the last motion: GNOME sees the speed");
}

TEST(test_inertia_guard_and_pointer)
{
    setup();
    finger f = { 3, 0.0, 10.0 };

    send(0, 0, 0, 1, &f);
    for (int i = 0; i < 15; i++) {
        f.y -= 2.0;
        send(0, 0, 0, 1, &f);
    }
    send(0, 0, 0, 0, NULL);
    idle(100);
    double y = cap.tp_y;
    send(40, 0, 0, 0, NULL);
    send(40, 0, 0, 0, NULL);
    send(40, 0, 0, 0, NULL);
    idle(30);
    double y2 = cap.tp_y;
    idle(500);

    check(fabs(cap.tp_y - y2) < 0.01 && y - y2 < 30.0, "moving the mouse stops inertia");
    check(cap.tp_n == 0, "and the virtual fingers lift");
    check(cap.tp_lift_gap >= 200000, "only after %lld ms of stillness (apps add no inertia of their own)",
          (long long)(cap.tp_lift_gap / 1000));
}

TEST(test_repeated_flicks)
{
    setup();
    finger f = { 3, 0.0, 20.0 };
    double worst = 1e9;

    for (int k = 0; k < 40; k++) {
        f.y = 20.0;
        double before = cap.tp_path;
        send(0, 0, 0, 1, &f);
        for (int i = 0; i < 8; i++) {
            f.y -= 4.0;
            send(0, 0, 0, 1, &f);
        }
        send(0, 0, 0, 0, NULL);
        idle(150);
        if (cap.tp_path - before < worst)
            worst = cap.tp_path - before;
    }
    idle(6000);

    check(worst > 60.0, "every one of 40 flicks scrolls fully (worst %.1f mm): the contact never hits the edge", worst);
    check(cap.tp_n == 0, "released at the end");
}

TEST(test_vertical_two_finger)
{
    setup();
    finger f[2] = { { 1, -8.0, 0.0 }, { 2, 8.0, 0.0 } };

    send(0, 0, 0, 2, f);
    for (int i = 0; i < 12; i++) {
        f[0].y -= 1.5; f[1].y -= 1.5;
        send(0, 0, 0, 2, f);
    }
    send(0, 0, 0, 0, NULL);
    check(cap.tp_max_n == 3 && cap.tp_y < cap.tp_y0 - 20.0, "two fingers up -> overview gesture");

    idle(600);
    cfg.swipe2_vertical = MM_V2_SCROLL;
    setup();
    f[0].y = f[1].y = 0.0;
    send(0, 0, 0, 2, f);
    for (int i = 0; i < 12; i++) {
        f[0].y -= 1.5; f[1].y -= 1.5;
        send(0, 0, 0, 2, f);
    }
    send(0, 0, 0, 0, NULL);
    idle(3000);
    check(cap.tp_max_n == 2, "two_finger_vertical = scroll -> plain scroll");
    cfg.swipe2_vertical = MM_V2_OVERVIEW;
}

TEST(test_burst_spacing)
{
    setup();
    finger f = { 1, 0.0, 0.0 };
    send(0, 0, 0, 1, &f);

    for (int i = 0; i < 20; i++) {
        f.y -= 1.0;
        send_dt(0, 0, 0, 1, &f, 50);
        f.y -= 1.0;
        send_dt(0, 0, 0, 1, &f, 21950);
    }
    send(0, 0, 0, 0, NULL);
    idle(4000);

    check(cap.tp_min_gap >= 4000, "virtual frames at least 4 ms apart (min %lld us)", (long long)cap.tp_min_gap);
    check(cap.tp_y0 - cap.tp_y > 35.0, "no motion lost (%.1f mm)", cap.tp_y0 - cap.tp_y);
}

static uint32_t rng = 12345;
static int rnd(int n)
{
    rng = rng * 1103515245u + 12345u;
    return (int)((rng >> 8) % (uint32_t)n);
}

TEST(test_fuzz)
{
    int spins = 0, stuck = 0;
    int modes[4][3] = {
        { MM_SCROLL_TOUCHPAD, 1, MM_SWIPE2_NATIVE },
        { MM_SCROLL_TOUCHPAD, 0, MM_SWIPE2_NATIVE },
        { MM_SCROLL_WHEEL, 1, MM_SWIPE2_KEYS },
        { MM_SCROLL_TOUCHPAD, 1, MM_SWIPE2_KEYS },
    };

    for (int m = 0; m < 4; m++) {
        cfg.scroll_mode = modes[m][0];
        cfg.momentum = modes[m][1];
        cfg.swipe2 = modes[m][2];
        setup();

        finger f[3] = { { 1, 0, 0 }, { 2, 10, 0 }, { 3, -10, 5 } };
        int n = 0, btn = 0;

        for (int step = 0; step < 20000; step++) {
            int r = rnd(100);
            if (r < 6)       n = rnd(4);
            else if (r < 8)  btn = !btn;
            for (int k = 0; k < 3; k++) {
                f[k].x += (rnd(41) - 20) / 10.0;
                f[k].y += (rnd(41) - 20) / 10.0;
                if (f[k].x < -22 || f[k].x > 25) f[k].x = 0;
                if (f[k].y < -35 || f[k].y > 45) f[k].y = 0;
            }
            int dx = rnd(10) < 3 ? rnd(41) - 20 : 0;

            uint8_t buf[256];
            mm_report rep;
            size_t len = encode(buf, dx, 0, btn, n, f, &cfg);
            mm_parse_report(buf, len, &cfg, &rep);
            mm_engine_report(&eng, &rep, g_now);

            int64_t d = mm_engine_next_deadline(&eng);
            if (d && d <= g_now)
                spins++;

            int64_t adv = rnd(20) == 0 ? rnd(800000) : 1000 + rnd(25000);
            int64_t target = g_now + adv;
            while (g_now < target) {
                d = mm_engine_next_deadline(&eng);
                if (!d || d > target) { g_now = target; break; }
                if (d > g_now) g_now = d;
                mm_engine_tick(&eng, g_now);
                int64_t d2 = mm_engine_next_deadline(&eng);
                if (d2 && d2 <= g_now) { spins++; g_now++; }
            }
        }

        uint8_t buf[64];
        mm_report rep;
        size_t len = encode(buf, 0, 0, 0, 0, f, &cfg);
        mm_parse_report(buf, len, &cfg, &rep);
        mm_engine_report(&eng, &rep, g_now);
        idle(8000);
        if (cap.tp_n != 0 || eng.btn_held)
            stuck++;
    }

    check(spins == 0, "timers never stay due after a tick (%d)", spins);
    check(stuck == 0, "no stuck virtual fingers or buttons (%d)", stuck);

    cfg.scroll_mode = MM_SCROLL_TOUCHPAD;
    cfg.momentum = 1;
    cfg.swipe2 = MM_SWIPE2_NATIVE;
}

TEST(test_palm_ignored)
{
    setup();
    finger palm = { 6, 0.0, 46.0 };

    send(0, 0, 0, 1, &palm);
    for (int i = 0; i < 10; i++) {
        palm.y -= 1.0;
        send(0, 0, 0, 1, &palm);
    }
    send(0, 0, 0, 0, NULL);

    check(cap.tp_frames == 0, "palm contact produces nothing (%d frames)", cap.tp_frames);
}

TEST(test_click_drag_no_scroll)
{
    setup();
    finger f = { 2, -10.0, -5.0 };

    send(0, 0, 0, 1, &f);
    send(0, 0, 1, 1, &f);
    for (int i = 0; i < 10; i++) {
        f.y -= 1.0;
        send(30, 0, 1, 1, &f);
    }
    send(0, 0, 0, 1, &f);
    send(0, 0, 0, 0, NULL);

    check(fabs(cap.tp_y - cap.tp_y0) < 0.01, "no scrolling while the button is held");
    check(cap.ptr_x == 300, "pointer moved (%ld)", cap.ptr_x);
    check(cap.nbtn == 2, "press and release");
}

TEST(test_key_page_swipe)
{
    cfg.hswipe = MM_HSWIPE_KEYS;
    setup();
    finger f = { 1, -15.0, -5.0 };

    send(0, 0, 0, 1, &f);
    for (int i = 0; i < 10; i++) {
        f.x += 2.5;
        send(0, 0, 0, 1, &f);
    }
    send(0, 0, 0, 0, NULL);
    idle(400);

    check(cap.nkeys == 1 && cap.key_first[0] == 56 && cap.key_n[0] == 2, "ALT+LEFT (back) once");
    check(fabs(cap.tp_x - cap.tp_x0) < 0.01, "no horizontal scroll in keys mode");
    cfg.hswipe = MM_HSWIPE_SCROLL;
}

TEST(test_wheel_momentum)
{
    cfg.scroll_mode = MM_SCROLL_WHEEL;
    setup();
    finger f = { 1, 0.0, 10.0 };

    send(0, 0, 0, 1, &f);
    check(cap.tp_frames == 0, "wheel mode does not use the virtual touchpad");

    for (int i = 0; i < 10; i++) {
        f.y -= 2.0;
        send(0, 0, 0, 1, &f);
    }
    long during = cap.wheel_v;
    send(0, 0, 0, 0, NULL);
    idle(300);
    long after = cap.wheel_v;

    check(during < 0, "natural direction: finger forward scrolls down (%ld)", during);
    check(after < during - 120, "inertia keeps scrolling after release (%ld -> %ld)", during, after);

    /* touching stops it */
    send(0, 0, 0, 1, &f);
    long stop = cap.wheel_v;
    idle(300);
    check(cap.wheel_v == stop, "touch stops inertia");
    send(0, 0, 0, 0, NULL);

    cfg.scroll_mode = MM_SCROLL_TOUCHPAD;
}

TEST(test_jump_limit)
{
    setup();
    finger f = { 1, 0.0, 0.0 };
    send(0, 0, 0, 1, &f);

    /* Very fast flick: the virtual motion per frame must stay under
     * libinput's jump threshold, the rest is spread over later frames. */
    double prev = cap.tp_y;
    double worst = 0.0;
    for (int i = 0; i < 6; i++) {
        f.y -= 8.0;
        send(0, 0, 0, 1, &f);
        double step = prev - cap.tp_y;
        if (step > worst)
            worst = step;
        prev = cap.tp_y;
    }
    send(0, 0, 0, 0, NULL);

    check(worst < 20.0, "largest step per report %.1f mm", worst);
}

TEST(test_config_parse)
{
    mm_action a;
    check(mm_parse_action("CTRL+ALT+LEFT", &a) == 0 && a.type == MM_ACT_KEYS && a.n == 3 &&
          a.codes[0] == 29 && a.codes[1] == 56 && a.codes[2] == 105, "combo parsed");
    check(mm_parse_action("none", &a) == 0 && a.type == MM_ACT_NONE, "none");
    check(mm_parse_action("smart-zoom", &a) == 0 && a.type == MM_ACT_SMART_ZOOM, "smart-zoom");
    check(mm_parse_action("CTRL+NOPE", &a) < 0, "unknown key rejected");
    check(mm_parse_action("0", &a) == 0 && a.codes[0] == 11, "digit key");
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    mm_config_defaults(&cfg);

    struct { const char *name; void (*fn)(void); } tests[] = {
        { "parser", test_parser },
        { "config", test_config_parse },
        { "vertical scroll + inertia", test_vertical_scroll },
        { "touch stops inertia", test_touch_stops_inertia },
        { "momentum = 0", test_app_inertia_mode },
        { "moving the mouse does not scroll", test_pointer_motion_blocks_scroll },
        { "sideways roll", test_sideways_roll_no_scroll },
        { "scroll acceleration", test_scroll_acceleration },
        { "double click is not a double tap", test_click_is_not_tap },
        { "secondary click", test_secondary_click },
        { "two-finger double tap", test_two_finger_double_tap },
        { "two-finger double tap, lost finger", test_two_finger_double_tap_lost_finger },
        { "single tap", test_single_tap_is_harmless },
        { "smart zoom", test_smart_zoom },
        { "resting finger + scroll", test_resting_finger_scroll },
        { "two-finger swipe", test_two_finger_swipe },
        { "two-finger swipe with drift", test_swipe_with_drift },
        { "two-finger flick", test_swipe_flick },
        { "inertia: guard, pointer stops it", test_inertia_guard_and_pointer },
        { "repeated flicks", test_repeated_flicks },
        { "two fingers up/down", test_vertical_two_finger },
        { "Bluetooth burst spacing", test_burst_spacing },
        { "fuzz", test_fuzz },
        { "palm rejection", test_palm_ignored },
        { "click-drag", test_click_drag_no_scroll },
        { "page swipe keys", test_key_page_swipe },
        { "wheel momentum", test_wheel_momentum },
        { "jump limit", test_jump_limit },
    };

    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        int before = failures;
        tests[i].fn();
        printf("%s %s\n", failures == before ? "ok  " : "FAIL", tests[i].name);
    }

    printf("\n%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
