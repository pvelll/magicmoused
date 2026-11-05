/*
 * Virtual input devices (uinput): mouse, touchpad and keyboard.
 *
 * The devices are created once at startup and survive Bluetooth
 * reconnects of the real mouse, so the compositor never sees them flap.
 *
 * Every frame (all events up to and including SYN_REPORT) is written
 * with a single write() call.
 */
#include "vinput.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

/* pid.codes test VID; products are local to this daemon. */
#define VIRT_VENDOR        0x1209
#define VIRT_PRODUCT_MOUSE 0x4d01
#define VIRT_PRODUCT_TP    0x4d02
#define VIRT_PRODUCT_KBD   0x4d03

#define FRAME_MAX 48

typedef struct {
    struct input_event ev[FRAME_MAX];
    int n;
} frame;

static void put(frame *f, int type, int code, int value)
{
    if (f->n >= FRAME_MAX - 1)      /* keep room for SYN_REPORT */
        return;

    struct input_event *ev = &f->ev[f->n++];
    memset(ev, 0, sizeof(*ev));
    ev->type = (unsigned short)type;
    ev->code = (unsigned short)code;
    ev->value = value;
}

static void send_frame(int fd, frame *f)
{
    struct input_event *ev = &f->ev[f->n++];
    memset(ev, 0, sizeof(*ev));
    ev->type = EV_SYN;
    ev->code = SYN_REPORT;

    size_t len = (size_t)f->n * sizeof(struct input_event);
    ssize_t r;

    do {
        r = write(fd, f->ev, len);
    } while (r < 0 && errno == EINTR);

    if (r != (ssize_t)len)
        mm_debug("uinput write failed: %s", r < 0 ? strerror(errno) : "short write");

    f->n = 0;
}

static int open_uinput(void)
{
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);

    if (fd < 0)
        fd = open("/dev/input/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);

    return fd;
}

static int finish_device(int fd, const char *name, int bus, int product)
{
    struct uinput_setup us;

    memset(&us, 0, sizeof(us));
    snprintf(us.name, sizeof(us.name), "%s", name);
    us.id.bustype = (unsigned short)bus;
    us.id.vendor = VIRT_VENDOR;
    us.id.product = (unsigned short)product;
    us.id.version = 1;

    if (ioctl(fd, UI_DEV_SETUP, &us) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) {
        mm_log("uinput: cannot create '%s': %s", name, strerror(errno));
        close(fd);
        return -1;
    }

    return fd;
}

static void set_abs(int fd, int code, int max, int res)
{
    struct uinput_abs_setup a;

    memset(&a, 0, sizeof(a));
    a.code = (unsigned short)code;
    a.absinfo.minimum = 0;
    a.absinfo.maximum = max;
    a.absinfo.resolution = res;

    ioctl(fd, UI_SET_ABSBIT, code);
    ioctl(fd, UI_ABS_SETUP, &a);
}

static int create_mouse(void)
{
    int fd = open_uinput();
    if (fd < 0)
        return -1;

    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_KEYBIT, BTN_LEFT);
    ioctl(fd, UI_SET_KEYBIT, BTN_RIGHT);
    ioctl(fd, UI_SET_KEYBIT, BTN_MIDDLE);

    ioctl(fd, UI_SET_EVBIT, EV_REL);
    ioctl(fd, UI_SET_RELBIT, REL_X);
    ioctl(fd, UI_SET_RELBIT, REL_Y);
    ioctl(fd, UI_SET_RELBIT, REL_WHEEL);
    ioctl(fd, UI_SET_RELBIT, REL_HWHEEL);
    ioctl(fd, UI_SET_RELBIT, REL_WHEEL_HI_RES);
    ioctl(fd, UI_SET_RELBIT, REL_HWHEEL_HI_RES);

    return finish_device(fd, "Magic Mouse 2 (magicmoused)", BUS_USB, VIRT_PRODUCT_MOUSE);
}

static int create_touchpad(void)
{
    int fd = open_uinput();
    if (fd < 0)
        return -1;

    int w = (int)(MM_VTP_WIDTH_MM * MM_VTP_RES);
    int h = (int)(MM_VTP_HEIGHT_MM * MM_VTP_RES);

    ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_POINTER);
    ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_BUTTONPAD);

    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_KEYBIT, BTN_LEFT);
    ioctl(fd, UI_SET_KEYBIT, BTN_TOUCH);
    ioctl(fd, UI_SET_KEYBIT, BTN_TOOL_FINGER);
    ioctl(fd, UI_SET_KEYBIT, BTN_TOOL_DOUBLETAP);
    ioctl(fd, UI_SET_KEYBIT, BTN_TOOL_TRIPLETAP);
    ioctl(fd, UI_SET_KEYBIT, BTN_TOOL_QUADTAP);

    ioctl(fd, UI_SET_EVBIT, EV_ABS);
    set_abs(fd, ABS_X, w, MM_VTP_RES);
    set_abs(fd, ABS_Y, h, MM_VTP_RES);
    set_abs(fd, ABS_MT_SLOT, MM_VTP_SLOTS - 1, 0);
    set_abs(fd, ABS_MT_POSITION_X, w, MM_VTP_RES);
    set_abs(fd, ABS_MT_POSITION_Y, h, MM_VTP_RES);
    set_abs(fd, ABS_MT_TRACKING_ID, 65535, 0);

    /* Bluetooth bus => libinput tags it as an external touchpad: it is never
     * paired with the keyboard for disable-while-typing and is not switched
     * off by "disable touchpad when a mouse is connected". */
    return finish_device(fd, "Magic Mouse 2 Surface (magicmoused)", BUS_BLUETOOTH, VIRT_PRODUCT_TP);
}

static int create_keyboard(void)
{
    int fd = open_uinput();
    if (fd < 0)
        return -1;

    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    for (int k = 1; k < 249; k++)
        ioctl(fd, UI_SET_KEYBIT, k);
    ioctl(fd, UI_SET_KEYBIT, KEY_ZOOMIN);
    ioctl(fd, UI_SET_KEYBIT, KEY_ZOOMOUT);
    ioctl(fd, UI_SET_KEYBIT, KEY_ZOOMRESET);

    return finish_device(fd, "Magic Mouse 2 Shortcuts (magicmoused)", BUS_USB, VIRT_PRODUCT_KBD);
}

/* ---------------------------------------------------------------- */
/* Output callbacks                                                  */
/* ---------------------------------------------------------------- */

static void out_pointer(void *ctx, int dx, int dy)
{
    mm_vinput *v = ctx;
    frame f = { .n = 0 };

    if (dx)
        put(&f, EV_REL, REL_X, dx);
    if (dy)
        put(&f, EV_REL, REL_Y, dy);
    send_frame(v->mouse_fd, &f);
}

static void out_button(void *ctx, int code, int down)
{
    mm_vinput *v = ctx;
    frame f = { .n = 0 };

    put(&f, EV_KEY, code, down ? 1 : 0);
    send_frame(v->mouse_fd, &f);
}

static void out_wheel(void *ctx, int v120, int h120)
{
    mm_vinput *v = ctx;
    frame f = { .n = 0 };

    if (v120) {
        put(&f, EV_REL, REL_WHEEL_HI_RES, v120);
        v->wheel_acc += v120;
        int notches = v->wheel_acc / 120;
        if (notches) {
            put(&f, EV_REL, REL_WHEEL, notches);
            v->wheel_acc -= notches * 120;
        }
    }

    if (h120) {
        put(&f, EV_REL, REL_HWHEEL_HI_RES, h120);
        v->hwheel_acc += h120;
        int notches = v->hwheel_acc / 120;
        if (notches) {
            put(&f, EV_REL, REL_HWHEEL, notches);
            v->hwheel_acc -= notches * 120;
        }
    }

    send_frame(v->mouse_fd, &f);
}

static int to_units(double mm, double max_mm)
{
    if (mm < 0.0)
        mm = 0.0;
    if (mm > max_mm)
        mm = max_mm;
    return (int)lround(mm * MM_VTP_RES);
}

static void out_touchpad(void *ctx, int n, const mm_point *pts)
{
    mm_vinput *v = ctx;
    frame f = { .n = 0 };

    if (v->tp_fd < 0)
        return;

    if (n > MM_VTP_SLOTS)
        n = MM_VTP_SLOTS;

    for (int s = 0; s < MM_VTP_SLOTS; s++) {
        if (s < n) {
            put(&f, EV_ABS, ABS_MT_SLOT, s);
            if (v->tp_tid[s] < 0) {
                v->tp_tid[s] = v->tp_next_tid;
                v->tp_next_tid = (v->tp_next_tid + 1) & 0xffff;
                put(&f, EV_ABS, ABS_MT_TRACKING_ID, v->tp_tid[s]);
            }
            put(&f, EV_ABS, ABS_MT_POSITION_X, to_units(pts[s].x, MM_VTP_WIDTH_MM));
            put(&f, EV_ABS, ABS_MT_POSITION_Y, to_units(pts[s].y, MM_VTP_HEIGHT_MM));
        } else if (v->tp_tid[s] >= 0) {
            put(&f, EV_ABS, ABS_MT_SLOT, s);
            put(&f, EV_ABS, ABS_MT_TRACKING_ID, -1);
            v->tp_tid[s] = -1;
        }
    }

    if (n > 0) {
        put(&f, EV_ABS, ABS_X, to_units(pts[0].x, MM_VTP_WIDTH_MM));
        put(&f, EV_ABS, ABS_Y, to_units(pts[0].y, MM_VTP_HEIGHT_MM));
    }

    if (n != v->tp_n) {
        put(&f, EV_KEY, BTN_TOUCH, n > 0);
        put(&f, EV_KEY, BTN_TOOL_FINGER, n == 1);
        put(&f, EV_KEY, BTN_TOOL_DOUBLETAP, n == 2);
        put(&f, EV_KEY, BTN_TOOL_TRIPLETAP, n == 3);
        put(&f, EV_KEY, BTN_TOOL_QUADTAP, n >= 4);
        v->tp_n = n;
    }

    send_frame(v->tp_fd, &f);
}

static void pause_ms(int ms)
{
    struct timespec ts = { 0, ms * 1000000L };
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR)
        ;
}

static void out_keys(void *ctx, const int *codes, int n)
{
    mm_vinput *v = ctx;
    frame f = { .n = 0 };

    if (v->kbd_fd < 0 || n <= 0)
        return;

    for (int i = 0; i < n; i++) {
        put(&f, EV_KEY, codes[i], 1);
        send_frame(v->kbd_fd, &f);
        pause_ms(4);
    }

    pause_ms(8);

    for (int i = n - 1; i >= 0; i--) {
        put(&f, EV_KEY, codes[i], 0);
        send_frame(v->kbd_fd, &f);
        pause_ms(4);
    }
}

/* ---------------------------------------------------------------- */
/* Public                                                            */
/* ---------------------------------------------------------------- */

int mm_vinput_create(mm_vinput *v)
{
    memset(v, 0, sizeof(*v));
    v->mouse_fd = v->tp_fd = v->kbd_fd = -1;
    for (int s = 0; s < MM_VTP_SLOTS; s++)
        v->tp_tid[s] = -1;

    v->mouse_fd = create_mouse();
    if (v->mouse_fd < 0) {
        mm_log("cannot open /dev/uinput: %s (run as root, or load the module: modprobe uinput)",
               strerror(errno));
        return -1;
    }

    v->tp_fd = create_touchpad();
    if (v->tp_fd < 0)
        mm_log("warning: no virtual touchpad, scrolling falls back to the wheel");

    v->kbd_fd = create_keyboard();
    if (v->kbd_fd < 0)
        mm_log("warning: no virtual keyboard, keyboard shortcuts are disabled");

    /* Give udev and the compositor a moment to pick the devices up. */
    pause_ms(300);
    return 0;
}

void mm_vinput_output(mm_vinput *v, mm_output *out)
{
    memset(out, 0, sizeof(*out));
    out->ctx = v;
    out->pointer = out_pointer;
    out->button = out_button;
    out->wheel = out_wheel;
    out->touchpad = out_touchpad;
    out->keys = out_keys;
}

void mm_vinput_destroy(mm_vinput *v)
{
    int fds[3] = { v->mouse_fd, v->tp_fd, v->kbd_fd };

    for (int i = 0; i < 3; i++) {
        if (fds[i] >= 0) {
            ioctl(fds[i], UI_DEV_DESTROY);
            close(fds[i]);
        }
    }

    v->mouse_fd = v->tp_fd = v->kbd_fd = -1;
}
