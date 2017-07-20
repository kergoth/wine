/*
 * Window related functions
 *
 * Copyright 1993, 1994, 1995, 1996, 2001, 2013 Alexandre Julliard
 * Copyright 1993 David Metcalfe
 * Copyright 1995, 1996 Alex Korobka
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

#define NONAMELESSUNION
#define NONAMELESSSTRUCT

#include "config.h"
#include "wine/port.h"

#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#ifdef HAVE_UNISTD_H
# include <unistd.h>
#endif

#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "winuser.h"
#include "wine/unicode.h"

#include "android.h"
#include "wine/debug.h"
#include "wine/server.h"

WINE_DEFAULT_DEBUG_CHANNEL(android);

#define SWP_AGG_NOPOSCHANGE (SWP_NOSIZE | SWP_NOMOVE | SWP_NOCLIENTSIZE | SWP_NOCLIENTMOVE | SWP_NOZORDER)

static CRITICAL_SECTION win_data_section;
static CRITICAL_SECTION_DEBUG critsect_debug =
{
    0, 0, &win_data_section,
    { &critsect_debug.ProcessLocksList, &critsect_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": win_data_section") }
};
static CRITICAL_SECTION win_data_section = { &critsect_debug, -1, 0, 0, 0, 0 };

ANativeWindow *main_window = NULL;

/* FIXME: quick & dirty handle context management */
static void *context[65536];

static void save_context( HWND hwnd, void *data )
{
    context[LOWORD(hwnd)] = data;
}

static void *find_context( HWND hwnd )
{
    return context[LOWORD(hwnd)];
}

static void delete_context( HWND hwnd )
{
    context[LOWORD(hwnd)] = NULL;
}

/* only for use on sanitized BITMAPINFO structures */
static inline int get_dib_info_size( const BITMAPINFO *info, UINT coloruse )
{
    if (info->bmiHeader.biCompression == BI_BITFIELDS)
        return sizeof(BITMAPINFOHEADER) + 3 * sizeof(DWORD);
    if (coloruse == DIB_PAL_COLORS)
        return sizeof(BITMAPINFOHEADER) + info->bmiHeader.biClrUsed * sizeof(WORD);
    return FIELD_OFFSET( BITMAPINFO, bmiColors[info->bmiHeader.biClrUsed] );
}

static inline int get_dib_stride( int width, int bpp )
{
    return ((width * bpp + 31) >> 3) & ~3;
}

static inline int get_dib_image_size( const BITMAPINFO *info )
{
    return get_dib_stride( info->bmiHeader.biWidth, info->bmiHeader.biBitCount )
        * abs( info->bmiHeader.biHeight );
}


/***********************************************************************
 *		alloc_win_data
 */
static struct android_win_data *alloc_win_data( HWND hwnd )
{
    struct android_win_data *data;

    if ((data = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*data))))
    {
        data->hwnd = hwnd;
        data->window = create_ioctl_window( hwnd );
        EnterCriticalSection( &win_data_section );
        save_context( hwnd, data );
    }
    return data;
}


/***********************************************************************
 *		free_win_data
 */
static void free_win_data( struct android_win_data *data )
{
    delete_context( data->hwnd );
    LeaveCriticalSection( &win_data_section );
    if (data->window) release_ioctl_window( data->window );
    HeapFree( GetProcessHeap(), 0, data );
}


/***********************************************************************
 *		get_win_data
 *
 * Lock and return the data structure associated with a window.
 */
struct android_win_data *get_win_data( HWND hwnd )
{
    struct android_win_data *data;

    if (!hwnd) return NULL;
    EnterCriticalSection( &win_data_section );
    if ((data = find_context( hwnd )) && data->hwnd == hwnd) return data;
    LeaveCriticalSection( &win_data_section );
    return NULL;
}


/***********************************************************************
 *		release_win_data
 *
 * Release the data returned by get_win_data.
 */
void release_win_data( struct android_win_data *data )
{
    if (data) LeaveCriticalSection( &win_data_section );
}


/***********************************************************************
 *		get_ioctl_window
 */
struct ANativeWindow *get_ioctl_window( HWND hwnd )
{
    struct ANativeWindow *ret;
    struct android_win_data *data = get_win_data( hwnd );

    if (!data || !data->window) return NULL;
    grab_ioctl_window( data->window );
    ret = data->window;
    release_win_data( data );
    return ret;
}


/***********************************************************************
 *		apply_line_region
 *
 * Apply the window region to a single line of the destination image.
 */
static void apply_line_region( DWORD *dst, int width, int x, int y, const RECT *rect, const RECT *end )
{
    while (rect < end && rect->top <= y && width > 0)
    {
        if (rect->left > x)
        {
            memset( dst, 0, min( rect->left - x, width ) * sizeof(*dst) );
            dst += rect->left - x;
            width -= rect->left - x;
            x = rect->left;
        }
        if (rect->right > x)
        {
            dst += rect->right - x;
            width -= rect->right - x;
            x = rect->right;
        }
        rect++;
    }
    if (width > 0) memset( dst, 0, width * sizeof(*dst) );
}



struct android_thread_data *desktop_thread;

struct java_event
{
    struct list      entry;
    union event_data data;
};

int send_event( struct android_thread_data *thread, const union event_data *data )
{
    int res;

    if (!thread || (res = write( thread->event_pipe[1], data, sizeof(*data) )) != sizeof(*data))
    {
        p__android_log_print(ANDROID_LOG_ERROR, "wine", "failed to send event");
        return -1;
    }
    return 0;
}

jboolean motion_event( JNIEnv *env, jobject obj, jint win, jint action, jint x, jint y, jint state, jint vscroll )
{
    static LONG button_state;
    union event_data data;
    int prev_state;

    int mask = action & AMOTION_EVENT_ACTION_MASK;

    if (!( mask == AMOTION_EVENT_ACTION_DOWN ||
           mask == AMOTION_EVENT_ACTION_UP ||
           mask == AMOTION_EVENT_ACTION_SCROLL ||
           mask == AMOTION_EVENT_ACTION_MOVE ||
           mask == AMOTION_EVENT_ACTION_HOVER_MOVE ))
        return JNI_FALSE;

    prev_state = InterlockedExchange( &button_state, state );

    data.type = HARDWARE_INPUT;
    data.hw.hwnd = LongToHandle( win );
    data.hw.input.type             = INPUT_MOUSE;
    data.hw.input.u.mi.dx          = x;
    data.hw.input.u.mi.dy          = y;
    data.hw.input.u.mi.mouseData   = 0;
    data.hw.input.u.mi.time        = 0;
    data.hw.input.u.mi.dwExtraInfo = 0;
    data.hw.input.u.mi.dwFlags     = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
    switch (action & AMOTION_EVENT_ACTION_MASK)
    {
    case AMOTION_EVENT_ACTION_DOWN:
        if ((state & ~prev_state) & AMOTION_EVENT_BUTTON_PRIMARY)
            data.hw.input.u.mi.dwFlags |= MOUSEEVENTF_LEFTDOWN;
        if ((state & ~prev_state) & AMOTION_EVENT_BUTTON_SECONDARY)
            data.hw.input.u.mi.dwFlags |= MOUSEEVENTF_RIGHTDOWN;
        if ((state & ~prev_state) & AMOTION_EVENT_BUTTON_TERTIARY)
            data.hw.input.u.mi.dwFlags |= MOUSEEVENTF_MIDDLEDOWN;
        if (!(state & ~prev_state)) /* touch event */
            data.hw.input.u.mi.dwFlags |= MOUSEEVENTF_LEFTDOWN;
        break;
    case AMOTION_EVENT_ACTION_UP:
        if ((prev_state & ~state) & AMOTION_EVENT_BUTTON_PRIMARY)
            data.hw.input.u.mi.dwFlags |= MOUSEEVENTF_LEFTUP;
        if ((prev_state & ~state) & AMOTION_EVENT_BUTTON_SECONDARY)
            data.hw.input.u.mi.dwFlags |= MOUSEEVENTF_RIGHTUP;
        if ((prev_state & ~state) & AMOTION_EVENT_BUTTON_TERTIARY)
            data.hw.input.u.mi.dwFlags |= MOUSEEVENTF_MIDDLEUP;
        if (!(prev_state & ~state)) /* touch event */
            data.hw.input.u.mi.dwFlags |= MOUSEEVENTF_LEFTUP;
        break;
    case AMOTION_EVENT_ACTION_SCROLL:
        data.hw.input.u.mi.dwFlags |= MOUSEEVENTF_WHEEL;
        data.hw.input.u.mi.mouseData = vscroll < 0 ? -WHEEL_DELTA : WHEEL_DELTA;
        break;
    case AMOTION_EVENT_ACTION_MOVE:
    case AMOTION_EVENT_ACTION_HOVER_MOVE:
        break;
    default:
        return JNI_FALSE;
    }
    send_event( desktop_thread, &data );
    return JNI_TRUE;
}

void surface_changed( JNIEnv *env, jobject obj, jint win, jobject surface )
{
    union event_data data;

    memset( &data, 0, sizeof(data) );
    data.surface.hwnd = LongToHandle( win );
    if (surface)
    {
        int width, height;
        ANativeWindow *win = pANativeWindow_fromSurface( env, surface );

        if (win->query( win, NATIVE_WINDOW_WIDTH, &width ) < 0) width = 0;
        if (win->query( win, NATIVE_WINDOW_HEIGHT, &height ) < 0) height = 0;
        data.surface.window = win;
        data.surface.width = width;
        data.surface.height = height;
        p__android_log_print( ANDROID_LOG_INFO, "wine", "init_window: %p %ux%u\n",
                              data.surface.hwnd, width, height );
    }
    data.type = SURFACE_CHANGED;
    send_event( desktop_thread, &data );
}

void desktop_changed( JNIEnv *env, jobject obj, jint width, jint height )
{
    union event_data data;

    memset( &data, 0, sizeof(data) );
    data.type = DESKTOP_CHANGED;
    data.desktop.width = width;
    data.desktop.height = height;
    p__android_log_print( ANDROID_LOG_INFO, "wine", "desktop_changed: %ux%u\n", width, height );
    send_event( desktop_thread, &data );
}

void config_changed( JNIEnv *env, jobject obj, jint dpi, jboolean force )
{
    union event_data data;

    data.type = CONFIG_CHANGED;
    data.cfg.dpi = dpi;
    data.cfg.force = force;
    send_event( desktop_thread, &data );
    p__android_log_print( ANDROID_LOG_INFO, "wine", "config_changed dpi=%d force=%d\n",
                          data.cfg.dpi, data.cfg.force );
    return;
}


/* pull events from the event pipe and add them to the queue */
static void pull_events(void)
{
    struct android_thread_data *thread_data = android_init_thread_data();
    struct java_event *event;
    int res;

    for (;;)
    {
        if (!(event = HeapAlloc( GetProcessHeap(), 0, sizeof(*event) ))) break;

        res = read( thread_data->event_pipe[0], &event->data, sizeof(event->data) );
        if (res != sizeof(event->data)) break;
        list_add_tail( &thread_data->event_queue, &event->entry );
    }
    HeapFree( GetProcessHeap(), 0, event );
}

static int process_events( DWORD mask )
{
    struct android_thread_data *thread_data = android_init_thread_data();
    struct java_event *event, *next;
    union event_data *previous;
    unsigned int count = 0;

    previous = thread_data->current_event;

    LIST_FOR_EACH_ENTRY_SAFE( event, next, &thread_data->event_queue, struct java_event, entry )
    {
        switch (event->data.type)
        {
        case HARDWARE_INPUT:
            if (event->data.hw.input.type == INPUT_KEYBOARD)
            {
                if (mask & QS_KEY) break;
            }
            else if (event->data.hw.input.u.mi.dwFlags & (MOUSEEVENTF_LEFTDOWN|MOUSEEVENTF_RIGHTDOWN|
                                                          MOUSEEVENTF_MIDDLEDOWN|MOUSEEVENTF_LEFTUP|
                                                          MOUSEEVENTF_RIGHTUP|MOUSEEVENTF_MIDDLEUP))
            {
                if (mask & QS_MOUSEBUTTON) break;
            }
            else if (mask & QS_MOUSEMOVE) break;
            continue;  /* skip it */
        case SURFACE_CHANGED:
            break;  /* always process it to unblock other threads */
        default:
            if (mask & QS_SENDMESSAGE) break;
            continue;  /* skip it */
        }

        /* remove it first, in case we process events recursively */
        list_remove( &event->entry );
        thread_data->current_event = &event->data;

        switch (event->data.type)
        {
        case HARDWARE_INPUT:
            if (event->data.hw.input.type == INPUT_KEYBOARD)
            {
                if (event->data.hw.input.u.ki.dwFlags & KEYEVENTF_KEYUP)
                    TRACE("KEYUP hwnd %p vkey %x '%c' scancode %x\n", event->data.hw.hwnd,
                          event->data.hw.input.u.ki.wVk, event->data.hw.input.u.ki.wVk,
                          event->data.hw.input.u.ki.wScan );
                else
                    TRACE("KEYDOWN hwnd %p vkey %x '%c' scancode %x\n", event->data.hw.hwnd,
                          event->data.hw.input.u.ki.wVk, event->data.hw.input.u.ki.wVk,
                          event->data.hw.input.u.ki.wScan );
                update_keyboard_lock_state( event->data.hw.input.u.ki.wVk,
                                            event->data.hw.input.u.ki.dwExtraInfo );
                event->data.hw.input.u.ki.dwExtraInfo = 0;
                __wine_send_input( 0, &event->data.hw.input );
            }
            else
            {
                HWND capture = get_capture_window();

                if (event->data.hw.input.u.mi.dwFlags & (MOUSEEVENTF_LEFTDOWN|MOUSEEVENTF_RIGHTDOWN|MOUSEEVENTF_MIDDLEDOWN))
                    TRACE( "BUTTONDOWN pos %d,%d hwnd %p flags %x\n",
                           event->data.hw.input.u.mi.dx, event->data.hw.input.u.mi.dy,
                           event->data.hw.hwnd, event->data.hw.input.u.mi.dwFlags );
                else if (event->data.hw.input.u.mi.dwFlags & (MOUSEEVENTF_LEFTUP|MOUSEEVENTF_RIGHTUP|MOUSEEVENTF_MIDDLEUP))
                    TRACE( "BUTTONUP pos %d,%d hwnd %p flags %x\n",
                           event->data.hw.input.u.mi.dx, event->data.hw.input.u.mi.dy,
                           event->data.hw.hwnd, event->data.hw.input.u.mi.dwFlags );
                else
                    TRACE( "MOUSEMOVE pos %d,%d hwnd %p flags %x\n",
                           event->data.hw.input.u.mi.dx, event->data.hw.input.u.mi.dy,
                           event->data.hw.hwnd, event->data.hw.input.u.mi.dwFlags );
                if (!capture && (event->data.hw.input.u.mi.dwFlags & MOUSEEVENTF_ABSOLUTE))
                {
                    RECT rect;
                    SetRect( &rect, event->data.hw.input.u.mi.dx, event->data.hw.input.u.mi.dy,
                             event->data.hw.input.u.mi.dx + 1, event->data.hw.input.u.mi.dy + 1 );
                    MapWindowPoints( 0, event->data.hw.hwnd, (POINT *)&rect, 2 );

                    SERVER_START_REQ( update_window_zorder )
                    {
                        req->window      = wine_server_user_handle( event->data.hw.hwnd );
                        req->rect.left   = rect.left;
                        req->rect.top    = rect.top;
                        req->rect.right  = rect.right;
                        req->rect.bottom = rect.bottom;
                        wine_server_call( req );
                    }
                    SERVER_END_REQ;
                }
                __wine_send_input( capture ? capture : event->data.hw.hwnd, &event->data.hw.input );
            }
            break;

        case SURFACE_CHANGED:
            TRACE("SURFACE_CHANGED %p %p size %ux%u\n", event->data.surface.hwnd,
                  event->data.surface.window, event->data.surface.width, event->data.surface.height );

            register_native_window( event->data.surface.hwnd, event->data.surface.window );
            break;

        case CONFIG_CHANGED:
            TRACE("CONFIG_CHANGED dpi %u\n", event->data.cfg.dpi );
            set_screen_dpi( event->data.cfg.dpi, event->data.cfg.force );
            break;

        case CLIPBOARD_CHANGED:
            TRACE("CLIPBOARD_CHANGED\n");
            handle_clipboard_changed();
            break;

        case IMPORT_CLIPBOARD_DATA:
            TRACE("IMPORT_CLIPBOARD_DATA %i\n", event->data.clipdata.android_format);
            handle_import_clipboard_data( event->data.clipdata.android_format,
                event->data.clipdata.data, event->data.clipdata.len );
            break;

        case CLIPBOARD_REQUEST:
            TRACE("CLIPBOARD_REQUEST %i\n", event->data.clipdata.android_format);
            handle_clipboard_request( event->data.clipdata.android_format );
            break;

        case DESKTOP_CHANGED:
            TRACE("DESKTOP_CHANGED %ux%u\n", event->data.desktop.width, event->data.desktop.height );
            screen_width = event->data.desktop.width;
            screen_height = event->data.desktop.height;
            init_monitors( screen_width, screen_height );
            SetWindowPos( GetDesktopWindow(), 0, 0, 0, screen_width, screen_height,
                          SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW );
            break;

        case IME_TEXT:
            ERR( "IME_TEXT target %u, length %u\n", event->data.ime_text.target , event->data.ime_text.length );
            handle_IME_TEXT( event->data.ime_text.target, event->data.ime_text.length);
            break;

        case IME_FINISH:
            TRACE( "IME_FINISH target %u, length %u\n", event->data.ime_finish.target, event->data.ime_text.length );
            handle_IME_FINISH( event->data.ime_finish.target, event->data.ime_finish.length );
            break;

        case IME_CANCEL:
            TRACE( "IME_CANCEL\n" );
            handle_IME_CANCEL();
            break;

        case IME_START:
            TRACE( "IME_START\n" );
            handle_IME_START();
            break;

        case RUN_CMDLINE:
            handle_run_cmdline( event->data.runcmd.cmdline, event->data.runcmd.env );
            free( event->data.runcmd.cmdline );
            if (event->data.runcmd.env)
            {
                LPWSTR *strs = event->data.runcmd.env;
                while (*strs) free( *strs++ );
                free( event->data.runcmd.env );
            }
            break;
        case CLEAR_META:
            TRACE( "CLEAR_META" );
            handle_clear_meta_key_states( event->data.clearmeta.states );
            break;
        default:
            FIXME( "got event %u\n", event->data.type );
        }
        HeapFree( GetProcessHeap(), 0, event );
        count++;
    }
    thread_data->current_event = previous;
    return count;
}

static int wait_events( int timeout )
{
    struct android_thread_data *thread_data = android_init_thread_data();

    if (thread_data->event_pipe[0] == -1) return -1;

    for (;;)
    {
        struct pollfd pollfd;
        int ret;

        pollfd.fd = thread_data->event_pipe[0];
        pollfd.events = POLLIN | POLLHUP;
        ret = poll( &pollfd, 1, timeout );
        if (ret == -1 && errno == EINTR) continue;
        if (ret && (pollfd.revents & (POLLHUP | POLLERR))) ret = -1;
        if (ret > 0) pull_events();
        return ret;
    }
}

/* store the palette or color mask data in the bitmap info structure */
static void set_color_info( BITMAPINFO *info, BOOL has_alpha )
{
    DWORD *colors = (DWORD *)info->bmiColors;

    info->bmiHeader.biSize = sizeof(info->bmiHeader);
    info->bmiHeader.biClrUsed = 0;
    info->bmiHeader.biBitCount = 32;
    if (has_alpha)
    {
        info->bmiHeader.biCompression = BI_RGB;
        return;
    }
    info->bmiHeader.biCompression = BI_BITFIELDS;
    colors[0] = 0xff0000;
    colors[1] = 0x00ff00;
    colors[2] = 0x0000ff;
}


/* Window surface support */

struct android_window_surface
{
    struct window_surface header;
    HWND                  hwnd;
    ANativeWindow        *window;
    RECT                  bounds;
    BOOL                  byteswap;
    RGNDATA              *win_region;
    HRGN                  region;
    COLORREF              color_key;
    void                 *bits;
    CRITICAL_SECTION      crit;
    BITMAPINFO            info;   /* variable size, must be last */
};

static struct android_window_surface *get_android_surface( struct window_surface *surface )
{
    return (struct android_window_surface *)surface;
}

/***********************************************************************
 *           android_surface_lock
 */
static void android_surface_lock( struct window_surface *window_surface )
{
    struct android_window_surface *surface = get_android_surface( window_surface );

    EnterCriticalSection( &surface->crit );
}

/***********************************************************************
 *           android_surface_unlock
 */
static void android_surface_unlock( struct window_surface *window_surface )
{
    struct android_window_surface *surface = get_android_surface( window_surface );

    LeaveCriticalSection( &surface->crit );
}

/***********************************************************************
 *           android_surface_get_bitmap_info
 */
static void *android_surface_get_bitmap_info( struct window_surface *window_surface, BITMAPINFO *info )
{
    struct android_window_surface *surface = get_android_surface( window_surface );

    memcpy( info, &surface->info, get_dib_info_size( &surface->info, DIB_RGB_COLORS ));
    return surface->bits;
}

/***********************************************************************
 *           android_surface_get_bounds
 */
static RECT *android_surface_get_bounds( struct window_surface *window_surface )
{
    struct android_window_surface *surface = get_android_surface( window_surface );

    return &surface->bounds;
}

/***********************************************************************
 *           android_surface_set_region
 */
static void android_surface_set_region( struct window_surface *window_surface, HRGN region )
{
    struct android_window_surface *surface = get_android_surface( window_surface );

    TRACE( "updating surface %p with %p\n", surface, region );

    window_surface->funcs->lock( window_surface );
    if (!region)
    {
        if (surface->region) DeleteObject( surface->region );
        surface->region = 0;
    }
    else
    {
        if (!surface->region) surface->region = CreateRectRgn( 0, 0, 0, 0 );
        CombineRgn( surface->region, region, 0, RGN_COPY );
    }
    window_surface->funcs->unlock( window_surface );
}

/***********************************************************************
 *           android_surface_flush
 */
static void android_surface_flush( struct window_surface *window_surface )
{
    struct android_window_surface *surface = get_android_surface( window_surface );
    ANativeWindow_Buffer buffer;
    ARect rc;
    RECT rect;
    BOOL needs_flush;

    window_surface->funcs->lock( window_surface );
    SetRect( &rect, 0, 0, surface->header.rect.right - surface->header.rect.left,
             surface->header.rect.bottom - surface->header.rect.top );
    needs_flush = IntersectRect( &rect, &rect, &surface->bounds );
    reset_bounds( &surface->bounds );
    window_surface->funcs->unlock( window_surface );
    if (!needs_flush) return;

    TRACE( "flushing %p hwnd %p surface %s rect %s win %p bits %p key %08x\n",
           surface, surface->hwnd, wine_dbgstr_rect( &surface->header.rect ), wine_dbgstr_rect( &rect ),
           main_window, surface->bits, surface->color_key );

    rc.left   = rect.left;
    rc.top    = rect.top;
    rc.right  = rect.right;
    rc.bottom = rect.bottom;

    if (!surface->window->perform( surface->window, NATIVE_WINDOW_LOCK, &buffer, &rc ))
    {
        const RECT *rgn_rect = NULL, *end = NULL;
        unsigned int *src, *dst;
        int x, y, width;

        rect.left   = rc.left;
        rect.top    = rc.top;
        rect.right  = rc.right;
        rect.bottom = rc.bottom;
        IntersectRect( &rect, &rect, &surface->header.rect );

        if (surface->win_region)
        {
            rgn_rect = (RECT *)surface->win_region->Buffer;
            end = rgn_rect + surface->win_region->rdh.nCount;
        }
        src = (unsigned int *)surface->bits
            + (rect.top - surface->header.rect.top) * surface->info.bmiHeader.biWidth
            + (rect.left - surface->header.rect.left);
        dst = (unsigned int *)buffer.bits + rect.top * buffer.stride + rect.left;
        width = min( rect.right - rect.left, buffer.stride );
        for (y = rect.top; y < min( buffer.height, rect.bottom); y++)
        {
            if (surface->info.bmiHeader.biCompression == BI_RGB)
                memcpy( dst, src, width * sizeof(*dst) );
            else
                for (x = 0; x < width; x++) dst[x] = src[x] | 0xff000000;

            if (surface->color_key != CLR_INVALID)
                for (x = 0; x < width; x++) if ((src[x] & 0xffffff) == surface->color_key) dst[x] = 0;

            if (rgn_rect)
            {
                while (rgn_rect < end && rgn_rect->bottom <= y) rgn_rect++;
                apply_line_region( dst, width, rect.left, y, rgn_rect, end );
            }

            src += surface->info.bmiHeader.biWidth;
            dst += buffer.stride;
        }
        surface->window->perform( surface->window, NATIVE_WINDOW_UNLOCK_AND_POST );
    }
    else TRACE( "Unable to lock surface %p window %p buffer %p\n",
                surface, surface->hwnd, surface->window );
}

/***********************************************************************
 *           android_surface_destroy
 */
static void android_surface_destroy( struct window_surface *window_surface )
{
    struct android_window_surface *surface = get_android_surface( window_surface );

    TRACE( "freeing %p bits %p\n", surface, surface->bits );

    surface->crit.DebugInfo->Spare[0] = 0;
    DeleteCriticalSection( &surface->crit );
    HeapFree( GetProcessHeap(), 0, surface->win_region );
    if (surface->region) DeleteObject( surface->region );
    release_ioctl_window( surface->window );
    HeapFree( GetProcessHeap(), 0, surface->bits );
    HeapFree( GetProcessHeap(), 0, surface );
}

static const struct window_surface_funcs android_surface_funcs =
{
    android_surface_lock,
    android_surface_unlock,
    android_surface_get_bitmap_info,
    android_surface_get_bounds,
    android_surface_set_region,
    android_surface_flush,
    android_surface_destroy
};

static BOOL is_argb_surface( struct window_surface *surface )
{
    return surface && surface->funcs == &android_surface_funcs &&
        get_android_surface( surface )->info.bmiHeader.biCompression == BI_RGB;
}

/***********************************************************************
 *           set_color_key
 */
static void set_color_key( struct android_window_surface *surface, COLORREF key )
{
    if (key == CLR_INVALID)
        surface->color_key = CLR_INVALID;
    else if (surface->info.bmiHeader.biBitCount <= 8)
        surface->color_key = CLR_INVALID;
    else if (key & (1 << 24))  /* PALETTEINDEX */
        surface->color_key = 0;
    else if (key >> 16 == 0x10ff)  /* DIBINDEX */
        surface->color_key = 0;
    else if (surface->info.bmiHeader.biBitCount == 24)
        surface->color_key = key;
    else
        surface->color_key = (GetRValue(key) << 16) | (GetGValue(key) << 8) | GetBValue(key);
}

/***********************************************************************
 *           set_surface_region
 */
static void set_surface_region( struct window_surface *window_surface, HRGN win_region )
{
    struct android_window_surface *surface = get_android_surface( window_surface );
    struct android_win_data *win_data;
    HRGN region = win_region;
    RGNDATA *data = NULL;
    DWORD size;
    int offset_x, offset_y;

    if (window_surface->funcs != &android_surface_funcs) return;  /* we may get the null surface */

    if (!(win_data = get_win_data( surface->hwnd ))) return;
    offset_x = win_data->window_rect.left - win_data->whole_rect.left;
    offset_y = win_data->window_rect.top - win_data->whole_rect.top;
    release_win_data( win_data );

    if (win_region == (HRGN)1)  /* hack: win_region == 1 means retrieve region from server */
    {
        region = CreateRectRgn( 0, 0, 0, 0 );
        if (GetWindowRgn( surface->hwnd, region ) == ERROR) goto done;
    }

    OffsetRgn( region, offset_x, offset_y );

    if (!(size = GetRegionData( region, 0, NULL ))) goto done;
    if (!(data = HeapAlloc( GetProcessHeap(), 0, size ))) goto done;

    if (!GetRegionData( region, size, data ))
    {
        HeapFree( GetProcessHeap(), 0, data );
        data = NULL;
    }

done:
    window_surface->funcs->lock( window_surface );
    HeapFree( GetProcessHeap(), 0, surface->win_region );
    surface->win_region = data;
    *window_surface->funcs->get_bounds( window_surface ) = surface->header.rect;
    window_surface->funcs->unlock( window_surface );
    if (region != win_region) DeleteObject( region );
}

/***********************************************************************
 *           create_surface
 */
static struct window_surface *create_surface( HWND hwnd, const RECT *rect,
                                              COLORREF color_key, BOOL use_alpha )
{
    struct android_window_surface *surface;
    int width = rect->right - rect->left, height = rect->bottom - rect->top;

    surface = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY,
                         FIELD_OFFSET( struct android_window_surface, info.bmiColors[3] ));
    if (!surface) return NULL;
    set_color_info( &surface->info, use_alpha );
    surface->info.bmiHeader.biWidth       = width;
    surface->info.bmiHeader.biHeight      = -height; /* top-down */
    surface->info.bmiHeader.biPlanes      = 1;
    surface->info.bmiHeader.biSizeImage   = get_dib_image_size( &surface->info );

    InitializeCriticalSection( &surface->crit );
    surface->crit.DebugInfo->Spare[0] = (DWORD_PTR)(__FILE__ ": surface");

    surface->header.funcs = &android_surface_funcs;
    surface->header.rect  = *rect;
    surface->header.ref   = 1;
    surface->hwnd         = hwnd;
    surface->window       = get_ioctl_window( hwnd );
    set_color_key( surface, color_key );
    set_surface_region( &surface->header, (HRGN)1 );
    reset_bounds( &surface->bounds );

    if (!(surface->bits = HeapAlloc( GetProcessHeap(), 0, surface->info.bmiHeader.biSizeImage )))
        goto failed;

    TRACE( "created %p %s bits %p-%p\n", surface, wine_dbgstr_rect(rect),
           surface->bits, (char *)surface->bits + surface->info.bmiHeader.biSizeImage );

    ioctl_set_surface_alpha( hwnd, use_alpha || surface->color_key != CLR_INVALID );
    return &surface->header;

failed:
    android_surface_destroy( &surface->header );
    return NULL;
}

/***********************************************************************
 *           set_surface_color_key
 */
static void set_surface_color_key( struct window_surface *window_surface, COLORREF color_key )
{
    struct android_window_surface *surface = get_android_surface( window_surface );
    COLORREF prev;

    if (window_surface->funcs != &android_surface_funcs) return;  /* we may get the null surface */

    window_surface->funcs->lock( window_surface );
    prev = surface->color_key;
    set_color_key( surface, color_key );
    if (surface->color_key != prev)  /* refresh */
        *window_surface->funcs->get_bounds( window_surface ) = surface->header.rect;
    ioctl_set_surface_alpha( surface->hwnd, (surface->info.bmiHeader.biCompression == BI_RGB ||
                                             surface->color_key != CLR_INVALID) );
    window_surface->funcs->unlock( window_surface );
}

/***********************************************************************
 *              get_bitmap_argb
 *
 * Return the bitmap bits in ARGB format. Helper for setting icon hints.
 */
static unsigned int *get_bitmap_argb( HDC hdc, HBITMAP color, HBITMAP mask, unsigned int *width,
                                      unsigned int *height )
{
    char buffer[FIELD_OFFSET( BITMAPINFO, bmiColors[256] )];
    BITMAPINFO *info = (BITMAPINFO *)buffer;
    BITMAP bm;
    unsigned int *ptr, *bits = NULL;
    unsigned char *mask_bits = NULL;
    int i, j;
    BOOL has_alpha = FALSE;

    if (!GetObjectW( color, sizeof(bm), &bm )) return NULL;
    info->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info->bmiHeader.biWidth = bm.bmWidth;
    info->bmiHeader.biHeight = -bm.bmHeight;
    info->bmiHeader.biPlanes = 1;
    info->bmiHeader.biBitCount = 32;
    info->bmiHeader.biCompression = BI_RGB;
    info->bmiHeader.biSizeImage = bm.bmWidth * bm.bmHeight * 4;
    info->bmiHeader.biXPelsPerMeter = 0;
    info->bmiHeader.biYPelsPerMeter = 0;
    info->bmiHeader.biClrUsed = 0;
    info->bmiHeader.biClrImportant = 0;
    if (!(bits = HeapAlloc( GetProcessHeap(), 0, bm.bmWidth * bm.bmHeight * sizeof(unsigned int) )))
        goto failed;
    if (!GetDIBits( hdc, color, 0, bm.bmHeight, bits, info, DIB_RGB_COLORS )) goto failed;

    *width = bm.bmWidth;
    *height = bm.bmHeight;

    for (i = 0; i < bm.bmWidth * bm.bmHeight; i++)
        if ((has_alpha = (bits[i] & 0xff000000) != 0)) break;

    if (!has_alpha)
    {
        unsigned int width_bytes = (bm.bmWidth + 31) / 32 * 4;
        /* generate alpha channel from the mask */
        info->bmiHeader.biBitCount = 1;
        info->bmiHeader.biSizeImage = width_bytes * bm.bmHeight;
        if (!(mask_bits = HeapAlloc( GetProcessHeap(), 0, info->bmiHeader.biSizeImage ))) goto failed;
        if (!GetDIBits( hdc, mask, 0, bm.bmHeight, mask_bits, info, DIB_RGB_COLORS )) goto failed;
        ptr = bits;
        for (i = 0; i < bm.bmHeight; i++)
            for (j = 0; j < bm.bmWidth; j++, ptr++)
                if (!((mask_bits[i * width_bytes + j / 8] << (j % 8)) & 0x80)) *ptr |= 0xff000000;
        HeapFree( GetProcessHeap(), 0, mask_bits );
    }

    return bits;

failed:
    HeapFree( GetProcessHeap(), 0, bits );
    HeapFree( GetProcessHeap(), 0, mask_bits );
    *width = *height = 0;
    return NULL;
}


/***********************************************************************
 *              fetch_window_icon
 */
static void fetch_window_icon( HWND hwnd, HICON icon )
{
    ICONINFO ii;
    unsigned int width = 0, height = 0, *bits = NULL;

    if (!icon) icon = (HICON)SendMessageW( hwnd, WM_GETICON, ICON_BIG, 0 );
    if (!icon) icon = (HICON)GetClassLongPtrW( hwnd, GCLP_HICON );

    if (GetIconInfo( icon, &ii ))
    {
        HDC hdc = CreateCompatibleDC( 0 );
        bits = get_bitmap_argb( hdc, ii.hbmColor, ii.hbmMask, &width, &height );
        DeleteDC( hdc );
        DeleteObject( ii.hbmColor );
        DeleteObject( ii.hbmMask );
    }

    ioctl_set_window_icon( hwnd, width, height, bits );
    HeapFree( GetProcessHeap(), 0, bits );
}


/***********************************************************************
 *           MsgWaitForMultipleObjectsEx
 */
DWORD CDECL ANDROID_MsgWaitForMultipleObjectsEx( DWORD count, const HANDLE *handles,
                                                 DWORD timeout, DWORD mask, DWORD flags )
{
    struct android_thread_data *thread_data = android_init_thread_data();

    /* don't process nested events */
    if (thread_data->current_event) mask = 0;

    pull_events();
    if (process_events( mask )) return count - 1;

    return WaitForMultipleObjectsEx( count, handles, flags & MWMO_WAITALL,
                                     timeout, flags & MWMO_ALERTABLE );
}

/*****************************************************************
 *		SetFocus
 */
void CDECL ANDROID_SetFocus( HWND hwnd )
{
    IME_UpdateAssociation(hwnd);
    ioctl_set_window_focus( GetAncestor( hwnd, GA_ROOT ));
}


/**********************************************************************
 *		CreateDesktopWindow   (ANDROID.@)
 */
BOOL CDECL ANDROID_CreateDesktopWindow( HWND hwnd )
{
    android_init_thread_data();
    return TRUE;
}


/**********************************************************************
 *		CreateWindow   (ANDROID.@)
 */
BOOL CDECL ANDROID_CreateWindow( HWND hwnd )
{
    TRACE( "%p\n", hwnd );

    android_init_thread_data();

    if (hwnd == GetDesktopWindow())
    {
        struct android_win_data *data;

        desktop_thread = android_thread_data();

        start_android_device();

        if (!(data = alloc_win_data( hwnd ))) return FALSE;
        release_win_data( data );
    }
    return TRUE;
}


/***********************************************************************
 *		DestroyWindow   (ANDROID.@)
 */
void CDECL ANDROID_DestroyWindow( HWND hwnd )
{
    struct android_win_data *data;

    if (!(data = get_win_data( hwnd ))) return;

    if (data->surface) window_surface_release( data->surface );
    data->surface = NULL;
    free_win_data( data );
    destroy_gl_drawable( hwnd );
}


/***********************************************************************
 *		ANDROID_create_win_data
 *
 * Create an X11 data window structure for an existing window.
 */
static struct android_win_data *ANDROID_create_win_data( HWND hwnd, const RECT *window_rect,
                                                         const RECT *client_rect )
{
    struct android_win_data *data;
    HWND parent;

    if (!(parent = GetAncestor( hwnd, GA_PARENT ))) return NULL;  /* desktop */

    /* don't create win data for HWND_MESSAGE windows */
    if (parent != GetDesktopWindow() && !GetAncestor( parent, GA_PARENT )) return NULL;

    /* don't create win data for children */
    if (parent != GetDesktopWindow()) return NULL;

    if (GetWindowThreadProcessId( hwnd, NULL ) != GetCurrentThreadId()) return NULL;

    if (!(data = alloc_win_data( hwnd ))) return NULL;

    data->whole_rect = data->window_rect = *window_rect;
    data->client_rect = *client_rect;
    return data;
}


static inline RECT get_surface_rect( const RECT *visible_rect )
{
    RECT rect;

    IntersectRect( &rect, visible_rect, &virtual_screen_rect );
    OffsetRect( &rect, -visible_rect->left, -visible_rect->top );
    rect.left &= ~31;
    rect.top  &= ~31;
    rect.right  = max( rect.left + 32, (rect.right + 31) & ~31 );
    rect.bottom = max( rect.top + 32, (rect.bottom + 31) & ~31 );
    return rect;
}


static WNDPROC desktop_orig_wndproc;

static LRESULT CALLBACK desktop_wndproc_wrapper( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp )
{
    switch (msg)
    {
    case WM_PARENTNOTIFY:
        if (LOWORD(wp) == WM_DESTROY) destroy_ioctl_window( (HWND)lp );
        break;
    }
    return desktop_orig_wndproc( hwnd, msg, wp, lp );
}


/***********************************************************************
 *		WindowPosChanging   (ANDROID.@)
 */
void CDECL ANDROID_WindowPosChanging( HWND hwnd, HWND insert_after, UINT swp_flags,
                                     const RECT *window_rect, const RECT *client_rect, RECT *visible_rect,
                                     struct window_surface **surface )
{
    struct android_win_data *data = get_win_data( hwnd );
    RECT surface_rect;
    DWORD flags;
    COLORREF key;
    BOOL layered = GetWindowLongW( hwnd, GWL_EXSTYLE ) & WS_EX_LAYERED;

    TRACE( "win %p window %s client %s style %08x flags %08x\n",
           hwnd, wine_dbgstr_rect(window_rect), wine_dbgstr_rect(client_rect),
           GetWindowLongW( hwnd, GWL_STYLE ), swp_flags );

    if (!data)
    {
        WCHAR text[1024];

        if (!(data = ANDROID_create_win_data( hwnd, window_rect, client_rect ))) return;
        if (InternalGetWindowText( hwnd, text, sizeof(text)/sizeof(WCHAR) ))
            ioctl_set_window_text( hwnd, text );
    }

    *visible_rect = *window_rect;

    /* create the window surface if necessary */

    if (swp_flags & SWP_HIDEWINDOW) goto done;
    if (is_argb_surface( data->surface )) goto done;

    if (*surface) window_surface_release( *surface );
    *surface = NULL;  /* indicate that we want to draw directly to the window */

    surface_rect = get_surface_rect( visible_rect );
    if (data->surface)
    {
        if (!memcmp( &data->surface->rect, &surface_rect, sizeof(surface_rect) ))
        {
            /* existing surface is good enough */
            window_surface_add_ref( data->surface );
            *surface = data->surface;
            goto done;
        }
    }
    else if (!(swp_flags & SWP_SHOWWINDOW) && !(GetWindowLongW( hwnd, GWL_STYLE ) & WS_VISIBLE)) goto done;

    if (!layered || !GetLayeredWindowAttributes( hwnd, &key, NULL, &flags ) || !(flags & LWA_COLORKEY))
        key = CLR_INVALID;

    *surface = create_surface( data->hwnd, &surface_rect, key, FALSE );

done:
    release_win_data( data );
}


/***********************************************************************
 *		WindowPosChanged   (ANDROID.@)
 */
void CDECL ANDROID_WindowPosChanged( HWND hwnd, HWND insert_after, UINT swp_flags,
                                    const RECT *rectWindow, const RECT *rectClient,
                                    const RECT *visible_rect, const RECT *valid_rects,
                                    struct window_surface *surface )
{
    struct android_win_data *data;
    DWORD new_style = GetWindowLongW( hwnd, GWL_STYLE );

    if (!(data = get_win_data( hwnd ))) return;

    data->window_rect = *rectWindow;
    data->whole_rect  = *visible_rect;
    data->client_rect = *rectClient;

    if (!is_argb_surface( data->surface ))
    {
        if (surface) window_surface_add_ref( surface );
        if (data->surface) window_surface_release( data->surface );
        data->surface = surface;
    }

    TRACE( "win %p window %s client %s style %08x flags %08x\n",
           hwnd, wine_dbgstr_rect(rectWindow), wine_dbgstr_rect(rectClient), new_style, swp_flags );

    release_win_data( data );

    if ((swp_flags & SWP_SHOWWINDOW) && !desktop_orig_wndproc && hwnd == GetDesktopWindow())
        desktop_orig_wndproc = (WNDPROC)SetWindowLongPtrW( GetDesktopWindow(), GWLP_WNDPROC,
                                                           (LONG_PTR)desktop_wndproc_wrapper );

    if ((swp_flags & (SWP_SHOWWINDOW|SWP_NOZORDER)) == (SWP_SHOWWINDOW|SWP_NOZORDER))
    {
        /* If this is the topmost visible window, bring the view to the top when showing it. */
        HWND prev = GetWindow( hwnd, GW_HWNDPREV );
        while (prev && !(GetWindowLongW( prev, GWL_STYLE ) & WS_VISIBLE))
            prev = GetWindow( prev, GW_HWNDPREV );
        if (!prev)
        {
            swp_flags &= ~SWP_NOZORDER;
            insert_after = HWND_TOP;
        }
    }

    ioctl_window_pos_changed( hwnd, visible_rect, new_style, swp_flags,
                              insert_after, GetWindow( hwnd, GW_OWNER ));
    if (swp_flags & SWP_SHOWWINDOW) fetch_window_icon( hwnd, 0 );
}


/***********************************************************************
 *           ANDROID_ShowWindow
 */
UINT CDECL ANDROID_ShowWindow( HWND hwnd, INT cmd, RECT *rect, UINT swp )
{
    static const WCHAR trayW[] = {'S','h','e','l','l','_','T','r','a','y','W','n','d',0};

    if (IsRectEmpty( rect )) return swp;
    if (!IsIconic( hwnd )) return swp;
    /* hide icons when the taskbar is active */
    if (!IsWindowVisible( FindWindowW( trayW, NULL ))) return swp;
    if (rect->left != -32000 || rect->top != -32000)
    {
        OffsetRect( rect, -32000 - rect->left, -32000 - rect->top );
        swp &= ~(SWP_NOMOVE | SWP_NOCLIENTMOVE);
    }
    return swp;
}


/*****************************************************************
 *		ANDROID_SetParent
 */
void CDECL ANDROID_SetParent( HWND hwnd, HWND parent, HWND old_parent )
{
    struct android_win_data *data;

    if (parent == old_parent) return;
    if (!(data = get_win_data( hwnd ))) return;

    TRACE( "win %p parent %p -> %p\n", hwnd, old_parent, parent );

    if (parent != GetDesktopWindow() && old_parent == GetDesktopWindow())
    {
        /* destroy the old window */
        free_win_data( data );
        return;
    }

    release_win_data( data );
}


/***********************************************************************
 *		ANDROID_SetWindowStyle
 */
void CDECL ANDROID_SetWindowStyle( HWND hwnd, INT offset, STYLESTRUCT *style )
{
    struct android_win_data *data;
    DWORD changed = style->styleNew ^ style->styleOld;

    if (hwnd == GetDesktopWindow()) return;
    if (!(data = get_win_data( hwnd ))) return;

    if (offset == GWL_EXSTYLE && (changed & WS_EX_LAYERED)) /* changing WS_EX_LAYERED resets attributes */
    {
        if (is_argb_surface( data->surface ))
        {
            if (data->surface) window_surface_release( data->surface );
            data->surface = NULL;
        }
        else ioctl_set_window_layered( hwnd, CLR_INVALID, 255 );
    }

    release_win_data( data );
}


/*****************************************************************
 *		ANDROID_SetWindowIcon
 */
void CDECL ANDROID_SetWindowIcon( HWND hwnd, UINT type, HICON icon )
{
    if (type != ICON_BIG) return;  /* small icons not supported */
    fetch_window_icon( hwnd, icon );
}


/*****************************************************************
 *		ANDROID_SetWindowText
 */
void CDECL ANDROID_SetWindowText( HWND hwnd, WCHAR *text /* technically const */ )
{
    ioctl_set_window_text( hwnd, text );
}


/***********************************************************************
 *		ANDROID_SetWindowRgn
 */
void CDECL ANDROID_SetWindowRgn( HWND hwnd, HRGN hrgn, BOOL redraw )
{
    struct android_win_data *data;

    if ((data = get_win_data( hwnd )))
    {
        if (data->surface) set_surface_region( data->surface, hrgn );
        release_win_data( data );
    }
    else FIXME( "not supported on other process window %p\n", hwnd );

    ioctl_set_window_rgn( hwnd, hrgn );
}


/***********************************************************************
 *		ANDROID_SetCapture
 */
void CDECL ANDROID_SetCapture( HWND hwnd, UINT flags )
{
    if (!(flags & (GUI_INMOVESIZE | GUI_INMENUMODE))) return;
    ioctl_set_capture( hwnd );
}


/***********************************************************************
 *		ANDROID_SetLayeredWindowAttributes
 */
void CDECL ANDROID_SetLayeredWindowAttributes( HWND hwnd, COLORREF key, BYTE alpha, DWORD flags )
{
    struct android_win_data *data;

    if (!(flags & LWA_ALPHA)) alpha = 255;
    if (!(flags & LWA_COLORKEY)) key = CLR_INVALID;

    if ((data = get_win_data( hwnd )))
    {
        if (data->surface) set_surface_color_key( data->surface, key );
        release_win_data( data );
    }
    ioctl_set_window_layered( hwnd, key, alpha );
}


/*****************************************************************************
 *              ANDROID_UpdateLayeredWindow
 */
BOOL CDECL ANDROID_UpdateLayeredWindow( HWND hwnd, const UPDATELAYEREDWINDOWINFO *info,
                                        const RECT *window_rect )
{
    struct window_surface *surface;
    struct android_win_data *data;
    BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, 0 };
    COLORREF color_key = (info->dwFlags & ULW_COLORKEY) ? info->crKey : CLR_INVALID;
    char buffer[FIELD_OFFSET( BITMAPINFO, bmiColors[256] )];
    BITMAPINFO *bmi = (BITMAPINFO *)buffer;
    void *src_bits, *dst_bits;
    RECT rect;
    HDC hdc = 0;
    HBITMAP dib;
    BOOL ret = FALSE;

    if (!(data = get_win_data( hwnd ))) return FALSE;

    rect = *window_rect;
    OffsetRect( &rect, -window_rect->left, -window_rect->top );

    surface = data->surface;
    if (!is_argb_surface( surface ))
    {
        if (surface) window_surface_release( surface );
        surface = NULL;
    }

    if (!surface || memcmp( &surface->rect, &rect, sizeof(RECT) ))
    {
        data->surface = create_surface( data->hwnd, &rect, color_key, TRUE );
        if (surface) window_surface_release( surface );
        surface = data->surface;
    }
    else set_surface_color_key( surface, color_key );

    if (surface) window_surface_add_ref( surface );
    release_win_data( data );

    if (!surface) return FALSE;
    if (!info->hdcSrc)
    {
        window_surface_release( surface );
        return TRUE;
    }

    dst_bits = surface->funcs->get_info( surface, bmi );

    if (!(dib = CreateDIBSection( info->hdcDst, bmi, DIB_RGB_COLORS, &src_bits, NULL, 0 ))) goto done;
    if (!(hdc = CreateCompatibleDC( 0 ))) goto done;

    SelectObject( hdc, dib );

    surface->funcs->lock( surface );

    if (info->prcDirty)
    {
        IntersectRect( &rect, &rect, info->prcDirty );
        memcpy( src_bits, dst_bits, bmi->bmiHeader.biSizeImage );
        PatBlt( hdc, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, BLACKNESS );
    }
    ret = GdiAlphaBlend( hdc, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
                         info->hdcSrc,
                         rect.left + (info->pptSrc ? info->pptSrc->x : 0),
                         rect.top + (info->pptSrc ? info->pptSrc->y : 0),
                         rect.right - rect.left, rect.bottom - rect.top,
                         (info->dwFlags & ULW_ALPHA) ? *info->pblend : blend );
    if (ret)
    {
        memcpy( dst_bits, src_bits, bmi->bmiHeader.biSizeImage );
        add_bounds_rect( surface->funcs->get_bounds( surface ), &rect );
    }

    surface->funcs->unlock( surface );
    surface->funcs->flush( surface );

done:
    window_surface_release( surface );
    if (hdc) DeleteDC( hdc );
    if (dib) DeleteObject( dib );
    return ret;
}


/**********************************************************************
 *           ANDROID_WindowMessage
 */
LRESULT CDECL ANDROID_WindowMessage( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp )
{
    struct android_win_data *data;

    switch (msg)
    {
    case WM_ANDROID_REFRESH:
        if ((data = get_win_data( hwnd )))
        {
            struct window_surface *surface = data->surface;
            if (surface)
            {
                surface->funcs->lock( surface );
                *surface->funcs->get_bounds( surface ) = surface->rect;
                surface->funcs->unlock( surface );
            }
            release_win_data( data );
        }
        return 0;
    case WM_ANDROID_IME_CONTROL:
        return Ime_Control(hwnd, msg, wp, lp);
    case WM_ANDROID_RENDERFORMAT:
        ioctl_render_clipboard_data( (int)wp );
        return 0;
    case WM_ANDROID_CLIPBOARD_REQUEST:
        handle_clipboard_request( (int)wp );
        return 0;
    default:
        FIXME( "got window msg %x hwnd %p wp %lx lp %lx\n", msg, hwnd, wp, lp );
        return 0;
    }
}


/***********************************************************************
 *		ANDROID_create_desktop
 */
BOOL CDECL ANDROID_create_desktop( UINT width, UINT height )
{
    /* wait until we receive the surface changed event */
    while (!screen_width)
    {
        if (wait_events( 2000 ) != 1)
        {
            ERR( "wait timed out\n" );
            break;
        }
        process_events( QS_ALLINPUT );
    }
    return TRUE;
}
