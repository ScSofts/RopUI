#include "wayland_backend.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

// static void handle_ping(void *data, struct xdg_wm_base *wm, uint32_t serial) {
//   xdg_wm_base_pong(wm, serial);
// }

// static const struct xdg_wm_base_listener wm_base_listener = {.ping =
//                                                                  handle_ping};

// static void handle_xdg_surface_configure(void *data,
//                                          struct xdg_surface *xdg_surface,
//                                          uint32_t serial) {
//   struct WLContext *ctx = reinterpret_cast<WLContext*>(data);
//   xdg_surface_ack_configure(xdg_surface, serial);
// }

// static const struct xdg_surface_listener xdg_surface_listener = {
//     .configure = handle_xdg_surface_configure};

// static void handle_xdg_toplevel_configure(void *data,
//                                           struct xdg_toplevel *toplevel,
//                                           int32_t w, int32_t h,
//                                           struct wl_array *states) {
//   // Resize 可在此处理
// }

// static void handle_xdg_toplevel_close(void *data,
//                                       struct xdg_toplevel *toplevel) {
//   exit(0);
// }

// static const struct xdg_toplevel_listener xdg_toplevel_listener = {
//     .configure = handle_xdg_toplevel_configure,
//     .close = handle_xdg_toplevel_close};

// // ---------------- Registry ------------------

// static void registry_global(void *data, struct wl_registry *registry,
//                             uint32_t name, const char *interface,
//                             uint32_t version) {
//   struct WLContext *ctx = reinterpret_cast<WLContext*>(data);

//   if (strcmp(interface, wl_compositor_interface.name) == 0) {
//     ctx->compositor = reinterpret_cast<wl_compositor*>(
//         wl_registry_bind(registry, name, &wl_compositor_interface, 4));
//   } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
//     ctx->wm_base = reinterpret_cast<xdg_wm_base*>(wl_registry_bind(registry, name, &xdg_wm_base_interface, 1));
//     xdg_wm_base_add_listener(ctx->wm_base, &wm_base_listener, ctx);
//   } else if (strcmp(interface, zxdg_decoration_manager_v1_interface.name) ==
//              0) {
//     ctx->decoration_manager = reinterpret_cast<zxdg_decoration_manager_v1*>(wl_registry_bind(
//         registry, name, &zxdg_decoration_manager_v1_interface, 1));
//   }
// }

// static void registry_remove(void *data, struct wl_registry *registry,
//                             uint32_t name) {
//   // ignore
// }

// static const struct wl_registry_listener registry_listener = {
//     .global = registry_global, .global_remove = registry_remove};

// // ---------------- Public API ------------------

int wl_init(struct WLContext *ctx, int width, int height) {
  memset(ctx, 0, sizeof(*ctx));
  ctx->width = width;
  ctx->height = height;

  ctx->display = wl_display_connect(NULL);
  if (!ctx->display) {
    fprintf(stderr, "Failed to connect to Wayland.\n");
    return 0;
  }

  struct wl_registry *registry = wl_display_get_registry(ctx->display);
  wl_registry_add_listener(registry, &registry_listener, ctx);
  wl_display_roundtrip(ctx->display);

  ctx->surface = wl_compositor_create_surface(ctx->compositor);

  ctx->xdg_surface = xdg_wm_base_get_xdg_surface(ctx->wm_base, ctx->surface);
  xdg_surface_add_listener(ctx->xdg_surface, &xdg_surface_listener, ctx);

  // After creating xdg_toplevel
  ctx->xdg_toplevel = xdg_surface_get_toplevel(ctx->xdg_surface);
  xdg_toplevel_set_title(ctx->xdg_toplevel, "NanoUI Wayland Window");

  // If compositor supports server-side decoration
  if (ctx->decoration_manager) {
    ctx->decoration = zxdg_decoration_manager_v1_get_toplevel_decoration(
        ctx->decoration_manager, ctx->xdg_toplevel);

    // Request SSD
    zxdg_toplevel_decoration_v1_set_mode(
        ctx->decoration, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
  }

  wl_surface_commit(ctx->surface);
  wl_display_roundtrip(ctx->display);

  return 1;
}

// void wl_dispatch(struct WLContext *ctx) {
//   wl_display_dispatch_pending(ctx->display);
//   wl_display_flush(ctx->display);
// }