// SPDX-License-Identifier: MIT
/*
 * Copyright © 2011 Benjamin Franzke
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

// Build (see details below):
/*   # build once
      wayland-scanner client-header < /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml > xdg-shell-client-protocol.h
      wayland-scanner private-code < /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml > xdg-shell-protocol.c
     # compile and link :
     gcc -Wall -Wextra -O0 -g -o wayland-egl-input-test wayland-egl-input.c xdg-shell-protocol.c \
       $(pkg-config --cflags --libs wayland-client wayland-egl egl glesv2 xkbcommon) -lm
*/
#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <wayland-client.h>
#include <wayland-egl.h>

#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include <xkbcommon/xkbcommon.h>

// Generated headers (create with wayland-scanner; see build section)
#include "xdg-shell-client-protocol.h"

// ----------------------- App state -----------------------
static volatile sig_atomic_t g_running = 1;
static void handle_sigint(int) { g_running = 0; }

static int32_t g_init_width  = 1280;
static int32_t g_init_height = 720;
static int     g_swap_interval = 1;

struct app {
    // Wayland core
    struct wl_display    *wl_display;
    int                   wl_fd;
    struct wl_registry   *wl_registry;
    struct wl_compositor *wl_compositor;
    struct wl_seat       *wl_seat;

    // Shell
    struct xdg_wm_base   *xdg_wm_base;
    struct wl_surface    *wl_surface;
    struct xdg_surface   *xdg_surface;
    struct xdg_toplevel  *xdg_toplevel;

    // Input
    struct wl_pointer    *wl_pointer;
    struct wl_keyboard   *wl_keyboard;

    // Keyboard state
    struct xkb_context   *xkb_ctx;
    struct xkb_keymap    *xkb_keymap;
    struct xkb_state     *xkb_state;

    // EGL / GL
    EGLDisplay            egl_display;
    EGLConfig             egl_config;
    EGLContext            egl_context;
    EGLSurface            egl_surface;
    struct wl_egl_window *egl_window;

    // Sizes & configure
    int have_configure;
    int width, height;
    int pending_width, pending_height;

    // For focused-input friendliness
    bool pointer_focused;
    bool keyboard_focused;
};

// ----------------------- Utilities -----------------------
static void die(const char *msg) {
    fprintf(stderr, "FATAL: %s\n", msg);
    exit(EXIT_FAILURE);
}
static void die_egl(const char *where) {
    EGLint err = eglGetError();
    fprintf(stderr, "FATAL EGL (%s): 0x%04x\n", where, err);
    exit(EXIT_FAILURE);
}
static void load_env_overrides(void) {
    const char *w = getenv("WIDTH");
    const char *h = getenv("HEIGHT");
    const char *si = getenv("SWAP_INTERVAL");
    if (w)  g_init_width = atoi(w);
    if (h)  g_init_height = atoi(h);
    if (si) g_swap_interval = atoi(si);
}

// ----------------------- xdg_wm_base ---------------------
static void xdg_wm_base_ping(void *data, struct xdg_wm_base *base, uint32_t serial) {
    (void)data;
    xdg_wm_base_pong(base, serial);
}
static const struct xdg_wm_base_listener xdg_wm_base_listener = {
    .ping = xdg_wm_base_ping,
};

// ----------------------- xdg_toplevel --------------------
static void xdg_toplevel_configure(void *data, struct xdg_toplevel *toplevel,
                                   int32_t width, int32_t height,
                                   struct wl_array *states) {
    (void)toplevel; (void)states;
    struct app *a = data;
    if (width  > 0) a->pending_width  = width;
    if (height > 0) a->pending_height = height;
}
static void xdg_toplevel_close(void *data, struct xdg_toplevel *toplevel) {
    (void)data; (void)toplevel;
    g_running = 0;
}
// v4+ (bounds suggestion)
static void xdg_toplevel_configure_bounds(void *data, struct xdg_toplevel *toplevel,
                                          int32_t width, int32_t height) {
    (void)data; (void)toplevel; (void)width; (void)height;
}
// v5+ (capabilities)
static void xdg_toplevel_wm_capabilities(void *data, struct xdg_toplevel *toplevel,
                                         struct wl_array *capabilities) {
    (void)data; (void)toplevel; (void)capabilities;
}
static const struct xdg_toplevel_listener xdg_toplevel_listener = {
    .configure         = xdg_toplevel_configure,
    .close             = xdg_toplevel_close,
    .configure_bounds  = xdg_toplevel_configure_bounds,
    .wm_capabilities   = xdg_toplevel_wm_capabilities,
};

// ----------------------- xdg_surface ---------------------
static void xdg_surface_configure(void *data, struct xdg_surface *surf, uint32_t serial) {
    struct app *a = data;
    xdg_surface_ack_configure(surf, serial);

    int w = (a->pending_width  > 0) ? a->pending_width  : (a->width  > 0 ? a->width  : g_init_width);
    int h = (a->pending_height > 0) ? a->pending_height : (a->height > 0 ? a->height : g_init_height);

    if (a->egl_window) wl_egl_window_resize(a->egl_window, w, h, 0, 0);
    a->width = w; a->height = h;

    a->have_configure = 1;
}
static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_configure,
};

// ----------------------- Keyboard (xkb) ------------------
static void xkb_reset_keymap(struct app *a) {
    if (a->xkb_state)  { xkb_state_unref(a->xkb_state);  a->xkb_state  = NULL; }
    if (a->xkb_keymap) { xkb_keymap_unref(a->xkb_keymap); a->xkb_keymap = NULL; }
}
static void keyboard_keymap(void *data, struct wl_keyboard *kbd,
                            uint32_t format, int32_t fd, uint32_t size) {
    (void)kbd; // silence -Wunused-parameter
    struct app *a = data;
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        close(fd);
        xkb_reset_keymap(a);
        return;
    }
    void *map = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        close(fd);
        xkb_reset_keymap(a);
        return;
    }
    xkb_reset_keymap(a);
    a->xkb_keymap = xkb_keymap_new_from_string(a->xkb_ctx, (const char*)map,
                                               XKB_KEYMAP_FORMAT_TEXT_V1,
                                               XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(map, size);
    close(fd);
    if (!a->xkb_keymap) return;
    a->xkb_state = xkb_state_new(a->xkb_keymap);
}
static void keyboard_enter(void *data, struct wl_keyboard *kbd,
                           uint32_t serial, struct wl_surface *surface, struct wl_array *keys) {
    (void)kbd; (void)serial; (void)keys;
    struct app *a = data;
    a->keyboard_focused = (surface == a->wl_surface);
    fprintf(stderr, "keyboard enter (focused=%d)\n", a->keyboard_focused ? 1 : 0);
}
static void keyboard_leave(void *data, struct wl_keyboard *kbd,
                           uint32_t serial, struct wl_surface *surface) {
    (void)kbd; (void)serial; (void)surface;
    struct app *a = data;
    a->keyboard_focused = false;
    fprintf(stderr, "keyboard leave\n");
}
static void keyboard_key(void *data, struct wl_keyboard *kbd,
                         uint32_t serial, uint32_t time, uint32_t key, uint32_t state) {
    (void)kbd; (void)serial; (void)time;
    struct app *a = data;
    if (!a->keyboard_focused || !a->xkb_state) return;

    if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        // Wayland keycodes are Linux evdev codes; add 8 for XKB:
        xkb_keysym_t sym = xkb_state_key_get_one_sym(a->xkb_state, key + 8);
        // Try to get UTF-32 representation:
        uint32_t u = xkb_keysym_to_utf32(sym);
        if (u) {
            if (u >= 0x21 && u <= 0x7E) {
                fprintf(stderr, "key '%c' pressed\n", (char)u);
                if (u == 'q') g_running = 0;           // 'q' quits
            } else {
                fprintf(stderr, "key U+%04X pressed\n", u);
                if (u == 0x001B) g_running = 0;        // ESC quits
            }
        } else {
            char name[64];
            xkb_keysym_get_name(sym, name, sizeof(name));
            fprintf(stderr, "key %s pressed\n", name);
        }
    }
}
static void keyboard_modifiers(void *data, struct wl_keyboard *kbd,
                               uint32_t serial, uint32_t depressed, uint32_t latched,
                               uint32_t locked, uint32_t group) {
    (void)kbd; (void)serial;
    struct app *a = data;
    if (!a->xkb_state) return;
    xkb_state_update_mask(a->xkb_state, depressed, latched, locked, 0, 0, group);
}
static void keyboard_repeat_info(void *data,
                                 struct wl_keyboard *keyboard,
                                 int32_t rate,
                                 int32_t delay)
{
    (void)data;
    (void)keyboard;
    // Optional: store repeat rate/delay, or just log them
    fprintf(stderr, "repeat_info: rate=%d delay=%d\n", rate, delay);
}
static const struct wl_keyboard_listener keyboard_listener = {
    .keymap     = keyboard_keymap,
    .enter      = keyboard_enter,
    .leave      = keyboard_leave,
    .key        = keyboard_key,
    .modifiers  = keyboard_modifiers,
    .repeat_info = keyboard_repeat_info,
};

// ----------------------- Pointer -------------------------
static void pointer_enter(void *data, struct wl_pointer *ptr, uint32_t serial,
                          struct wl_surface *surface, wl_fixed_t sx, wl_fixed_t sy) {
    (void)ptr; (void)serial;
    struct app *a = data;
    a->pointer_focused = (surface == a->wl_surface);
    fprintf(stderr, "pointer enter (focused=%d) @ %.2f, %.2f\n",
            a->pointer_focused ? 1 : 0,
            wl_fixed_to_double(sx), wl_fixed_to_double(sy));
}
static void pointer_leave(void *data, struct wl_pointer *ptr, uint32_t serial,
                          struct wl_surface *surface) {
    (void)ptr; (void)serial; (void)surface;
    struct app *a = data;
    a->pointer_focused = false;
    fprintf(stderr, "pointer leave\n");
}
static void pointer_motion(void *data, struct wl_pointer *ptr, uint32_t time,
                           wl_fixed_t sx, wl_fixed_t sy) {
    (void)ptr; (void)time;
    struct app *a = data;
    if (!a->pointer_focused) return;
    fprintf(stderr, "pointer motion %.2f, %.2f\n",
            wl_fixed_to_double(sx), wl_fixed_to_double(sy));
}
static void pointer_button(void *data, struct wl_pointer *ptr, uint32_t serial,
                           uint32_t time, uint32_t button, uint32_t state) {
    (void)ptr; (void)serial; (void)time;
    struct app *a = data;
    if (!a->pointer_focused) return;
    fprintf(stderr, "pointer button (button=%u, state=%u)\n", button, state);
}
static void pointer_axis(void *data, struct wl_pointer *ptr, uint32_t time,
                         uint32_t axis, wl_fixed_t value) {
    (void)ptr; (void)time;
    struct app *a = data;
    if (!a->pointer_focused) return;
    fprintf(stderr, "pointer axis (axis=%u, value=%.2f)\n",
            axis, wl_fixed_to_double(value));
}
/* REQUIRED: called to delimit a logical group of pointer events */
static void pointer_frame(void *data, struct wl_pointer *ptr) {
    (void)data; (void)ptr;
    // You can flush accumulated motion/axis deltas here if you want
}
/* wl_pointer v5+ commonly used callbacks */
static void pointer_axis_source(void *data, struct wl_pointer *ptr, uint32_t source) {
    (void)data; (void)ptr; (void)source;
    // sources: WL_POINTER_AXIS_SOURCE_* (finger, wheel, continuous)
}
static void pointer_axis_stop(void *data, struct wl_pointer *ptr, uint32_t time, uint32_t axis) {
    (void)data; (void)ptr; (void)time; (void)axis;
}
static void pointer_axis_discrete(void *data, struct wl_pointer *ptr, uint32_t axis, int32_t discrete) {
    (void)data; (void)ptr; (void)axis; (void)discrete;
}
static const struct wl_pointer_listener pointer_listener = {
    .enter = pointer_enter,
    .leave = pointer_leave,
    .motion = pointer_motion,
    .button = pointer_button,
    .axis = pointer_axis,
    .frame          = pointer_frame,          // <-- REQUIRED (fixes your crash)
    .axis_source    = pointer_axis_source,    // optional but recommended
    .axis_stop      = pointer_axis_stop,      // optional but recommended
    .axis_discrete  = pointer_axis_discrete,  // optional but recommended
};

// ----------------------- Seat ----------------------------
static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps) {
    struct app *a = data;

    // Pointer capability
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !a->wl_pointer) {
        a->wl_pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(a->wl_pointer, &pointer_listener, a);
        fprintf(stderr, "seat: pointer bound\n");
    } else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && a->wl_pointer) {
        wl_pointer_destroy(a->wl_pointer);
        a->wl_pointer = NULL;
        fprintf(stderr, "seat: pointer unbound\n");
    }

    // Keyboard capability
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !a->wl_keyboard) {
        a->wl_keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(a->wl_keyboard, &keyboard_listener, a);
        fprintf(stderr, "seat: keyboard bound\n");
    } else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && a->wl_keyboard) {
        wl_keyboard_destroy(a->wl_keyboard);
        a->wl_keyboard = NULL;
        fprintf(stderr, "seat: keyboard unbound\n");
    }
}
static void seat_name(void *data, struct wl_seat *seat, const char *name) {
    (void)data; (void)seat;
    if (name) fprintf(stderr, "seat name: %s\n", name);
}
static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
    .name         = seat_name, // may be ignored by older compositors
};

// ----------------------- Registry ------------------------
static void handle_global(void *data, struct wl_registry *reg,
                          uint32_t name, const char *iface, uint32_t version) {
    struct app *a = data;

    if (strcmp(iface, "wl_compositor") == 0) {
        uint32_t v = version < 5 ? version : 5;
        a->wl_compositor = wl_registry_bind(reg, name, &wl_compositor_interface, v);
    } else if (strcmp(iface, "xdg_wm_base") == 0) {
        uint32_t v = version < 6 ? version : 6;
        a->xdg_wm_base = wl_registry_bind(reg, name, &xdg_wm_base_interface, v);
        xdg_wm_base_add_listener(a->xdg_wm_base, &xdg_wm_base_listener, a);
    } else if (strcmp(iface, "wl_seat") == 0) {
        uint32_t v = version < 5 ? version : 5;
        a->wl_seat = wl_registry_bind(reg, name, &wl_seat_interface, v);
        wl_seat_add_listener(a->wl_seat, &seat_listener, a);
    }
}
static void handle_global_remove(void *data, struct wl_registry *reg, uint32_t name) {
    (void)data; (void)reg; (void)name;
}
static const struct wl_registry_listener registry_listener = {
    .global        = handle_global,
    .global_remove = handle_global_remove,
};

// ----------------------- EGL Init ------------------------
static void egl_init(struct app *a) {
    a->egl_display = eglGetDisplay((EGLNativeDisplayType)a->wl_display);
    if (a->egl_display == EGL_NO_DISPLAY) die_egl("eglGetDisplay");
    if (!eglInitialize(a->egl_display, NULL, NULL)) die_egl("eglInitialize");
    if (!eglBindAPI(EGL_OPENGL_ES_API)) die_egl("eglBindAPI");

    EGLint cfg_attribs[] = {
        EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE,        8,
        EGL_GREEN_SIZE,      8,
        EGL_BLUE_SIZE,       8,
        EGL_ALPHA_SIZE,      8,
        EGL_NONE
    };
    EGLint num = 0;
    if (!eglChooseConfig(a->egl_display, cfg_attribs, &a->egl_config, 1, &num) || num < 1)
        die_egl("eglChooseConfig");

    EGLint ctx_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    a->egl_context = eglCreateContext(a->egl_display, a->egl_config, EGL_NO_CONTEXT, ctx_attribs);
    if (a->egl_context == EGL_NO_CONTEXT) die_egl("eglCreateContext");
}

static void create_surface_chain(struct app *a) {
    // wl_surface
    a->wl_surface = wl_compositor_create_surface(a->wl_compositor);
    if (!a->wl_surface) die("wl_compositor_create_surface failed");

    // xdg_surface / xdg_toplevel
    a->xdg_surface = xdg_wm_base_get_xdg_surface(a->xdg_wm_base, a->wl_surface);
    if (!a->xdg_surface) die("xdg_wm_base_get_xdg_surface failed");
    xdg_surface_add_listener(a->xdg_surface, &xdg_surface_listener, a);

    a->xdg_toplevel = xdg_surface_get_toplevel(a->xdg_surface);
    if (!a->xdg_toplevel) die("xdg_surface_get_toplevel failed");
    xdg_toplevel_add_listener(a->xdg_toplevel, &xdg_toplevel_listener, a);
    xdg_toplevel_set_app_id(a->xdg_toplevel, "wayland-egl-input");
    xdg_toplevel_set_title(a->xdg_toplevel,  "Wayland EGL Input Sample");

    // First commit triggers configure
    wl_surface_commit(a->wl_surface);
    wl_display_flush(a->wl_display);

    // Wait for first configure
    while (!a->have_configure) {
        if (wl_display_dispatch(a->wl_display) < 0) die("dispatch failed (configure)");
    }

    if (a->width <= 0)  a->width  = g_init_width;
    if (a->height <= 0) a->height = g_init_height;

    a->egl_window = wl_egl_window_create(a->wl_surface, a->width, a->height);
    if (!a->egl_window) die("wl_egl_window_create failed");

    a->egl_surface = eglCreateWindowSurface(
        a->egl_display, a->egl_config,
        (EGLNativeWindowType)a->egl_window,
        NULL
    );
    if (a->egl_surface == EGL_NO_SURFACE) die_egl("eglCreateWindowSurface");

    if (!eglMakeCurrent(a->egl_display, a->egl_surface, a->egl_surface, a->egl_context))
        die_egl("eglMakeCurrent");

    eglSwapInterval(a->egl_display, g_swap_interval);
}

// ----------------------- Rendering -----------------------
static void render_frame(struct app *a, float t) {
    float green = 0.5f + 0.5f * sinf(t);
    glViewport(0, 0, a->width, a->height);
    glClearColor(0.0f, green, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    eglSwapBuffers(a->egl_display, a->egl_surface);
}

// ----------------------- Main ----------------------------
int main(void) {
    signal(SIGINT,  handle_sigint);
    signal(SIGTERM, handle_sigint);
    load_env_overrides();

    struct app a = {0};

    // Wayland connect + registry
    a.wl_display = wl_display_connect(NULL);
    if (!a.wl_display) die("wl_display_connect failed");
    a.wl_fd = wl_display_get_fd(a.wl_display);
    if (a.wl_fd < 0) die("wl_display_get_fd failed");

    a.wl_registry = wl_display_get_registry(a.wl_display);
    if (!a.wl_registry) die("wl_display_get_registry failed");
    wl_registry_add_listener(a.wl_registry, &registry_listener, &a);

    // Roundtrips to bind globals
    wl_display_roundtrip(a.wl_display);
    wl_display_roundtrip(a.wl_display);
    if (!a.wl_compositor) die("wl_compositor missing");
    if (!a.xdg_wm_base)  die("xdg_wm_base missing");

    // xkb context
    a.xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!a.xkb_ctx) die("xkb_context_new failed");

    // EGL init + surface chain
    egl_init(&a);
    create_surface_chain(&a);

    // Main loop
    double tick = 0.0;
    while (g_running) {
        // Non-blocking event flow that cooperates with rendering
        while (wl_display_prepare_read(a.wl_display) != 0) {
            wl_display_dispatch_pending(a.wl_display);
        }
        wl_display_flush(a.wl_display);

        // Poll for at most ~16ms for ~60fps cadence
        struct pollfd pfd = { .fd = a.wl_fd, .events = POLLIN };
        (void)poll(&pfd, 1, 16);

        if (pfd.revents & POLLIN) {
            if (wl_display_read_events(a.wl_display) < 0) break;
        } else {
            wl_display_cancel_read(a.wl_display);
        }
        wl_display_dispatch_pending(a.wl_display);

        // Draw one frame
        render_frame(&a, (float)tick);
        tick += 0.016f; // ~60 FPS
    }

    // -------- Cleanup (destroy in safe order) --------
    if (a.egl_display) {
        eglMakeCurrent(a.egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (a.egl_surface) eglDestroySurface(a.egl_display, a.egl_surface);
        if (a.egl_context) eglDestroyContext(a.egl_display, a.egl_context);
        eglTerminate(a.egl_display);
    }
    if (a.egl_window)   wl_egl_window_destroy(a.egl_window);

    if (a.wl_pointer)   wl_pointer_destroy(a.wl_pointer);
    if (a.wl_keyboard)  wl_keyboard_destroy(a.wl_keyboard);

    if (a.xkb_state)    xkb_state_unref(a.xkb_state);
    if (a.xkb_keymap)   xkb_keymap_unref(a.xkb_keymap);
    if (a.xkb_ctx)      xkb_context_unref(a.xkb_ctx);

    if (a.xdg_toplevel) xdg_toplevel_destroy(a.xdg_toplevel);
    if (a.xdg_surface)  xdg_surface_destroy(a.xdg_surface);
    if (a.wl_surface)   wl_surface_destroy(a.wl_surface);
    if (a.xdg_wm_base)  xdg_wm_base_destroy(a.xdg_wm_base);

    if (a.wl_seat)      wl_seat_destroy(a.wl_seat);
    if (a.wl_compositor) wl_compositor_destroy(a.wl_compositor);
    if (a.wl_registry)  wl_registry_destroy(a.wl_registry);

    if (a.wl_display)   wl_display_disconnect(a.wl_display);
    return 0;
}

