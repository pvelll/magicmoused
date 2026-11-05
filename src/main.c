#define _GNU_SOURCE

#include "device.h"
#include "mm.h"
#include "vinput.h"

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <poll.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC 0
#endif

#define DEFAULT_CONFIG "/etc/magicmouse/magicmouse.conf"

static volatile sig_atomic_t g_stop = 0;
static volatile sig_atomic_t g_dump = 0;
static volatile sig_atomic_t g_reload = 0;

static void on_signal(int sig)
{
    if (sig == SIGHUP)
        g_reload = 1;
    else
        g_stop = 1;
}

static void on_usr1(int sig)
{
    (void)sig;
    g_dump = 1;
}

static int64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static void usage(const char *argv0)
{
    fprintf(stderr,
            "Usage: %s [options]\n"
            "  -c FILE   config file (default " DEFAULT_CONFIG "); SIGHUP re-reads it\n"
            "  -d DEV    use this /dev/hidrawN instead of searching\n"
            "  -n        do not grab the kernel input device (debugging)\n"
            "  -p        print the effective configuration and exit\n"
            "  -v        verbose: log touches, clicks and gestures\n"
            "  -h        this help\n",
            argv0);
}

static int single_instance(void)
{
    static const char name[] = "magicmoused";
    struct sockaddr_un addr;

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path + 1, name, sizeof(name) - 1);

    socklen_t len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + sizeof(name) - 1);

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -2;

    if (bind(fd, (struct sockaddr *)&addr, len) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

typedef struct {
    int64_t window_start;
    int64_t prev;
    int64_t active_us;
    int64_t max_gap;
    int64_t last_log;
    long    n;
    long    total;
    int     slow;
    int     max_queue;
    int     logged;
    double  cpu_start;
} link_stats;

static link_stats g_stats;

static double cpu_seconds(void)
{
    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru) < 0)
        return 0.0;
    return ru.ru_utime.tv_sec + ru.ru_stime.tv_sec + (ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) / 1e6;
}

static void stats_window_reset(link_stats *s, int64_t t)
{
    s->window_start = t;
    s->active_us = 0;
    s->max_gap = 0;
    s->n = 0;
    s->slow = 0;
    s->max_queue = 0;
    s->cpu_start = cpu_seconds();
}

static void stats_print(link_stats *s, int64_t t)
{
    double wall = (t - s->window_start) / 1e6;

    if (!s->window_start || s->n < 20 || s->active_us <= 0 || wall <= 0.0) {
        mm_log("health: not enough mouse activity yet; move the mouse for a few seconds and ask again");
        return;
    }

    double cpu = (cpu_seconds() - s->cpu_start) / wall * 100.0;

    mm_log("health: %.0f reports/s while in use, max gap %lld ms, %d gaps > 25 ms (of %ld), "
           "queued <= %d, driver cpu %.2f%% (last %.0f s)",
           s->n / (s->active_us / 1e6), (long long)(s->max_gap / 1000), s->slow, s->n,
           s->max_queue, cpu, wall);
}

static void stats_add(link_stats *s, int64_t t, int queued)
{
    if (!s->window_start)
        stats_window_reset(s, t);

    if (s->prev) {
        int64_t gap = t - s->prev;

        if (gap < 300000) {         /* longer pauses: the mouse was idle */
            s->n++;
            s->total++;
            s->active_us += gap;
            if (gap > s->max_gap)
                s->max_gap = gap;
            if (gap > 25000)
                s->slow++;
        }
    }

    if (queued > s->max_queue)
        s->max_queue = queued;

    s->prev = t;

    if (t - s->window_start < 10000000)
        return;

    int due = mm_verbose ||
              (!s->logged && s->total >= 1500) ||
              (s->logged && t - s->last_log >= 600000000LL);

    if (due && s->n >= 100) {
        stats_print(s, t);
        s->logged = 1;
        s->last_log = t;
    }

    stats_window_reset(s, t);
}

static void stats_dump_if_asked(void)
{
    if (!g_dump)
        return;

    g_dump = 0;
    int64_t t = now_us();
    stats_print(&g_stats, t);
    g_stats.logged = 1;
    g_stats.last_log = t;
    stats_window_reset(&g_stats, t);
}

static void run_device(mm_device *dev, mm_engine *eng, const mm_config *cfg)
{
    uint8_t buf[256];
    int64_t last_grab_try = now_us();

    g_stats.prev = 0;

    int fl = fcntl(dev->hid_fd, F_GETFL);
    if (fl >= 0)
        fcntl(dev->hid_fd, F_SETFL, fl | O_NONBLOCK);

    while (!g_stop && !g_reload) {
        int timeout = -1;
        int64_t deadline = mm_engine_next_deadline(eng);

        if (deadline) {
            int64_t wait = deadline - now_us();
            timeout = wait <= 0 ? 0 : (int)((wait + 999) / 1000);
        }

        int need_grab = cfg->grab && dev->nev == 0;
        if (need_grab) {
            if (now_us() - last_grab_try >= 500000) {
                last_grab_try = now_us();
                if (mm_device_grab(dev) > 0)
                    mm_log("grabbed %d kernel input node(s)", dev->nev);
            }
            if (timeout < 0 || timeout > 500)
                timeout = 500;
        }

        struct pollfd pfd = { .fd = dev->hid_fd, .events = POLLIN };
        int r = poll(&pfd, 1, timeout);

        stats_dump_if_asked();

        if (r < 0) {
            if (errno == EINTR)
                continue;
            mm_log("poll: %s", strerror(errno));
            return;
        }

        if (r > 0) {
            if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
                return;

            int queued = 0;

            for (int k = 0; k < 64; k++) {
                ssize_t n = read(dev->hid_fd, buf, sizeof(buf));

                if (n < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK)
                        break;
                    if (errno == EINTR)
                        continue;
                    if (errno != ENODEV && errno != EIO)
                        mm_log("read %s: %s", dev->hidraw, strerror(errno));
                    return;
                }
                if (n == 0)
                    return;

                int64_t t = now_us();
                stats_add(&g_stats, t, ++queued);

                mm_report rep;
                if (mm_parse_report(buf, (size_t)n, cfg, &rep))
                    mm_engine_report(eng, &rep, t);
            }
        }

        mm_engine_tick(eng, now_us());
    }
}

static int load_config(mm_config *cfg, const char *path, int explicit_config, int no_grab, int verbose)
{
    mm_config_defaults(cfg);

    int rc = mm_config_load(cfg, path);
    if (rc < 0) {
        if (explicit_config) {
            mm_log("cannot read %s: %s", path, strerror(errno));
            return -1;
        }
    } else if (rc > 0) {
        mm_log("%s: %d invalid line(s) ignored", path, rc);
    }

    if (no_grab)
        cfg->grab = 0;
    if (verbose)
        cfg->verbose = 1;

    return 0;
}

static void apply_fallbacks(mm_config *cfg)
{
    cfg->scroll_mode = MM_SCROLL_WHEEL;
    if (cfg->swipe2 == MM_SWIPE2_NATIVE)
        cfg->swipe2 = MM_SWIPE2_KEYS;
    if (cfg->smart_zoom == MM_ZOOM_PINCH)
        cfg->smart_zoom = MM_ZOOM_KEYS;
}

static void wait_ms(int ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    while (!g_stop && !g_reload && nanosleep(&ts, &ts) < 0 && errno == EINTR)
        ;
}

int main(int argc, char **argv)
{
    const char *config_path = DEFAULT_CONFIG;
    const char *device_path = NULL;
    int explicit_config = 0;
    int no_grab = 0;
    int print_only = 0;
    int verbose = 0;
    int opt;

    while ((opt = getopt(argc, argv, "c:d:npvh")) != -1) {
        switch (opt) {
        case 'c': config_path = optarg; explicit_config = 1; break;
        case 'd': device_path = optarg; break;
        case 'n': no_grab = 1; break;
        case 'p': print_only = 1; break;
        case 'v': verbose = 1; break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 2;
        }
    }

    setvbuf(stderr, NULL, _IOLBF, 0);

    mm_config cfg;
    if (load_config(&cfg, config_path, explicit_config, no_grab, verbose) < 0)
        return 1;
    mm_verbose = cfg.verbose;

    if (print_only) {
        mm_config_print(&cfg, stdout);
        return 0;
    }

    int lock = single_instance();
    if (lock == -1) {
        mm_log("magicmoused is already running (probably the service).\n"
               "Stop it first:  sudo systemctl stop magicmouse");
        return 1;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    sa.sa_handler = on_usr1;
    sigaction(SIGUSR1, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    mm_vinput vin;
    if (mm_vinput_create(&vin) < 0)
        return 1;

    if (vin.tp_fd < 0)
        apply_fallbacks(&cfg);

    mm_output out;
    mm_vinput_output(&vin, &out);

    mm_engine eng;
    mm_engine_init(&eng, &cfg, &out);

    mm_log("magicmoused started (scroll: %s, grab: %s)",
           cfg.scroll_mode == MM_SCROLL_TOUCHPAD ? "touchpad" : "wheel",
           cfg.grab ? "yes" : "no");

    int waiting_logged = 0;

    while (!g_stop) {
        mm_device dev;

        if (g_reload) {
            g_reload = 0;
            mm_config fresh;
            if (load_config(&fresh, config_path, explicit_config, no_grab, verbose) == 0) {
                if (vin.tp_fd < 0)
                    apply_fallbacks(&fresh);
                mm_engine_reset(&eng, now_us());
                cfg = fresh;
                mm_verbose = cfg.verbose;
                mm_log("configuration reloaded");
            }
        }

        if (mm_device_open(&dev, device_path, cfg.grab) < 0) {
            if (g_dump) {
                g_dump = 0;
                mm_log("health: Magic Mouse is not connected");
            }
            if (!waiting_logged) {
                mm_log("waiting for Magic Mouse 2...");
                waiting_logged = 1;
            }
            wait_ms(1000);
            continue;
        }

        waiting_logged = 0;
        mm_log("connected: %s (%04x:%04x) on %s, multitouch %s, %d input node(s) grabbed",
               dev.name[0] ? dev.name : "Magic Mouse", dev.vendor, dev.product,
               dev.hidraw, dev.multitouch ? "on" : "NOT confirmed", dev.nev);

        run_device(&dev, &eng, &cfg);

        mm_engine_reset(&eng, now_us());
        mm_device_close(&dev);

        if (!g_stop && !g_reload) {
            mm_log("disconnected");
            wait_ms(500);
        }
    }

    mm_engine_reset(&eng, now_us());
    mm_vinput_destroy(&vin);
    if (lock >= 0)
        close(lock);
    mm_log("stopped");
    return 0;
}
