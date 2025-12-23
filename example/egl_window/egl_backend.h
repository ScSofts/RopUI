#ifndef __EGL_BACKEND_H
#define __EGL_BACKEND_H
#include <glad/egl.h>
#include <wayland-egl.h>
#include <platform/linux/wayland/wayland_backend.h>

struct EGLContextWl {
    EGLDisplay display;
    EGLConfig  config;
    EGLContext context;
    EGLSurface surface;
    struct wl_egl_window* wl_window;
};

int egl_init(struct EGLContextWl* egl, struct WLContext* wl);
void egl_swap(struct EGLContextWl* egl);

#endif // __EGL_BACKEND_H

// egl wayland