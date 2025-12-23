#include "egl_backend.h"

#include <glad/egl.h>
#include <string.h>

int egl_init(struct EGLContextWl* egl, struct WLContext* wl)
{
    memset(egl, 0, sizeof(*egl));

    egl->display = eglGetDisplay((EGLNativeDisplayType) wl->display);
    eglInitialize(egl->display, NULL, NULL);

    // Load EGL function pointers
    gladLoadEGL();

    EGLint attribs[] = {
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_NONE
    };

    EGLint num;
    eglChooseConfig(egl->display, attribs, &egl->config, 1, &num);

    EGLint ctx_attribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };

    eglBindAPI(EGL_OPENGL_ES_API);

    egl->context = eglCreateContext(egl->display, egl->config, EGL_NO_CONTEXT, ctx_attribs);

    // Wayland EGL Window
    egl->wl_window = wl_egl_window_create(wl->surface, wl->width, wl->height);

    egl->surface = eglCreateWindowSurface(
        egl->display,
        egl->config,
        (EGLNativeWindowType) egl->wl_window,
        NULL);

    eglMakeCurrent(egl->display, egl->surface, egl->surface, egl->context);

    return 1;
}

void egl_swap(struct EGLContextWl* egl)
{
    eglSwapBuffers(egl->display, egl->surface);
}