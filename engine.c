/* JetAutoClicker engine */
#define _GNU_SOURCE
#include "engine.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <time.h>
#include <unistd.h>

#define MAX_DEVS        64
#define RESCAN_MS       5000
#define STOP_POLL_NS    20000000LL   /* max uninterrupted sleep: 20 ms */
#define MAX_HOLD_NS     20000000LL   /* button held down at most 20 ms  */

static struct {
    pthread_mutex_t lock;
    pthread_cond_t  cond;
    pthread_t       clicker, hotkey;
    bool            clicker_started;
    atomic_bool     running, quit, capturing, hold_mode, ext_ready, wl_ready;
    atomic_llong    period_ns;
    atomic_int      button, hk_key, ndevs, backend;
    EngineInjectFn  ext, wl;
        atomic_ullong   clicks;
    int             uifd;
    char            err[160];
    EngineChangeFn  on_change;
    EngineCaptureFn on_capture;
    void           *user;
} S = { .lock = PTHREAD_MUTEX_INITIALIZER, .cond = PTHREAD_COND_INITIALIZER, .uifd = -1 };

/* ------------------------------------------------------------------ */
/* time helpers                                                       */
/* ------------------------------------------------------------------ */

static long long now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000000000LL + t.tv_nsec;
}

/* Sleep until `target`, but wake up early if the clicker was stopped, so
 * that very low CPS values (e.g. 0.1 = 10 s period) still stop instantly. */
static void sleep_until_ns(long long target)
{
    for (;;) {
        if (!atomic_load(&S.running) || atomic_load(&S.quit))
            return;
        long long n = now_ns();
        if (n >= target)
            return;
        long long t = (target - n > STOP_POLL_NS) ? n + STOP_POLL_NS : target;
        struct timespec ts = { t / 1000000000LL, t % 1000000000LL };
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
    }
}

/* ------------------------------------------------------------------ */
/* uinput virtual mouse                                               */
/* ------------------------------------------------------------------ */

static bool uinput_create(void)
{
#ifdef ENGINE_TEST
    S.uifd = open("/dev/null", O_WRONLY);
    return S.uifd >= 0;
#else
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        fd = open("/dev/input/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        snprintf(S.err, sizeof S.err, "Cannot open /dev/uinput: %s", strerror(errno));
        return false;
    }

    int ok = 0;
    ok |= ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ok |= ioctl(fd, UI_SET_KEYBIT, BTN_LEFT);
    ok |= ioctl(fd, UI_SET_KEYBIT, BTN_RIGHT);
    ok |= ioctl(fd, UI_SET_KEYBIT, BTN_MIDDLE);
    /* REL_X/REL_Y make libinput/KWin classify the device as a pointer. */
    ok |= ioctl(fd, UI_SET_EVBIT, EV_REL);
    ok |= ioctl(fd, UI_SET_RELBIT, REL_X);
    ok |= ioctl(fd, UI_SET_RELBIT, REL_Y);

    struct uinput_setup us;
    memset(&us, 0, sizeof us);
    us.id.bustype = BUS_VIRTUAL;
    us.id.vendor  = 0x1209;
    us.id.product = 0xB1A0;
    us.id.version = 1;
    snprintf(us.name, UINPUT_MAX_NAME_SIZE, "JetAutoClicker virtual mouse");
    ok |= ioctl(fd, UI_DEV_SETUP, &us);
    ok |= ioctl(fd, UI_DEV_CREATE);
    if (ok) {
        snprintf(S.err, sizeof S.err, "uinput setup failed: %s", strerror(errno));
        close(fd);
        return false;
    }

    /* Give the compositor a moment to pick up the new device. */
    struct timespec d = { 0, 300 * 1000000L };
    nanosleep(&d, NULL);
    S.uifd = fd;
    return true;
#endif
}

static void emit_button(int backend, int btn, int value)
{
    if (backend == ENGINE_BACKEND_EXTERNAL) {
        if (S.ext)
            S.ext(btn, value);
        return;
    }
    if (backend == ENGINE_BACKEND_WAYLAND) {
        if (S.wl)
            S.wl(btn, value);
        return;
    }
    if (S.uifd < 0)
        return;
    struct input_event ev[2];
    memset(ev, 0, sizeof ev);
    ev[0].type = EV_KEY; ev[0].code = btn;        ev[0].value = value;
    ev[1].type = EV_SYN; ev[1].code = SYN_REPORT; ev[1].value = 0;
    if (write(S.uifd, ev, sizeof ev) < 0) { /* EAGAIN etc.: drop this event */ }
}

/* ------------------------------------------------------------------ */
/* clicker thread                                                     */
/* ------------------------------------------------------------------ */

static void *clicker_thread(void *arg)
{
    (void)arg;
    prctl(PR_SET_TIMERSLACK, 1UL, 0, 0, 0);   /* 1 ns timer slack for this thread */

    while (!atomic_load(&S.quit)) {
        pthread_mutex_lock(&S.lock);
        while (!atomic_load(&S.running) && !atomic_load(&S.quit))
            pthread_cond_wait(&S.cond, &S.lock);
        pthread_mutex_unlock(&S.lock);
        if (atomic_load(&S.quit))
            break;

        long long next = now_ns();
        while (atomic_load(&S.running) && !atomic_load(&S.quit)) {
            long long period = atomic_load(&S.period_ns);
            long long hold   = period / 2;
            if (hold > MAX_HOLD_NS)
                hold = MAX_HOLD_NS;
            int btn = atomic_load(&S.button);
            int be  = atomic_load(&S.backend);      /* same backend for press and release */

            emit_button(be, btn, 1);
            next += hold;
            sleep_until_ns(next);
            emit_button(be, btn, 0);         /* always release, even if stopped */
            atomic_fetch_add(&S.clicks, 1);

            next += period - hold;
            sleep_until_ns(next);

            /* If we fell behind (system stall), don't fire a burst to catch up. */
            long long n = now_ns();
            if (n - next > period)
                next = n;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* hotkey listener (evdev)                                            */
/* ------------------------------------------------------------------ */

typedef struct { int fd; char path[300]; } Dev;

static inline bool has_bit(const unsigned char *a, int bit) { return a[bit / 8] & (1 << (bit % 8)); }

static bool is_candidate(int fd)
{
    unsigned char ev[(EV_MAX + 8) / 8 + 1];
    unsigned char key[(KEY_MAX + 8) / 8 + 1];
    memset(ev, 0, sizeof ev);
    memset(key, 0, sizeof key);
    if (ioctl(fd, EVIOCGBIT(0, sizeof ev), ev) < 0 || !has_bit(ev, EV_KEY))
        return false;
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof key), key) < 0)
        return false;
    if (has_bit(key, KEY_A) && has_bit(key, KEY_ENTER))      /* keyboard */
        return true;
    if (has_bit(key, BTN_SIDE) || has_bit(key, BTN_EXTRA))   /* mouse side buttons */
        return true;
    return false;
}

static void rescan(Dev *devs, int *n)
{
    DIR *dir = opendir("/dev/input");
    if (!dir)
        return;
    struct dirent *e;
    while ((e = readdir(dir))) {
        if (strncmp(e->d_name, "event", 5) != 0)
            continue;
        char path[300];
        snprintf(path, sizeof path, "/dev/input/%s", e->d_name);
        bool known = false;
        for (int i = 0; i < *n; i++)
            if (strcmp(devs[i].path, path) == 0) { known = true; break; }
        if (known)
            continue;
        if (*n >= MAX_DEVS)
            break;
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
            continue;
        if (!is_candidate(fd)) { close(fd); continue; }
        devs[*n].fd = fd;
        snprintf(devs[*n].path, sizeof devs[*n].path, "%s", path);
        (*n)++;
    }
    closedir(dir);
}

static bool is_modifier(int code)
{
    switch (code) {
    case KEY_LEFTCTRL:  case KEY_RIGHTCTRL:
    case KEY_LEFTALT:   case KEY_RIGHTALT:
    case KEY_LEFTSHIFT: case KEY_RIGHTSHIFT:
    case KEY_LEFTMETA:  case KEY_RIGHTMETA:
        return true;
    }
    return false;
}

static void notify_change(void)
{
    if (S.on_change)
        S.on_change(S.user);
}

static void handle_key(int code, int value)
{
    if (value == 2)                       /* ignore autorepeat */
        return;
    bool down = (value == 1);

    /* Modifiers are never a hotkey and never part of one: the hotkey is a
     * single key and works regardless of which modifiers are held. */
    if (is_modifier(code))
        return;
    if (code == BTN_LEFT || code == BTN_RIGHT || code == BTN_MIDDLE)
        return;

    if (atomic_load(&S.capturing)) {
        if (!down)
            return;
        atomic_store(&S.capturing, false);
        bool cancel = (code == KEY_ESC);
        if (!cancel)
            atomic_store(&S.hk_key, code);
        if (S.on_capture)
            S.on_capture(code, cancel, S.user);
        return;
    }

    if (code != atomic_load(&S.hk_key))
        return;

    if (atomic_load(&S.hold_mode)) {
        engine_set_running(down);
    } else if (down) {
        engine_set_running(!atomic_load(&S.running));
    }
}

static void *hotkey_thread(void *arg)
{
    (void)arg;
    Dev devs[MAX_DEVS];
    int n = 0;
    long long last_scan = 0;

    while (!atomic_load(&S.quit)) {
        if (now_ns() - last_scan >= RESCAN_MS * 1000000LL || last_scan == 0) {
            int before = n;
            rescan(devs, &n);
            last_scan = now_ns();
            if (n != before) { atomic_store(&S.ndevs, n); notify_change(); }
        }

        struct pollfd pf[MAX_DEVS];
        for (int i = 0; i < n; i++) { pf[i].fd = devs[i].fd; pf[i].events = POLLIN; pf[i].revents = 0; }
        int r = (n > 0) ? poll(pf, n, 1000) : (usleep(1000 * 1000), 0);
        if (r <= 0)
            continue;

        bool dead[MAX_DEVS] = { false };
        for (int i = 0; i < n; i++) {
            if (pf[i].revents & (POLLERR | POLLHUP | POLLNVAL)) { dead[i] = true; continue; }
            if (!(pf[i].revents & POLLIN))
                continue;
            struct input_event ev[64];
            for (;;) {
                ssize_t k = read(devs[i].fd, ev, sizeof ev);
                if (k < 0) {
                    if (errno == EINTR) continue;
                    if (errno != EAGAIN) dead[i] = true;
                    break;
                }
                if (k == 0) { dead[i] = true; break; }
                for (size_t j = 0; j < (size_t)k / sizeof ev[0]; j++)
                    if (ev[j].type == EV_KEY)
                        handle_key(ev[j].code, ev[j].value);
            }
        }

        int w = 0;
        for (int i = 0; i < n; i++) {
            if (dead[i]) { close(devs[i].fd); continue; }
            devs[w++] = devs[i];
        }
        if (w != n) { n = w; atomic_store(&S.ndevs, n); notify_change(); }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* public API                                                         */
/* ------------------------------------------------------------------ */

bool engine_init(EngineChangeFn on_change, EngineCaptureFn on_capture, void *user)
{
    S.on_change = on_change;
    S.on_capture = on_capture;
    S.user = user;
    atomic_store(&S.period_ns, 100000000LL);   /* 10 CPS */
    atomic_store(&S.button, BTN_LEFT);
    atomic_store(&S.hk_key, KEY_F6);

    bool ok = uinput_create();
    pthread_create(&S.clicker, NULL, clicker_thread, NULL);
    S.clicker_started = true;
    pthread_t t;
    pthread_create(&t, NULL, hotkey_thread, NULL);
    pthread_detach(t);
    return ok;
}

void engine_shutdown(void)
{
    atomic_store(&S.running, false);
    atomic_store(&S.quit, true);
    pthread_mutex_lock(&S.lock);
    pthread_cond_broadcast(&S.cond);
    pthread_mutex_unlock(&S.lock);
    if (S.clicker_started)
        pthread_join(S.clicker, NULL);
    if (S.uifd >= 0) {
#ifndef ENGINE_TEST
        ioctl(S.uifd, UI_DEV_DESTROY);
#endif
        close(S.uifd);
        S.uifd = -1;
    }
}

const char *engine_error(void) { return S.err; }

void engine_set_cps(double cps)
{
    if (cps < MIN_CPS) cps = MIN_CPS;
    if (cps > MAX_CPS) cps = MAX_CPS;
    atomic_store(&S.period_ns, (long long)(1e9 / cps));
}

void engine_set_button(int btn)       { atomic_store(&S.button, btn); }
void engine_set_hold_mode(bool hold)  { atomic_store(&S.hold_mode, hold); }
void engine_set_hotkey(int key)       { atomic_store(&S.hk_key, key); }
void engine_get_hotkey(int *key)      { *key = atomic_load(&S.hk_key); }
void engine_start_capture(void)       { atomic_store(&S.capturing, true); }
void engine_cancel_capture(void)      { atomic_store(&S.capturing, false); }
bool engine_is_capturing(void)        { return atomic_load(&S.capturing); }
bool engine_is_running(void)          { return atomic_load(&S.running); }
unsigned long long engine_click_count(void) { return atomic_load(&S.clicks); }
int  engine_hotkey_device_count(void) { return atomic_load(&S.ndevs); }

void engine_set_external(EngineInjectFn fn) { S.ext = fn; }
void engine_set_external_ready(bool r)
{
    atomic_store(&S.ext_ready, r);
    if (!r && atomic_load(&S.backend) == ENGINE_BACKEND_EXTERNAL)
        engine_set_running(false);
}
void engine_set_wayland(EngineInjectFn fn) { S.wl = fn; }
void engine_set_wayland_ready(bool r)
{
    atomic_store(&S.wl_ready, r);
    if (!r && atomic_load(&S.backend) == ENGINE_BACKEND_WAYLAND)
        engine_set_running(false);
}
void engine_set_backend(int b)
{
    if (atomic_exchange(&S.backend, b) != b)
        engine_set_running(false);
}
bool engine_uinput_ok(void) { return S.uifd >= 0; }
bool engine_backend_ready(void)
{
    int be = atomic_load(&S.backend);
    if (be == ENGINE_BACKEND_EXTERNAL)
        return atomic_load(&S.ext_ready);
    if (be == ENGINE_BACKEND_WAYLAND)
        return atomic_load(&S.wl_ready);
    return S.uifd >= 0;
}

void engine_set_running(bool on)
{
    if (on && !engine_backend_ready())
        return;                            /* nothing to click with */
    pthread_mutex_lock(&S.lock);
    bool changed = (atomic_load(&S.running) != on);
    atomic_store(&S.running, on);
    pthread_cond_broadcast(&S.cond);
    pthread_mutex_unlock(&S.lock);
    if (changed)
        notify_change();
}

void engine_hotkey_name(int c, char *out, size_t n)
{
    static const char *r1 = "QWERTYUIOP", *r2 = "ASDFGHJKL", *r3 = "ZXCVBNM";
    char key[32];

    if      (c >= KEY_Q && c <= KEY_P)   snprintf(key, sizeof key, "%c", r1[c - KEY_Q]);
    else if (c >= KEY_A && c <= KEY_L)   snprintf(key, sizeof key, "%c", r2[c - KEY_A]);
    else if (c >= KEY_Z && c <= KEY_M)   snprintf(key, sizeof key, "%c", r3[c - KEY_Z]);
    else if (c >= KEY_1 && c <= KEY_9)   snprintf(key, sizeof key, "%d", c - KEY_1 + 1);
    else if (c == KEY_0)                 snprintf(key, sizeof key, "0");
    else if (c >= KEY_F1 && c <= KEY_F10)  snprintf(key, sizeof key, "F%d", c - KEY_F1 + 1);
    else if (c == KEY_F11)               snprintf(key, sizeof key, "F11");
    else if (c == KEY_F12)               snprintf(key, sizeof key, "F12");
    else if (c >= KEY_F13 && c <= KEY_F24) snprintf(key, sizeof key, "F%d", c - KEY_F13 + 13);
    else {
        const char *s = NULL;
        switch (c) {
        case KEY_ESC: s = "Esc"; break;          case KEY_TAB: s = "Tab"; break;
        case KEY_ENTER: s = "Enter"; break;      case KEY_SPACE: s = "Space"; break;
        case KEY_BACKSPACE: s = "Backspace"; break;
        case KEY_CAPSLOCK: s = "CapsLock"; break; case KEY_SCROLLLOCK: s = "ScrollLock"; break;
        case KEY_PAUSE: s = "Pause"; break;      case KEY_SYSRQ: s = "PrintScreen"; break;
        case KEY_INSERT: s = "Insert"; break;    case KEY_DELETE: s = "Delete"; break;
        case KEY_HOME: s = "Home"; break;        case KEY_END: s = "End"; break;
        case KEY_PAGEUP: s = "PageUp"; break;    case KEY_PAGEDOWN: s = "PageDown"; break;
        case KEY_UP: s = "Up"; break;            case KEY_DOWN: s = "Down"; break;
        case KEY_LEFT: s = "Left"; break;        case KEY_RIGHT: s = "Right"; break;
        case KEY_GRAVE: s = "`"; break;          case KEY_MINUS: s = "-"; break;
        case KEY_EQUAL: s = "="; break;          case KEY_COMMA: s = ","; break;
        case KEY_DOT: s = "."; break;            case KEY_SLASH: s = "/"; break;
        case KEY_SEMICOLON: s = ";"; break;      case KEY_APOSTROPHE: s = "'"; break;
        case KEY_LEFTBRACE: s = "["; break;      case KEY_RIGHTBRACE: s = "]"; break;
        case KEY_BACKSLASH: s = "\\"; break;
        case BTN_SIDE: s = "Mouse Side"; break;  case BTN_EXTRA: s = "Mouse Extra"; break;
        case BTN_FORWARD: s = "Mouse Forward"; break; case BTN_BACK: s = "Mouse Back"; break;
        case BTN_TASK: s = "Mouse Task"; break;
        }
        if (s) snprintf(key, sizeof key, "%s", s);
        else   snprintf(key, sizeof key, "Key %d", c);
    }

    snprintf(out, n, "%s", key);
}
