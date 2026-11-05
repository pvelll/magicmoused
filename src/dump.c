/*
 * magicmouse-dump — print decoded Magic Mouse 2 reports.
 *
 * Useful to check the surface orientation and zones before tuning
 * magicmouse.conf:
 *
 *   sudo magicmouse-dump            # search for the mouse
 *   sudo magicmouse-dump /dev/hidraw3
 *   sudo magicmouse-dump -r         # also print raw bytes
 *
 * Does not grab the mouse: it keeps working normally while dumping.
 * Stop magicmoused first if it is running, otherwise both read the same
 * reports (which is harmless, but confusing).
 */
#define _GNU_SOURCE

#include "device.h"
#include "mm.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const char *state_name(int s)
{
    switch (s) {
    case 0x00: return "none";
    case 0x10: return "in-range";
    case 0x20: return "hover";
    case 0x30: return "start";
    case 0x40: return "drag";
    case 0x50: return "break";
    case 0x60: return "linger";
    case 0x70: return "out";
    default:   return "?";
    }
}

int main(int argc, char **argv)
{
    int raw = 0;
    const char *path = NULL;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-r"))
            raw = 1;
        else if (!strcmp(argv[i], "-h")) {
            fprintf(stderr, "Usage: %s [-r] [/dev/hidrawN]\n", argv[0]);
            return 0;
        } else
            path = argv[i];
    }

    mm_config cfg;
    mm_config_defaults(&cfg);
    mm_config_load(&cfg, "/etc/magicmouse/magicmouse.conf");

    mm_device dev;
    if (mm_device_open(&dev, path, 0) < 0) {
        fprintf(stderr, "Magic Mouse 2 not found (are you root? is it connected?)\n");
        return 1;
    }

    printf("%s (%04x:%04x) on %s, multitouch %s\n", dev.name, dev.vendor, dev.product,
           dev.hidraw, dev.multitouch ? "on" : "NOT confirmed");
    printf("x: mm to the right of centre, y: mm towards the palm; xn/yn: 0..1 (yn 0 = front)\n");
    printf("zones: edge < %.2f or > %.2f, palm yn > %.2f; right click when xn > %.2f\n\n",
           cfg.edge_zone, 1.0 - cfg.edge_zone, 1.0 - cfg.palm_zone, cfg.click_split);

    uint8_t buf[256];

    for (;;) {
        ssize_t n = read(dev.hid_fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            perror("read");
            break;
        }

        if (raw) {
            printf("raw[%zd]:", n);
            for (ssize_t i = 0; i < n; i++)
                printf(" %02x", buf[i]);
            printf("\n");
        }

        mm_report r;
        if (!mm_parse_report(buf, (size_t)n, &cfg, &r)) {
            printf("report 0x%02x, %zd bytes (ignored)\n", n > 0 ? buf[0] : 0, n);
            continue;
        }

        printf("btn=%d%d move=%+4d,%+4d", r.buttons & 1, (r.buttons >> 1) & 1, r.dx, r.dy);

        if (!r.has_touch)
            printf("  (no touch data)");

        for (int i = 0; i < r.ntouch; i++) {
            const mm_touch *t = &r.touch[i];
            const char *zone = "";

            if (t->xn < cfg.edge_zone || t->xn > 1.0 - cfg.edge_zone)
                zone = " EDGE";
            else if (t->yn > 1.0 - cfg.palm_zone)
                zone = " PALM";

            printf("\n   #%d %-6s x=%6.1f y=%6.1f  xn=%.2f yn=%.2f  size=%2d maj=%3d min=%3d%s",
                   t->id, state_name(t->state), t->xmm, t->ymm, t->xn, t->yn,
                   t->size, t->major, t->minor, zone);
        }

        printf("\n");
        fflush(stdout);
    }

    mm_device_close(&dev);
    return 0;
}
