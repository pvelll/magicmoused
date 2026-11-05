/*
 * magicmoused — userspace driver for Apple Magic Mouse 2 on Linux,
 * modelled on how the mouse behaves under macOS.
 *
 * Data flow:
 *
 *   /dev/hidrawN  --(report 0x12)-->  report.c  -->  engine.c  -->  vinput.c (uinput)
 *                                                                   |- virtual mouse     (pointer, clicks, wheel)
 *                                                                   |- virtual touchpad  (scroll, swipes, pinch)
 *                                                                   `- virtual keyboard  (key shortcuts)
 *
 * The kernel's own input device for the mouse is grabbed (EVIOCGRAB) so
 * its coarse wheel emulation and the middle-click zone go silent. If the
 * daemon stops, the grab is released and the mouse falls back to the
 * kernel behaviour automatically.
 *
 * report.c, engine.c and config.c contain no Linux-specific code and are
 * unit-tested on any POSIX system (see tests/).
 */
#ifndef MM_H
#define MM_H

#include <stddef.h>
#include <stdint.h>

/* ---------------------------------------------------------------- */
/* Hardware                                                          */
/* ---------------------------------------------------------------- */

#define MM_VENDOR_BT          0x004c
#define MM_VENDOR_USB         0x05ac
#define MM_PRODUCT_MM2        0x0269   /* Magic Mouse 2 (Lightning) */
#define MM_PRODUCT_MM2_USBC   0x0323   /* Magic Mouse (USB-C, 2024) */

#define MM_REPORT_ID          0x12
#define MM_HEADER_SIZE        14
#define MM_TOUCH_SIZE         8
#define MM_MAX_TOUCHES        16

/* Touch surface range in device units (same as the kernel driver). */
#define MM_MIN_X  (-1100)
#define MM_MAX_X  1258
#define MM_MIN_Y  (-1589)
#define MM_MAX_Y  2047

/* Touch state, upper nibble of byte 7 of a touch record. */
#define MM_STATE_START        0x30
#define MM_STATE_DRAG         0x40

/* Linux input codes the engine needs (identical to linux/input-event-codes.h). */
#define MM_BTN_LEFT           0x110
#define MM_BTN_RIGHT          0x111
#define MM_BTN_MIDDLE         0x112

/* Virtual touchpad geometry. It is huge so that a long stroke plus the
 * inertia that follows it never reaches the edge of the virtual pad. */
#define MM_VTP_WIDTH_MM       4000.0
#define MM_VTP_HEIGHT_MM      4000.0
#define MM_VTP_RES            20       /* units per mm */
#define MM_VTP_SLOTS          5

#define MM_MOT_SAMPLES        16

/* ---------------------------------------------------------------- */
/* Configuration                                                     */
/* ---------------------------------------------------------------- */

enum { MM_SCROLL_TOUCHPAD, MM_SCROLL_WHEEL };
enum { MM_HSWIPE_SCROLL, MM_HSWIPE_KEYS };
enum { MM_SWIPE2_NATIVE, MM_SWIPE2_KEYS, MM_SWIPE2_OFF };
enum { MM_V2_OVERVIEW, MM_V2_SCROLL, MM_V2_OFF };
enum { MM_ZOOM_PINCH, MM_ZOOM_KEYS };
enum { MM_SECONDARY_RIGHT, MM_SECONDARY_LEFT, MM_SECONDARY_OFF };

enum { MM_ACT_NONE, MM_ACT_KEYS, MM_ACT_SMART_ZOOM };

#define MM_MAX_COMBO 6

typedef struct {
    int type;                   /* MM_ACT_* */
    int n;
    int codes[MM_MAX_COMBO];    /* Linux KEY_* codes, pressed in order */
} mm_action;

typedef struct {
    /* surface geometry */
    double units_per_mm_x;
    double units_per_mm_y;
    int    invert_x;
    int    invert_y;
    double edge_zone;           /* fraction of width ignored at the left/right edge */
    double palm_zone;           /* fraction of length ignored at the back */

    /* scrolling */
    int    scroll_mode;         /* MM_SCROLL_* */
    double scroll_speed;
    double scroll_accel;
    double scroll_threshold_mm;
    int    axis_lock;
    int    natural_scroll;      /* wheel mode only; touchpad mode uses the GNOME setting */
    int    momentum;            /* driver-side inertia; 0 = leave it to the applications */
    double momentum_time_ms;    /* decay time constant */

    /* swipes */
    int    hswipe;              /* one finger horizontal: MM_HSWIPE_* */
    int    swipe2;              /* two fingers: MM_SWIPE2_* */
    int    swipe2_vertical;     /* two fingers up/down: MM_V2_* */
    double swipe_speed;
    mm_action back_keys;
    mm_action forward_keys;
    mm_action workspace_left_keys;
    mm_action workspace_right_keys;
    mm_action swipe_up_keys;
    mm_action swipe_down_keys;

    /* taps */
    int    tap_time_ms;
    int    double_tap_ms;
    double tap_move_mm;
    mm_action double_tap1;
    mm_action double_tap2;
    int    smart_zoom;          /* MM_ZOOM_* */
    mm_action zoom_in_keys;
    mm_action zoom_out_keys;

    /* clicks */
    int    secondary_click;     /* MM_SECONDARY_* */
    double click_split;         /* 0..1, dividing line between left and right halves */
    int    middle_click;

    /* misc */
    int    grab;
    int    verbose;
} mm_config;

void mm_config_defaults(mm_config *cfg);
/* Returns 0 on success, -1 if the file cannot be opened, >0 = number of bad lines. */
int  mm_config_load(mm_config *cfg, const char *path);
int  mm_parse_action(const char *text, mm_action *act);
void mm_config_print(const mm_config *cfg, void *stream);

/* ---------------------------------------------------------------- */
/* Reports                                                           */
/* ---------------------------------------------------------------- */

typedef struct {
    int    id;
    int    x, y;                /* raw device units, y as in the kernel (grows towards the palm) */
    double xmm, ymm;            /* millimetres, x to the right, y towards the palm */
    double xn, yn;              /* 0..1: xn 0 = left edge, yn 0 = front edge */
    int    major, minor, size, orientation;
    int    state;
    int    active;              /* finger is really touching (START or DRAG) */
} mm_touch;

typedef struct {
    int      dx, dy;            /* optical sensor motion */
    int      buttons;           /* bit 0 = left, bit 1 = right (the mouse's own guess) */
    int      has_touch;         /* 0 for short 8-byte reports */
    int      ntouch;
    mm_touch touch[MM_MAX_TOUCHES];
} mm_report;

/* Returns 1 if buf holds a usable Magic Mouse 2 report. */
int mm_parse_report(const uint8_t *buf, size_t len, const mm_config *cfg, mm_report *r);

/* ---------------------------------------------------------------- */
/* Engine output                                                     */
/* ---------------------------------------------------------------- */

typedef struct { double x, y; } mm_point;

typedef struct {
    void *ctx;
    void (*pointer)(void *ctx, int dx, int dy);
    void (*button)(void *ctx, int code, int down);
    void (*wheel)(void *ctx, int v120, int h120);
    void (*touchpad)(void *ctx, int n, const mm_point *pts);   /* mm on the virtual pad */
    void (*keys)(void *ctx, const int *codes, int n);          /* press+release a combo */
} mm_output;

/* ---------------------------------------------------------------- */
/* Engine                                                            */
/* ---------------------------------------------------------------- */

typedef struct {
    int64_t t;
    double  dx, dy;
} mm_motion;

typedef struct {
    int     active;
    int     ignored;            /* palm / grip contact */
    double  x, y;               /* mm */
    double  xn, yn;
    double  sx, sy;             /* reference position */
    double  lx, ly;             /* last position used for deltas */
    double  max_disp;
} mm_track;

typedef struct {
    const mm_config *cfg;
    mm_output out;

    mm_track tr[MM_MAX_TOUCHES];

    /* contact session: first finger down .. last finger up */
    int     phase;
    int64_t s_start;
    int     s_clicked;
    double  s_max_disp;
    long    s_pointer_travel;
    double  s_x, s_y;
    int     primary;
    unsigned swipe_mask;
    int     swipe_axis;
    double  sw_lx, sw_ly;
    double  acc_x, acc_y;
    int64_t stroke_start;
    int     lock;
    int     fired;
    int64_t t_last;
    double  vel;
    unsigned s_seen;
    int     s_interrupted;      /* this touch stopped inertia: not a tap */
    mm_motion mot[MM_MOT_SAMPLES];
    int     mot_n, mot_head;

    /* physical button */
    int     btn_held;
    int64_t t_btn_release;

    /* first tap of a possible double tap */
    int     tap_valid;
    int64_t tap_end;
    int     tap_fingers;
    double  tap_x, tap_y;

    /* virtual touchpad contact */
    int     vtp_n;
    double  vtp_cx, vtp_cy;
    double  vtp_travel;
    int64_t vtp_t_down;
    int64_t vtp_lift_at;
    double  vtp_res_x, vtp_res_y;   /* motion not emitted yet (rate limiting) */
    int64_t vtp_t_emit;
    int64_t vtp_t_move;         /* last frame that actually moved */
    double  vtp_last_norm;

    /* pinch animation (smart zoom) */
    int     anim_pending;
    int64_t anim_pending_since;
    int     anim_active;
    int     anim_step;
    double  anim_from, anim_to;
    int64_t anim_next;
    int     zoomed;

    /* inertia (both modes) */
    int     mom_active;
    int     mom_glide;          /* short glide after a swipe flick, not scroll inertia */
    double  mom_vx, mom_vy;
    int64_t mom_last;
    int64_t mom_start;
    double  mom_carry_vx, mom_carry_vy;
    int64_t mom_carry_t;

    /* wheel mode */
    double  wh_acc_v, wh_acc_h;

    /* pointer */
    double  p_recent;           /* decaying sum of recent sensor motion */
    int64_t p_mv_t;
} mm_engine;

void    mm_engine_init(mm_engine *e, const mm_config *cfg, const mm_output *out);
void    mm_engine_report(mm_engine *e, const mm_report *r, int64_t now_us);
void    mm_engine_tick(mm_engine *e, int64_t now_us);
int64_t mm_engine_next_deadline(const mm_engine *e);   /* 0 = nothing scheduled */
void    mm_engine_reset(mm_engine *e, int64_t now_us);  /* release everything */

/* ---------------------------------------------------------------- */
/* Logging                                                           */
/* ---------------------------------------------------------------- */

extern int mm_verbose;
void mm_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void mm_debug(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif
