/*
 * Gesture engine: turns Magic Mouse touches, clicks and motion into
 * macOS-like behaviour.
 *
 *   one finger moves          -> scroll. In touchpad mode the movement drives
 *                                two fingers on a virtual touchpad, so apps get
 *                                pixel-precise touchpad scrolling. When the
 *                                finger leaves the surface while moving, the
 *                                driver keeps the virtual fingers gliding and
 *                                slowing down (inertia), so coasting works in
 *                                every application, not only in GTK/Firefox.
 *                                Touching the mouse stops it, as on macOS.
 *   two fingers move together -> three-finger swipe on the virtual touchpad:
 *                                GNOME switches workspaces / opens the overview
 *                                1:1 with the fingers.
 *   click                     -> left, or right if every finger on the surface
 *                                is on the right half (macOS secondary click).
 *   one-finger double tap     -> smart zoom (virtual pinch).
 *   two-finger double tap     -> overview (macOS: Mission Control).
 *
 * The engine has no Linux dependencies; all time values are microseconds
 * supplied by the caller, and all output goes through mm_output.
 */
#include "mm.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

enum { PH_IDLE, PH_PENDING, PH_SCROLL, PH_SWIPE, PH_DONE };
enum { LOCK_NONE, LOCK_V, LOCK_H };

#define MS(x) ((int64_t)(x) * 1000)

#define VTP_MIN_CONTACT_US   MS(300)
#define VTP_MOVED_MM         2.0
#define VTP_SPACING_MM       16.0
#define VTP_MARGIN_X         300.0
#define VTP_MARGIN_Y         700.0
#define VTP_RECENTER_MM      500.0
#define APP_KINETIC_GUARD_US MS(200)
#define VTP_FLUSH_US         MS(4)
#define VTP_MIN_FRAME_US     MS(4)
#define VTP_MAX_NORM_MM      15.0
#define VTP_MAX_NORM_STEP    6.0

#define POST_CLICK_QUIET_US  MS(150)
#define POINTER_DECAY_US     80000.0
#define POINTER_MOVING       80.0
#define HSCROLL_FACTOR       2.0
#define TAP_POINTER_MAX      60
#define DOUBLE_TAP_DIST_MM   25.0
#define JUMP_MM              15.0

#define KEY_SWIPE1_MM        14.0
#define KEY_SWIPE1_TIME_US   MS(800)
#define KEY_SWIPE2_MM        12.0

#define SWIPE_START_MM       1.2
#define SWIPE_FLING_MIN      90.0
#define SWIPE_GLIDE_US       MS(64)

#define PINCH_STEPS          14
#define PINCH_INTERVAL_US    MS(10)
#define PINCH_NEAR_MM        12.0
#define PINCH_FAR_MM         36.0
#define ANIM_PENDING_MAX_US  MS(700)

#define MOM_INTERVAL_US      MS(8)
#define MOM_WINDOW_US        MS(70)
#define MOM_STALE_US         MS(50)
#define MOM_MIN_START        60.0
#define MOM_MIN_STOP         8.0
#define MOM_MAX_US           MS(5000)
#define MOM_CARRY_US         MS(450)
#define MOM_CARRY            0.6
#define REPORT_INTERVAL_US   MS(11)

#define WHEEL_UNITS_PER_MM   24.0

static double clampd(double v, double lo, double hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static int is_valid(const mm_track *t)
{
    return t->active && !t->ignored;
}

/* ---------------------------------------------------------------- */
/* Virtual touchpad contact                                          */
/* ---------------------------------------------------------------- */

static void vtp_emit(mm_engine *e)
{
    mm_point p[MM_VTP_SLOTS];

    for (int k = 0; k < e->vtp_n; k++) {
        p[k].x = e->vtp_cx + (k - 0.5) * VTP_SPACING_MM;
        p[k].y = e->vtp_cy;
    }

    if (e->out.touchpad)
        e->out.touchpad(e->out.ctx, e->vtp_n, p);
}

static int vtp_busy(const mm_engine *e)
{
    return e->anim_active || e->anim_pending;
}

static void vtp_down(mm_engine *e, int n, int64_t now)
{
    if (vtp_busy(e))
        return;

    e->vtp_lift_at = 0;

    if (e->vtp_n == n)
        return;

    if (e->vtp_n == 0) {
        e->vtp_cx = MM_VTP_WIDTH_MM / 2.0;
        e->vtp_cy = MM_VTP_HEIGHT_MM / 2.0;
        e->vtp_travel = 0.0;
        e->vtp_t_down = now;
        e->vtp_t_emit = now;
        e->vtp_t_move = now;
        e->vtp_res_x = e->vtp_res_y = 0.0;
        e->vtp_last_norm = 0.0;
    }

    e->vtp_n = n;
    vtp_emit(e);
}

static void vtp_lift(mm_engine *e)
{
    e->vtp_lift_at = 0;
    e->vtp_res_x = e->vtp_res_y = 0.0;

    if (e->vtp_n == 0)
        return;

    e->vtp_n = 0;
    vtp_emit(e);
}

static void vtp_release(mm_engine *e, int64_t now)
{
    if (e->vtp_n == 0 || e->mom_active)
        return;

    int64_t at = now;

    if (e->vtp_travel < VTP_MOVED_MM && e->vtp_t_down + VTP_MIN_CONTACT_US > at)
        at = e->vtp_t_down + VTP_MIN_CONTACT_US;

    if (e->vtp_n == 2 && e->cfg->momentum && e->vtp_travel >= VTP_MOVED_MM &&
        e->vtp_t_move + APP_KINETIC_GUARD_US > at)
        at = e->vtp_t_move + APP_KINETIC_GUARD_US;

    if (at <= now)
        vtp_lift(e);
    else
        e->vtp_lift_at = at;
}

static void vtp_flush(mm_engine *e, int64_t now)
{
    if (e->vtp_n == 0) {
        e->vtp_res_x = e->vtp_res_y = 0.0;
        return;
    }

    double want = hypot(e->vtp_res_x, e->vtp_res_y);
    if (want < 1e-4)
        return;

    if (now - e->vtp_t_emit < VTP_MIN_FRAME_US)
        return;

    double dt_ms = clampd((now - e->vtp_t_emit) / 1000.0, 4.0, 24.0);
    double cap_norm = fmin(VTP_MAX_NORM_MM, e->vtp_last_norm + VTP_MAX_NORM_STEP);
    double cap = cap_norm * dt_ms / 12.0;
    double k = want > cap ? cap / want : 1.0;

    double sx = e->vtp_res_x * k;
    double sy = e->vtp_res_y * k;

    e->vtp_res_x -= sx;
    e->vtp_res_y -= sy;

    double nx = clampd(e->vtp_cx + sx, VTP_MARGIN_X, MM_VTP_WIDTH_MM - VTP_MARGIN_X);
    double ny = clampd(e->vtp_cy + sy, VTP_MARGIN_Y, MM_VTP_HEIGHT_MM - VTP_MARGIN_Y);
    double moved = hypot(nx - e->vtp_cx, ny - e->vtp_cy);

    if (moved == 0.0)
        e->vtp_res_x = e->vtp_res_y = 0.0;

    e->vtp_cx = nx;
    e->vtp_cy = ny;
    e->vtp_travel += moved;
    if (moved > 0.0)
        e->vtp_t_move = now;
    e->vtp_last_norm = moved * 12.0 / dt_ms;
    e->vtp_t_emit = now;

    if (moved > 0.0)
        vtp_emit(e);
}

static void vtp_move(mm_engine *e, double dx, double dy, int64_t now)
{
    if (e->vtp_n == 0)
        return;

    e->vtp_res_x += dx;
    e->vtp_res_y += dy;
    vtp_flush(e, now);
}

static int vtp_has_residual(const mm_engine *e)
{
    return e->vtp_n && (fabs(e->vtp_res_x) > 0.01 || fabs(e->vtp_res_y) > 0.01);
}

/* ---------------------------------------------------------------- */
/* Smart zoom: a synthetic pinch on the virtual touchpad             */
/* ---------------------------------------------------------------- */

static void anim_frame(mm_engine *e, int64_t now)
{
    double t = (double)e->anim_step / PINCH_STEPS;
    double ease = t * t * (3.0 - 2.0 * t);
    double spread = e->anim_from + (e->anim_to - e->anim_from) * ease;
    double cx = MM_VTP_WIDTH_MM / 2.0;
    double cy = MM_VTP_HEIGHT_MM / 2.0;

    mm_point p[2] = {
        { cx - spread / 2.0, cy },
        { cx + spread / 2.0, cy },
    };

    if (e->out.touchpad)
        e->out.touchpad(e->out.ctx, 2, p);

    if (e->anim_step >= PINCH_STEPS) {
        if (e->out.touchpad)
            e->out.touchpad(e->out.ctx, 0, NULL);
        e->anim_active = 0;
        return;
    }

    e->anim_step++;
    e->anim_next = now + PINCH_INTERVAL_US;
}

static void anim_start(mm_engine *e, int64_t now)
{
    e->anim_pending = 0;
    e->anim_active = 1;
    e->anim_step = 0;
    anim_frame(e, now);
}

/* ---------------------------------------------------------------- */
/* Actions                                                           */
/* ---------------------------------------------------------------- */

static void run_keys(mm_engine *e, const mm_action *a)
{
    if (a->type == MM_ACT_KEYS && a->n > 0 && e->out.keys)
        e->out.keys(e->out.ctx, a->codes, a->n);
}

static void smart_zoom(mm_engine *e, int64_t now)
{
    const mm_config *c = e->cfg;

    if (c->smart_zoom == MM_ZOOM_KEYS) {
        run_keys(e, e->zoomed ? &c->zoom_out_keys : &c->zoom_in_keys);
        e->zoomed = !e->zoomed;
        return;
    }

    if (vtp_busy(e))
        return;

    e->anim_from = e->zoomed ? PINCH_FAR_MM : PINCH_NEAR_MM;
    e->anim_to = e->zoomed ? PINCH_NEAR_MM : PINCH_FAR_MM;
    e->zoomed = !e->zoomed;
    e->anim_pending = 1;
    e->anim_pending_since = now;
}

static void run_action(mm_engine *e, const mm_action *a, int64_t now)
{
    switch (a->type) {
    case MM_ACT_KEYS:
        run_keys(e, a);
        break;
    case MM_ACT_SMART_ZOOM:
        smart_zoom(e, now);
        break;
    default:
        break;
    }
}

/* ---------------------------------------------------------------- */
/* Wheel output                                                      */
/* ---------------------------------------------------------------- */

static void wheel_emit(mm_engine *e, double dx_mm, double dy_mm)
{
    const mm_config *c = e->cfg;

    double v = (c->natural_scroll ? dy_mm : -dy_mm) * WHEEL_UNITS_PER_MM;
    double h = (c->natural_scroll ? -dx_mm : dx_mm) * WHEEL_UNITS_PER_MM;

    e->wh_acc_v += v;
    e->wh_acc_h += h;

    int iv = (int)e->wh_acc_v;
    int ih = (int)e->wh_acc_h;

    e->wh_acc_v -= iv;
    e->wh_acc_h -= ih;

    if ((iv || ih) && e->out.wheel)
        e->out.wheel(e->out.ctx, iv, ih);
}

static void scroll_out(mm_engine *e, double dx, double dy, int64_t now)
{
    if (e->cfg->scroll_mode == MM_SCROLL_TOUCHPAD)
        vtp_move(e, dx, dy, now);
    else
        wheel_emit(e, dx, dy);
}

/* ---------------------------------------------------------------- */
/* Inertia                                                           */
/* ---------------------------------------------------------------- */

static void mot_push(mm_engine *e, int64_t t, double dx, double dy)
{
    e->mot[e->mot_head].t = t;
    e->mot[e->mot_head].dx = dx;
    e->mot[e->mot_head].dy = dy;
    e->mot_head = (e->mot_head + 1) % MM_MOT_SAMPLES;
    if (e->mot_n < MM_MOT_SAMPLES)
        e->mot_n++;
}

static int mot_velocity(const mm_engine *e, int64_t now, double *vx, double *vy)
{
    if (e->mot_n == 0)
        return 0;

    int newest = (e->mot_head + MM_MOT_SAMPLES - 1) % MM_MOT_SAMPLES;
    int64_t t_new = e->mot[newest].t;

    if (now - t_new > MOM_STALE_US)
        return 0;

    double sx = 0.0, sy = 0.0;
    int64_t t_from = 0;
    int k;

    for (k = 0; k < e->mot_n; k++) {
        const mm_motion *m = &e->mot[(newest - k + MM_MOT_SAMPLES) % MM_MOT_SAMPLES];

        if (k > 0 && t_new - m->t > MOM_WINDOW_US) {
            t_from = m->t;
            break;
        }

        sx += m->dx;
        sy += m->dy;
    }

    if (k == e->mot_n)
        t_from = e->mot[(newest - (k - 1) + MM_MOT_SAMPLES) % MM_MOT_SAMPLES].t - REPORT_INTERVAL_US;

    double span = (t_new - t_from) / 1e6;
    if (span < 0.005)
        span = 0.005;

    *vx = sx / span;
    *vy = sy / span;
    return 1;
}

static void momentum_stop(mm_engine *e, int64_t now)
{
    if (!e->mom_active)
        return;

    int glide = e->mom_glide;

    e->mom_active = 0;
    e->mom_glide = 0;

    if (e->phase != PH_SCROLL && e->phase != PH_SWIPE &&
        (glide || e->cfg->scroll_mode == MM_SCROLL_TOUCHPAD))
        vtp_release(e, now);
}

static void momentum_step(mm_engine *e, int64_t now)
{
    double dt = (now - e->mom_last) / 1e6;
    e->mom_last = now;

    if (e->mom_glide) {
        vtp_move(e, e->mom_vx * dt, e->mom_vy * dt, now);
        if (now - e->mom_start >= SWIPE_GLIDE_US)
            momentum_stop(e, now);
        return;
    }

    double tau = e->cfg->momentum_time_ms / 1000.0;
    double decay = exp(-dt / tau);

    double dx = e->mom_vx * tau * (1.0 - decay);
    double dy = e->mom_vy * tau * (1.0 - decay);

    e->mom_vx *= decay;
    e->mom_vy *= decay;

    scroll_out(e, dx, dy, now);

    if (hypot(e->mom_vx, e->mom_vy) < MOM_MIN_STOP || now - e->mom_start > MOM_MAX_US)
        momentum_stop(e, now);
}

/* ---------------------------------------------------------------- */
/* Pointer                                                           */
/* ---------------------------------------------------------------- */

static int pointer_moving(const mm_engine *e, int64_t now)
{
    if (!e->p_mv_t)
        return 0;
    return e->p_recent * exp(-(double)(now - e->p_mv_t) / POINTER_DECAY_US) > POINTER_MOVING;
}

static void emit_pointer(mm_engine *e, int dx, int dy, int64_t now)
{
    if (!dx && !dy)
        return;

    long travel = labs((long)dx) + labs((long)dy);

    if (e->phase != PH_IDLE)
        e->s_pointer_travel += travel;

    e->p_recent = (e->p_mv_t ? e->p_recent * exp(-(double)(now - e->p_mv_t) / POINTER_DECAY_US) : 0.0) + travel;
    e->p_mv_t = now;

    if (e->mom_active && !e->mom_glide && e->phase == PH_IDLE && pointer_moving(e, now))
        momentum_stop(e, now);

    if (e->out.pointer)
        e->out.pointer(e->out.ctx, dx, dy);
}

/* ---------------------------------------------------------------- */
/* Buttons                                                           */
/* ---------------------------------------------------------------- */

static int on_secondary_side(const mm_config *c, double xn)
{
    if (c->secondary_click == MM_SECONDARY_LEFT)
        return xn < c->click_split;
    return xn > c->click_split;
}

static int choose_button(mm_engine *e, int hw)
{
    const mm_config *c = e->cfg;

    if (c->secondary_click == MM_SECONDARY_OFF)
        return MM_BTN_LEFT;

    int n = 0;
    int nsec = 0;

    for (int i = 0; i < MM_MAX_TOUCHES; i++) {
        const mm_track *t = &e->tr[i];
        if (!is_valid(t))
            continue;
        n++;
        if (on_secondary_side(c, t->xn))
            nsec++;
    }

    if (n == 0)
        return (c->secondary_click == MM_SECONDARY_RIGHT && (hw & 2)) ? MM_BTN_RIGHT : MM_BTN_LEFT;

    if (c->middle_click && n >= 2 && nsec > 0 && nsec < n)
        return MM_BTN_MIDDLE;

    return nsec == n ? MM_BTN_RIGHT : MM_BTN_LEFT;
}

static void handle_buttons(mm_engine *e, int hw, int64_t now)
{
    hw &= 3;

    if (hw && !e->btn_held) {
        int code = choose_button(e, hw);

        e->btn_held = code;
        e->s_clicked = 1;
        e->tap_valid = 0;
        momentum_stop(e, now);

        if (e->out.button)
            e->out.button(e->out.ctx, code, 1);

        mm_debug("click %s", code == MM_BTN_RIGHT ? "right" : code == MM_BTN_MIDDLE ? "middle" : "left");
    } else if (!hw && e->btn_held) {
        if (e->out.button)
            e->out.button(e->out.ctx, e->btn_held, 0);

        e->btn_held = 0;
        e->t_btn_release = now;
    }
}

/* ---------------------------------------------------------------- */
/* Touch tracking                                                    */
/* ---------------------------------------------------------------- */

static int touch_rejected(const mm_config *c, const mm_touch *t)
{
    if (t->xn < c->edge_zone || t->xn > 1.0 - c->edge_zone)
        return 1;   /* thumb / ring finger gripping the side */

    if (t->yn > 1.0 - c->palm_zone)
        return 1;   /* palm resting on the back */

    return 0;
}

static void update_tracks(mm_engine *e, const mm_report *r, int64_t now)
{
    int seen[MM_MAX_TOUCHES] = { 0 };

    for (int i = 0; i < r->ntouch; i++) {
        const mm_touch *t = &r->touch[i];

        if (!t->active)
            continue;

        int id = t->id & (MM_MAX_TOUCHES - 1);
        mm_track *tr = &e->tr[id];

        if (!tr->active) {
            memset(tr, 0, sizeof(*tr));
            tr->active = 1;
            tr->ignored = touch_rejected(e->cfg, t);
            tr->sx = tr->lx = t->xmm;
            tr->sy = tr->ly = t->ymm;

            mm_debug("touch %d down at %.1f,%.1f mm%s", id, t->xmm, t->ymm,
                     tr->ignored ? " (palm/edge, ignored)" : "");
        } else if (hypot(t->xmm - tr->x, t->ymm - tr->y) > JUMP_MM) {
            tr->sx = tr->lx = t->xmm;
            tr->sy = tr->ly = t->ymm;
        }

        tr->x = t->xmm;
        tr->y = t->ymm;
        tr->xn = t->xn;
        tr->yn = t->yn;

        double d = hypot(tr->x - tr->sx, tr->y - tr->sy);
        if (d > tr->max_disp)
            tr->max_disp = d;

        seen[id] = 1;
    }

    for (int id = 0; id < MM_MAX_TOUCHES; id++) {
        if (e->tr[id].active && !seen[id]) {
            e->tr[id].active = 0;
            mm_debug("touch %d up", id);
        }
    }
}

static int valid_count(const mm_engine *e)
{
    int n = 0;
    for (int i = 0; i < MM_MAX_TOUCHES; i++)
        n += is_valid(&e->tr[i]);
    return n;
}

static void centroid(const mm_engine *e, unsigned mask, double *x, double *y)
{
    double sx = 0.0, sy = 0.0;
    int n = 0;

    for (int i = 0; i < MM_MAX_TOUCHES; i++) {
        if (!(mask & (1u << i)) || !e->tr[i].active)
            continue;
        sx += e->tr[i].x;
        sy += e->tr[i].y;
        n++;
    }

    *x = n ? sx / n : 0.0;
    *y = n ? sy / n : 0.0;
}

static double start_metric(const mm_track *t)
{
    return hypot((t->x - t->sx) / HSCROLL_FACTOR, t->y - t->sy);
}

/* ---------------------------------------------------------------- */
/* Scroll                                                            */
/* ---------------------------------------------------------------- */

static int hswipe_keys(const mm_engine *e)
{
    return e->cfg->hswipe == MM_HSWIPE_KEYS && e->lock == LOCK_H;
}

static void start_scroll(mm_engine *e, int id, int64_t now)
{
    const mm_config *c = e->cfg;
    mm_track *t = &e->tr[id];

    double dx = t->x - t->sx;
    double dy = t->y - t->sy;

    e->phase = PH_SCROLL;
    e->primary = id;
    e->lock = LOCK_NONE;

    if (c->axis_lock) {
        if (fabs(dy) > 1.5 * fabs(dx))
            e->lock = LOCK_V;
        else if (fabs(dx) > 1.5 * fabs(dy))
            e->lock = LOCK_H;
    }

    t->lx = t->x;
    t->ly = t->y;

    e->t_last = now;
    e->stroke_start = now;
    e->vel = 0.0;
    e->acc_x = e->acc_y = 0.0;
    e->fired = 0;
    e->mot_n = 0;
    e->mot_head = 0;

    if (hswipe_keys(e)) {
        vtp_release(e, now);
    } else if (c->scroll_mode == MM_SCROLL_TOUCHPAD) {
        if (e->vtp_n == 2 && !vtp_busy(e) &&
            (fabs(e->vtp_cx - MM_VTP_WIDTH_MM / 2.0) > VTP_RECENTER_MM ||
             fabs(e->vtp_cy - MM_VTP_HEIGHT_MM / 2.0) > VTP_RECENTER_MM))
            vtp_lift(e);

        vtp_down(e, 2, now);
    }

    mm_debug("scroll start (finger %d, lock %s)", id,
             e->lock == LOCK_V ? "vertical" : e->lock == LOCK_H ? "horizontal" : "none");
}

static void scroll_update(mm_engine *e, int64_t now)
{
    const mm_config *c = e->cfg;
    mm_track *t = &e->tr[e->primary];

    double dx = t->x - t->lx;
    double dy = t->y - t->ly;

    t->lx = t->x;
    t->ly = t->y;

    double dt = clampd((now - e->t_last) / 1e6, 0.004, 0.05);
    e->t_last = now;

    if (e->btn_held)
        return;

    if (e->lock == LOCK_V)
        dx = 0.0;
    else if (e->lock == LOCK_H)
        dy = 0.0;

    e->acc_x += dx;
    e->acc_y += dy;

    e->vel = 0.5 * e->vel + 0.5 * (hypot(dx, dy) / dt);

    double gain = 1.0 + c->scroll_accel * clampd((e->vel - 50.0) / 250.0, 0.0, 1.0);
    double vdx = dx * c->scroll_speed * gain;
    double vdy = dy * c->scroll_speed * gain;

    if (hswipe_keys(e)) {
        if (!e->fired && fabs(e->acc_x) >= KEY_SWIPE1_MM && now - e->stroke_start <= KEY_SWIPE1_TIME_US) {
            e->fired = 1;
            mm_debug("page swipe %s", e->acc_x > 0 ? "back" : "forward");
            /* Finger to the right = previous page, as on macOS. */
            run_keys(e, e->acc_x > 0 ? &c->back_keys : &c->forward_keys);
        }
        return;
    }

    if (vtp_busy(e))
        return;

    mot_push(e, now, vdx, vdy);
    scroll_out(e, vdx, vdy, now);
}

static void scroll_finish(mm_engine *e, int64_t now)
{
    const mm_config *c = e->cfg;
    double vx = 0.0, vy = 0.0;

    int coast = c->momentum && !hswipe_keys(e) && !vtp_busy(e) &&
                (c->scroll_mode == MM_SCROLL_WHEEL || e->vtp_n > 0) &&
                mot_velocity(e, now, &vx, &vy) &&
                hypot(vx, vy) >= MOM_MIN_START;

    if (!coast) {
        if (c->scroll_mode == MM_SCROLL_TOUCHPAD)
            vtp_release(e, now);
        mm_debug("scroll end");
        return;
    }

    if (now - e->mom_carry_t <= MOM_CARRY_US &&
        vx * e->mom_carry_vx + vy * e->mom_carry_vy > 0.0) {
        vx += MOM_CARRY * e->mom_carry_vx;
        vy += MOM_CARRY * e->mom_carry_vy;
    }

    e->mom_carry_t = 0;
    e->mom_active = 1;
    e->mom_vx = vx;
    e->mom_vy = vy;
    e->mom_last = now;
    e->mom_start = now;

    mm_debug("scroll end, coasting at %.0f mm/s", hypot(vx, vy));
}

/* ---------------------------------------------------------------- */
/* Two-finger swipe                                                  */
/* ---------------------------------------------------------------- */

static void start_swipe(mm_engine *e, unsigned mask, int best, int64_t now)
{
    const mm_config *c = e->cfg;

    double cx, cy, sx = 0.0, sy = 0.0;
    int n = 0;

    centroid(e, mask, &cx, &cy);
    for (int i = 0; i < MM_MAX_TOUCHES; i++) {
        if ((mask & (1u << i)) && e->tr[i].active) {
            sx += e->tr[i].sx;
            sy += e->tr[i].sy;
            n++;
        }
    }
    double dx0 = n ? cx - sx / n : 0.0;
    double dy0 = n ? cy - sy / n : 0.0;

    int horizontal = fabs(dx0) >= 0.7 * fabs(dy0);

    if (!horizontal && c->swipe2_vertical == MM_V2_SCROLL) {
        start_scroll(e, best, now);
        return;
    }

    e->phase = PH_SWIPE;
    e->swipe_mask = mask;
    e->swipe_axis = horizontal ? LOCK_H : LOCK_V;
    e->fired = 0;
    e->sw_lx = cx;
    e->sw_ly = cy;
    e->acc_x = horizontal ? dx0 : 0.0;
    e->acc_y = horizontal ? 0.0 : dy0;
    e->mot_n = 0;
    e->mot_head = 0;

    if (c->swipe2 == MM_SWIPE2_OFF || (!horizontal && c->swipe2_vertical == MM_V2_OFF)) {
        e->phase = PH_DONE;
        vtp_release(e, now);
        return;
    }

    if (c->swipe2 == MM_SWIPE2_NATIVE) {
        if (e->vtp_n && e->vtp_n != 3 &&
            (e->vtp_travel >= VTP_MOVED_MM || now - e->vtp_t_down >= VTP_MIN_CONTACT_US))
            vtp_lift(e);

        vtp_down(e, 3, now);

        vtp_move(e, e->acc_x * c->swipe_speed, e->acc_y * c->swipe_speed, now);
        mot_push(e, now, e->acc_x * c->swipe_speed, e->acc_y * c->swipe_speed);
    } else {
        vtp_release(e, now);
    }

    mm_debug("swipe start (%s)", horizontal ? "horizontal" : "vertical");
}

static void swipe_finish(mm_engine *e, int64_t now)
{
    double vx = 0.0, vy = 0.0;

    if (e->cfg->swipe2 == MM_SWIPE2_NATIVE && e->vtp_n == 3 && !vtp_busy(e) &&
        mot_velocity(e, now, &vx, &vy) && hypot(vx, vy) >= SWIPE_FLING_MIN) {
        e->mom_active = 1;
        e->mom_glide = 1;
        e->mom_vx = vx;
        e->mom_vy = vy;
        e->mom_last = now;
        e->mom_start = now;
        mm_debug("swipe end, flick at %.0f mm/s", hypot(vx, vy));
        return;
    }

    vtp_release(e, now);
    mm_debug("swipe end");
}

static void swipe_update(mm_engine *e, int64_t now)
{
    const mm_config *c = e->cfg;

    for (int i = 0; i < MM_MAX_TOUCHES; i++) {
        if ((e->swipe_mask & (1u << i)) && !e->tr[i].active) {
            e->phase = PH_DONE;
            swipe_finish(e, now);
            return;
        }
    }

    double cx, cy;
    centroid(e, e->swipe_mask, &cx, &cy);

    double dx = cx - e->sw_lx;
    double dy = cy - e->sw_ly;

    e->sw_lx = cx;
    e->sw_ly = cy;

    if (e->swipe_axis == LOCK_H)
        dy = 0.0;
    else
        dx = 0.0;

    e->acc_x += dx;
    e->acc_y += dy;

    if (c->swipe2 == MM_SWIPE2_NATIVE) {
        if (!vtp_busy(e)) {
            vtp_move(e, dx * c->swipe_speed, dy * c->swipe_speed, now);
            mot_push(e, now, dx * c->swipe_speed, dy * c->swipe_speed);
        }
        return;
    }

    if (e->fired)
        return;

    if (fabs(e->acc_x) >= KEY_SWIPE2_MM && fabs(e->acc_x) > 2.0 * fabs(e->acc_y)) {
        e->fired = 1;
        run_keys(e, e->acc_x < 0 ? &c->workspace_right_keys : &c->workspace_left_keys);
    } else if (fabs(e->acc_y) >= KEY_SWIPE2_MM && fabs(e->acc_y) > 2.0 * fabs(e->acc_x)) {
        e->fired = 1;
        run_keys(e, e->acc_y < 0 ? &c->swipe_up_keys : &c->swipe_down_keys);
    }
}

/* ---------------------------------------------------------------- */
/* Contact session                                                   */
/* ---------------------------------------------------------------- */

static void session_begin(mm_engine *e, int64_t now)
{
    e->phase = PH_PENDING;
    e->s_start = now;
    e->s_seen = 0;
    e->s_clicked = e->btn_held != 0;
    e->s_interrupted = 0;
    e->s_max_disp = 0.0;
    e->s_pointer_travel = 0;
    e->primary = -1;
    e->swipe_mask = 0;
    e->lock = LOCK_NONE;
    e->fired = 0;

    e->s_x = e->s_y = 0.0;

    if (e->mom_active && e->mom_glide) {
        momentum_stop(e, now);
    } else if (e->mom_active) {
        e->mom_active = 0;
        e->mom_carry_vx = e->mom_vx;
        e->mom_carry_vy = e->mom_vy;
        e->mom_carry_t = now;
        e->s_interrupted = 1;
    }

    if (e->cfg->scroll_mode == MM_SCROLL_TOUCHPAD && !e->cfg->momentum)
        vtp_down(e, 2, now);
}

static void pending_update(mm_engine *e, int64_t now)
{
    const mm_config *c = e->cfg;

    int quiet = e->btn_held ||
                (e->t_btn_release && now - e->t_btn_release < POST_CLICK_QUIET_US) ||
                pointer_moving(e, now);

    if (quiet) {
        for (int i = 0; i < MM_MAX_TOUCHES; i++) {
            mm_track *t = &e->tr[i];
            if (is_valid(t)) {
                t->sx = t->x;
                t->sy = t->y;
            }
        }
        return;
    }

    int fa = -1, fb = -1;
    double ea = 0.0, eb = 0.0;

    for (int i = 0; i < MM_MAX_TOUCHES; i++) {
        const mm_track *t = &e->tr[i];
        if (!is_valid(t))
            continue;

        double d = hypot(t->x - t->sx, t->y - t->sy);
        if (fa < 0 || d > ea) {
            fb = fa;
            eb = ea;
            fa = i;
            ea = d;
        } else if (fb < 0 || d > eb) {
            fb = i;
            eb = d;
        }
    }

    if (fb >= 0 && eb >= SWIPE_START_MM && eb >= 0.4 * ea) {
        const mm_track *a = &e->tr[fa];
        const mm_track *b = &e->tr[fb];
        double ax = a->x - a->sx, ay = a->y - a->sy;
        double bx = b->x - b->sx, by = b->y - b->sy;

        if (ax * bx + ay * by > 0.7 * ea * eb) {
            unsigned mask = (1u << fa) | (1u << fb);

            for (int i = 0; i < MM_MAX_TOUCHES; i++) {
                const mm_track *t = &e->tr[i];
                if (!is_valid(t) || i == fa || i == fb)
                    continue;
                double tx = t->x - t->sx, ty = t->y - t->sy;
                double d = hypot(tx, ty);
                if (d >= 0.4 * ea && tx * ax + ty * ay > 0.7 * d * ea)
                    mask |= 1u << i;
            }

            start_swipe(e, mask, fa, now);
            return;
        }
    }

    int best = -1, second = -1, nvalid = 0;
    double d1 = 0.0, d2 = 0.0;

    for (int i = 0; i < MM_MAX_TOUCHES; i++) {
        const mm_track *t = &e->tr[i];
        if (!is_valid(t))
            continue;

        nvalid++;
        double d = start_metric(t);

        if (best < 0 || d > d1) {
            second = best;
            d2 = d1;
            best = i;
            d1 = d;
        } else if (second < 0 || d > d2) {
            second = i;
            d2 = d;
        }
    }

    if (best < 0 || d1 < c->scroll_threshold_mm)
        return;

    if (nvalid == 1) {
        start_scroll(e, best, now);
        return;
    }

    const mm_track *a = &e->tr[best];
    const mm_track *b = &e->tr[second];
    double ax = a->x - a->sx, ay = a->y - a->sy;
    double bx = b->x - b->sx, by = b->y - b->sy;
    double na = hypot(ax, ay), nb = hypot(bx, by);
    int same_dir = (ax * bx + ay * by) > 0.5 * na * nb;

    if (d2 >= 0.3 * d1 && same_dir) {
        unsigned mask = 0;

        for (int i = 0; i < MM_MAX_TOUCHES; i++) {
            const mm_track *t = &e->tr[i];
            if (!is_valid(t))
                continue;

            double tx = t->x - t->sx, ty = t->y - t->sy;

            if (start_metric(t) >= 0.4 * d1 && (tx * ax + ty * ay) > 0.5 * hypot(tx, ty) * na)
                mask |= 1u << i;
        }

        start_swipe(e, mask, best, now);
    } else if (d2 <= 0.25 * d1 || d1 >= 3.0 * c->scroll_threshold_mm) {
        start_scroll(e, best, now);
    }
}

static void evaluate_tap(mm_engine *e, int64_t now)
{
    const mm_config *c = e->cfg;

    int fingers = __builtin_popcount(e->s_seen);
    int ok = now - e->s_start <= MS(c->tap_time_ms) &&
             !e->s_clicked && !e->btn_held && !e->s_interrupted &&
             e->s_max_disp <= c->tap_move_mm &&
             e->s_pointer_travel <= TAP_POINTER_MAX &&
             fingers >= 1 && fingers <= 2;

    if (!ok) {
        e->tap_valid = 0;
        return;
    }

    if (e->tap_valid &&
        e->s_start - e->tap_end <= MS(c->double_tap_ms) &&
        hypot(e->s_x - e->tap_x, e->s_y - e->tap_y) <= DOUBLE_TAP_DIST_MM) {
        int n = fingers > e->tap_fingers ? fingers : e->tap_fingers;

        e->tap_valid = 0;
        mm_debug("double tap, %d finger(s)", n);
        run_action(e, n == 1 ? &c->double_tap1 : &c->double_tap2, now);
        return;
    }

    e->tap_valid = 1;
    e->tap_end = now;
    e->tap_fingers = fingers;
    e->tap_x = e->s_x;
    e->tap_y = e->s_y;
}

static void session_end(mm_engine *e, int64_t now)
{
    int was = e->phase;

    if (was == PH_SCROLL)
        scroll_finish(e, now);
    else if (was == PH_SWIPE)
        swipe_finish(e, now);

    e->phase = PH_IDLE;
    vtp_release(e, now);

    if (was == PH_PENDING)
        evaluate_tap(e, now);
    else
        e->tap_valid = 0;

    e->primary = -1;
    e->swipe_mask = 0;
}

static void handle_touches(mm_engine *e, int64_t now)
{
    int n = valid_count(e);

    if (e->phase == PH_IDLE) {
        if (n == 0)
            return;
        session_begin(e, now);
    }

    if (n == 0) {
        session_end(e, now);
        return;
    }

    for (int i = 0; i < MM_MAX_TOUCHES; i++) {
        const mm_track *t = &e->tr[i];
        if (!is_valid(t))
            continue;

        if (!(e->s_seen & (1u << i))) {
            int k = __builtin_popcount(e->s_seen);
            e->s_x = (e->s_x * k + t->sx) / (k + 1);
            e->s_y = (e->s_y * k + t->sy) / (k + 1);
            e->s_seen |= 1u << i;
        }

        if (t->max_disp > e->s_max_disp)
            e->s_max_disp = t->max_disp;
    }

    switch (e->phase) {
    case PH_PENDING:
        pending_update(e, now);
        break;

    case PH_SCROLL:
        if (!e->tr[e->primary].active) {
            scroll_finish(e, now);
            e->phase = PH_DONE;
        } else {
            scroll_update(e, now);
        }
        break;

    case PH_SWIPE:
        swipe_update(e, now);
        break;

    default:
        break;
    }
}

/* ---------------------------------------------------------------- */
/* Public API                                                        */
/* ---------------------------------------------------------------- */

void mm_engine_init(mm_engine *e, const mm_config *cfg, const mm_output *out)
{
    memset(e, 0, sizeof(*e));
    e->cfg = cfg;
    e->out = *out;
    e->primary = -1;
}

void mm_engine_report(mm_engine *e, const mm_report *r, int64_t now)
{
    if (r->has_touch)
        update_tracks(e, r, now);

    emit_pointer(e, r->dx, r->dy, now);
    handle_buttons(e, r->buttons, now);

    if (r->has_touch)
        handle_touches(e, now);

    mm_engine_tick(e, now);
}

void mm_engine_tick(mm_engine *e, int64_t now)
{
    if (e->vtp_lift_at && now >= e->vtp_lift_at)
        vtp_lift(e);

    if (e->mom_active && now - e->mom_last >= MOM_INTERVAL_US)
        momentum_step(e, now);

    if (vtp_has_residual(e) && now - e->vtp_t_emit >= VTP_FLUSH_US)
        vtp_flush(e, now);

    if (e->anim_pending) {
        if (now - e->anim_pending_since > ANIM_PENDING_MAX_US) {
            e->anim_pending = 0;
            e->zoomed = !e->zoomed;
        } else if (e->vtp_n == 0) {
            anim_start(e, now);
        }
    }

    if (e->anim_active && now >= e->anim_next)
        anim_frame(e, now);
}

int64_t mm_engine_next_deadline(const mm_engine *e)
{
    int64_t d = 0;

#define CONSIDER(t) do { int64_t t_ = (t); if (t_ && (!d || t_ < d)) d = t_; } while (0)

    CONSIDER(e->vtp_lift_at);

    if (vtp_has_residual(e))
        CONSIDER(e->vtp_t_emit + VTP_FLUSH_US);

    if (e->anim_pending)
        CONSIDER(e->vtp_n == 0 ? e->anim_pending_since : e->anim_pending_since + ANIM_PENDING_MAX_US + 1);

    if (e->anim_active)
        CONSIDER(e->anim_next);

    if (e->mom_active)
        CONSIDER(e->mom_last + MOM_INTERVAL_US);

#undef CONSIDER

    return d;
}

void mm_engine_reset(mm_engine *e, int64_t now)
{
    (void)now;

    if (e->btn_held && e->out.button)
        e->out.button(e->out.ctx, e->btn_held, 0);

    if ((e->vtp_n || e->anim_active) && e->out.touchpad)
        e->out.touchpad(e->out.ctx, 0, NULL);

    memset(e->tr, 0, sizeof(e->tr));

    e->btn_held = 0;
    e->t_btn_release = 0;
    e->phase = PH_IDLE;
    e->primary = -1;
    e->swipe_mask = 0;
    e->tap_valid = 0;
    e->vtp_n = 0;
    e->vtp_lift_at = 0;
    e->vtp_res_x = e->vtp_res_y = 0.0;
    e->anim_active = 0;
    e->anim_pending = 0;
    e->mom_active = 0;
    e->mom_glide = 0;
    e->mom_carry_t = 0;
    e->mot_n = 0;
    e->wh_acc_v = e->wh_acc_h = 0.0;
    e->p_mv_t = 0;
    e->p_recent = 0.0;
}
