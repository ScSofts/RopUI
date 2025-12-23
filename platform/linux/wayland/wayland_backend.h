#ifndef __WAYLAND_BACKEND_H
#define __WAYLAND_BACKEND_H
#include <cstring>
#include <wayland-client.h>


#include "wayland-client-protocol.h"
#include "xdg-shell-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "pointer-constraints-unstable-v1-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"
#include "xdg-activation-v1-client-protocol.h"
#include "idle-inhibit-unstable-v1-client-protocol.h"

// struct WLContext {
//     struct wl_display* display;
//     struct wl_compositor* compositor;
//     struct wl_surface* surface;
//     struct xdg_wm_base* wm_base;
//     struct xdg_surface* xdg_surface;
//     struct xdg_toplevel* xdg_toplevel;
//     struct zxdg_decoration_manager_v1* decoration_manager;
//     struct zxdg_toplevel_decoration_v1* decoration;
//     int width;
//     int height;
// };

// int wl_init(struct WLContext* ctx, int width, int height);
// void wl_dispatch(struct WLContext* ctx);

struct WLContext {
    struct wl_display *display;
    struct wl_compositor *compositor;
    struct xdg_wm_base *wm_base;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *xdg_toplevel;
    struct wl_surface *surface;
    
    // 输入相关
    struct wl_seat *seat;
    struct wl_pointer *pointer;
    float mouse_x, mouse_y;
    bool button_clicked; // 触发一次点击

    int width, height;
    struct zxdg_decoration_manager_v1 *decoration_manager;
    struct zxdg_toplevel_decoration_v1 *decoration;
};

static void pointer_handle_enter(void *data, struct wl_pointer *p, uint32_t serial,
                                 struct wl_surface *sx, wl_fixed_t x, wl_fixed_t y) {}
static void pointer_handle_leave(void *data, struct wl_pointer *p, uint32_t serial, struct wl_surface *sx) {}
static void pointer_handle_motion(void *data, struct wl_pointer *p, uint32_t time, wl_fixed_t x, wl_fixed_t y) {
    auto* ctx = static_cast<WLContext*>(data);
    ctx->mouse_x = (float)wl_fixed_to_double(x);
    ctx->mouse_y = (float)wl_fixed_to_double(y);
}
static void pointer_handle_button(void *data, struct wl_pointer *p, uint32_t serial,
                                  uint32_t time, uint32_t button, uint32_t state) {
    auto* ctx = static_cast<WLContext*>(data);
    // 272 是 Linux 输入子系统的 BTN_LEFT
    if (button == 272 && state == WL_POINTER_BUTTON_STATE_PRESSED) {
        ctx->button_clicked = true;
    }
}
static void pointer_handle_frame(void *data, struct wl_pointer *p) {}
static void pointer_handle_axis(void *data, struct wl_pointer *p, uint32_t time, uint32_t axis, wl_fixed_t value) {}

static const struct wl_pointer_listener pointer_listener = {
    pointer_handle_enter, pointer_handle_leave, pointer_handle_motion,
    pointer_handle_button, pointer_handle_axis, pointer_handle_frame
};

// --- Seat Listener ---
static void seat_handle_capabilities(void *data, struct wl_seat *seat, uint32_t caps) {
    auto* ctx = static_cast<WLContext*>(data);
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !ctx->pointer) {
        ctx->pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(ctx->pointer, &pointer_listener, ctx);
    }
}
static void seat_handle_name(void *data, struct wl_seat *seat, const char *name) {}
static const struct wl_seat_listener seat_listener = { seat_handle_capabilities, seat_handle_name };

// --- XDG & Registry ---
static void handle_ping(void *data, struct xdg_wm_base *wm, uint32_t serial) {
    xdg_wm_base_pong(wm, serial);
}
static const struct xdg_wm_base_listener wm_base_listener = {.ping = handle_ping};

static void handle_xdg_surface_configure(void *data, struct xdg_surface *xdg_surface, uint32_t serial) {
    xdg_surface_ack_configure(xdg_surface, serial);
}
static const struct xdg_surface_listener xdg_surface_listener = {.configure = handle_xdg_surface_configure};

static void registry_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version) {
    auto* ctx = static_cast<WLContext*>(data);
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        ctx->compositor = (wl_compositor*)wl_registry_bind(registry, name, &wl_compositor_interface, 4);
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        ctx->wm_base = (xdg_wm_base*)wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(ctx->wm_base, &wm_base_listener, ctx);
    } else if (strcmp(interface, wl_seat_interface.name) == 0) {
        ctx->seat = (wl_seat*)wl_registry_bind(registry, name, &wl_seat_interface, 7);
        wl_seat_add_listener(ctx->seat, &seat_listener, ctx);
    } else if (strcmp(interface, zxdg_decoration_manager_v1_interface.name) == 0) {
        ctx->decoration_manager = (zxdg_decoration_manager_v1*)wl_registry_bind(registry, name, &zxdg_decoration_manager_v1_interface, 1);
    }
}

static void registry_remove(void *data, struct wl_registry *registry, uint32_t name) {}
static const struct wl_registry_listener registry_listener = {registry_global, registry_remove};

int wl_init(struct WLContext *ctx, int width, int height);

#endif // __WAYLAND_BACKEND_H