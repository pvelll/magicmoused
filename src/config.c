/*
 * Configuration: defaults and a simple "key = value" file parser.
 */
#include "mm.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ---------------------------------------------------------------- */
/* Logging                                                           */
/* ---------------------------------------------------------------- */

int mm_verbose = 0;

void mm_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

void mm_debug(const char *fmt, ...)
{
    if (!mm_verbose)
        return;

    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

/* ---------------------------------------------------------------- */
/* Key names                                                         */
/* ---------------------------------------------------------------- */

typedef struct { const char *name; int code; } key_name;

/* Values from linux/input-event-codes.h. */
static const key_name KEYS[] = {
    { "ESC", 1 }, { "1", 2 }, { "2", 3 }, { "3", 4 }, { "4", 5 }, { "5", 6 },
    { "6", 7 }, { "7", 8 }, { "8", 9 }, { "9", 10 }, { "0", 11 },
    { "MINUS", 12 }, { "EQUAL", 13 }, { "PLUS", 13 }, { "BACKSPACE", 14 }, { "TAB", 15 },
    { "Q", 16 }, { "W", 17 }, { "E", 18 }, { "R", 19 }, { "T", 20 }, { "Y", 21 },
    { "U", 22 }, { "I", 23 }, { "O", 24 }, { "P", 25 },
    { "LEFTBRACE", 26 }, { "RIGHTBRACE", 27 }, { "ENTER", 28 },
    { "CTRL", 29 }, { "LEFTCTRL", 29 },
    { "A", 30 }, { "S", 31 }, { "D", 32 }, { "F", 33 }, { "G", 34 }, { "H", 35 },
    { "J", 36 }, { "K", 37 }, { "L", 38 }, { "SEMICOLON", 39 }, { "APOSTROPHE", 40 },
    { "GRAVE", 41 }, { "SHIFT", 42 }, { "LEFTSHIFT", 42 }, { "BACKSLASH", 43 },
    { "Z", 44 }, { "X", 45 }, { "C", 46 }, { "V", 47 }, { "B", 48 }, { "N", 49 },
    { "M", 50 }, { "COMMA", 51 }, { "DOT", 52 }, { "SLASH", 53 },
    { "RIGHTSHIFT", 54 }, { "ALT", 56 }, { "LEFTALT", 56 }, { "SPACE", 57 },
    { "F1", 59 }, { "F2", 60 }, { "F3", 61 }, { "F4", 62 }, { "F5", 63 }, { "F6", 64 },
    { "F7", 65 }, { "F8", 66 }, { "F9", 67 }, { "F10", 68 }, { "F11", 87 }, { "F12", 88 },
    { "KPMINUS", 74 }, { "KPPLUS", 78 },
    { "RIGHTCTRL", 97 }, { "RIGHTALT", 100 }, { "ALTGR", 100 },
    { "HOME", 102 }, { "UP", 103 }, { "PAGEUP", 104 }, { "LEFT", 105 }, { "RIGHT", 106 },
    { "END", 107 }, { "DOWN", 108 }, { "PAGEDOWN", 109 }, { "INSERT", 110 }, { "DELETE", 111 },
    { "SUPER", 125 }, { "META", 125 }, { "LEFTMETA", 125 }, { "WIN", 125 },
    { "RIGHTMETA", 126 }, { "RIGHTSUPER", 126 },
    { "BACK", 158 }, { "FORWARD", 159 },
    { "ZOOMIN", 418 }, { "ZOOMOUT", 419 }, { "ZOOMRESET", 420 },
};

static int key_code(const char *name)
{
    for (size_t i = 0; i < sizeof(KEYS) / sizeof(KEYS[0]); i++)
        if (strcasecmp(KEYS[i].name, name) == 0)
            return KEYS[i].code;

    /* "KEY_LEFTMETA" style */
    if (strncasecmp(name, "KEY_", 4) == 0)
        return key_code(name + 4);

    /* raw numeric code */
    char *end = NULL;
    long v = strtol(name, &end, 0);
    if (end && *end == '\0' && v > 0 && v < 0x2ff && strlen(name) > 1)
        return (int)v;

    return -1;
}

static const char *key_name_of(int code)
{
    for (size_t i = 0; i < sizeof(KEYS) / sizeof(KEYS[0]); i++)
        if (KEYS[i].code == code)
            return KEYS[i].name;
    return "?";
}

/* "none", "smart-zoom" or a combo such as "CTRL+ALT+LEFT". */
int mm_parse_action(const char *text, mm_action *act)
{
    memset(act, 0, sizeof(*act));

    if (strcasecmp(text, "none") == 0 || strcasecmp(text, "off") == 0 || *text == '\0') {
        act->type = MM_ACT_NONE;
        return 0;
    }

    if (strcasecmp(text, "smart-zoom") == 0 || strcasecmp(text, "zoom") == 0) {
        act->type = MM_ACT_SMART_ZOOM;
        return 0;
    }

    const char *p = text;
    if (strncasecmp(p, "keys:", 5) == 0)
        p += 5;

    char buf[128];
    snprintf(buf, sizeof(buf), "%s", p);

    act->type = MM_ACT_KEYS;

    char *save = NULL;
    for (char *tok = strtok_r(buf, "+ ", &save); tok; tok = strtok_r(NULL, "+ ", &save)) {
        int code = key_code(tok);
        if (code < 0 || act->n >= MM_MAX_COMBO) {
            act->type = MM_ACT_NONE;
            act->n = 0;
            return -1;
        }
        act->codes[act->n++] = code;
    }

    if (act->n == 0) {
        act->type = MM_ACT_NONE;
        return -1;
    }

    return 0;
}

static void action_text(const mm_action *a, char *buf, size_t size)
{
    if (a->type == MM_ACT_NONE) {
        snprintf(buf, size, "none");
        return;
    }
    if (a->type == MM_ACT_SMART_ZOOM) {
        snprintf(buf, size, "smart-zoom");
        return;
    }

    buf[0] = '\0';
    for (int i = 0; i < a->n; i++) {
        size_t len = strlen(buf);
        snprintf(buf + len, size - len, "%s%s", i ? "+" : "", key_name_of(a->codes[i]));
    }
}

/* ---------------------------------------------------------------- */
/* Defaults                                                          */
/* ---------------------------------------------------------------- */

void mm_config_defaults(mm_config *c)
{
    memset(c, 0, sizeof(*c));

    c->units_per_mm_x = 45.77;
    c->units_per_mm_y = 40.15;
    c->edge_zone = 0.05;
    c->palm_zone = 0.12;

    c->scroll_mode = MM_SCROLL_TOUCHPAD;
    c->scroll_speed = 1.0;
    c->scroll_accel = 1.0;
    c->scroll_threshold_mm = 1.5;
    c->axis_lock = 1;
    c->natural_scroll = 1;
    c->momentum = 1;
    c->momentum_time_ms = 500.0;

    c->hswipe = MM_HSWIPE_SCROLL;
    c->swipe2 = MM_SWIPE2_NATIVE;
    c->swipe2_vertical = MM_V2_OVERVIEW;
    c->swipe_speed = 1.6;
    mm_parse_action("ALT+LEFT", &c->back_keys);
    mm_parse_action("ALT+RIGHT", &c->forward_keys);
    mm_parse_action("SUPER+PAGEUP", &c->workspace_left_keys);
    mm_parse_action("SUPER+PAGEDOWN", &c->workspace_right_keys);
    mm_parse_action("SUPER", &c->swipe_up_keys);
    mm_parse_action("none", &c->swipe_down_keys);

    c->tap_time_ms = 350;
    c->double_tap_ms = 450;
    c->tap_move_mm = 3.0;
    mm_parse_action("smart-zoom", &c->double_tap1);
    mm_parse_action("SUPER", &c->double_tap2);
    c->smart_zoom = MM_ZOOM_PINCH;
    mm_parse_action("CTRL+EQUAL", &c->zoom_in_keys);
    mm_parse_action("CTRL+0", &c->zoom_out_keys);

    c->secondary_click = MM_SECONDARY_RIGHT;
    c->click_split = 0.5;
    c->middle_click = 0;

    c->grab = 1;
    c->verbose = 0;
}

/* ---------------------------------------------------------------- */
/* Parser                                                            */
/* ---------------------------------------------------------------- */

typedef struct { const char *name; int value; } enum_name;

static const enum_name E_SCROLL[] = { { "touchpad", MM_SCROLL_TOUCHPAD }, { "wheel", MM_SCROLL_WHEEL }, { 0, 0 } };
static const enum_name E_HSWIPE[] = { { "scroll", MM_HSWIPE_SCROLL }, { "native", MM_HSWIPE_SCROLL },
                                      { "keys", MM_HSWIPE_KEYS }, { 0, 0 } };
static const enum_name E_SWIPE2[] = { { "native", MM_SWIPE2_NATIVE }, { "keys", MM_SWIPE2_KEYS },
                                      { "off", MM_SWIPE2_OFF }, { "none", MM_SWIPE2_OFF }, { 0, 0 } };
static const enum_name E_V2[] = { { "overview", MM_V2_OVERVIEW }, { "native", MM_V2_OVERVIEW },
                                  { "scroll", MM_V2_SCROLL }, { "off", MM_V2_OFF }, { "none", MM_V2_OFF }, { 0, 0 } };
static const enum_name E_ZOOM[] = { { "pinch", MM_ZOOM_PINCH }, { "keys", MM_ZOOM_KEYS }, { 0, 0 } };
static const enum_name E_SECONDARY[] = { { "right", MM_SECONDARY_RIGHT }, { "left", MM_SECONDARY_LEFT },
                                         { "off", MM_SECONDARY_OFF }, { "none", MM_SECONDARY_OFF }, { 0, 0 } };

static int parse_enum(const enum_name *e, const char *v, int *out)
{
    for (; e->name; e++) {
        if (strcasecmp(e->name, v) == 0) {
            *out = e->value;
            return 0;
        }
    }
    return -1;
}

static const char *enum_text(const enum_name *e, int value)
{
    for (; e->name; e++)
        if (e->value == value)
            return e->name;
    return "?";
}

static int parse_bool(const char *v, int *out)
{
    if (!strcasecmp(v, "1") || !strcasecmp(v, "yes") || !strcasecmp(v, "true") || !strcasecmp(v, "on")) {
        *out = 1;
        return 0;
    }
    if (!strcasecmp(v, "0") || !strcasecmp(v, "no") || !strcasecmp(v, "false") || !strcasecmp(v, "off")) {
        *out = 0;
        return 0;
    }
    return -1;
}

static int parse_double(const char *v, double *out, double min, double max)
{
    char *end = NULL;
    errno = 0;
    double d = strtod(v, &end);
    if (errno || !end || *end != '\0' || d < min || d > max)
        return -1;
    *out = d;
    return 0;
}

static int parse_int(const char *v, int *out, int min, int max)
{
    double d;
    if (parse_double(v, &d, min, max) < 0)
        return -1;
    *out = (int)d;
    return 0;
}

static char *trim(char *s)
{
    while (isspace((unsigned char)*s))
        s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1]))
        *--end = '\0';
    return s;
}

static int apply(mm_config *c, const char *k, const char *v)
{
#define D(name, field, lo, hi) if (!strcasecmp(k, name)) return parse_double(v, &c->field, lo, hi)
#define I(name, field, lo, hi) if (!strcasecmp(k, name)) return parse_int(v, &c->field, lo, hi)
#define B(name, field)         if (!strcasecmp(k, name)) return parse_bool(v, &c->field)
#define E(name, field, table)  if (!strcasecmp(k, name)) return parse_enum(table, v, &c->field)
#define A(name, field)         if (!strcasecmp(k, name)) { \
                                   mm_action a_; \
                                   if (mm_parse_action(v, &a_) < 0) return -1; \
                                   c->field = a_; \
                                   return 0; \
                               }

    D("units_per_mm_x", units_per_mm_x, 1, 1000);
    D("units_per_mm_y", units_per_mm_y, 1, 1000);
    B("invert_x", invert_x);
    B("invert_y", invert_y);
    D("edge_zone", edge_zone, 0, 0.3);
    D("palm_zone", palm_zone, 0, 0.6);

    E("scroll_mode", scroll_mode, E_SCROLL);
    D("scroll_speed", scroll_speed, 0.05, 10);
    D("scroll_accel", scroll_accel, 0, 5);
    D("scroll_threshold_mm", scroll_threshold_mm, 0.1, 10);
    B("axis_lock", axis_lock);
    B("natural_scroll", natural_scroll);
    B("momentum", momentum);
    D("momentum_time_ms", momentum_time_ms, 50, 3000);

    E("horizontal_swipe", hswipe, E_HSWIPE);
    E("two_finger_swipe", swipe2, E_SWIPE2);
    E("two_finger_vertical", swipe2_vertical, E_V2);
    D("swipe_speed", swipe_speed, 0.1, 10);
    A("back_keys", back_keys);
    A("forward_keys", forward_keys);
    A("workspace_left_keys", workspace_left_keys);
    A("workspace_right_keys", workspace_right_keys);
    A("swipe_up_keys", swipe_up_keys);
    A("swipe_down_keys", swipe_down_keys);

    I("tap_time_ms", tap_time_ms, 50, 1000);
    I("double_tap_ms", double_tap_ms, 50, 1500);
    D("tap_move_mm", tap_move_mm, 0.1, 10);
    A("one_finger_double_tap", double_tap1);
    A("two_finger_double_tap", double_tap2);
    E("smart_zoom", smart_zoom, E_ZOOM);
    A("zoom_in_keys", zoom_in_keys);
    A("zoom_out_keys", zoom_out_keys);

    E("secondary_click", secondary_click, E_SECONDARY);
    D("click_split", click_split, 0.1, 0.9);
    B("middle_click", middle_click);

    B("grab", grab);
    B("verbose", verbose);
    
    static const char *const retired[] = {
        "pointer_accel", "pointer_speed", "accel_min", "accel_max", "accel_half",
    };
    for (size_t i = 0; i < sizeof(retired) / sizeof(retired[0]); i++)
        if (!strcasecmp(k, retired[i]))
            return 0;

#undef D
#undef I
#undef B
#undef E
#undef A

    return -2;
}

int mm_config_load(mm_config *c, const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;

    char line[512];
    int lineno = 0;
    int bad = 0;

    while (fgets(line, sizeof(line), f)) {
        lineno++;

        char *hash = strchr(line, '#');
        if (hash)
            *hash = '\0';

        char *s = trim(line);
        if (*s == '\0')
            continue;

        char *eq = strchr(s, '=');
        if (!eq) {
            mm_log("%s:%d: expected 'key = value'", path, lineno);
            bad++;
            continue;
        }

        *eq = '\0';
        char *k = trim(s);
        char *v = trim(eq + 1);

        int r = apply(c, k, v);
        if (r == -2) {
            mm_log("%s:%d: unknown option '%s'", path, lineno, k);
            bad++;
        } else if (r < 0) {
            mm_log("%s:%d: bad value '%s' for '%s'", path, lineno, v, k);
            bad++;
        }
    }

    fclose(f);
    return bad;
}

void mm_config_print(const mm_config *c, void *stream)
{
    FILE *f = stream;
    char a[96];

    fprintf(f, "units_per_mm_x = %.2f\nunits_per_mm_y = %.2f\n", c->units_per_mm_x, c->units_per_mm_y);
    fprintf(f, "invert_x = %d\ninvert_y = %d\n", c->invert_x, c->invert_y);
    fprintf(f, "edge_zone = %.2f\npalm_zone = %.2f\n", c->edge_zone, c->palm_zone);
    fprintf(f, "scroll_mode = %s\n", enum_text(E_SCROLL, c->scroll_mode));
    fprintf(f, "scroll_speed = %.2f\nscroll_accel = %.2f\nscroll_threshold_mm = %.2f\n",
            c->scroll_speed, c->scroll_accel, c->scroll_threshold_mm);
    fprintf(f, "axis_lock = %d\nnatural_scroll = %d\nmomentum = %d\nmomentum_time_ms = %.0f\n",
            c->axis_lock, c->natural_scroll, c->momentum, c->momentum_time_ms);
    fprintf(f, "horizontal_swipe = %s\n", enum_text(E_HSWIPE, c->hswipe));
    fprintf(f, "two_finger_swipe = %s\n", enum_text(E_SWIPE2, c->swipe2));
    fprintf(f, "two_finger_vertical = %s\n", enum_text(E_V2, c->swipe2_vertical));
    fprintf(f, "swipe_speed = %.2f\n", c->swipe_speed);
    action_text(&c->back_keys, a, sizeof(a));            fprintf(f, "back_keys = %s\n", a);
    action_text(&c->forward_keys, a, sizeof(a));         fprintf(f, "forward_keys = %s\n", a);
    action_text(&c->workspace_left_keys, a, sizeof(a));  fprintf(f, "workspace_left_keys = %s\n", a);
    action_text(&c->workspace_right_keys, a, sizeof(a)); fprintf(f, "workspace_right_keys = %s\n", a);
    action_text(&c->swipe_up_keys, a, sizeof(a));        fprintf(f, "swipe_up_keys = %s\n", a);
    action_text(&c->swipe_down_keys, a, sizeof(a));      fprintf(f, "swipe_down_keys = %s\n", a);
    fprintf(f, "tap_time_ms = %d\ndouble_tap_ms = %d\ntap_move_mm = %.2f\n",
            c->tap_time_ms, c->double_tap_ms, c->tap_move_mm);
    action_text(&c->double_tap1, a, sizeof(a));          fprintf(f, "one_finger_double_tap = %s\n", a);
    action_text(&c->double_tap2, a, sizeof(a));          fprintf(f, "two_finger_double_tap = %s\n", a);
    fprintf(f, "smart_zoom = %s\n", enum_text(E_ZOOM, c->smart_zoom));
    action_text(&c->zoom_in_keys, a, sizeof(a));         fprintf(f, "zoom_in_keys = %s\n", a);
    action_text(&c->zoom_out_keys, a, sizeof(a));        fprintf(f, "zoom_out_keys = %s\n", a);
    fprintf(f, "secondary_click = %s\n", enum_text(E_SECONDARY, c->secondary_click));
    fprintf(f, "click_split = %.2f\nmiddle_click = %d\n", c->click_split, c->middle_click);
    fprintf(f, "grab = %d\n", c->grab);
}
