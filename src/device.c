/*
 * Finding the Magic Mouse, enabling multitouch reports and grabbing the
 * kernel input device.
 *
 * sysfs layout used:
 *
 *   /sys/class/hidraw/hidrawN/device            -> the HID device
 *   /sys/class/hidraw/hidrawN/device/uevent     -> HID_ID=0005:0000004C:00000269
 *   /sys/class/hidraw/hidrawN/device/input/inputX/eventY -> its evdev nodes
 */
#define _GNU_SOURCE

#include "device.h"
#include "mm.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <linux/hidraw.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

static int is_magic_mouse(int vendor, int product)
{
    return (vendor == MM_VENDOR_BT || vendor == MM_VENDOR_USB) &&
           (product == MM_PRODUCT_MM2 || product == MM_PRODUCT_MM2_USBC);
}

static void sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static int fmt(char *dst, size_t size, const char *format, ...) __attribute__((format(printf, 3, 4)));

static int fmt(char *dst, size_t size, const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    int n = vsnprintf(dst, size, format, ap);
    va_end(ap);
    return (n < 0 || (size_t)n >= size) ? -1 : 0;
}

static int read_uevent(const char *sysname, int *bus, int *vendor, int *product, char *name, size_t nsize)
{
    char path[512];
    if (fmt(path, sizeof(path), "/sys/class/hidraw/%s/device/uevent", sysname) < 0)
        return -1;

    FILE *f = fopen(path, "r");
    if (!f)
        return -1;

    char line[256];
    int found = 0;

    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = '\0';

        if (strncmp(line, "HID_ID=", 7) == 0) {
            unsigned b, v, p;
            if (sscanf(line + 7, "%x:%x:%x", &b, &v, &p) == 3) {
                *bus = (int)b;
                *vendor = (int)v;
                *product = (int)p;
                found = 1;
            }
        } else if (strncmp(line, "HID_NAME=", 9) == 0 && name) {
            fmt(name, nsize, "%s", line + 9);
        }
    }

    fclose(f);
    return found ? 0 : -1;
}

static int enable_multitouch(int fd)
{
    for (int attempt = 0; attempt < 5; attempt++) {
        unsigned char feature[] = { 0xF1, 0x02, 0x01 };

        if (ioctl(fd, HIDIOCSFEATURE(sizeof(feature)), feature) >= 0)
            return 0;

        sleep_ms(200);
    }

    return -1;
}

static void grab_inputs(mm_device *d, const char *sysname)
{
    char path[512];
    if (fmt(path, sizeof(path), "/sys/class/hidraw/%s/device/input", sysname) < 0)
        return;

    DIR *inputs = opendir(path);
    if (!inputs)
        return;

    struct dirent *in;
    while ((in = readdir(inputs)) != NULL && d->nev < MM_MAX_GRABS) {
        if (strncmp(in->d_name, "input", 5) != 0)
            continue;

        char sub[1024];
        if (fmt(sub, sizeof(sub), "%s/%s", path, in->d_name) < 0)
            continue;

        DIR *events = opendir(sub);
        if (!events)
            continue;

        struct dirent *ev;
        while ((ev = readdir(events)) != NULL && d->nev < MM_MAX_GRABS) {
            if (strncmp(ev->d_name, "event", 5) != 0)
                continue;

            char node[512];
            if (fmt(node, sizeof(node), "/dev/input/%s", ev->d_name) < 0)
                continue;

            int fd = open(node, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (fd < 0) {
                mm_log("warning: cannot open %s: %s", node, strerror(errno));
                continue;
            }

            if (ioctl(fd, EVIOCGRAB, (void *)1) < 0) {
                if (errno == EBUSY)
                    mm_log("warning: %s is already grabbed by another program "
                           "(another magicmoused?); the kernel's scrolling stays active", node);
                else
                    mm_log("warning: cannot grab %s: %s", node, strerror(errno));
                close(fd);
                continue;
            }

            mm_debug("grabbed %s", node);
            d->ev_fd[d->nev++] = fd;
        }

        closedir(events);
    }

    closedir(inputs);
}

static int open_candidate(mm_device *d, const char *sysname, int *mt_ok)
{
    char node[64];
    if (fmt(node, sizeof(node), "/dev/%s", sysname) < 0)
        return -1;

    int fd = open(node, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        mm_log("cannot open %s: %s", node, strerror(errno));
        return -1;
    }

    struct hidraw_devinfo info;
    memset(&info, 0, sizeof(info));

    if (ioctl(fd, HIDIOCGRAWINFO, &info) == 0) {
        d->bus = info.bustype;
        d->vendor = (unsigned short)info.vendor;
        d->product = (unsigned short)info.product;
    }

    if (!d->name[0])
        ioctl(fd, HIDIOCGRAWNAME(sizeof(d->name)), d->name);

    *mt_ok = enable_multitouch(fd) == 0;
    d->multitouch = *mt_ok;
    memcpy(d->hidraw, node, sizeof(d->hidraw));

    return fd;
}

int mm_device_open(mm_device *d, const char *path, int grab)
{
    memset(d, 0, sizeof(*d));
    d->hid_fd = -1;

    char chosen[64] = "";

    if (path) {
        const char *slash = strrchr(path, '/');
        if (fmt(chosen, sizeof(chosen), "%s", slash ? slash + 1 : path) < 0)
            return -1;
        read_uevent(chosen, &d->bus, &d->vendor, &d->product, d->name, sizeof(d->name));

        int mt_ok = 0;
        d->hid_fd = open_candidate(d, chosen, &mt_ok);
        if (d->hid_fd < 0)
            return -1;
    } else {
        DIR *dir = opendir("/sys/class/hidraw");
        if (!dir)
            return -1;

        struct dirent *de;
        while ((de = readdir(dir)) != NULL) {
            if (strncmp(de->d_name, "hidraw", 6) != 0)
                continue;

            int bus = 0, vendor = 0, product = 0;
            char name[128] = "";

            if (read_uevent(de->d_name, &bus, &vendor, &product, name, sizeof(name)) < 0)
                continue;
            if (!is_magic_mouse(vendor, product))
                continue;

            mm_device tmp;
            memset(&tmp, 0, sizeof(tmp));
            memcpy(tmp.name, name, sizeof(tmp.name));

            int mt_ok = 0;
            int fd = open_candidate(&tmp, de->d_name, &mt_ok);
            if (fd < 0)
                continue;

            /* Over USB the mouse has several HID interfaces; prefer the one
             * that accepts the multitouch feature report. */
            if (d->hid_fd < 0 || mt_ok) {
                if (d->hid_fd >= 0)
                    close(d->hid_fd);
                tmp.hid_fd = fd;
                *d = tmp;
                fmt(chosen, sizeof(chosen), "%s", de->d_name);
                if (mt_ok)
                    break;
            } else {
                close(fd);
            }
        }

        closedir(dir);

        if (d->hid_fd < 0)
            return -1;
    }

    memcpy(d->sysname, chosen, sizeof(d->sysname));

    if (grab)
        mm_device_grab(d);

    return 0;
}

int mm_device_grab(mm_device *d)
{
    if (d->nev == 0)
        grab_inputs(d, d->sysname);
    return d->nev;
}

void mm_device_close(mm_device *d)
{
    for (int i = 0; i < d->nev; i++) {
        ioctl(d->ev_fd[i], EVIOCGRAB, (void *)0);
        close(d->ev_fd[i]);
    }
    d->nev = 0;

    if (d->hid_fd >= 0)
        close(d->hid_fd);
    d->hid_fd = -1;
}
