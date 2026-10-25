/* Click injection through zwlr_virtual_pointer_manager_v1 (niri/sway/wlroots). */
#define _GNU_SOURCE
#include "wayland.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wayland-client.h>

#include "protocols/wlr-virtual-pointer-unstable-v1-client-protocol.h"

static struct wl_display                      *display;
static struct wl_registry                     *registry;
static struct wl_seat                         *seat;
static struct zwlr_virtual_pointer_manager_v1 *manager;
static struct zwlr_virtual_pointer_v1         *pointer;

static atomic_bool ready;
static char        status[256] = "Wayland: not started";
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)((t.tv_sec * 1000LL + t.tv_nsec / 1000000LL) & 0xffffffffu);
}

static void seat_capabilities(void *data, struct wl_seat *s, uint32_t caps)
{
    (void)data; (void)s; (void)caps;
}

static void seat_name(void *data, struct wl_seat *s, const char *name)
{
    (void)data; (void)s; (void)name;
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
    .name         = seat_name,
};

static void registry_global(void *data, struct wl_registry *reg, uint32_t name,
                            const char *iface, uint32_t version)
{
    (void)data;
    if (strcmp(iface, zwlr_virtual_pointer_manager_v1_interface.name) == 0) {
        if (version > 2)
            version = 2;
        manager = wl_registry_bind(reg, name, &zwlr_virtual_pointer_manager_v1_interface, version);
    } else if (strcmp(iface, wl_seat_interface.name) == 0 && !seat) {
        seat = wl_registry_bind(reg, name, &wl_seat_interface, 1);
        wl_seat_add_listener(seat, &seat_listener, NULL);
    }
}

static void registry_global_remove(void *data, struct wl_registry *reg, uint32_t name)
{
    (void)data; (void)reg; (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global        = registry_global,
    .global_remove = registry_global_remove,
};

static void fail(const char *msg)
{
    snprintf(status, sizeof status, "%s", msg);
    atomic_store(&ready, false);
}

static void close_connection(void)
{
    if (pointer) {
        zwlr_virtual_pointer_v1_destroy(pointer);
        pointer = NULL;
    }
    if (manager) {
        zwlr_virtual_pointer_manager_v1_destroy(manager);
        manager = NULL;
    }
    if (seat) {
        wl_seat_destroy(seat);
        seat = NULL;
    }
    if (registry) {
        wl_registry_destroy(registry);
        registry = NULL;
    }
    if (display) {
        wl_display_flush(display);
        wl_display_disconnect(display);
        display = NULL;
    }
}

bool wayland_init(void)
{
    const char *wayland_display = getenv("WAYLAND_DISPLAY");
    const char *wayland_socket  = getenv("WAYLAND_SOCKET");

    if (!wayland_display && !wayland_socket) {
        fail("Wayland virtual pointer: not running under Wayland");
        return false;
    }

    display = wl_display_connect(NULL);
    if (!display) {
        snprintf(status, sizeof status,
                 "Wayland virtual pointer: cannot connect to %s",
                 wayland_display ? wayland_display : "the compositor");
        return false;
    }

    registry = wl_display_get_registry(display);
    if (!registry) {
        fail("Wayland virtual pointer: cannot get the registry");
        close_connection();
        return false;
    }
    wl_registry_add_listener(registry, &registry_listener, NULL);

    if (wl_display_roundtrip(display) < 0) {
        fail("Wayland virtual pointer: the compositor closed the connection");
        close_connection();
        return false;
    }

    if (!manager) {
        fail("Wayland virtual pointer: compositor does not support wlr-virtual-pointer "
             "(niri 25.02+ and sway do)");
        close_connection();
        return false;
    }

    pointer = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(manager, seat);
    if (!pointer) {
        fail("Wayland virtual pointer: cannot create a virtual pointer");
        close_connection();
        return false;
    }

    /* Catch protocol errors (if any) before reporting the backend as ready. */
    if (wl_display_roundtrip(display) < 0) {
        fail("Wayland virtual pointer: the compositor rejected the virtual pointer");
        close_connection();
        return false;
    }

    snprintf(status, sizeof status, "Wayland virtual pointer: ready");
    atomic_store(&ready, true);
    return true;
}

bool wayland_ready(void)
{
    return atomic_load(&ready);
}

const char *wayland_status(void)
{
    return status;
}

void wayland_inject(int btn, int state)
{
    uint32_t button_state = state ? WL_POINTER_BUTTON_STATE_PRESSED
                                  : WL_POINTER_BUTTON_STATE_RELEASED;

    pthread_mutex_lock(&lock);
    if (atomic_load(&ready) && pointer && display) {
        zwlr_virtual_pointer_v1_button(pointer, now_ms(), (uint32_t)btn, button_state);
        if (wl_display_flush(display) < 0 && errno != EAGAIN)
            atomic_store(&ready, false);
    }
    pthread_mutex_unlock(&lock);
}

void wayland_close(void)
{
    atomic_store(&ready, false);
    pthread_mutex_lock(&lock);
    close_connection();
    pthread_mutex_unlock(&lock);
}
