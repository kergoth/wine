/*
 * X11DRV OpenGL functions
 *
 * Copyright 2000 Lionel Ulmer
 * Copyright 2005 Alex Woods
 * Copyright 2005 Raphael Junqueira
 * Copyright 2006-2009 Roderick Colenbrander
 * Copyright 2006 Tomas Carnecky
 * Copyright 2013 Matteo Bruni
 * Copyright 2012, 2013, 2014 Alexandre Julliard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "config.h"
#include "wine/port.h"

#include <assert.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include "android.h"
#include "winternl.h"

#define GLAPIENTRY /* nothing */
#include "wine/wgl.h"
#undef GLAPIENTRY
#include "wine/wgl_driver.h"
#include "wine/wglext.h"
#include "wine/library.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(android);

#define SONAME_LIBEGL "libEGL.so"
#define SONAME_LIBGLES "libGLESv2.so"

#define DECL_FUNCPTR(f) typeof(f) * p_##f = NULL
DECL_FUNCPTR( eglCreateContext );
DECL_FUNCPTR( eglCreateWindowSurface );
DECL_FUNCPTR( eglDestroyContext );
DECL_FUNCPTR( eglDestroySurface );
DECL_FUNCPTR( eglGetConfigAttrib );
DECL_FUNCPTR( eglGetConfigs );
DECL_FUNCPTR( eglGetDisplay );
DECL_FUNCPTR( eglGetProcAddress );
DECL_FUNCPTR( eglInitialize );
DECL_FUNCPTR( eglMakeCurrent );
DECL_FUNCPTR( eglSwapBuffers );
DECL_FUNCPTR( eglSwapInterval );
#undef DECL_FUNCPTR

static const int egl_client_version = 2;

struct wgl_pixel_format
{
    EGLConfig config;
};

struct wgl_context
{
    EGLConfig  config;
    EGLContext context;
    EGLSurface surface;
};

struct gl_drawable
{
    struct list     entry;
    HWND            hwnd;
    HDC             hdc;
    int             format;
    ANativeWindow  *window;
    EGLSurface      surface;
};

static void *egl_handle;
static void *opengl_handle;
static struct wgl_pixel_format *pixel_formats;
static int nb_pixel_formats, nb_onscreen_formats;
static EGLDisplay display;
static int swap_interval;
static char wgl_extensions[4096];
static struct opengl_funcs egl_funcs;

static struct list gl_drawables = LIST_INIT( gl_drawables );

static CRITICAL_SECTION drawable_section;
static CRITICAL_SECTION_DEBUG critsect_debug =
{
    0, 0, &drawable_section,
    { &critsect_debug.ProcessLocksList, &critsect_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": drawable_section") }
};
static CRITICAL_SECTION drawable_section = { &critsect_debug, -1, 0, 0, 0, 0 };

static inline BOOL is_valid_pixel_format( int format )
{
    return format > 0 && format <= nb_pixel_formats;
}

static inline BOOL is_onscreen_pixel_format( int format )
{
    return format > 0 && format <= nb_onscreen_formats;
}

static struct gl_drawable *create_gl_drawable( HWND hwnd, HDC hdc, int format )
{
    struct gl_drawable *gl = HeapAlloc( GetProcessHeap(), 0, sizeof(*gl) );
    gl->hwnd   = hwnd;
    gl->hdc    = hdc;
    gl->format = format;
    gl->window = get_ioctl_window( hwnd );
    gl->surface = p_eglCreateWindowSurface( display, pixel_formats[format - 1].config, gl->window, NULL );
    if (gl->surface == EGL_NO_SURFACE) ERR("Failed to create EGL surface.\n");
    EnterCriticalSection( &drawable_section );
    list_add_head( &gl_drawables, &gl->entry );
    return gl;
}

static struct gl_drawable *get_gl_drawable( HWND hwnd, HDC hdc )
{
    struct gl_drawable *gl;

    EnterCriticalSection( &drawable_section );
    LIST_FOR_EACH_ENTRY( gl, &gl_drawables, struct gl_drawable, entry )
    {
        if (hwnd && gl->hwnd == hwnd) return gl;
        if (hdc && gl->hdc == hdc) return gl;
    }
    LeaveCriticalSection( &drawable_section );
    return NULL;
}

static void release_gl_drawable( struct gl_drawable *gl )
{
    if (gl) LeaveCriticalSection( &drawable_section );
}

void destroy_gl_drawable( HWND hwnd )
{
    struct gl_drawable *gl;

    EnterCriticalSection( &drawable_section );
    LIST_FOR_EACH_ENTRY( gl, &gl_drawables, struct gl_drawable, entry )
    {
        if (gl->hwnd != hwnd) continue;
        list_remove( &gl->entry );
        p_eglDestroySurface( display, gl->surface );
        release_ioctl_window( gl->window );
        HeapFree( GetProcessHeap(), 0, gl );
        break;
    }
    LeaveCriticalSection( &drawable_section );
}

static BOOL set_pixel_format( HDC hdc, int format, BOOL allow_change )
{
    struct gl_drawable *gl;
    HWND hwnd = WindowFromDC( hdc );
    int prev = 0;

    if (!hwnd || hwnd == GetDesktopWindow())
    {
        WARN( "not a proper window DC %p/%p\n", hdc, hwnd );
        return FALSE;
    }
    if (!is_onscreen_pixel_format( format ))
    {
        WARN( "Invalid format %d\n", format );
        return FALSE;
    }
    TRACE( "%p/%p format %d\n", hdc, hwnd, format );

    if ((gl = get_gl_drawable( hwnd, 0 )))
    {
        prev = gl->format;
        if (allow_change)
        {
            EGLint pf;
            p_eglGetConfigAttrib( display, pixel_formats[format - 1].config, EGL_NATIVE_VISUAL_ID, &pf );
            gl->window->perform( gl->window, NATIVE_WINDOW_SET_BUFFERS_FORMAT, pf );
            gl->format = format;
        }
    }
    else gl = create_gl_drawable( hwnd, 0, format );

    release_gl_drawable( gl );

    if (prev && prev != format && !allow_change) return FALSE;
    if (__wine_set_pixel_format( hwnd, format )) return TRUE;
    destroy_gl_drawable( hwnd );
    return FALSE;
}

static struct wgl_context *create_context( HDC hdc, struct wgl_context *share, const int *attribs )
{
    struct gl_drawable *gl;
    struct wgl_context *ctx;

    if (!(gl = get_gl_drawable( WindowFromDC( hdc ), hdc ))) return NULL;

    ctx = HeapAlloc( GetProcessHeap(), 0, sizeof(*ctx) );

    ctx->config  = pixel_formats[gl->format - 1].config;
    ctx->surface = 0;
    ctx->context = p_eglCreateContext( display, ctx->config,
                                       share ? share->context : EGL_NO_CONTEXT, attribs );
    TRACE( "%p fmt %d ctx %p\n", hdc, gl->format, ctx->context );
    release_gl_drawable( gl );
    return ctx;
}

static void dump_PIXELFORMATDESCRIPTOR(const PIXELFORMATDESCRIPTOR *ppfd)
{
    TRACE("  - size / version : %d / %d\n", ppfd->nSize, ppfd->nVersion);
    TRACE("  - dwFlags : ");
#define TEST_AND_DUMP(t,tv) if ((t) & (tv)) TRACE(#tv " ")
    TEST_AND_DUMP(ppfd->dwFlags, PFD_DEPTH_DONTCARE);
    TEST_AND_DUMP(ppfd->dwFlags, PFD_DOUBLEBUFFER);
    TEST_AND_DUMP(ppfd->dwFlags, PFD_DOUBLEBUFFER_DONTCARE);
    TEST_AND_DUMP(ppfd->dwFlags, PFD_DRAW_TO_WINDOW);
    TEST_AND_DUMP(ppfd->dwFlags, PFD_DRAW_TO_BITMAP);
    TEST_AND_DUMP(ppfd->dwFlags, PFD_GENERIC_ACCELERATED);
    TEST_AND_DUMP(ppfd->dwFlags, PFD_GENERIC_FORMAT);
    TEST_AND_DUMP(ppfd->dwFlags, PFD_NEED_PALETTE);
    TEST_AND_DUMP(ppfd->dwFlags, PFD_NEED_SYSTEM_PALETTE);
    TEST_AND_DUMP(ppfd->dwFlags, PFD_STEREO);
    TEST_AND_DUMP(ppfd->dwFlags, PFD_STEREO_DONTCARE);
    TEST_AND_DUMP(ppfd->dwFlags, PFD_SUPPORT_GDI);
    TEST_AND_DUMP(ppfd->dwFlags, PFD_SUPPORT_OPENGL);
    TEST_AND_DUMP(ppfd->dwFlags, PFD_SWAP_COPY);
    TEST_AND_DUMP(ppfd->dwFlags, PFD_SWAP_EXCHANGE);
    TEST_AND_DUMP(ppfd->dwFlags, PFD_SWAP_LAYER_BUFFERS);
    /* PFD_SUPPORT_COMPOSITION is new in Vista, it is similar to composition
     * under X e.g. COMPOSITE + GLX_EXT_TEXTURE_FROM_PIXMAP. */
    TEST_AND_DUMP(ppfd->dwFlags, PFD_SUPPORT_COMPOSITION);
#undef TEST_AND_DUMP
    TRACE("\n");

    TRACE("  - iPixelType : ");
    switch (ppfd->iPixelType) {
    case PFD_TYPE_RGBA: TRACE("PFD_TYPE_RGBA"); break;
    case PFD_TYPE_COLORINDEX: TRACE("PFD_TYPE_COLORINDEX"); break;
    }
    TRACE("\n");

    TRACE("  - Color   : %d\n", ppfd->cColorBits);
    TRACE("  - Red     : %d\n", ppfd->cRedBits);
    TRACE("  - Green   : %d\n", ppfd->cGreenBits);
    TRACE("  - Blue    : %d\n", ppfd->cBlueBits);
    TRACE("  - Alpha   : %d\n", ppfd->cAlphaBits);
    TRACE("  - Accum   : %d\n", ppfd->cAccumBits);
    TRACE("  - Depth   : %d\n", ppfd->cDepthBits);
    TRACE("  - Stencil : %d\n", ppfd->cStencilBits);
    TRACE("  - Aux     : %d\n", ppfd->cAuxBuffers);

    TRACE("  - iLayerType : ");
    switch (ppfd->iLayerType) {
    case PFD_MAIN_PLANE: TRACE("PFD_MAIN_PLANE"); break;
    case PFD_OVERLAY_PLANE: TRACE("PFD_OVERLAY_PLANE"); break;
    case (BYTE)PFD_UNDERLAY_PLANE: TRACE("PFD_UNDERLAY_PLANE"); break;
    }
    TRACE("\n");
}

/***********************************************************************
 *		android_wglGetExtensionsStringARB
 */
static const char *android_wglGetExtensionsStringARB( HDC hdc )
{
    TRACE( "() returning \"%s\"\n", wgl_extensions );
    return wgl_extensions;
}

/***********************************************************************
 *		android_wglGetExtensionsStringEXT
 */
static const char *android_wglGetExtensionsStringEXT(void)
{
    TRACE( "() returning \"%s\"\n", wgl_extensions );
    return wgl_extensions;
}

/***********************************************************************
 *		android_wglCreateContextAttribsARB
 */
static struct wgl_context *android_wglCreateContextAttribsARB( HDC hdc, struct wgl_context *share,
                                                               const int *attribs )
{
    int count = 0, egl_attribs[3];
    BOOL opengl_es = FALSE;

    while (attribs && *attribs && count < 2)
    {
        switch (*attribs)
        {
        case WGL_CONTEXT_PROFILE_MASK_ARB:
            if (attribs[1] == WGL_CONTEXT_ES2_PROFILE_BIT_EXT)
                opengl_es = TRUE;
            break;
        case WGL_CONTEXT_MAJOR_VERSION_ARB:
            egl_attribs[count++] = EGL_CONTEXT_CLIENT_VERSION;
            egl_attribs[count++] = attribs[1];
            break;
        default:
            FIXME("Unhandled attributes: %#x %#x\n", attribs[0], attribs[1]);
        }
        attribs += 2;
    }
    if (!opengl_es)
    {
        WARN("Requested creation of an OpenGL (non ES) context, that's not supported.\n");
        return NULL;
    }
    if (!count)  /* FIXME: force version if not specified */
    {
        egl_attribs[count++] = EGL_CONTEXT_CLIENT_VERSION;
        egl_attribs[count++] = egl_client_version;
    }
    egl_attribs[count] = EGL_NONE;

    return create_context( hdc, share, egl_attribs );
}

/***********************************************************************
 *		android_wglMakeContextCurrentARB
 */
static BOOL android_wglMakeContextCurrentARB( HDC draw_hdc, HDC read_hdc, struct wgl_context *ctx )
{
    BOOL ret = FALSE;
    struct gl_drawable *draw_gl, *read_gl = NULL;

    TRACE( "%p %p %p\n", draw_hdc, read_hdc, ctx );

    if (!ctx)
    {
        p_eglMakeCurrent( display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT );
        NtCurrentTeb()->glContext = NULL;
        return TRUE;
    }

    if ((draw_gl = get_gl_drawable( WindowFromDC( draw_hdc ), draw_hdc )))
    {
        read_gl = get_gl_drawable( WindowFromDC( read_hdc ), read_hdc );
        TRACE( "%p/%p context %p surface %p/%p\n",
               draw_hdc, read_hdc, ctx->context, draw_gl->surface, read_gl->surface );
        ret = p_eglMakeCurrent( display, draw_gl->surface, read_gl->surface, ctx->context );
        if (ret)
        {
            ctx->surface = draw_gl->surface;
            NtCurrentTeb()->glContext = ctx;
            goto done;
        }
    }
    SetLastError( ERROR_INVALID_HANDLE );

done:
    release_gl_drawable( read_gl );
    release_gl_drawable( draw_gl );
    return ret;
}

/***********************************************************************
 *		android_wglSwapIntervalEXT
 */
static BOOL android_wglSwapIntervalEXT( int interval )
{
    BOOL ret = TRUE;

    TRACE("(%d)\n", interval);

    if (interval < 0)
    {
        SetLastError(ERROR_INVALID_DATA);
        return FALSE;
    }

    ret = p_eglSwapInterval( display, interval );

    if (ret)
        swap_interval = interval;
    else
        SetLastError( ERROR_DC_NOT_FOUND );

    return ret;
}

/***********************************************************************
 *		android_wglGetSwapIntervalEXT
 */
static int android_wglGetSwapIntervalEXT(void)
{
    return swap_interval;
}

/***********************************************************************
 *		android_wglSetPixelFormatWINE
 */
static BOOL android_wglSetPixelFormatWINE( HDC hdc, int format )
{
    return set_pixel_format( hdc, format, TRUE );
}

/***********************************************************************
 *		android_wglCopyContext
 */
static BOOL android_wglCopyContext( struct wgl_context *src, struct wgl_context *dst, UINT mask )
{
    FIXME( "%p -> %p mask %#x unsupported\n", src, dst, mask );
    return FALSE;
}

/***********************************************************************
 *		android_wglCreateContext
 */
static struct wgl_context *android_wglCreateContext( HDC hdc )
{
    int egl_attribs[3] = { EGL_CONTEXT_CLIENT_VERSION, egl_client_version, EGL_NONE };

    return create_context( hdc, NULL, egl_attribs );
}

/***********************************************************************
 *		android_wglDeleteContext
 */
static void android_wglDeleteContext( struct wgl_context *ctx )
{
    p_eglDestroyContext( display, ctx->context );
    HeapFree( GetProcessHeap(), 0, ctx );
}

/***********************************************************************
 *		android_wglDescribePixelFormat
 */
static int android_wglDescribePixelFormat( HDC hdc, int fmt, UINT size, PIXELFORMATDESCRIPTOR *pfd )
{
    EGLint val;
    EGLConfig config;

    if (!pfd) return nb_onscreen_formats;
    if (!is_onscreen_pixel_format( fmt )) return 0;
    if (size < sizeof(*pfd)) return 0;
    config = pixel_formats[fmt - 1].config;

    memset( pfd, 0, sizeof(*pfd) );
    pfd->nSize = sizeof(*pfd);
    pfd->nVersion = 1;
    pfd->dwFlags = PFD_SUPPORT_OPENGL | PFD_DRAW_TO_WINDOW | PFD_DOUBLEBUFFER;
    pfd->iPixelType = PFD_TYPE_RGBA;
    pfd->iLayerType = PFD_MAIN_PLANE;

    p_eglGetConfigAttrib( display, config, EGL_BUFFER_SIZE, &val );
    pfd->cColorBits = val;
    p_eglGetConfigAttrib( display, config, EGL_RED_SIZE, &val );
    pfd->cRedBits = val;
    p_eglGetConfigAttrib( display, config, EGL_GREEN_SIZE, &val );
    pfd->cGreenBits = val;
    p_eglGetConfigAttrib( display, config, EGL_BLUE_SIZE, &val );
    pfd->cBlueBits = val;
    p_eglGetConfigAttrib( display, config, EGL_ALPHA_SIZE, &val );
    pfd->cAlphaBits = val;
    p_eglGetConfigAttrib( display, config, EGL_DEPTH_SIZE, &val );
    pfd->cDepthBits = val;
    p_eglGetConfigAttrib( display, config, EGL_STENCIL_SIZE, &val );
    pfd->cStencilBits = val;

    pfd->cAlphaShift = 0;
    pfd->cBlueShift = pfd->cAlphaShift + pfd->cAlphaBits;
    pfd->cGreenShift = pfd->cBlueShift + pfd->cBlueBits;
    pfd->cRedShift = pfd->cGreenShift + pfd->cGreenBits;

    dump_PIXELFORMATDESCRIPTOR( pfd );
    return nb_onscreen_formats;
}

/***********************************************************************
 *		android_wglGetPixelFormat
 */
static int android_wglGetPixelFormat( HDC hdc )
{
    struct gl_drawable *gl;
    int ret = 0;

    if ((gl = get_gl_drawable( WindowFromDC( hdc ), hdc )))
    {
        ret = gl->format;
        /* offscreen formats can't be used with traditional WGL calls */
        if (!is_onscreen_pixel_format( ret )) ret = 1;
        release_gl_drawable( gl );
    }
    return ret;
}

/***********************************************************************
 *		android_wglGetProcAddress
 */
static PROC android_wglGetProcAddress( LPCSTR name )
{
    PROC ret;
    if (!strncmp( name, "wgl", 3 )) return NULL;
    ret = (PROC)p_eglGetProcAddress( name );
    TRACE( "%s -> %p\n", name, ret );
    return ret;
}

/***********************************************************************
 *		android_wglMakeCurrent
 */
static BOOL android_wglMakeCurrent( HDC hdc, struct wgl_context *ctx )
{
    BOOL ret = FALSE;
    struct gl_drawable *gl;

    TRACE( "%p %p\n", hdc, ctx );

    if (!ctx)
    {
        p_eglMakeCurrent( display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT );
        NtCurrentTeb()->glContext = NULL;
        return TRUE;
    }

    if ((gl = get_gl_drawable( WindowFromDC( hdc ), hdc )))
    {
        TRACE( "%p context %p surface %p\n", hdc, ctx->context, gl->surface );
        ret = p_eglMakeCurrent( display, gl->surface, gl->surface, ctx->context );
        if (ret)
        {
            ctx->surface = gl->surface;
            NtCurrentTeb()->glContext = ctx;
            goto done;
        }
    }
    SetLastError( ERROR_INVALID_HANDLE );

done:
    release_gl_drawable( gl );
    return ret;
}

/***********************************************************************
 *		android_wglSetPixelFormat
 */
static BOOL android_wglSetPixelFormat( HDC hdc, int format, const PIXELFORMATDESCRIPTOR *pfd )
{
    return set_pixel_format( hdc, format, FALSE );
}

/***********************************************************************
 *		android_wglShareLists
 */
static BOOL android_wglShareLists( struct wgl_context *org, struct wgl_context *dest )
{
    FIXME( "%p %p\n", org, dest );
    return FALSE;
}

/***********************************************************************
 *		android_wglSwapBuffers
 */
static BOOL android_wglSwapBuffers( HDC hdc )
{
    struct wgl_context *ctx = NtCurrentTeb()->glContext;

    TRACE( "%p context %p surface %p\n", hdc, ctx->context, ctx->surface );
    if (ctx) p_eglSwapBuffers( display, ctx->surface );
    return TRUE;
}

static void register_extension( const char *ext )
{
    if (wgl_extensions[0]) strcat( wgl_extensions, " " );
    strcat( wgl_extensions, ext );
    TRACE( "%s\n", ext );
}

static void init_extensions(void)
{
    void *ptr;

    register_extension("WGL_ARB_create_context");
    register_extension("WGL_ARB_create_context_profile");
    egl_funcs.ext.p_wglCreateContextAttribsARB = android_wglCreateContextAttribsARB;

    register_extension("WGL_ARB_extensions_string");
    egl_funcs.ext.p_wglGetExtensionsStringARB = android_wglGetExtensionsStringARB;

    register_extension("WGL_ARB_make_current_read");
    egl_funcs.ext.p_wglGetCurrentReadDCARB   = (void *)1;  /* never called */
    egl_funcs.ext.p_wglMakeContextCurrentARB = android_wglMakeContextCurrentARB;

#if 0  /* FIXME */
    register_extension("WGL_ARB_pbuffer");
    egl_funcs.ext.p_wglCreatePbufferARB    = android_wglCreatePbufferARB;
    egl_funcs.ext.p_wglDestroyPbufferARB   = android_wglDestroyPbufferARB;
    egl_funcs.ext.p_wglGetPbufferDCARB     = android_wglGetPbufferDCARB;
    egl_funcs.ext.p_wglQueryPbufferARB     = android_wglQueryPbufferARB;
    egl_funcs.ext.p_wglReleasePbufferDCARB = android_wglReleasePbufferDCARB;
    egl_funcs.ext.p_wglSetPbufferAttribARB = android_wglSetPbufferAttribARB;
#endif

#if 0  /* FIXME */
    register_extension("WGL_ARB_pixel_format");
    egl_funcs.ext.p_wglChoosePixelFormatARB      = android_wglChoosePixelFormatARB;
    egl_funcs.ext.p_wglGetPixelFormatAttribfvARB = android_wglGetPixelFormatAttribfvARB;
    egl_funcs.ext.p_wglGetPixelFormatAttribivARB = android_wglGetPixelFormatAttribivARB;
#endif

    /* EXT Extensions */

    register_extension("WGL_EXT_extensions_string");
    egl_funcs.ext.p_wglGetExtensionsStringEXT = android_wglGetExtensionsStringEXT;

    register_extension("WGL_EXT_swap_control");
    egl_funcs.ext.p_wglSwapIntervalEXT = android_wglSwapIntervalEXT;
    egl_funcs.ext.p_wglGetSwapIntervalEXT = android_wglGetSwapIntervalEXT;

    register_extension("WGL_EXT_framebuffer_sRGB");

    /* WINE-specific WGL Extensions */

    /* In WineD3D we need the ability to set the pixel format more than once (e.g. after a device reset).
     * The default wglSetPixelFormat doesn't allow this, so add our own which allows it.
     */
    register_extension("WGL_WINE_pixel_format_passthrough");
    egl_funcs.ext.p_wglSetPixelFormatWINE = android_wglSetPixelFormatWINE;

    /* standard functions available through eglGetProcAddress */

#define USE_GL_FUNC(func) if ((ptr = p_eglGetProcAddress( #func ))) egl_funcs.gl.p_##func = ptr;
    ALL_WGL_FUNCS
#undef USE_GL_FUNC

    /* extensions exported from the OpenGL library */

#define LOAD_FUNCPTR(func) egl_funcs.ext.p_##func = wine_dlsym( opengl_handle, #func, NULL, 0 )
    LOAD_FUNCPTR( glActiveTexture );
    LOAD_FUNCPTR( glAttachShader );
    LOAD_FUNCPTR( glBindAttribLocation );
    LOAD_FUNCPTR( glBindBuffer );
    LOAD_FUNCPTR( glBindFramebuffer );
    LOAD_FUNCPTR( glBindRenderbuffer );
    LOAD_FUNCPTR( glBlendColor );
    LOAD_FUNCPTR( glBlendEquation );
    LOAD_FUNCPTR( glBlendEquationSeparate );
    LOAD_FUNCPTR( glBlendFuncSeparate );
    LOAD_FUNCPTR( glBufferData );
    LOAD_FUNCPTR( glBufferSubData );
    LOAD_FUNCPTR( glCheckFramebufferStatus );
    LOAD_FUNCPTR( glClearDepthf );
    LOAD_FUNCPTR( glCompileShader );
    LOAD_FUNCPTR( glCompressedTexImage2D );
    LOAD_FUNCPTR( glCompressedTexSubImage2D );
    LOAD_FUNCPTR( glCreateProgram );
    LOAD_FUNCPTR( glCreateShader );
    LOAD_FUNCPTR( glDeleteBuffers );
    LOAD_FUNCPTR( glDeleteFramebuffers );
    LOAD_FUNCPTR( glDeleteProgram );
    LOAD_FUNCPTR( glDeleteRenderbuffers );
    LOAD_FUNCPTR( glDeleteShader );
    LOAD_FUNCPTR( glDepthRangef );
    LOAD_FUNCPTR( glDetachShader );
    LOAD_FUNCPTR( glDisableVertexAttribArray );
    LOAD_FUNCPTR( glEnableVertexAttribArray );
    LOAD_FUNCPTR( glFramebufferRenderbuffer );
    LOAD_FUNCPTR( glFramebufferTexture2D );
    LOAD_FUNCPTR( glGenBuffers );
    LOAD_FUNCPTR( glGenFramebuffers );
    LOAD_FUNCPTR( glGenRenderbuffers );
    LOAD_FUNCPTR( glGenerateMipmap );
    LOAD_FUNCPTR( glGetActiveAttrib );
    LOAD_FUNCPTR( glGetActiveUniform );
    LOAD_FUNCPTR( glGetAttachedShaders );
    LOAD_FUNCPTR( glGetAttribLocation );
    LOAD_FUNCPTR( glGetBufferParameteriv );
    LOAD_FUNCPTR( glGetFramebufferAttachmentParameteriv );
    LOAD_FUNCPTR( glGetProgramInfoLog );
    LOAD_FUNCPTR( glGetProgramiv );
    LOAD_FUNCPTR( glGetRenderbufferParameteriv );
    LOAD_FUNCPTR( glGetShaderInfoLog );
    LOAD_FUNCPTR( glGetShaderPrecisionFormat );
    LOAD_FUNCPTR( glGetShaderSource );
    LOAD_FUNCPTR( glGetShaderiv );
    LOAD_FUNCPTR( glGetUniformLocation );
    LOAD_FUNCPTR( glGetUniformfv );
    LOAD_FUNCPTR( glGetUniformiv );
    LOAD_FUNCPTR( glGetVertexAttribPointerv );
    LOAD_FUNCPTR( glGetVertexAttribfv );
    LOAD_FUNCPTR( glGetVertexAttribiv );
    LOAD_FUNCPTR( glIsBuffer );
    LOAD_FUNCPTR( glIsFramebuffer );
    LOAD_FUNCPTR( glIsProgram );
    LOAD_FUNCPTR( glIsRenderbuffer );
    LOAD_FUNCPTR( glIsShader );
    LOAD_FUNCPTR( glLinkProgram );
    LOAD_FUNCPTR( glReleaseShaderCompiler );
    LOAD_FUNCPTR( glRenderbufferStorage );
    LOAD_FUNCPTR( glSampleCoverage );
    LOAD_FUNCPTR( glShaderBinary );
    LOAD_FUNCPTR( glShaderSource );
    LOAD_FUNCPTR( glStencilFuncSeparate );
    LOAD_FUNCPTR( glStencilMaskSeparate );
    LOAD_FUNCPTR( glStencilOpSeparate );
    LOAD_FUNCPTR( glUniform1f );
    LOAD_FUNCPTR( glUniform1fv );
    LOAD_FUNCPTR( glUniform1i );
    LOAD_FUNCPTR( glUniform1iv );
    LOAD_FUNCPTR( glUniform2f );
    LOAD_FUNCPTR( glUniform2fv );
    LOAD_FUNCPTR( glUniform2i );
    LOAD_FUNCPTR( glUniform2iv );
    LOAD_FUNCPTR( glUniform3f );
    LOAD_FUNCPTR( glUniform3fv );
    LOAD_FUNCPTR( glUniform3i );
    LOAD_FUNCPTR( glUniform3iv );
    LOAD_FUNCPTR( glUniform4f );
    LOAD_FUNCPTR( glUniform4fv );
    LOAD_FUNCPTR( glUniform4i );
    LOAD_FUNCPTR( glUniform4iv );
    LOAD_FUNCPTR( glUniformMatrix2fv );
    LOAD_FUNCPTR( glUniformMatrix3fv );
    LOAD_FUNCPTR( glUniformMatrix4fv );
    LOAD_FUNCPTR( glUseProgram );
    LOAD_FUNCPTR( glValidateProgram );
    LOAD_FUNCPTR( glVertexAttrib1f );
    LOAD_FUNCPTR( glVertexAttrib1fv );
    LOAD_FUNCPTR( glVertexAttrib2f );
    LOAD_FUNCPTR( glVertexAttrib2fv );
    LOAD_FUNCPTR( glVertexAttrib3f );
    LOAD_FUNCPTR( glVertexAttrib3fv );
    LOAD_FUNCPTR( glVertexAttrib4f );
    LOAD_FUNCPTR( glVertexAttrib4fv );
    LOAD_FUNCPTR( glVertexAttribPointer );
#undef LOAD_FUNCPTR
}

static BOOL egl_init(void)
{
    static int retval = -1;
    EGLConfig *configs;
    EGLint major, minor, count, i, pass;
    char buffer[200];

    if (retval != -1) return retval;
    retval = 0;

    if (!(egl_handle = wine_dlopen( SONAME_LIBEGL, RTLD_NOW|RTLD_GLOBAL, buffer, sizeof(buffer) )))
    {
        ERR( "failed to load %s: %s\n", SONAME_LIBEGL, buffer );
        return FALSE;
    }
    if (!(opengl_handle = wine_dlopen( SONAME_LIBGLES, RTLD_NOW|RTLD_GLOBAL, buffer, sizeof(buffer) )))
    {
        ERR( "failed to load %s: %s\n", SONAME_LIBGLES, buffer );
        return FALSE;
    }

#define LOAD_FUNCPTR(func) do { \
        if (!(p_##func = wine_dlsym( egl_handle, #func, NULL, 0 ))) \
        { ERR( "can't find symbol %s\n", #func); return FALSE; }    \
    } while(0)
    LOAD_FUNCPTR( eglCreateContext );
    LOAD_FUNCPTR( eglCreateWindowSurface );
    LOAD_FUNCPTR( eglDestroyContext );
    LOAD_FUNCPTR( eglDestroySurface );
    LOAD_FUNCPTR( eglGetConfigAttrib );
    LOAD_FUNCPTR( eglGetConfigs );
    LOAD_FUNCPTR( eglGetDisplay );
    LOAD_FUNCPTR( eglGetProcAddress );
    LOAD_FUNCPTR( eglInitialize );
    LOAD_FUNCPTR( eglMakeCurrent );
    LOAD_FUNCPTR( eglSwapBuffers );
    LOAD_FUNCPTR( eglSwapInterval );
#undef LOAD_FUNCPTR

    display = p_eglGetDisplay( EGL_DEFAULT_DISPLAY );
    if (!p_eglInitialize( display, &major, &minor )) return 0;
    TRACE( "display %p version %u.%u\n", display, major, minor );

    p_eglGetConfigs( display, NULL, 0, &count );
    configs = HeapAlloc( GetProcessHeap(), 0, count * sizeof(*configs) );
    pixel_formats = HeapAlloc( GetProcessHeap(), 0, count * sizeof(*pixel_formats) );
    p_eglGetConfigs( display, configs, count, &count );
    if (!count || !configs || !pixel_formats)
    {
        HeapFree( GetProcessHeap(), 0, configs );
        HeapFree( GetProcessHeap(), 0, pixel_formats );
        ERR( "eglGetConfigs returned no configs\n" );
        return 0;
    }

    for (pass = 0; pass < 2; pass++)
    {
        for (i = 0; i < count; i++)
        {
            EGLint id, type, visual_id, native, render, color, r, g, b, d, s;

            p_eglGetConfigAttrib( display, configs[i], EGL_SURFACE_TYPE, &type );
            if (!(type & EGL_WINDOW_BIT) == !pass) continue;
            p_eglGetConfigAttrib( display, configs[i], EGL_RENDERABLE_TYPE, &render );
            if (egl_client_version == 2 && !(render & EGL_OPENGL_ES2_BIT)) continue;

            pixel_formats[nb_pixel_formats++].config = configs[i];

            p_eglGetConfigAttrib( display, configs[i], EGL_CONFIG_ID, &id );
            p_eglGetConfigAttrib( display, configs[i], EGL_NATIVE_VISUAL_ID, &visual_id );
            p_eglGetConfigAttrib( display, configs[i], EGL_NATIVE_RENDERABLE, &native );
            p_eglGetConfigAttrib( display, configs[i], EGL_COLOR_BUFFER_TYPE, &color );
            p_eglGetConfigAttrib( display, configs[i], EGL_RED_SIZE, &r );
            p_eglGetConfigAttrib( display, configs[i], EGL_GREEN_SIZE, &g );
            p_eglGetConfigAttrib( display, configs[i], EGL_BLUE_SIZE, &b );
            p_eglGetConfigAttrib( display, configs[i], EGL_DEPTH_SIZE, &d );
            p_eglGetConfigAttrib( display, configs[i], EGL_STENCIL_SIZE, &s );
            TRACE( "%u: config %u id %u type %x visual %u native %u render %x colortype %u rgb %u,%u,%u depth %u stencil %u\n",
                   nb_pixel_formats, i, id, type, visual_id, native, render, color, r, g, b, d, s );
        }
        if (!pass) nb_onscreen_formats = nb_pixel_formats;
    }

    init_extensions();
    retval = 1;
    return TRUE;
}


/* generate stubs for GL functions that are not exported on Android */

#define USE_GL_FUNC(name) \
static void glstub_##name(void) \
{ \
    ERR( #name " called\n" ); \
    assert( 0 ); \
    ExitProcess( 1 ); \
}

ALL_WGL_FUNCS
#undef USE_GL_FUNC

static struct opengl_funcs egl_funcs =
{
    {
        android_wglCopyContext,
        android_wglCreateContext,
        android_wglDeleteContext,
        android_wglDescribePixelFormat,
        android_wglGetPixelFormat,
        android_wglGetProcAddress,
        android_wglMakeCurrent,
        android_wglSetPixelFormat,
        android_wglShareLists,
        android_wglSwapBuffers,
    },
#define USE_GL_FUNC(name) (void *)glstub_##name,
    { ALL_WGL_FUNCS }
#undef USE_GL_FUNC
};

struct opengl_funcs *get_wgl_driver( UINT version )
{
    if (version != WINE_WGL_DRIVER_VERSION)
    {
        ERR( "version mismatch, opengl32 wants %u but driver has %u\n", version, WINE_WGL_DRIVER_VERSION );
        return NULL;
    }
    if (!egl_init()) return NULL;
    return &egl_funcs;
}
