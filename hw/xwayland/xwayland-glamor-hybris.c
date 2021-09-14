/*
 * Copyright © 2011-2014 Intel Corporation
 * Copyright © 2017 Red Hat Inc.
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including
 * the next paragraph) shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *    Lyude Paul <lyude@redhat.com>
 *
 */

#include "xwayland.h"

#include <fcntl.h>
#include <sys/stat.h>

#define MESA_EGL_NO_X11_HEADERS
#define EGL_NO_X11
#include <glamor_egl.h>

#include <glamor.h>
#include <glamor_priv.h>

#include <wayland-client.h>
#include <wayland-egl.h>
#include "wayland-android-client-protocol.h"

#define DRIHYBRIS
#ifdef DRIHYBRIS
#include <xorg/drihybris.h>
#include <hybris/eglplatformcommon/hybris_nativebufferext.h>
#endif

struct xwl_pixmap {
    struct wl_buffer *buffer;
    EGLClientBuffer buf;
    EGLImage image;
    unsigned int texture;
    int stride;
    int format;
};

struct glamor_egl_screen_private {
    PFNEGLHYBRISCREATENATIVEBUFFERPROC eglHybrisCreateNativeBuffer;
    PFNEGLHYBRISRELEASENATIVEBUFFERPROC eglHybrisReleaseNativeBuffer;
    PFNEGLHYBRISCREATEREMOTEBUFFERPROC eglHybrisCreateRemoteBuffer;
    PFNEGLHYBRISGETNATIVEBUFFERINFOPROC eglHybrisGetNativeBufferInfo;
    PFNEGLHYBRISSERIALIZENATIVEBUFFERPROC eglHybrisSerializeNativeBuffer;
    PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR;
    struct android_wlegl * android_wlegl;
};

static struct glamor_egl_screen_private *glamor_egl = NULL;

static PixmapPtr
xwl_glamor_hybris_create_pixmap_for_native_buffer(ScreenPtr screen,  EGLClientBuffer buf, int width, int height,
                                    int depth, int format, int stride)
{
    PixmapPtr pixmap;
    struct xwl_pixmap *xwl_pixmap;
    struct xwl_screen *xwl_screen = xwl_screen_get(screen);
    
    xwl_pixmap = malloc(sizeof *xwl_pixmap);
    if (xwl_pixmap == NULL)
        return NULL;

    pixmap = glamor_create_pixmap(screen,
                                  width,
                                  height,
                                  depth,
                                  GLAMOR_CREATE_PIXMAP_NO_TEXTURE);
    if (!pixmap) {
        free(xwl_pixmap);
        return NULL;
    }

    xwl_glamor_egl_make_current(xwl_screen);
    xwl_pixmap->buf = buf;
    xwl_pixmap->buffer = NULL;
    xwl_pixmap->stride = stride;
    xwl_pixmap->format = format;
    xwl_pixmap->image = eglCreateImageKHR(xwl_screen->egl_display,
                                          EGL_NO_CONTEXT,
                                          EGL_NATIVE_BUFFER_HYBRIS,
                                          xwl_pixmap->buf, NULL);
    if (xwl_pixmap->image == EGL_NO_IMAGE_KHR)
        goto error;

    glGenTextures(1, &xwl_pixmap->texture);
    glBindTexture(GL_TEXTURE_2D, xwl_pixmap->texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, xwl_pixmap->image);
    if (eglGetError() != EGL_SUCCESS)
      goto error;

    glBindTexture(GL_TEXTURE_2D, 0);

    glamor_set_pixmap_texture(pixmap, xwl_pixmap->texture);
    /* `set_pixmap_texture()` may fail silently if the FBO creation failed,
     * so we check again the texture to be sure it worked.
     */
    if (!glamor_get_pixmap_texture(pixmap))
      goto error;

    glamor_set_pixmap_type(pixmap, GLAMOR_TEXTURE_DRM);
    xwl_pixmap_set_private(pixmap, xwl_pixmap);

    return pixmap;

error:
    if (xwl_pixmap->image != EGL_NO_IMAGE_KHR)
      eglDestroyImageKHR(xwl_screen->egl_display, xwl_pixmap->image);
    if (pixmap)
      glamor_destroy_pixmap(pixmap);
    free(xwl_pixmap);

    return NULL;

}

static PixmapPtr
xwl_glamor_hybris_create_pixmap(ScreenPtr screen,
                             int width, int height, int depth,
                             unsigned int hint)
{
    struct xwl_screen *xwl_screen = xwl_screen_get(screen);
    PixmapPtr pixmap = NULL;

    if (width > 0 && height > 0 && depth >= 15 &&
        (hint == 0 ||
         hint == CREATE_PIXMAP_USAGE_BACKING_PIXMAP ||
         hint == CREATE_PIXMAP_USAGE_SHARED)) {
        int m_format = HYBRIS_PIXEL_FORMAT_RGBA_8888;
        int m_usage = HYBRIS_USAGE_HW_RENDER;
        EGLint stride = 0;

        EGLClientBuffer buf;
        glamor_egl->eglHybrisCreateNativeBuffer(width, height,
                                                m_usage,
                                                m_format,
                                                &stride, &buf);
        pixmap = xwl_glamor_hybris_create_pixmap_for_native_buffer(screen, buf, width, height, depth, m_format, (uint32_t) stride);
        if (pixmap && xwl_screen->rootless && hint == CREATE_PIXMAP_USAGE_BACKING_PIXMAP) {
            glamor_clear_pixmap(pixmap);
        }
    }

    if (!pixmap)
        pixmap = glamor_create_pixmap(screen, width, height, depth, hint);

    return pixmap;

}

static Bool
xwl_glamor_hybris_destroy_pixmap(PixmapPtr pixmap)
{
    struct xwl_screen *xwl_screen = xwl_screen_get(pixmap->drawable.pScreen);
    struct xwl_pixmap *xwl_pixmap = xwl_pixmap_get(pixmap);

    if (xwl_pixmap && pixmap->refcnt == 1) {
        if (xwl_pixmap->buffer)
            wl_buffer_destroy(xwl_pixmap->buffer);
	
        eglDestroyImageKHR(xwl_screen->egl_display, xwl_pixmap->image);
        if (xwl_pixmap->buf)
            glamor_egl->eglHybrisReleaseNativeBuffer(xwl_pixmap->buf);

        free(xwl_pixmap);
        xwl_pixmap_set_private(pixmap, NULL);
    }

    return fbDestroyPixmap(pixmap);
}

static struct wl_buffer *
xwl_glamor_hybris_get_wl_buffer_for_pixmap(PixmapPtr pixmap,
                                        Bool *created)
{
    int numInts = 0;
    int numFds = 0;
    int *ints = NULL;
    int *fds = NULL;

    int width =  pixmap->drawable.width;
    int height =  pixmap->drawable.height;
    
    struct android_wlegl_handle *wlegl_handle;
    struct wl_array wl_ints;
    int *the_ints;

    struct xwl_pixmap *xwl_pixmap = xwl_pixmap_get(pixmap);
    
    if (xwl_pixmap == NULL)
       return NULL;

    if (xwl_pixmap->buffer) {
        /* Buffer already exists. Return it and inform caller if interested. */
        if (created)
            *created = FALSE;
        return xwl_pixmap->buffer;
    }

    if (created)
        *created = TRUE;

    if (!xwl_pixmap->buf)
       return NULL;

    glamor_egl->eglHybrisGetNativeBufferInfo(xwl_pixmap->buf, &numInts, &numFds);

    ints = malloc(numInts * sizeof(int));
    fds = malloc(numFds * sizeof(int));

    glamor_egl->eglHybrisSerializeNativeBuffer(xwl_pixmap->buf, ints, fds);

    wl_array_init(&wl_ints);
    the_ints = (int *)wl_array_add(&wl_ints, numInts * sizeof(int));
    memcpy(the_ints, ints, numInts * sizeof(int));
    wlegl_handle = android_wlegl_create_handle(glamor_egl->android_wlegl, numFds, &wl_ints);
    wl_array_release(&wl_ints);

    for (int i = 0; i < numFds; i++) {
        android_wlegl_handle_add_fd(wlegl_handle, fds[i]);
    }

    xwl_pixmap->buffer = android_wlegl_create_buffer(glamor_egl->android_wlegl, width, height, xwl_pixmap->stride, xwl_pixmap->format, HYBRIS_USAGE_HW_RENDER, wlegl_handle);
    android_wlegl_handle_destroy(wlegl_handle);

    return xwl_pixmap->buffer;
}

static Bool
xwl_glamor_hybris_init_wl_registry(struct xwl_screen *xwl_screen,
                                struct wl_registry *wl_registry,
                                uint32_t id, const char *name,
                                uint32_t version)
{
    if(strcmp(name, "android_wlegl") == 0) {
        glamor_egl->android_wlegl = wl_registry_bind(wl_registry, id,
                                                     &android_wlegl_interface, version);
        return TRUE;
    }

    /* no match */
    return FALSE;
}

static Bool
xwl_glamor_hybris_has_wl_interfaces(struct xwl_screen *xwl_screen)
{
    return TRUE;
}

static Bool
hybris_init_hybris_native_buffer(struct xwl_screen *xwl_screen)
{
    glamor_egl->eglHybrisCreateNativeBuffer = (PFNEGLHYBRISCREATENATIVEBUFFERPROC) eglGetProcAddress("eglHybrisCreateNativeBuffer");
    assert(glamor_egl->eglHybrisCreateNativeBuffer != NULL);

    glamor_egl->eglHybrisCreateRemoteBuffer = (PFNEGLHYBRISCREATEREMOTEBUFFERPROC) eglGetProcAddress("eglHybrisCreateRemoteBuffer");
    assert(glamor_egl->eglHybrisCreateRemoteBuffer != NULL);

    glamor_egl->eglHybrisReleaseNativeBuffer = (PFNEGLHYBRISRELEASENATIVEBUFFERPROC) eglGetProcAddress("eglHybrisReleaseNativeBuffer");
    assert(glamor_egl->eglHybrisReleaseNativeBuffer != NULL);

    glamor_egl->eglHybrisGetNativeBufferInfo = (PFNEGLHYBRISGETNATIVEBUFFERINFOPROC) eglGetProcAddress("eglHybrisGetNativeBufferInfo");
    assert(glamor_egl->eglHybrisGetNativeBufferInfo != NULL);

    glamor_egl->eglHybrisSerializeNativeBuffer = (PFNEGLHYBRISSERIALIZENATIVEBUFFERPROC) eglGetProcAddress("eglHybrisSerializeNativeBuffer");
    assert(glamor_egl->eglHybrisSerializeNativeBuffer != NULL);

    glamor_egl->eglCreateImageKHR = (PFNEGLCREATEIMAGEKHRPROC) eglGetProcAddress("eglCreateImageKHR");
    return TRUE;
}

static Bool
xwl_glamor_hybris_init_egl(struct xwl_screen *xwl_screen)
{
    EGLint config_attribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };

    const EGLint config_attribs_gles2[] = {
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24,
        EGL_STENCIL_SIZE, 8,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
            EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
            EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_NONE
    };

    EGLint num_configs;
    EGLConfig egl_config;

    int fd = 0;
    EGLint major, minor;
    xwl_screen->egl_display = eglGetDisplay((EGLNativeDisplayType) (intptr_t) fd);

    if (!eglInitialize
        (xwl_screen->egl_display, &major, &minor)) {
        xwl_screen->egl_display = EGL_NO_DISPLAY;
        goto error;
    }

    eglBindAPI(EGL_OPENGL_ES_API);
    if (!eglChooseConfig(xwl_screen->egl_display , config_attribs_gles2, 0, 0, &num_configs)) {
        ErrorF("eglChooseConfig Fail to get Confings\n");
        return false;
    }

    if (!eglChooseConfig(xwl_screen->egl_display, config_attribs_gles2, &egl_config, 1, &num_configs)) {
        ErrorF("Fail to get Config, num_configs=%d\n",num_configs);
        return false;
    }
    xwl_screen->egl_context = eglCreateContext(xwl_screen->egl_display,
                                           egl_config, EGL_NO_CONTEXT,
                                           config_attribs);
    if (xwl_screen->egl_context == EGL_NO_CONTEXT) {
        goto error;
    }

    if (!epoxy_has_gl_extension("GL_OES_EGL_image")) {
        ErrorF("GL_OES_EGL_image not available\n");
        goto error;
    }

    if (!eglMakeCurrent(xwl_screen->egl_display,
                        EGL_NO_SURFACE, EGL_NO_SURFACE, xwl_screen->egl_context)) {
        goto error;
    }

    hybris_init_hybris_native_buffer(xwl_screen);
    return TRUE;

error:
    if (xwl_screen->egl_context != EGL_NO_CONTEXT) {
        eglDestroyContext(xwl_screen->egl_display, xwl_screen->egl_context);
        xwl_screen->egl_context = EGL_NO_CONTEXT;
    }

    if (xwl_screen->egl_display != EGL_NO_DISPLAY) {
        eglTerminate(xwl_screen->egl_display);
        xwl_screen->egl_display = EGL_NO_DISPLAY;
    }

    free(glamor_egl);
    return FALSE;
}

static Bool
glamor_back_pixmap_from_hybris_buffer(ScreenPtr screen, PixmapPtr * pixmap,
                           CARD16 width,
                           CARD16 height,
                           CARD16 stride, CARD8 depth, CARD8 bpp,
                           int numInts, int *ints,
                           int numFds, int *fds)
{
    int format = HYBRIS_PIXEL_FORMAT_RGBA_8888;
    EGLClientBuffer buf;

    if (bpp != 32 || !(depth == 24 || depth == 32) || width == 0 || height == 0)
        return FALSE;

    glamor_egl->eglHybrisCreateRemoteBuffer(width, height, HYBRIS_USAGE_HW_TEXTURE,
                                            format, stride,
                                            numInts, ints, numFds, fds, &buf);

    *pixmap = xwl_glamor_hybris_create_pixmap_for_native_buffer(screen, buf, width, height, depth, format, stride);
    if (!*pixmap)
        return FALSE;

    return TRUE;
}

static PixmapPtr
glamor_pixmap_from_hybris_buffer(ScreenPtr screen,
                      CARD16 width,
                      CARD16 height,
                      CARD16 stride, CARD8 depth, CARD8 bpp,
                      int numInts, int *ints,
                      int numFds, int *fds)
{
    PixmapPtr pixmap;
    Bool ret;

    ret = glamor_back_pixmap_from_hybris_buffer(screen, &pixmap, width, height,
                                     stride, depth, bpp,
                                     numInts, ints,
                                     numFds, fds);

    if (ret == FALSE) {
        screen->DestroyPixmap(pixmap);
        return NULL;
    }
    return pixmap;
}

static int
glamor_hybris_buffer_from_pixmap(ScreenPtr screen,
                            PixmapPtr pixmap, CARD16 *stride,
                            int *numInts, int **ints,
                            int *numFds, int **fds)
{
    struct xwl_pixmap *xwl_pixmap = xwl_pixmap_get(pixmap);

    glamor_egl->eglHybrisGetNativeBufferInfo(xwl_pixmap->buffer, numInts, numFds);

    *ints = malloc(*numInts * sizeof(int));
    *fds = malloc(*numFds * sizeof(int));

    glamor_egl->eglHybrisSerializeNativeBuffer(xwl_pixmap->buffer, *ints, *fds);
    return 0;
}

static drihybris_screen_info_rec glamor_drihybris_info = {
    .version = 1,
    .pixmap_from_buffer = glamor_pixmap_from_hybris_buffer,
    .buffer_from_pixmap = glamor_hybris_buffer_from_pixmap,
};

static Bool
xwl_glamor_hybris_init_screen(struct xwl_screen *xwl_screen)
{
    Bool ret;
    ret = drihybris_screen_init(xwl_screen->screen, &glamor_drihybris_info);

    xwl_screen->screen->CreatePixmap = xwl_glamor_hybris_create_pixmap;
    xwl_screen->screen->DestroyPixmap = xwl_glamor_hybris_destroy_pixmap;
    return ret;
}

void
xwl_glamor_init_hybris(struct xwl_screen *xwl_screen)
{
    glamor_egl = calloc(sizeof(*glamor_egl), 1);

    xwl_screen->glamor_hybris_backend.is_available = FALSE;
    drihybris_extension_init();

    xwl_screen->glamor_hybris_backend.init_wl_registry = xwl_glamor_hybris_init_wl_registry;
    xwl_screen->glamor_hybris_backend.has_wl_interfaces = xwl_glamor_hybris_has_wl_interfaces;
    xwl_screen->glamor_hybris_backend.init_egl = xwl_glamor_hybris_init_egl;
    xwl_screen->glamor_hybris_backend.init_screen = xwl_glamor_hybris_init_screen;
    xwl_screen->glamor_hybris_backend.get_wl_buffer_for_pixmap = xwl_glamor_hybris_get_wl_buffer_for_pixmap;
    xwl_screen->glamor_hybris_backend.is_available = TRUE;
}
