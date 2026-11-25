/**
 * File:            tmux_wifi.c
 * Author:          mohamadrnasr
 * Created:         Oct 27, 2026
 * Last change:     Oct 27, 2026
 * Description:     Event-driven Wi-Fi status monitor (NetworkManager over D-Bus, sd-bus).
 * Dependencies:    libsystemd_dev - NerdFont
 * build:           gcc -O2 -Wextera -Wall tmux_wifi.c -o tmux_wifi $(pkg-config --cflags --libs libsystemd)
 **/

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/ip_icmp.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <systemd/sd-bus.h>
#include <time.h>
#include <unistd.h>

#define NM_DEST   "org.freedesktop.NetworkManager"
#define NM_PATH   "/org/freedesktop/NetworkManager"
#define NM_IF     "org.freedesktop.NetworkManager"
#define NM_DEV_IF "org.freedesktop.NetworkManager.Device"
#define NM_WIFI_IF "org.freedesktop.NetworkManager.Device.Wireless"
#define NM_AP_IF  "org.freedesktop.NetworkManager.AccessPoint"

#define NM_DEVICE_TYPE_WIFI       2
#define NM_DEVICE_STATE_ACTIVATED 100

#define PING_TARGET     "8.8.8.8"
#define PING_TIMEOUT_US (250 * 1000)

static bool verbose;
#define DBG(...) do { if (verbose) fprintf(stderr, __VA_ARGS__); } while (0)

enum icon { I_STR4, I_STR3, I_STR2, I_STR1, I_ZERO, I_OFF, I_NONET };
static const char *const ICON[] = {
    [I_STR4]  = "\U000F0928", // 󰤨  > 75%
    [I_STR3]  = "\U000F0925", // 󰤥  50-75%
    [I_STR2]  = "\U000F0922", // 󰤢  25-50%
    [I_STR1]  = "\U000F091F", // 󰤟  < 25%
    [I_ZERO]  = "\U000F092F", // 󰤯  0% / not connected
    [I_OFF]   = "\U000F092D", // 󰤭  Wi-Fi disabled
    [I_NONET] = "\U000F092B", // 󰤫  connected but ping failed
};

enum mode { M_OFF, M_DISC, M_CONN };
enum pingst { P_NONE, P_PENDING, P_OK, P_FAIL };

struct ctx {
    sd_bus *bus;
    bool dirty;
    enum mode mode;
    uint8_t strength;
    char dev[128]; // cached Wi-Fi device path
    char ap[128];  // current access point path
    enum pingst ping;
    int pfd;
    int64_t deadline_us;
    int last; // last printed icon
};

static int64_t now_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

static int64_t cpu_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t);
    return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

/* ---------- non-blocking ping (unprivileged ICMP datagram socket) ---------- */

static void ping_stop(struct ctx *c)
{
    if (c->pfd >= 0) {
        close(c->pfd);
        c->pfd = -1;
    }
}

static void ping_start(struct ctx *c)
{
    ping_stop(c);
    c->ping = P_FAIL;

    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_ICMP);
    if (fd < 0)
        return;

    struct icmphdr h;
    memset(&h, 0, sizeof h);
    h.type = ICMP_ECHO;
    h.un.echo.sequence = htons(1); // id + checksum are filled by the kernel

    struct sockaddr_in dst = { .sin_family = AF_INET };
    inet_pton(AF_INET, PING_TARGET, &dst.sin_addr);

    if (sendto(fd, &h, sizeof h, 0, (struct sockaddr *)&dst, sizeof dst) != (ssize_t)sizeof h) {
        close(fd);
        return;
    }
    c->pfd = fd;
    c->ping = P_PENDING;
    c->deadline_us = now_us() + PING_TIMEOUT_US;
}

static void ping_read(struct ctx *c)
{
    uint8_t buf[128];
    ssize_t n = recv(c->pfd, buf, sizeof buf, 0);
    if (n >= 1 && buf[0] == ICMP_ECHOREPLY) {
        c->ping = P_OK;
        ping_stop(c);
    } else if (n < 0 && errno != EAGAIN && errno != EINTR) {
        c->ping = P_FAIL;
        ping_stop(c);
    }
}

/* ------------------------------ D-Bus helpers ------------------------------ */

static int get_prop(sd_bus *b, const char *path, const char *iface,
                    const char *name, char type, void *out)
{
    sd_bus_error e = SD_BUS_ERROR_NULL;
    int r = sd_bus_get_property_trivial(b, NM_DEST, path, iface, name, &e, type, out);
    if (r < 0)
        DBG("[dbg] get %s.%s @ %s failed: %s\n", iface, name, path,
            e.message ? e.message : strerror(-r));
    sd_bus_error_free(&e);
    return r;
}

// Read a property of type 'o' (object path) into a fixed buffer.
static int get_obj_prop(sd_bus *b, const char *path, const char *iface,
                        const char *name, char *out, size_t len)
{
    sd_bus_error e = SD_BUS_ERROR_NULL;
    sd_bus_message *m = NULL;
    const char *s = NULL;
    int r = sd_bus_get_property(b, NM_DEST, path, iface, name, &e, &m, "o");
    if (r >= 0)
        r = sd_bus_message_read(m, "o", &s);
    if (r >= 0)
        snprintf(out, len, "%s", s);
    else
        DBG("[dbg] get %s.%s @ %s failed: %s\n", iface, name, path,
            e.message ? e.message : strerror(-r));
    sd_bus_message_unref(m);
    sd_bus_error_free(&e);
    return r;
}

static int find_wifi_dev(struct ctx *c)
{
    sd_bus_message *m = NULL;
    sd_bus_error e = SD_BUS_ERROR_NULL;
    int r = sd_bus_call_method(c->bus, NM_DEST, NM_PATH, NM_IF, "GetDevices", &e, &m, "");
    if (r >= 0)
        r = sd_bus_message_enter_container(m, SD_BUS_TYPE_ARRAY, "o");
    if (r >= 0) {
        const char *p;
        while (sd_bus_message_read(m, "o", &p) > 0) {
            uint32_t t = 0;
            if (get_prop(c->bus, p, NM_DEV_IF, "DeviceType", 'u', &t) >= 0 &&
                t == NM_DEVICE_TYPE_WIFI) {
                snprintf(c->dev, sizeof c->dev, "%s", p);
                break;
            }
        }
    }
    sd_bus_message_unref(m);
    sd_bus_error_free(&e);
    return c->dev[0] ? 0 : -ENODEV;
}

static void go_idle(struct ctx *c, enum mode m)
{
    c->mode = m;
    c->ap[0] = 0;
    ping_stop(c);
    c->ping = P_NONE;
}

static void refresh(struct ctx *c)
{
    int en = 0;
    if (get_prop(c->bus, NM_PATH, NM_IF, "WirelessEnabled", 'b', &en) < 0 || !en) {
        go_idle(c, M_OFF);
        return;
    }
    if (!c->dev[0] && find_wifi_dev(c) < 0) {
        DBG("[dbg] no Wi-Fi device found\n");
        go_idle(c, M_DISC);
        return;
    }

    uint32_t st = 0;
    if (get_prop(c->bus, c->dev, NM_DEV_IF, "State", 'u', &st) < 0) {
        c->dev[0] = 0; // device vanished; rescan next time
        go_idle(c, M_DISC);
        return;
    }
    DBG("[dbg] dev=%s state=%u\n", c->dev, st);
    if (st != NM_DEVICE_STATE_ACTIVATED) {
        go_idle(c, M_DISC);
        return;
    }

    char ap[128];
    if (get_obj_prop(c->bus, c->dev, NM_WIFI_IF, "ActiveAccessPoint", ap, sizeof ap) < 0 ||
        !strcmp(ap, "/")) {
        DBG("[dbg] no active access point\n");
        go_idle(c, M_DISC);
        return;
    }

    bool fresh = c->mode != M_CONN || strcmp(ap, c->ap) != 0;
    snprintf(c->ap, sizeof c->ap, "%s", ap);

    uint8_t s = 0;
    if (get_prop(c->bus, c->ap, NM_AP_IF, "Strength", 'y', &s) < 0)
        s = 0;
    c->strength = s;
    c->mode = M_CONN;
    DBG("[dbg] connected: dev=%s ap=%s strength=%u\n", c->dev, c->ap, s);

    // Ping once per new connection (and retry on later events while it keeps failing).
    if (fresh || c->ping == P_FAIL)
        ping_start(c);
}

static void emit(struct ctx *c)
{
    int ic;
    switch (c->mode) {
    case M_OFF:  ic = I_OFF;  break;
    case M_DISC: ic = I_ZERO; break;
    default:
        if (c->ping == P_PENDING)
            return; // wait for the verdict, no flicker
        if (c->ping == P_FAIL)
            ic = I_NONET;
        else if (c->strength == 0)  ic = I_ZERO;
        else if (c->strength > 75)  ic = I_STR4;
        else if (c->strength > 50)  ic = I_STR3;
        else if (c->strength > 25)  ic = I_STR2;
        else                        ic = I_STR1;
    }
    if (ic != c->last) {
        c->last = ic;
        puts(ICON[ic]);
        fflush(stdout);
    }
}

/* ------------------------------- signal handlers ------------------------------- */

// PropertiesChanged: react only to objects we care about (NM, our device, our AP).
static int on_props(sd_bus_message *m, void *u, sd_bus_error *err)
{
    (void)err;
    struct ctx *c = u;
    const char *p = sd_bus_message_get_path(m);
    if (!p)
        return 0;
    if (!strcmp(p, NM_PATH) ||
        (c->dev[0] && !strcmp(p, c->dev)) ||
        (c->ap[0] && !strcmp(p, c->ap)))
        c->dirty = true;
    return 0;
}

// NM restarted / device added or removed: drop caches and re-evaluate.
static int on_reset(sd_bus_message *m, void *u, sd_bus_error *err)
{
    (void)m;
    (void)err;
    struct ctx *c = u;
    c->dev[0] = 0;
    c->ap[0] = 0;
    c->dirty = true;
    return 0;
}

/* ----------------------------------- main ----------------------------------- */

int main(int argc, char **argv)
{
    verbose = argc > 1 && !strcmp(argv[1], "-v");
    struct ctx c = { .pfd = -1, .dirty = true, .mode = M_DISC, .last = -1 };

    if (sd_bus_open_system(&c.bus) < 0) {
        fprintf(stderr, "cannot connect to system bus\n");
        return 1;
    }

    int r = sd_bus_add_match(c.bus, NULL,
        "type='signal',sender='" NM_DEST "',"
        "interface='org.freedesktop.DBus.Properties',member='PropertiesChanged',"
        "path_namespace='" NM_PATH "'", on_props, &c);
    if (r >= 0)
        r = sd_bus_add_match(c.bus, NULL,
            "type='signal',sender='" NM_DEST "',interface='" NM_IF "',"
            "path='" NM_PATH "'", on_reset, &c); // DeviceAdded / DeviceRemoved
    if (r >= 0)
        r = sd_bus_add_match(c.bus, NULL,
            "type='signal',sender='org.freedesktop.DBus',"
            "interface='org.freedesktop.DBus',member='NameOwnerChanged',"
            "arg0='" NM_DEST "'", on_reset, &c);
    if (r < 0) {
        fprintf(stderr, "add_match failed: %s\n", strerror(-r));
        return 1;
    }

    int64_t wake_us = 0, wake_cpu = 0;
    bool woke = false;

    for (;;) {
        while ((r = sd_bus_process(c.bus, NULL)) > 0)
            ;
        if (r < 0) {
            fprintf(stderr, "bus error: %s\n", strerror(-r));
            break;
        }

        if (c.dirty) {
            c.dirty = false;
            refresh(&c);
        }
        emit(&c);

        // Sleep until a D-Bus message, the ping reply, or the ping deadline.
        int64_t to = -1;
        uint64_t bt;
        if (sd_bus_get_timeout(c.bus, &bt) >= 0 && bt != UINT64_MAX) {
            int64_t d = (int64_t)bt - now_us();
            to = d < 0 ? 0 : d;
        }
        if (c.ping == P_PENDING) {
            int64_t d = c.deadline_us - now_us();
            if (d < 0)
                d = 0;
            if (to < 0 || d < to)
                to = d;
        }

        if (verbose && woke) {
            double wall = (now_us() - wake_us) / 1000.0;
            double cpu = (cpu_us() - wake_cpu) / 1000.0;
            DBG("[dbg] work per wakeup: wall %.3f ms, cpu %.3f ms\n", wall, cpu);
        }

        struct pollfd fds[2] = {
            { .fd = sd_bus_get_fd(c.bus), .events = (short)sd_bus_get_events(c.bus) },
            { .fd = c.pfd, .events = POLLIN }, // fd == -1 is ignored by poll()
        };
        int pr = poll(fds, 2, to < 0 ? -1 : (int)((to + 999) / 1000));
        if (pr < 0 && errno != EINTR)
            break;

        if (verbose) {
            wake_us = now_us();
            wake_cpu = cpu_us();
            woke = true;
        }

        if (c.ping == P_PENDING) {
            if (fds[1].revents)
                ping_read(&c);
            if (c.ping == P_PENDING && now_us() >= c.deadline_us) {
                c.ping = P_FAIL;
                ping_stop(&c);
            }
        }
    }

    ping_stop(&c);
    sd_bus_flush_close_unref(c.bus);
    return 0;
}
