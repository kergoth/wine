/*
 * Android pseudo-device handling
 *
 * Copyright 2014 Alexandre Julliard
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
#include <errno.h>
#include <stdio.h>
#include <stdarg.h>
#include <sys/ioctl.h>

#define NONAMELESSUNION
#define NONAMELESSSTRUCT

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "winioctl.h"
#include "psapi.h"
#include "ddk/wdm.h"
#include "android.h"
#include "wine/server.h"
#include "wine/unicode.h"
#include "wine/library.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(android);

#ifndef SYNC_IOC_WAIT
#define SYNC_IOC_WAIT _IOW('>', 0, __s32)
#endif

extern NTSTATUS CDECL wine_ntoskrnl_main_loop( HANDLE stop_event );
static DEVICE_OBJECT *ioctl_device;
static HANDLE stop_event;
static HANDLE thread;
static JNIEnv *jni_env;

static DRIVER_OBJECT *driver_obj;

static const WCHAR driver_nameW[] = {'\\','D','r','i','v','e','r','\\','W','i','n','e','A','n','d','r','o','i','d',0 };
static const WCHAR device_nameW[] = {'\\','D','e','v','i','c','e','\\','W','i','n','e','A','n','d','r','o','i','d',0 };
static const WCHAR device_linkW[] = {'\\','?','?','\\','W','i','n','e','A','n','d','r','o','i','d',0 };

#define ANDROIDCONTROLTYPE  ((ULONG)'A')
#define ANDROID_IOCTL(n) CTL_CODE(ANDROIDCONTROLTYPE, n, METHOD_BUFFERED, FILE_READ_ACCESS)

#define IOCTL_ANDROID_DEQUEUE_BUFFER          ANDROID_IOCTL(0)
#define IOCTL_ANDROID_QUEUE_BUFFER            ANDROID_IOCTL(1)
#define IOCTL_ANDROID_CANCEL_BUFFER           ANDROID_IOCTL(2)
#define IOCTL_ANDROID_QUERY                   ANDROID_IOCTL(3)
#define IOCTL_ANDROID_PERFORM                 ANDROID_IOCTL(4)
#define IOCTL_ANDROID_SET_SWAP_INT            ANDROID_IOCTL(5)
#define IOCTL_ANDROID_CREATE_WINDOW           ANDROID_IOCTL(6)
#define IOCTL_ANDROID_DESTROY_WINDOW          ANDROID_IOCTL(7)
#define IOCTL_ANDROID_WINDOW_POS_CHANGED      ANDROID_IOCTL(8)
#define IOCTL_ANDROID_SET_WINDOW_FOCUS        ANDROID_IOCTL(9)
#define IOCTL_ANDROID_SET_WINDOW_TEXT         ANDROID_IOCTL(10)
#define IOCTL_ANDROID_SET_WINDOW_ICON         ANDROID_IOCTL(11)
#define IOCTL_ANDROID_SET_WINDOW_RGN          ANDROID_IOCTL(12)
#define IOCTL_ANDROID_SET_WINDOW_LAYERED      ANDROID_IOCTL(13)
#define IOCTL_ANDROID_SET_SURFACE_ALPHA       ANDROID_IOCTL(14)
#define IOCTL_ANDROID_SET_CAPTURE             ANDROID_IOCTL(15)
#define IOCTL_ANDROID_GAMEPAD_QUERY           ANDROID_IOCTL(16)
#define IOCTL_ANDROID_IMETEXT                 ANDROID_IOCTL(17)
#define IOCTL_ANDROID_GET_CLIPBOARD_FORMATS   ANDROID_IOCTL(18)
#define IOCTL_ANDROID_GET_CLIPBOARD_DATA      ANDROID_IOCTL(19)
#define IOCTL_ANDROID_RENDER_CLIPBOARD_DATA   ANDROID_IOCTL(20)
#define IOCTL_ANDROID_EMPTY_CLIPBOARD         ANDROID_IOCTL(21)
#define IOCTL_ANDROID_SET_CLIPBOARD_DATA      ANDROID_IOCTL(22)
#define IOCTL_ANDROID_END_CLIPBOARD_UPDATE    ANDROID_IOCTL(23)
#define IOCTL_ANDROID_ACQUIRE_CLIPBOARD       ANDROID_IOCTL(24)
#define IOCTL_ANDROID_EXPORT_CLIPBOARD_DATA   ANDROID_IOCTL(25)
#define IOCTL_ANDROID_IMEFINISH               ANDROID_IOCTL(26)

#define IOCTL_ANDROID_FIRST IOCTL_ANDROID_DEQUEUE_BUFFER
#define IOCTL_ANDROID_LAST  IOCTL_ANDROID_IMEFINISH

#define NB_CACHED_BUFFERS 4

struct native_buffer_wrapper;

/* buffer for storing a variable-size native handle inside an ioctl structure */
union native_handle_buffer
{
    native_handle_t handle;
    int space[256];
};

/* data about the native window in the context of the Java process */
struct native_win_data
{
    struct ANativeWindow       *parent;
    struct ANativeWindowBuffer *buffers[NB_CACHED_BUFFERS];
    void                       *mappings[NB_CACHED_BUFFERS];
    HWND                        hwnd;
    int                         buffer_format;
    int                         buffer_lru[NB_CACHED_BUFFERS];
};

/* wrapper for a native window in the context of the client (non-Java) process */
struct native_win_wrapper
{
    struct ANativeWindow          win;
    struct native_buffer_wrapper *buffers[NB_CACHED_BUFFERS];
    struct ANativeWindowBuffer   *locked_buffer;
    LONG                          ref;
    HWND                          hwnd;
};

/* wrapper for a native buffer in the context of the client (non-Java) process */
struct native_buffer_wrapper
{
    struct ANativeWindowBuffer buffer;
    LONG                       ref;
    HWND                       hwnd;
    void                      *bits;
    int                        buffer_id;
    union native_handle_buffer native_handle;
};

struct ioctl_header
{
    int hwnd;
};

struct ioctl_android_dequeueBuffer
{
    struct ioctl_header hdr;
    int                 win32;
    int                 width;
    int                 height;
    int                 stride;
    int                 format;
    int                 usage;
    int                 buffer_id;
    union native_handle_buffer native_handle;
};

struct ioctl_android_queueBuffer
{
    struct ioctl_header hdr;
    int                 buffer_id;
};

struct ioctl_android_cancelBuffer
{
    struct ioctl_header hdr;
    int                 buffer_id;
};

struct ioctl_android_query
{
    struct ioctl_header hdr;
    int                 what;
    int                 value;
};

struct ioctl_android_perform
{
    struct ioctl_header hdr;
    int                 operation;
    int                 args[4];
};

struct ioctl_android_set_swap_interval
{
    struct ioctl_header hdr;
    int                 interval;
};

struct ioctl_android_create_window
{
    struct ioctl_header hdr;
};

struct ioctl_android_destroy_window
{
    struct ioctl_header hdr;
};

struct ioctl_android_window_pos_changed
{
    struct ioctl_header hdr;
    int                 left;
    int                 top;
    int                 right;
    int                 bottom;
    int                 style;
    int                 flags;
    int                 after;
    int                 owner;
};

struct ioctl_android_set_window_focus
{
    struct ioctl_header hdr;
};

struct ioctl_android_set_window_text
{
    struct ioctl_header hdr;
    WCHAR               text[1];
};

struct ioctl_android_set_window_icon
{
    struct ioctl_header hdr;
    int                 width;
    int                 height;
    int                 bits[1];
};

struct ioctl_android_set_window_rgn
{
    struct ioctl_header hdr;
    int                 has_region;
};

struct ioctl_android_set_window_layered
{
    struct ioctl_header hdr;
    int                 key;
    int                 alpha;
};

struct ioctl_android_set_surface_alpha
{
    struct ioctl_header hdr;
    int                 has_alpha;
};

struct ioctl_android_set_capture
{
    struct ioctl_header hdr;
};

struct ioctl_android_gamepad_value
{
    struct ioctl_header hdr;
    int                 index;
    int                 device;
    union
    {
            int          count;
            di_name      name;
            di_value_set value;
    } data;
};

struct ioctl_android_ime_text
{
    struct ioctl_header hdr;
    INT                 target;
    INT                 length;
    INT                 cursor;
    WCHAR               text[1];
};

struct ioctl_android_ime_finish
{
    struct ioctl_header hdr;
    INT                 target;
};

struct ioctl_android_clipboard_formats
{
    struct ioctl_header hdr;
    DWORD               seqno;
    DWORD               count;
    UINT                formats[1];
};

struct ioctl_android_clipboard_data
{
    struct ioctl_header hdr;
    UINT                format;
    BOOL                pending;
    BOOL                format_present;
    UINT                size;
    BYTE                data[1];
};

struct ioctl_android_render_clipboard_data
{
    struct ioctl_header hdr;
    INT                 android_format;
};

struct ioctl_android_empty_clipboard
{
    struct ioctl_header hdr;
};

struct ioctl_android_end_clipboard_update
{
    struct ioctl_header hdr;
};

struct ioctl_android_acquire_clipboard
{
    struct ioctl_header hdr;
};

struct irp_entry
{
    struct list entry;
    HWND        hwnd;
    IRP        *irp;
    DWORD       client;
};

static struct list irp_queue = LIST_INIT( irp_queue );

static HWND capture_window;
static DWORD current_client;

static inline BOOL is_in_desktop_process(void)
{
    return thread != NULL;
}

static inline DWORD current_client_id(void)
{
    return current_client ? current_client : HandleToUlong( PsGetCurrentProcessId() );
}

static inline BOOL is_client_in_process(void)
{
    return current_client_id() == GetCurrentProcessId();
}

/* queue an IRP for later processing once the window becomes ready */
static NTSTATUS queue_irp( HWND hwnd, IRP *irp )
{
    struct irp_entry *entry = HeapAlloc( GetProcessHeap(), 0, sizeof(*entry) );

    if (!entry) return STATUS_NO_MEMORY;
    TRACE( "hwnd %p irp %p\n", hwnd, irp );
    entry->hwnd = hwnd;
    entry->irp = irp;
    entry->client = current_client_id();
    list_add_tail( &irp_queue, &entry->entry );
    return STATUS_PENDING;
}

/* process IRPs pending for a given window */
static void process_pending_irp( HWND hwnd )
{
    PDRIVER_DISPATCH dispatch = ioctl_device->DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL];
    struct irp_entry *entry, *next;

    LIST_FOR_EACH_ENTRY_SAFE( entry, next, &irp_queue, struct irp_entry, entry )
    {
        if (entry->hwnd != hwnd) continue;
        list_remove( &entry->entry );
        TRACE( "hwnd %p irp %p\n", hwnd, entry->irp );
        current_client = entry->client;
        dispatch( ioctl_device, entry->irp );
        current_client = 0;
        HeapFree( GetProcessHeap(), 0, entry );
    }
}

#ifdef __i386__  /* the Java VM uses %fs for its own purposes, so we need to wrap the calls */

static WORD orig_fs, java_fs;
static inline void wrap_java_call(void)   { wine_set_fs( java_fs ); }
static inline void unwrap_java_call(void) { wine_set_fs( orig_fs ); }

#else

static inline void wrap_java_call(void) { }
static inline void unwrap_java_call(void) { }

#endif  /* __i386__ */

/* FIXME: quick & dirty window handle context management */
static struct native_win_data *data_map[65536];

static struct native_win_data *get_native_win_data( HWND hwnd )
{
    struct native_win_data *data = data_map[LOWORD(hwnd)];

    if (data && data->hwnd == hwnd) return data;
    WARN( "unknown win %p\n", hwnd );
    return NULL;
}

static void wait_fence_and_close( int fence )
{
    __s32 timeout = 1000;  /* FIXME: should be -1 for infinite timeout */

    if (fence == -1) return;
    ioctl( fence, SYNC_IOC_WAIT, &timeout );
    close( fence );
}

static int duplicate_fd( HANDLE client, int fd )
{
    HANDLE handle, ret = 0;

    if (!wine_server_fd_to_handle( dup(fd), GENERIC_READ | SYNCHRONIZE, 0, &handle ))
        DuplicateHandle( GetCurrentProcess(), handle, client, &ret,
                         DUPLICATE_SAME_ACCESS, FALSE, DUP_HANDLE_CLOSE_SOURCE );

    if (!ret) return -1;
    return HandleToLong( ret );
}

static int map_native_handle( union native_handle_buffer *dest, const native_handle_t *src,
                              HANDLE mapping, HANDLE client )
{
    const size_t size = offsetof( native_handle_t, data[src->numFds + src->numInts] );
    int i;

    if (mapping)  /* only duplicate the mapping handle */
    {
        HANDLE ret = 0;
        if (!DuplicateHandle( GetCurrentProcess(), mapping, client, &ret,
                              DUPLICATE_SAME_ACCESS, FALSE, DUP_HANDLE_CLOSE_SOURCE ))
            return -ENOSPC;
        dest->handle.numFds = 0;
        dest->handle.numInts = 1;
        dest->handle.data[0] = HandleToLong( ret );
        return 0;
    }
    if (is_client_in_process())  /* transfer the actual handle pointer */
    {
        dest->handle.numFds = 0;
        dest->handle.numInts = sizeof(src) / sizeof(int);
        memcpy( dest->handle.data, &src, sizeof(src) );
        return 0;
    }
    if (size > sizeof(*dest)) return -ENOSPC;
    memcpy( dest, src, size );
    /* transfer file descriptors to the client process */
    for (i = 0; i < dest->handle.numFds; i++)
        dest->handle.data[i] = duplicate_fd( client, src->data[i] );
    return 0;
}

static native_handle_t *unmap_native_handle( const native_handle_t *src )
{
    const size_t size = offsetof( native_handle_t, data[src->numFds + src->numInts] );
    native_handle_t *dest;
    int i;

    if (!is_in_desktop_process())
    {
        dest = HeapAlloc( GetProcessHeap(), 0, size );
        memcpy( dest, src, size );
        /* fetch file descriptors passed from the server process */
        for (i = 0; i < dest->numFds; i++)
            wine_server_handle_to_fd( LongToHandle(src->data[i]), GENERIC_READ | SYNCHRONIZE,
                                      &dest->data[i], NULL );
    }
    else memcpy( &dest, src->data, sizeof(dest) );
    return dest;
}

static void close_native_handle( native_handle_t *handle )
{
    int i;

    for (i = 0; i < handle->numFds; i++) close( handle->data[i] );
    HeapFree( GetProcessHeap(), 0, handle );
}

HWND get_capture_window(void)
{
    return capture_window;
}

/* insert a buffer index at the head of the LRU list */
static void insert_buffer_lru( struct native_win_data *win, int index )
{
    unsigned int i;

    for (i = 0; i < NB_CACHED_BUFFERS; i++)
    {
        if (win->buffer_lru[i] == index) break;
        if (win->buffer_lru[i] == -1) break;
    }

    assert( i < NB_CACHED_BUFFERS );
    memmove( win->buffer_lru + 1, win->buffer_lru, i * sizeof(win->buffer_lru[0]) );
    win->buffer_lru[0] = index;
}

static int register_buffer( struct native_win_data *win, struct ANativeWindowBuffer *buffer,
                            HANDLE *mapping, int *is_new )
{
    unsigned int i;

    *is_new = 0;
    for (i = 0; i < NB_CACHED_BUFFERS; i++)
    {
        if (win->buffers[i] == buffer) goto done;
        if (!win->buffers[i]) break;
    }

    if (i == NB_CACHED_BUFFERS)
    {
        /* reuse the least recently used buffer */
        i = win->buffer_lru[NB_CACHED_BUFFERS - 1];
        assert( i < NB_CACHED_BUFFERS );

        TRACE( "%p %p evicting buffer %p id %d from cache\n",
               win->hwnd, win->parent, win->buffers[i], i );
        win->buffers[i]->common.decRef( &win->buffers[i]->common );
        if (win->mappings[i]) UnmapViewOfFile( win->mappings[i] );
    }

    win->buffers[i] = buffer;
    win->mappings[i] = NULL;

    if (mapping)
    {
        *mapping = CreateFileMappingW( INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0,
                                       buffer->stride * buffer->height * 4, NULL );
        win->mappings[i] = MapViewOfFile( *mapping, FILE_MAP_READ, 0, 0, 0 );
    }
    buffer->common.incRef( &buffer->common );
    *is_new = 1;
    TRACE( "%p %p %p -> %d\n", win->hwnd, win->parent, buffer, i );

done:
    insert_buffer_lru( win, i );
    return i;
}

static struct ANativeWindowBuffer *get_registered_buffer( struct native_win_data *win, int id )
{
    if (id < 0 || id >= NB_CACHED_BUFFERS || !win->buffers[id])
    {
        ERR( "unknown buffer %d for %p %p\n", id, win->hwnd, win->parent );
        return NULL;
    }
    return win->buffers[id];
}

static void release_native_window( struct native_win_data *data )
{
    unsigned int i;

    if (data->parent) pANativeWindow_release( data->parent );
    for (i = 0; i < NB_CACHED_BUFFERS; i++)
    {
        if (data->buffers[i]) data->buffers[i]->common.decRef( &data->buffers[i]->common );
        if (data->mappings[i]) UnmapViewOfFile( data->mappings[i] );
        data->buffer_lru[i] = -1;
    }
    memset( data->buffers, 0, sizeof(data->buffers) );
    memset( data->mappings, 0, sizeof(data->mappings) );
}

static void free_native_win_data( struct native_win_data *data )
{
    unsigned int idx = LOWORD( data->hwnd );

    InterlockedCompareExchangePointer( (void **)&capture_window, 0, data->hwnd );
    release_native_window( data );
    HeapFree( GetProcessHeap(), 0, data );
    data_map[idx] = NULL;
}

static struct native_win_data *create_native_win_data( HWND hwnd )
{
    unsigned int i, idx = LOWORD( hwnd );
    struct native_win_data *data = data_map[idx];

    if (data)
    {
        WARN( "data for %p not freed correctly\n", data->hwnd );
        free_native_win_data( data );
    }
    if (!(data = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*data) ))) return NULL;
    data->hwnd = hwnd;
    data->buffer_format = PF_BGRA_8888;
    data_map[idx] = data;
    for (i = 0; i < NB_CACHED_BUFFERS; i++) data->buffer_lru[i] = -1;
    return data;
}

static void CALLBACK register_native_window_callback( ULONG_PTR arg1, ULONG_PTR arg2, ULONG_PTR arg3 )
{
    HWND hwnd = (HWND)arg1;
    struct ANativeWindow *win = (struct ANativeWindow *)arg2;
    struct native_win_data *data = get_native_win_data( hwnd );

    if (!data || data->parent == win)
    {
        if (win) pANativeWindow_release( win );
        return;
    }

    release_native_window( data );
    data->parent = win;
    if (win)
    {
        wrap_java_call();
        win->perform( win, NATIVE_WINDOW_SET_BUFFERS_FORMAT, data->buffer_format );
        /* switch to asynchronous mode to avoid buffer queue deadlocks */
        win->setSwapInterval( win, 0 );
        unwrap_java_call();
        PostMessageW( hwnd, WM_ANDROID_REFRESH, 0, 0 );
    }
    TRACE( "%p -> %p win %p\n", hwnd, data, win );
    process_pending_irp( hwnd );
}

/* register a native window received from the Java side for use in ioctls */
void register_native_window( HWND hwnd, struct ANativeWindow *win )
{
    NtQueueApcThread( thread, register_native_window_callback, (ULONG_PTR)hwnd, (ULONG_PTR)win, 0 );
}

static NTSTATUS android_error_to_status( int err )
{
    switch (err)
    {
    case 0:            return STATUS_SUCCESS;
    case -ENOMEM:      return STATUS_NO_MEMORY;
    case -ENOSYS:      return STATUS_NOT_SUPPORTED;
    case -EINVAL:      return STATUS_INVALID_PARAMETER;
    case -ENOENT:      return STATUS_INVALID_HANDLE;
    case -EPERM:       return STATUS_ACCESS_DENIED;
    case -ENODEV:      return STATUS_NO_SUCH_DEVICE;
    case -EEXIST:      return STATUS_DUPLICATE_NAME;
    case -EPIPE:       return STATUS_PIPE_DISCONNECTED;
    case -ENODATA:     return STATUS_NO_MORE_FILES;
    case -ETIMEDOUT:   return STATUS_IO_TIMEOUT;
    case -EBADMSG:     return STATUS_INVALID_DEVICE_REQUEST;
    case -EWOULDBLOCK: return STATUS_DEVICE_NOT_READY;
    default:
        FIXME( "unmapped error %d\n", err );
        return STATUS_UNSUCCESSFUL;
    }
}

static int status_to_android_error( NTSTATUS status )
{
    switch (status)
    {
    case STATUS_SUCCESS:                return 0;
    case STATUS_NO_MEMORY:              return -ENOMEM;
    case STATUS_NOT_SUPPORTED:          return -ENOSYS;
    case STATUS_INVALID_PARAMETER:      return -EINVAL;
    case STATUS_BUFFER_OVERFLOW:        return -EINVAL;
    case STATUS_INVALID_HANDLE:         return -ENOENT;
    case STATUS_ACCESS_DENIED:          return -EPERM;
    case STATUS_NO_SUCH_DEVICE:         return -ENODEV;
    case STATUS_DUPLICATE_NAME:         return -EEXIST;
    case STATUS_PIPE_DISCONNECTED:      return -EPIPE;
    case STATUS_NO_MORE_FILES:          return -ENODATA;
    case STATUS_IO_TIMEOUT:             return -ETIMEDOUT;
    case STATUS_INVALID_DEVICE_REQUEST: return -EBADMSG;
    case STATUS_DEVICE_NOT_READY:       return -EWOULDBLOCK;
    default:
        FIXME( "unmapped status %08x\n", status );
        return -EINVAL;
    }
}

static jobject load_java_method( jmethodID *method, const char *name, const char *args )
{
    jobject object = wine_get_java_object();

    if (!*method)
    {
        jclass class;

        wrap_java_call();
        class = (*jni_env)->GetObjectClass( jni_env, object );
        *method = (*jni_env)->GetMethodID( jni_env, class, name, args );
        unwrap_java_call();
        if (!*method)
        {
            FIXME( "method %s not found\n", name );
            return NULL;
        }
    }
    return object;
}

static NTSTATUS dequeueBuffer_ioctl( void *data, DWORD in_size, DWORD out_size, ULONG_PTR *ret_size )
{
    struct ANativeWindow *parent;
    struct ioctl_android_dequeueBuffer *res = data;
    struct native_win_data *win_data;
    struct ANativeWindowBuffer *buffer;
    int fence, ret, is_new;

    if (out_size < sizeof( *res )) return STATUS_BUFFER_OVERFLOW;

    if (in_size < offsetof( struct ioctl_android_dequeueBuffer, native_handle ))
        return STATUS_INVALID_PARAMETER;

    if (!(win_data = get_native_win_data( LongToHandle(res->hdr.hwnd) ))) return STATUS_INVALID_HANDLE;
    if (!(parent = win_data->parent)) return STATUS_PENDING;

    *ret_size = offsetof( struct ioctl_android_dequeueBuffer, native_handle );
    wrap_java_call();
    ret = parent->dequeueBuffer( parent, &buffer, &fence );
    unwrap_java_call();
    if (!ret)
    {
        HANDLE mapping = 0;

        TRACE( "%08x got buffer %p fence %d\n", res->hdr.hwnd, buffer, fence );
        res->width  = buffer->width;
        res->height = buffer->height;
        res->stride = buffer->stride;
        res->format = buffer->format;
        res->usage  = buffer->usage;
        res->buffer_id = register_buffer( win_data, buffer, res->win32 ? &mapping : NULL, &is_new );
        if (is_new)
        {
            HANDLE process = OpenProcess( PROCESS_DUP_HANDLE, FALSE, current_client_id() );
            map_native_handle( &res->native_handle, buffer->handle, mapping, process );
            CloseHandle( process );
            *ret_size = sizeof( *res );
        }
        wait_fence_and_close( fence );
        return STATUS_SUCCESS;
    }
    ERR( "%08x failed %d\n", res->hdr.hwnd, ret );
    return android_error_to_status( ret );
}

static NTSTATUS cancelBuffer_ioctl( void *data, DWORD in_size, DWORD out_size, ULONG_PTR *ret_size )
{
    struct ioctl_android_cancelBuffer *res = data;
    struct ANativeWindow *parent;
    struct ANativeWindowBuffer *buffer;
    struct native_win_data *win_data;
    int ret;

    if (in_size < sizeof(*res)) return STATUS_INVALID_PARAMETER;

    if (!(win_data = get_native_win_data( LongToHandle(res->hdr.hwnd) ))) return STATUS_INVALID_HANDLE;
    if (!(parent = win_data->parent)) return STATUS_PENDING;

    if (!(buffer = get_registered_buffer( win_data, res->buffer_id ))) return STATUS_INVALID_HANDLE;

    TRACE( "%08x buffer %p\n", res->hdr.hwnd, buffer );
    wrap_java_call();
    ret = parent->cancelBuffer( parent, buffer, -1 );
    unwrap_java_call();
    return android_error_to_status( ret );
}

static NTSTATUS queueBuffer_ioctl( void *data, DWORD in_size, DWORD out_size, ULONG_PTR *ret_size )
{
    struct ioctl_android_queueBuffer *res = data;
    struct ANativeWindow *parent;
    struct ANativeWindowBuffer *buffer;
    struct native_win_data *win_data;
    int ret;

    if (in_size < sizeof(*res)) return STATUS_INVALID_PARAMETER;

    if (!(win_data = get_native_win_data( LongToHandle(res->hdr.hwnd) ))) return STATUS_INVALID_HANDLE;
    if (!(parent = win_data->parent)) return STATUS_PENDING;

    if (!(buffer = get_registered_buffer( win_data, res->buffer_id ))) return STATUS_INVALID_HANDLE;

    TRACE( "%08x buffer %p mapping %p\n", res->hdr.hwnd, buffer, win_data->mappings[res->buffer_id] );
    if (win_data->mappings[res->buffer_id])
    {
        void *bits;
        int ret = gralloc_module->lock( gralloc_module, buffer->handle,
                                        GRALLOC_USAGE_SW_READ_OFTEN | GRALLOC_USAGE_SW_WRITE_OFTEN,
                                        0, 0, buffer->width, buffer->height, &bits );
        if (ret) return android_error_to_status( ret );
        memcpy( bits, win_data->mappings[res->buffer_id], buffer->stride * buffer->height * 4 );
        gralloc_module->unlock( gralloc_module, buffer->handle );
    }
    wrap_java_call();
    ret = parent->queueBuffer( parent, buffer, -1 );
    unwrap_java_call();
    return android_error_to_status( ret );
}

static NTSTATUS setSwapInterval_ioctl( void *data, DWORD in_size, DWORD out_size, ULONG_PTR *ret_size )
{
    struct ioctl_android_set_swap_interval *res = data;
    struct ANativeWindow *parent;
    struct native_win_data *win_data;
    int ret;

    if (in_size < sizeof(*res)) return STATUS_INVALID_PARAMETER;

    if (!(win_data = get_native_win_data( LongToHandle(res->hdr.hwnd) ))) return STATUS_INVALID_HANDLE;
    if (!(parent = win_data->parent)) return STATUS_PENDING;

    wrap_java_call();
    ret = parent->setSwapInterval( parent, res->interval );
    unwrap_java_call();
    return ret;
}

static NTSTATUS query_ioctl( void *data, DWORD in_size, DWORD out_size, ULONG_PTR *ret_size )
{
    struct ioctl_android_query *res = data;
    struct ANativeWindow *parent;
    struct native_win_data *win_data;
    int ret;

    if (in_size < sizeof(*res)) return STATUS_INVALID_PARAMETER;
    if (out_size < sizeof(*res)) return STATUS_BUFFER_OVERFLOW;

    if (!(win_data = get_native_win_data( LongToHandle(res->hdr.hwnd) ))) return STATUS_INVALID_HANDLE;
    if (!(parent = win_data->parent)) return STATUS_PENDING;

    *ret_size = sizeof( *res );
    wrap_java_call();
    ret = parent->query( parent, res->what, &res->value );
    unwrap_java_call();
    return android_error_to_status( ret );
}

static NTSTATUS start_opengl( int hwnd )
{
    static jmethodID method;
    jobject object;
    struct native_win_data *win_data;

    if (!(win_data = get_native_win_data( LongToHandle(hwnd) ))) return STATUS_INVALID_HANDLE;

    TRACE( "hwnd %08x\n", hwnd );

    if (!(object = load_java_method( &method, "startOpenGL", "(I)V" ))) return STATUS_NOT_SUPPORTED;

    wrap_java_call();
    (*jni_env)->CallVoidMethod( jni_env, object, method, hwnd );
    unwrap_java_call();
    return STATUS_SUCCESS;
}

static NTSTATUS perform_ioctl( void *data, DWORD in_size, DWORD out_size, ULONG_PTR *ret_size )
{
    struct ioctl_android_perform *res = data;
    struct ANativeWindow *parent;
    struct native_win_data *win_data;
    int ret = -ENOENT;

    if (in_size < sizeof(*res)) return STATUS_INVALID_PARAMETER;

    if (!(win_data = get_native_win_data( LongToHandle(res->hdr.hwnd) ))) return STATUS_INVALID_HANDLE;
    if (res->operation == NATIVE_WINDOW_API_CONNECT) start_opengl( res->hdr.hwnd );
    if (!(parent = win_data->parent)) return STATUS_PENDING;

    switch (res->operation)
    {
    case NATIVE_WINDOW_SET_BUFFERS_FORMAT:
        wrap_java_call();
        ret = parent->perform( parent, res->operation, res->args[0] );
        unwrap_java_call();
        if (!ret) win_data->buffer_format = res->args[0];
        break;
    case NATIVE_WINDOW_SET_USAGE:
    case NATIVE_WINDOW_SET_BUFFERS_TRANSFORM:
    case NATIVE_WINDOW_SET_SCALING_MODE:
    case NATIVE_WINDOW_API_CONNECT:
    case NATIVE_WINDOW_API_DISCONNECT:
        wrap_java_call();
        ret = parent->perform( parent, res->operation, res->args[0] );
        unwrap_java_call();
        break;
    case NATIVE_WINDOW_SET_BUFFER_COUNT:
        wrap_java_call();
        ret = parent->perform( parent, res->operation, (size_t)res->args[0] );
        unwrap_java_call();
        break;
    case NATIVE_WINDOW_SET_BUFFERS_DIMENSIONS:
    case NATIVE_WINDOW_SET_BUFFERS_USER_DIMENSIONS:
        wrap_java_call();
        ret = parent->perform( parent, res->operation, res->args[0], res->args[1] );
        unwrap_java_call();
        break;
    case NATIVE_WINDOW_SET_BUFFERS_GEOMETRY:
        wrap_java_call();
        ret = parent->perform( parent, res->operation, res->args[0], res->args[1], res->args[2] );
        unwrap_java_call();
        break;
    case NATIVE_WINDOW_SET_BUFFERS_TIMESTAMP:
        wrap_java_call();
        ret = parent->perform( parent, res->operation, res->args[0] | ((int64_t)res->args[1] << 32) );
        unwrap_java_call();
        break;
    case NATIVE_WINDOW_CONNECT:
    case NATIVE_WINDOW_DISCONNECT:
    case NATIVE_WINDOW_UNLOCK_AND_POST:
        wrap_java_call();
        ret = parent->perform( parent, res->operation );
        unwrap_java_call();
        break;
    case NATIVE_WINDOW_SET_CROP:
    {
        android_native_rect_t rect;
        rect.left   = res->args[0];
        rect.top    = res->args[1];
        rect.right  = res->args[2];
        rect.bottom = res->args[3];
        wrap_java_call();
        ret = parent->perform( parent, res->operation, &rect );
        unwrap_java_call();
        break;
    }
    case NATIVE_WINDOW_LOCK:
    default:
        FIXME( "unsupported perform op %d\n", res->operation );
        break;
    }
    return android_error_to_status( ret );
}

static void create_desktop_window( HWND hwnd )
{
    static jmethodID method;
    jobject object;

    if (!(object = load_java_method( &method, "createDesktopWindow", "(I)V" ))) return;

    wrap_java_call();
    (*jni_env)->CallVoidMethod( jni_env, object, method, HandleToLong( hwnd ));
    unwrap_java_call();
}

static NTSTATUS createWindow_ioctl( void *data, DWORD in_size, DWORD out_size, ULONG_PTR *ret_size )
{
    static jmethodID method;
    jobject object;
    struct ioctl_android_create_window *res = data;
    struct native_win_data *win_data;
    jstring str;
    char modpath[MAX_PATH], *modname;
    HANDLE process = OpenProcess( PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, current_client_id() );
    DWORD rc;

    if (in_size < sizeof(*res)) return STATUS_INVALID_PARAMETER;

    if (!(win_data = create_native_win_data( LongToHandle(res->hdr.hwnd) ))) return STATUS_NO_MEMORY;

    if ((rc = GetModuleFileNameExA( process, NULL, modpath, MAX_PATH )))
    {
        modname = strrchr( modpath, '\\' );
        modname = modname ? modname + 1 : modpath;
    }
    else
    {
        ERR( "Failed to get client executable name: %d\n", GetLastError() );
        modname = "none";
    }

    TRACE( "hwnd %08x modname %s\n", res->hdr.hwnd, modname );

    if (!(object = load_java_method( &method, "createWindow", "(ILjava/lang/String;)V" ))) return STATUS_NOT_SUPPORTED;

    wrap_java_call();
    str = (*jni_env)->NewStringUTF( jni_env, modname );
    (*jni_env)->CallVoidMethod( jni_env, object, method, res->hdr.hwnd, str );
    (*jni_env)->DeleteLocalRef( jni_env, str );
    unwrap_java_call();
    CloseHandle( process );
    return STATUS_SUCCESS;
}

static NTSTATUS destroyWindow_ioctl( void *data, DWORD in_size, DWORD out_size, ULONG_PTR *ret_size )
{
    static jmethodID method;
    jobject object;
    struct ioctl_android_destroy_window *res = data;
    struct native_win_data *win_data;

    if (in_size < sizeof(*res)) return STATUS_INVALID_PARAMETER;

    if (!(win_data = get_native_win_data( LongToHandle(res->hdr.hwnd) ))) return STATUS_INVALID_HANDLE;

    TRACE( "hwnd %08x\n", res->hdr.hwnd );

    if (!(object = load_java_method( &method, "destroyWindow", "(I)V" ))) return STATUS_NOT_SUPPORTED;

    wrap_java_call();
    (*jni_env)->CallVoidMethod( jni_env, object, method, res->hdr.hwnd );
    unwrap_java_call();
    free_native_win_data( win_data );
    process_pending_irp( LongToHandle(res->hdr.hwnd) );
    return STATUS_SUCCESS;
}

static NTSTATUS windowPosChanged_ioctl( void *data, DWORD in_size, DWORD out_size, ULONG_PTR *ret_size )
{
    static jmethodID method;
    jobject object;
    struct ioctl_android_window_pos_changed *res = data;
    struct native_win_data *win_data;

    if (in_size < sizeof(*res)) return STATUS_INVALID_PARAMETER;

    if (!(win_data = get_native_win_data( LongToHandle(res->hdr.hwnd) ))) return STATUS_INVALID_HANDLE;

    TRACE( "hwnd %08x pos %d,%d-%d,%d style %08x flags %08x after %08x owner %08x\n",
           res->hdr.hwnd, res->left, res->top, res->right, res->bottom,
           res->style, res->flags, res->after, res->owner );

    if (!(object = load_java_method( &method, "windowPosChanged", "(IIIIIIIII)V" )))
        return STATUS_NOT_SUPPORTED;

    wrap_java_call();
    (*jni_env)->CallVoidMethod( jni_env, object, method, res->hdr.hwnd, res->flags, res->after, res->owner,
                                res->style, res->left, res->top, res->right, res->bottom );
    unwrap_java_call();
    return STATUS_SUCCESS;
}

static NTSTATUS setWindowFocus_ioctl( void *data, DWORD in_size, DWORD out_size, ULONG_PTR *ret_size )
{
    static jmethodID method;
    jobject object;
    struct ioctl_android_set_window_focus *res = data;
    struct native_win_data *win_data;

    if (in_size < sizeof(*res)) return STATUS_INVALID_PARAMETER;

    if (!(win_data = get_native_win_data( LongToHandle(res->hdr.hwnd) ))) return STATUS_INVALID_HANDLE;

    TRACE( "hwnd %08x\n", res->hdr.hwnd );

    if (!(object = load_java_method( &method, "setFocus", "(I)V" ))) return STATUS_NOT_SUPPORTED;

    wrap_java_call();
    (*jni_env)->CallVoidMethod( jni_env, object, method, res->hdr.hwnd );
    unwrap_java_call();
    return STATUS_SUCCESS;
}

static NTSTATUS setWindowText_ioctl( void *data, DWORD in_size, DWORD out_size, ULONG_PTR *ret_size )
{
    static jmethodID method;
    jobject object;
    jstring str;
    int len;
    struct ioctl_android_set_window_text *res = data;
    struct native_win_data *win_data;

    len = in_size - offsetof( struct ioctl_android_set_window_text, text );
    if (len < 0 || (len % sizeof(WCHAR))) return STATUS_INVALID_PARAMETER;
    len /= sizeof(WCHAR);

    if (!(win_data = get_native_win_data( LongToHandle(res->hdr.hwnd) ))) return STATUS_INVALID_HANDLE;

    TRACE( "hwnd %08x text %s\n", res->hdr.hwnd, wine_dbgstr_wn( res->text, len ));

    if (!(object = load_java_method( &method, "setWindowText", "(ILjava/lang/String;)V" )))
        return STATUS_NOT_SUPPORTED;

    wrap_java_call();
    str = (*jni_env)->NewString( jni_env, res->text, len );
    (*jni_env)->CallVoidMethod( jni_env, object, method, res->hdr.hwnd, str );
    (*jni_env)->DeleteLocalRef( jni_env, str );
    unwrap_java_call();
    return STATUS_SUCCESS;
}

static NTSTATUS setWindowIcon_ioctl( void *data, DWORD in_size, DWORD out_size, ULONG_PTR *ret_size )
{
    static jmethodID method;
    jobject object;
    int size;
    struct ioctl_android_set_window_icon *res = data;
    struct native_win_data *win_data;

    if (in_size < offsetof( struct ioctl_android_set_window_icon, bits )) return STATUS_INVALID_PARAMETER;

    if (!(win_data = get_native_win_data( LongToHandle(res->hdr.hwnd) ))) return STATUS_INVALID_HANDLE;

    if (res->width < 0 || res->height < 0 || res->width > 256 || res->height > 256)
        return STATUS_INVALID_PARAMETER;

    size = res->width * res->height;
    if (in_size != offsetof( struct ioctl_android_set_window_icon, bits[size] ))
        return STATUS_INVALID_PARAMETER;

    TRACE( "hwnd %08x size %d\n", res->hdr.hwnd, size );

    if (!(object = load_java_method( &method, "setWindowIcon", "(III[I)V" )))
        return STATUS_NOT_SUPPORTED;

    wrap_java_call();

    if (size)
    {
        jintArray array = (*jni_env)->NewIntArray( jni_env, size );
        (*jni_env)->SetIntArrayRegion( jni_env, array, 0, size, (jint *)res->bits );
        (*jni_env)->CallVoidMethod( jni_env, object, method, res->hdr.hwnd, res->width, res->height, array );
        (*jni_env)->DeleteLocalRef( jni_env, array );
    }
    else (*jni_env)->CallVoidMethod( jni_env, object, method, res->hdr.hwnd, 0, 0, 0 );

    unwrap_java_call();

    return STATUS_SUCCESS;
}

static NTSTATUS setWindowRgn_ioctl( void *data, DWORD in_size, DWORD out_size, ULONG_PTR *ret_size )
{
    static jmethodID method;
    jobject object;
    struct ioctl_android_set_window_rgn *res = data;
    struct native_win_data *win_data;

    if (in_size < sizeof(*res)) return STATUS_INVALID_PARAMETER;

    if (!(win_data = get_native_win_data( LongToHandle(res->hdr.hwnd) ))) return STATUS_INVALID_HANDLE;

    TRACE( "hwnd %08x region %d\n", res->hdr.hwnd, res->has_region );

    if (!(object = load_java_method( &method, "setWindowRgn", "(II)V" ))) return STATUS_NOT_SUPPORTED;

    wrap_java_call();
    (*jni_env)->CallVoidMethod( jni_env, object, method, res->hdr.hwnd, res->has_region );
    unwrap_java_call();
    return STATUS_SUCCESS;
}

static NTSTATUS setWindowLayered_ioctl( void *data, DWORD in_size, DWORD out_size, ULONG_PTR *ret_size )
{
    static jmethodID method;
    jobject object;
    struct ioctl_android_set_window_layered *res = data;
    struct native_win_data *win_data;

    if (in_size < sizeof(*res)) return STATUS_INVALID_PARAMETER;

    if (!(win_data = get_native_win_data( LongToHandle(res->hdr.hwnd) ))) return STATUS_INVALID_HANDLE;

    TRACE( "hwnd %08x key %08x alpha %d\n", res->hdr.hwnd, res->key, res->alpha );

    if (!(object = load_java_method( &method, "setWindowLayered", "(III)V" ))) return STATUS_NOT_SUPPORTED;

    wrap_java_call();
    (*jni_env)->CallVoidMethod( jni_env, object, method, res->hdr.hwnd, res->key, res->alpha );
    unwrap_java_call();
    return STATUS_SUCCESS;
}

static NTSTATUS setSurfaceAlpha_ioctl( void *data, DWORD in_size, DWORD out_size, ULONG_PTR *ret_size )
{
    static jmethodID method;
    jobject object;
    struct ioctl_android_set_surface_alpha *res = data;
    struct native_win_data *win_data;

    if (in_size < sizeof(*res)) return STATUS_INVALID_PARAMETER;

    if (!(win_data = get_native_win_data( LongToHandle(res->hdr.hwnd) ))) return STATUS_INVALID_HANDLE;

    TRACE( "hwnd %08x has alpha %d\n", res->hdr.hwnd, res->has_alpha );

    if (!(object = load_java_method( &method, "setWindowSurface", "(IZ)V" ))) return STATUS_NOT_SUPPORTED;

    wrap_java_call();
    (*jni_env)->CallVoidMethod( jni_env, object, method, res->hdr.hwnd, res->has_alpha );
    unwrap_java_call();
    return STATUS_SUCCESS;
}

static NTSTATUS setCapture_ioctl( void *data, DWORD in_size, DWORD out_size, ULONG_PTR *ret_size )
{
    struct ioctl_android_set_capture *res = data;

    if (in_size < sizeof(*res)) return STATUS_INVALID_PARAMETER;

    if (res->hdr.hwnd && !get_native_win_data( LongToHandle(res->hdr.hwnd) )) return STATUS_INVALID_HANDLE;

    TRACE( "hwnd %08x\n", res->hdr.hwnd );

    InterlockedExchangePointer( (void **)&capture_window, LongToHandle( res->hdr.hwnd ));
    return STATUS_SUCCESS;
}


static NTSTATUS gamepad_query( void *data, DWORD in_size, DWORD out_size, ULONG_PTR *ret_size )
{
    struct ioctl_android_gamepad_value *res = data;

    if (res->device > di_controllers) return STATUS_INVALID_PARAMETER;
    if (in_size < sizeof(*res)) return STATUS_INVALID_PARAMETER;
    if (out_size < sizeof(*res)) return STATUS_BUFFER_OVERFLOW;

    switch (res->index) {
    case 0:  /* Count */
        res->data.count =  di_controllers;
        break;
    case 1: /* name */
        lstrcpynW(res->data.name, di_names[res->device], DI_NAME_LENGTH);
        break;
    case 2: /* values*/
        memcpy(res->data.value, di_value[res->device], sizeof(res->data.value));
        break;
    }
    *ret_size = sizeof(*res);
    return STATUS_SUCCESS;
}

static NTSTATUS imeText_ioctl( void *data, DWORD in_size, DWORD out_size, ULONG_PTR *ret_size )
{
    WORD length;
    struct ioctl_android_ime_text *res = data;

    if (in_size < sizeof(*res)) return STATUS_INVALID_PARAMETER;
    if (out_size < sizeof(*res) + ((res->length-1) * sizeof(WCHAR))) return STATUS_BUFFER_OVERFLOW;
    if (res->target < 0 || res->target > java_ime_count) return STATUS_INVALID_PARAMETER;
    if (!java_ime_text[res->target]) return STATUS_INVALID_PARAMETER;

    length = min(java_ime_text[res->target]->length, res->length);
    res->length = java_ime_text[res->target]->length;
    lstrcpynW(res->text, java_ime_text[res->target]->text, length);
    res->cursor = java_ime_text[res->target]->cursor_pos;

    *ret_size = sizeof(*res) + (length * sizeof(WCHAR));
    return STATUS_SUCCESS;
}

static NTSTATUS imeFinish_ioctl( void *data, DWORD in_size, DWORD out_size, ULONG_PTR *ret_size )
{
    struct ioctl_android_ime_finish *res = data;

    if (in_size < sizeof(*res)) return STATUS_INVALID_PARAMETER;
    if (res->target < 0 || res->target > java_ime_count) return STATUS_INVALID_PARAMETER;

    if (java_ime_text[res->target])
    {
        if (java_ime_text[res->target]->text)
            free(java_ime_text[res->target]->text);
        free(java_ime_text[res->target]);
        java_ime_text[res->target] = NULL;
    }
    *ret_size = 0;
    return STATUS_SUCCESS;
}

static NTSTATUS getClipboardFormats_ioctl( void *data, DWORD in_size, DWORD out_size, ULONG_PTR *ret_size )
{
    struct ioctl_android_clipboard_formats *res = data;
    NTSTATUS stat;

    if (in_size < sizeof(*res)) return STATUS_INVALID_PARAMETER;
    if (out_size < offsetof( struct ioctl_android_clipboard_formats, formats[res->count] ))
        return STATUS_BUFFER_OVERFLOW;

    if (res->count == 0)
    {
        stat = get_clipboard_formats( &res->seqno, NULL, &res->count );
        if (stat == STATUS_BUFFER_OVERFLOW)
            stat = STATUS_SUCCESS;
        *ret_size = sizeof(*res);
        return stat;
    }

    stat = get_clipboard_formats( &res->seqno, res->formats, &res->count );
    if (stat == STATUS_SUCCESS)
        *ret_size = offsetof( struct ioctl_android_clipboard_formats, formats[res->count] );
    return stat;
}

static NTSTATUS getClipboardData_ioctl( void *data, DWORD in_size, DWORD out_size, ULONG_PTR *ret_size )
{
    struct ioctl_android_clipboard_data *res = data;
    NTSTATUS stat;

    if (in_size < sizeof(*res)) return STATUS_INVALID_PARAMETER;
    if (out_size < offsetof( struct ioctl_android_clipboard_data, data[res->size] ))
        return STATUS_BUFFER_OVERFLOW;

    stat = handle_ioctl_get_clipboard_data( res->format, &res->format_present, &res->pending, res->data, &res->size );

    *ret_size = offsetof( struct ioctl_android_clipboard_data, data[res->size] );
    return stat;
}

static NTSTATUS renderClipboardData_ioctl( void *data, DWORD in_size, DWORD out_size, ULONG_PTR *ret_size )
{
    static jmethodID method;
    jobject object;
    struct ioctl_android_render_clipboard_data *res = data;

    if (in_size < sizeof(*res)) return STATUS_INVALID_PARAMETER;

    TRACE( "%i\n", res->android_format );

    if (!(object = load_java_method( &method, "renderClipboardData", "(I)V" ))) return STATUS_NOT_SUPPORTED;

    wrap_java_call();
    (*jni_env)->CallVoidMethod( jni_env, object, method, res->android_format );
    unwrap_java_call();

    return STATUS_SUCCESS;
}

static NTSTATUS emptyClipboard_ioctl( void *data, DWORD in_size, DWORD out_size, ULONG_PTR *ret_size )
{
    struct ioctl_android_empty_clipboard *res = data;

    if (in_size < sizeof(*res)) return STATUS_INVALID_PARAMETER;

    handle_ioctl_empty_clipboard();

    return STATUS_SUCCESS;
}

static NTSTATUS setClipboardData_ioctl( void *data, DWORD in_size, DWORD out_size, ULONG_PTR *ret_size )
{
    struct ioctl_android_clipboard_data *res = data;

    if (in_size < offsetof(struct ioctl_android_clipboard_data, data[0]) ||
        in_size < offsetof(struct ioctl_android_clipboard_data, data[res->size]))
        return STATUS_INVALID_PARAMETER;

    handle_ioctl_set_clipboard_data( res->format, res->format_present, res->data, res->size );

    return STATUS_SUCCESS;
}

static void do_acquire_clipboard( void )
{
    BOOL formats[1];
    static const int num_formats = sizeof(formats)/sizeof(formats[0]);
    static jmethodID method;
    jobject object;
    jbooleanArray format_array;
    jboolean *elements;
    int i;

    get_exported_formats( formats, num_formats );

    if (!(object = load_java_method( &method, "acquireClipboard", "([Z)V" ))) return;

    wrap_java_call();
    format_array = (*jni_env)->NewBooleanArray( jni_env, num_formats );

    elements = (*jni_env)->GetBooleanArrayElements( jni_env, format_array, NULL );
    for (i=0; i<num_formats; i++)
        elements[i] = formats[i];
    (*jni_env)->ReleaseBooleanArrayElements( jni_env, format_array, elements, JNI_COMMIT );

    (*jni_env)->CallVoidMethod( jni_env, object, method, format_array );
    (*jni_env)->DeleteLocalRef( jni_env, format_array );
    unwrap_java_call();
}

static NTSTATUS endClipboardUpdate_ioctl( void *data, DWORD in_size, DWORD out_size, ULONG_PTR *ret_size )
{
    struct ioctl_android_end_clipboard_update *res = data;

    if (in_size < sizeof(*res)) return STATUS_INVALID_PARAMETER;

    handle_ioctl_end_clipboard_update();

    do_acquire_clipboard();

    return STATUS_SUCCESS;
}

static NTSTATUS acquireClipboard_ioctl( void *data, DWORD in_size, DWORD out_size, ULONG_PTR *ret_size )
{
    struct ioctl_android_end_clipboard_update *res = data;

    if (in_size < sizeof(*res)) return STATUS_INVALID_PARAMETER;

    do_acquire_clipboard();

    return STATUS_SUCCESS;
}

static NTSTATUS exportClipboardData_ioctl( void *data, DWORD in_size, DWORD out_size, ULONG_PTR *ret_size )
{
    struct ioctl_android_clipboard_data *res = data;
    static jmethodID method;
    jobject object;
    jbyteArray data_array;
    jbyte *elements;

    if (in_size < offsetof(struct ioctl_android_clipboard_data, data[0]) ||
        in_size < offsetof(struct ioctl_android_clipboard_data, data[res->size]))
        return STATUS_INVALID_PARAMETER;

    if (!(object = load_java_method( &method, "exportClipboardData", "(I[B)V" )))
        return STATUS_NOT_SUPPORTED;

    wrap_java_call();
    data_array = (*jni_env)->NewByteArray( jni_env, res->size );

    elements = (*jni_env)->GetByteArrayElements( jni_env, data_array, NULL );
    memcpy( elements, res->data, res->size );
    (*jni_env)->ReleaseByteArrayElements( jni_env, data_array, elements, JNI_COMMIT );

    (*jni_env)->CallVoidMethod( jni_env, object, method, (jint)res->format, data_array );
    (*jni_env)->DeleteLocalRef( jni_env, data_array );
    unwrap_java_call();

    return STATUS_SUCCESS;
}

typedef NTSTATUS (*ioctl_func)( void *in, DWORD in_size, DWORD out_size, ULONG_PTR *ret_size );
static const ioctl_func ioctl_funcs[] =
{
    dequeueBuffer_ioctl,        /* IOCTL_ANDROID_DEQUEUE_BUFFER */
    queueBuffer_ioctl,          /* IOCTL_ANDROID_QUEUE_BUFFER */
    cancelBuffer_ioctl,         /* IOCTL_ANDROID_CANCEL_BUFFER */
    query_ioctl,                /* IOCTL_ANDROID_QUERY */
    perform_ioctl,              /* IOCTL_ANDROID_PERFORM */
    setSwapInterval_ioctl,      /* IOCTL_ANDROID_SET_SWAP_INT */
    createWindow_ioctl,         /* IOCTL_ANDROID_CREATE_WINDOW */
    destroyWindow_ioctl,        /* IOCTL_ANDROID_DESTROY_WINDOW */
    windowPosChanged_ioctl,     /* IOCTL_ANDROID_WINDOW_POS_CHANGED */
    setWindowFocus_ioctl,       /* IOCTL_ANDROID_SET_WINDOW_FOCUS */
    setWindowText_ioctl,        /* IOCTL_ANDROID_SET_WINDOW_TEXT */
    setWindowIcon_ioctl,        /* IOCTL_ANDROID_SET_WINDOW_ICON */
    setWindowRgn_ioctl,         /* IOCTL_ANDROID_SET_WINDOW_RGN */
    setWindowLayered_ioctl,     /* IOCTL_ANDROID_SET_WINDOW_LAYERED */
    setSurfaceAlpha_ioctl,      /* IOCTL_ANDROID_SET_SURFACE_ALPHA */
    setCapture_ioctl,           /* IOCTL_ANDROID_SET_CAPTURE */
    gamepad_query,              /* IOCTL_ANDROID_GAMEPAD_QUERY */
    imeText_ioctl,              /* IOCTL_ANDROID_IMETEXT */
    getClipboardFormats_ioctl,  /* IOCTL_ANDROID_GET_CLIPBOARD_FORMATS */
    getClipboardData_ioctl,     /* IOCTL_ANDROID_GET_CLIPBOARD_DATA */
    renderClipboardData_ioctl,  /* IOCTL_ANDROID_RENDER_CLIPBOARD_DATA */
    emptyClipboard_ioctl,       /* IOCTL_ANDROID_EMPTY_CLIPBOARD */
    setClipboardData_ioctl,     /* IOCTL_ANDROID_SET_CLIPBOARD_DATA */
    endClipboardUpdate_ioctl,   /* IOCTL_ANDROID_END_CLIPBOARD_UPDATE */
    acquireClipboard_ioctl,     /* IOCTL_ANDROID_ACQUIRE_CLIPBOARD */
    exportClipboardData_ioctl,  /* IOCTL_ANDROID_EXPORT_CLIPBOARD_DATA */
    imeFinish_ioctl,            /* IOCTL_ANDROID_IMEFINISH */
};

static NTSTATUS WINAPI ioctl_callback( DEVICE_OBJECT *device, IRP *irp )
{
    IO_STACK_LOCATION *irpsp = IoGetCurrentIrpStackLocation( irp );

    if (irpsp->Parameters.DeviceIoControl.IoControlCode >= IOCTL_ANDROID_FIRST &&
        irpsp->Parameters.DeviceIoControl.IoControlCode <= IOCTL_ANDROID_LAST)
    {
        struct ioctl_header *header = irp->AssociatedIrp.SystemBuffer;
        DWORD in_size = irpsp->Parameters.DeviceIoControl.InputBufferLength;
        ioctl_func func = ioctl_funcs[(irpsp->Parameters.DeviceIoControl.IoControlCode - IOCTL_ANDROID_FIRST) >> 2];

        if (in_size >= sizeof(*header))
        {
            irp->IoStatus.Information = 0;
            irp->IoStatus.u.Status = func( irp->AssociatedIrp.SystemBuffer, in_size,
                                           irpsp->Parameters.DeviceIoControl.OutputBufferLength,
                                           &irp->IoStatus.Information );
            if (irp->IoStatus.u.Status == STATUS_PENDING)
            {
                if (!is_client_in_process())
                    irp->IoStatus.u.Status = queue_irp( LongToHandle(header->hwnd), irp );
                else  /* we can't wait in the desktop process */
                    irp->IoStatus.u.Status = STATUS_DEVICE_NOT_READY;
            }
        }
        else irp->IoStatus.u.Status = STATUS_INVALID_PARAMETER;
    }
    else
    {
        FIXME( "ioctl %x not supported\n", irpsp->Parameters.DeviceIoControl.IoControlCode );
        irp->IoStatus.u.Status = STATUS_NOT_SUPPORTED;
    }
    if (irp->IoStatus.u.Status != STATUS_PENDING) IoCompleteRequest( irp, IO_NO_INCREMENT );
    return STATUS_SUCCESS;
}

static NTSTATUS CALLBACK init_android_driver( DRIVER_OBJECT *driver, UNICODE_STRING *name )
{
    driver->MajorFunction[IRP_MJ_DEVICE_CONTROL] = ioctl_callback;
    driver_obj = driver;
    return STATUS_SUCCESS;
}

static DWORD CALLBACK device_thread( void *arg )
{
    HANDLE start_event = arg;
    UNICODE_STRING nameW, linkW;
    NTSTATUS status;
    JavaVM *java_vm;
    DWORD ret;

    TRACE( "starting process %x\n", GetCurrentProcessId() );

    if (!(java_vm = wine_get_java_vm())) return 0;  /* not running under Java */

#ifdef __i386__
    orig_fs = wine_get_fs();
    (*java_vm)->AttachCurrentThread( java_vm, &jni_env, 0 );
    java_fs = wine_get_fs();
    wine_set_fs( orig_fs );
    if (java_fs != orig_fs) TRACE( "%%fs changed from %04x to %04x by Java VM\n", orig_fs, java_fs );
#else
    (*java_vm)->AttachCurrentThread( java_vm, &jni_env, 0 );
#endif

    create_desktop_window( GetDesktopWindow() );

    RtlInitUnicodeString( &nameW, driver_nameW );
    if ((status = IoCreateDriver( &nameW, init_android_driver )))
    {
        FIXME( "failed to create driver error %x\n", status );
        return status;
    }

    RtlInitUnicodeString( &nameW, device_nameW );
    RtlInitUnicodeString( &linkW, device_linkW );

    if (!(status = IoCreateDevice( driver_obj, 0, &nameW, 0, 0, FALSE, &ioctl_device )))
        status = IoCreateSymbolicLink( &linkW, &nameW );
    if (status)
    {
        FIXME( "failed to create device error %x\n", status );
        return status;
    }

    stop_event = CreateEventW( NULL, TRUE, FALSE, NULL );
    SetEvent( start_event );

    ret = wine_ntoskrnl_main_loop( stop_event );

    (*java_vm)->DetachCurrentThread( java_vm );
    return ret;
}

void start_android_device(void)
{
    HANDLE handles[2];

    handles[0] = CreateEventW( NULL, TRUE, FALSE, NULL );
    handles[1] = thread = CreateThread( NULL, 0, device_thread, handles[0], 0, NULL );
    WaitForMultipleObjects( 2, handles, FALSE, INFINITE );
    CloseHandle( handles[0] );
}

static int android_ioctl( DWORD code, void *in, DWORD in_size, void *out, DWORD *out_size )
{
    static const WCHAR deviceW[] = {'\\','\\','.','\\','W','i','n','e','A','n','d','r','o','i','d',0 };
    static HANDLE device;
    IO_STATUS_BLOCK iosb;
    NTSTATUS status;

    if (!device)
    {
        HANDLE file = CreateFileW( deviceW, GENERIC_READ,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, 0 );
        if (file == INVALID_HANDLE_VALUE) return -ENOENT;
        if (InterlockedCompareExchangePointer( &device, file, NULL )) CloseHandle( file );
    }

    status = NtDeviceIoControlFile( device, NULL, NULL, NULL, &iosb, code, in, in_size,
                                    out, out_size ? *out_size : 0 );
    if (status == STATUS_FILE_DELETED)
    {
        WARN( "parent process is gone\n" );
        ExitProcess( 1 );
    }
    if (out_size) *out_size = iosb.Information;
    return status_to_android_error( status );
}

static void win_incRef( struct android_native_base_t *base )
{
    struct native_win_wrapper *win = (struct native_win_wrapper *)base;
    InterlockedIncrement( &win->ref );
}

static void win_decRef( struct android_native_base_t *base )
{
    struct native_win_wrapper *win = (struct native_win_wrapper *)base;
    InterlockedDecrement( &win->ref );
}

static void buffer_incRef( struct android_native_base_t *base )
{
    struct native_buffer_wrapper *buffer = (struct native_buffer_wrapper *)base;
    InterlockedIncrement( &buffer->ref );
}

static void buffer_decRef( struct android_native_base_t *base )
{
    struct native_buffer_wrapper *buffer = (struct native_buffer_wrapper *)base;

    if (!InterlockedDecrement( &buffer->ref ))
    {
        if (!is_in_desktop_process())
        {
            if (gralloc_module) gralloc_module->unregisterBuffer( gralloc_module, buffer->buffer.handle );
            close_native_handle( (native_handle_t *)buffer->buffer.handle );
        }
        if (buffer->bits) UnmapViewOfFile( buffer->bits );
        HeapFree( GetProcessHeap(), 0, buffer );
    }
}

static int dequeueBuffer( struct ANativeWindow *window, struct ANativeWindowBuffer **buffer, int *fence )
{
    struct native_win_wrapper *win = (struct native_win_wrapper *)window;
    struct ioctl_android_dequeueBuffer res;
    DWORD size = sizeof(res);
    int ret, use_win32 = !gralloc_module;

    res.hdr.hwnd = HandleToLong( win->hwnd );
    res.win32 = use_win32;
    ret = android_ioctl( IOCTL_ANDROID_DEQUEUE_BUFFER,
                         &res, offsetof( struct ioctl_android_dequeueBuffer, native_handle ),
                         &res, &size );
    if (ret) return ret;

    /* if we received the native handle, this is a new buffer */
    if (size > offsetof( struct ioctl_android_dequeueBuffer, native_handle ))
    {
        struct native_buffer_wrapper *buf = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*buf) );

        buf->buffer.common.magic   = ANDROID_NATIVE_BUFFER_MAGIC;
        buf->buffer.common.version = sizeof( buf->buffer );
        buf->buffer.common.incRef  = buffer_incRef;
        buf->buffer.common.decRef  = buffer_decRef;
        buf->buffer.width          = res.width;
        buf->buffer.height         = res.height;
        buf->buffer.stride         = res.stride;
        buf->buffer.format         = res.format;
        buf->buffer.usage          = res.usage;
        buf->buffer.handle         = unmap_native_handle( &res.native_handle.handle );
        buf->ref                   = 1;
        buf->hwnd                  = win->hwnd;
        buf->buffer_id             = res.buffer_id;
        if (win->buffers[res.buffer_id])
            win->buffers[res.buffer_id]->buffer.common.decRef(&win->buffers[res.buffer_id]->buffer.common);
        win->buffers[res.buffer_id] = buf;

        if (use_win32)
        {
            HANDLE mapping = LongToHandle( res.native_handle.handle.data[0] );
            buf->bits = MapViewOfFile( mapping, FILE_MAP_WRITE, 0, 0, 0 );
            CloseHandle( mapping );
        }
        else if (!is_in_desktop_process())
        {
            if ((ret = gralloc_module->registerBuffer( gralloc_module, buf->buffer.handle )) < 0)
                WARN( "hwnd %p, buffer %p failed to register %d %s\n", win->hwnd, &buf->buffer, ret, strerror(-ret) );
        }
    }

    *buffer = &win->buffers[res.buffer_id]->buffer;
    *fence = -1;

    TRACE( "hwnd %p, buffer %p %dx%d stride %d fmt %d usage %d fence %d\n",
           win->hwnd, *buffer, res.width, res.height, res.stride, res.format, res.usage, *fence );
    return 0;
}

static int cancelBuffer( struct ANativeWindow *window, struct ANativeWindowBuffer *buffer, int fence )
{
    struct native_win_wrapper *win = (struct native_win_wrapper *)window;
    struct native_buffer_wrapper *buf = (struct native_buffer_wrapper *)buffer;
    struct ioctl_android_cancelBuffer cancel;

    TRACE( "hwnd %p buffer %p %dx%d stride %d fmt %d usage %d fence %d\n",
           win->hwnd, buffer, buffer->width, buffer->height,
           buffer->stride, buffer->format, buffer->usage, fence );
    cancel.buffer_id = buf->buffer_id;
    cancel.hdr.hwnd = HandleToLong( win->hwnd );
    wait_fence_and_close( fence );
    return android_ioctl( IOCTL_ANDROID_CANCEL_BUFFER, &cancel, sizeof(cancel), NULL, NULL );
}

static int queueBuffer( struct ANativeWindow *window, struct ANativeWindowBuffer *buffer, int fence )
{
    struct native_win_wrapper *win = (struct native_win_wrapper *)window;
    struct native_buffer_wrapper *buf = (struct native_buffer_wrapper *)buffer;
    struct ioctl_android_queueBuffer queue;

    TRACE( "hwnd %p buffer %p %dx%d stride %d fmt %d usage %d fence %d\n",
           win->hwnd, buffer, buffer->width, buffer->height,
           buffer->stride, buffer->format, buffer->usage, fence );
    queue.buffer_id = buf->buffer_id;
    queue.hdr.hwnd = HandleToLong( win->hwnd );
    wait_fence_and_close( fence );
    return android_ioctl( IOCTL_ANDROID_QUEUE_BUFFER, &queue, sizeof(queue), NULL, NULL );
}

static int dequeueBuffer_DEPRECATED( struct ANativeWindow *window, struct ANativeWindowBuffer **buffer )
{
    int fence, ret = dequeueBuffer( window, buffer, &fence );

    if (!ret) wait_fence_and_close( fence );
    return ret;
}

static int cancelBuffer_DEPRECATED( struct ANativeWindow *window, struct ANativeWindowBuffer *buffer )
{
    return cancelBuffer( window, buffer, -1 );
}

static int lockBuffer_DEPRECATED( struct ANativeWindow *window, struct ANativeWindowBuffer *buffer )
{
    return 0;  /* nothing to do */
}

static int queueBuffer_DEPRECATED( struct ANativeWindow *window, struct ANativeWindowBuffer *buffer )
{
    return queueBuffer( window, buffer, -1 );
}

static int setSwapInterval( struct ANativeWindow *window, int interval )
{
    struct native_win_wrapper *win = (struct native_win_wrapper *)window;
    struct ioctl_android_set_swap_interval swap;

    TRACE( "hwnd %p interval %d\n", win->hwnd, interval );
    swap.hdr.hwnd = HandleToLong( win->hwnd );
    swap.interval = interval;
    return android_ioctl( IOCTL_ANDROID_SET_SWAP_INT, &swap, sizeof(swap), NULL, NULL );
}

static int query( const ANativeWindow *window, int what, int *value )
{
    struct native_win_wrapper *win = (struct native_win_wrapper *)window;
    struct ioctl_android_query query;
    DWORD size = sizeof( query );
    int ret;

    query.hdr.hwnd = HandleToLong( win->hwnd );
    query.what = what;
    ret = android_ioctl( IOCTL_ANDROID_QUERY, &query, sizeof(query), &query, &size );
    TRACE( "hwnd %p what %d got %d -> %p\n", win->hwnd, what, query.value, value );
    if (!ret) *value = query.value;
    return ret;
}

static int perform( ANativeWindow *window, int operation, ... )
{
    static const char * const names[] =
    {
        "SET_USAGE", "CONNECT", "DISCONNECT", "SET_CROP", "SET_BUFFER_COUNT", "SET_BUFFERS_GEOMETRY",
        "SET_BUFFERS_TRANSFORM", "SET_BUFFERS_TIMESTAMP", "SET_BUFFERS_DIMENSIONS", "SET_BUFFERS_FORMAT",
        "SET_SCALING_MODE", "LOCK", "UNLOCK_AND_POST", "API_CONNECT", "API_DISCONNECT",
        "SET_BUFFERS_USER_DIMENSIONS", "SET_POST_TRANSFORM_CROP"
    };

    struct native_win_wrapper *win = (struct native_win_wrapper *)window;
    struct ioctl_android_perform perf;
    va_list args;

    perf.hdr.hwnd  = HandleToLong( win->hwnd );
    perf.operation = operation;
    memset( perf.args, 0, sizeof(perf.args) );

    va_start( args, operation );
    switch (operation)
    {
    case NATIVE_WINDOW_SET_USAGE:
    case NATIVE_WINDOW_SET_BUFFERS_TRANSFORM:
    case NATIVE_WINDOW_SET_BUFFERS_FORMAT:
    case NATIVE_WINDOW_SET_SCALING_MODE:
    case NATIVE_WINDOW_API_CONNECT:
    case NATIVE_WINDOW_API_DISCONNECT:
        perf.args[0] = va_arg( args, int );
        TRACE( "hwnd %p %s arg %d\n", win->hwnd, names[operation], perf.args[0] );
        break;
    case NATIVE_WINDOW_SET_BUFFER_COUNT:
        perf.args[0] = va_arg( args, size_t );
        TRACE( "hwnd %p %s count %d\n", win->hwnd, names[operation], perf.args[0] );
        break;
    case NATIVE_WINDOW_SET_BUFFERS_DIMENSIONS:
    case NATIVE_WINDOW_SET_BUFFERS_USER_DIMENSIONS:
        perf.args[0] = va_arg( args, int );
        perf.args[1] = va_arg( args, int );
        TRACE( "hwnd %p %s arg %dx%d\n", win->hwnd, names[operation], perf.args[0], perf.args[1] );
        break;
    case NATIVE_WINDOW_SET_BUFFERS_GEOMETRY:
        perf.args[0] = va_arg( args, int );
        perf.args[1] = va_arg( args, int );
        perf.args[2] = va_arg( args, int );
        TRACE( "hwnd %p %s arg %dx%d %d\n", win->hwnd, names[operation],
               perf.args[0], perf.args[1], perf.args[2] );
        break;
    case NATIVE_WINDOW_SET_CROP:
    {
        android_native_rect_t *rect = va_arg( args, android_native_rect_t * );
        perf.args[0] = rect->left;
        perf.args[1] = rect->top;
        perf.args[2] = rect->right;
        perf.args[3] = rect->bottom;
        TRACE( "hwnd %p %s rect %d,%d-%d,%d\n", win->hwnd, names[operation],
               perf.args[0], perf.args[1], perf.args[2], perf.args[3] );
        break;
    }
    case NATIVE_WINDOW_SET_BUFFERS_TIMESTAMP:
    {
        int64_t timestamp = va_arg( args, int64_t );
        perf.args[0] = timestamp;
        perf.args[1] = timestamp >> 32;
        TRACE( "hwnd %p %s arg %08x%08x\n", win->hwnd, names[operation], perf.args[1], perf.args[0] );
        break;
    }
    case NATIVE_WINDOW_LOCK:
    {
        struct ANativeWindowBuffer *buffer;
        struct ANativeWindow_Buffer *buffer_ret = va_arg( args, ANativeWindow_Buffer * );
        ARect *bounds = va_arg( args, ARect * );
        int ret = window->dequeueBuffer_DEPRECATED( window, &buffer );
        if (!ret)
        {
            if (gralloc_module)
            {
                if ((ret = gralloc_module->lock( gralloc_module, buffer->handle,
                                                 GRALLOC_USAGE_SW_READ_OFTEN | GRALLOC_USAGE_SW_WRITE_OFTEN,
                                                 0, 0, buffer->width, buffer->height, &buffer_ret->bits )))
                {
                    WARN( "gralloc->lock %p failed %d %s\n", win->hwnd, ret, strerror(-ret) );
                    window->cancelBuffer( window, buffer, -1 );
                }
            }
            else
                buffer_ret->bits = ((struct native_buffer_wrapper *)buffer)->bits;
        }
        if (!ret)
        {
            buffer_ret->width  = buffer->width;
            buffer_ret->height = buffer->height;
            buffer_ret->stride = buffer->stride;
            buffer_ret->format = buffer->format;
            win->locked_buffer = buffer;
            if (bounds)
            {
                bounds->left   = 0;
                bounds->top    = 0;
                bounds->right  = buffer->width;
                bounds->bottom = buffer->height;
            }
        }
        va_end( args );
        TRACE( "hwnd %p %s bits %p ret %d %s\n", win->hwnd, names[operation], buffer_ret->bits, ret, strerror(-ret) );
        return ret;
    }
    case NATIVE_WINDOW_UNLOCK_AND_POST:
    {
        int ret = -EINVAL;
        if (win->locked_buffer)
        {
            if (gralloc_module) gralloc_module->unlock( gralloc_module, win->locked_buffer->handle );
            ret = window->queueBuffer( window, win->locked_buffer, -1 );
            win->locked_buffer = NULL;
        }
        va_end( args );
        TRACE( "hwnd %p %s ret %d\n", win->hwnd, names[operation], ret );
        return ret;
    }
    case NATIVE_WINDOW_CONNECT:
    case NATIVE_WINDOW_DISCONNECT:
        TRACE( "hwnd %p %s\n", win->hwnd, names[operation] );
        break;
    case NATIVE_WINDOW_SET_POST_TRANSFORM_CROP:
    default:
        FIXME( "unsupported perform hwnd %p op %d %s\n", win->hwnd, operation,
               operation < sizeof(names)/sizeof(names[0]) ? names[operation] : "???" );
        break;
    }
    va_end( args );
    return android_ioctl( IOCTL_ANDROID_PERFORM, &perf, sizeof(perf), NULL, NULL );
}

int ioctl_window_pos_changed( HWND hwnd, const RECT *rect, UINT style, UINT flags, HWND after, HWND owner )
{
    struct ioctl_android_window_pos_changed req;

    req.hdr.hwnd = HandleToLong( hwnd );
    req.left     = rect->left;
    req.top      = rect->top;
    req.right    = rect->right;
    req.bottom   = rect->bottom;
    req.style    = style;
    req.flags    = flags;
    req.after    = HandleToLong( after );
    req.owner    = HandleToLong( owner );
    return android_ioctl( IOCTL_ANDROID_WINDOW_POS_CHANGED, &req, sizeof(req), NULL, NULL );
}

int ioctl_set_window_focus( HWND hwnd )
{
    struct ioctl_android_set_window_focus req;

    req.hdr.hwnd = HandleToLong( hwnd );
    return android_ioctl( IOCTL_ANDROID_SET_WINDOW_FOCUS, &req, sizeof(req), NULL, NULL );
}

int ioctl_set_window_text( HWND hwnd, const WCHAR *text )
{
    struct ioctl_android_set_window_text *req;
    unsigned int size = offsetof( struct ioctl_android_set_window_text, text[strlenW(text)] );
    int ret;

    if (!(req = HeapAlloc( GetProcessHeap(), 0, size ))) return -ENOMEM;
    req->hdr.hwnd = HandleToLong( hwnd );
    memcpy( req->text, text, strlenW( text ) * sizeof(WCHAR) );
    ret = android_ioctl( IOCTL_ANDROID_SET_WINDOW_TEXT, req, size, NULL, NULL );
    HeapFree( GetProcessHeap(), 0, req );
    return ret;
}

int ioctl_set_window_icon( HWND hwnd, int width, int height, const unsigned int *bits )
{
    struct ioctl_android_set_window_icon *req;
    unsigned int size = offsetof( struct ioctl_android_set_window_icon, bits[width * height] );
    int ret;

    if (!(req = HeapAlloc( GetProcessHeap(), 0, size ))) return -ENOMEM;
    req->hdr.hwnd = HandleToLong( hwnd );
    req->width    = width;
    req->height   = height;
    memcpy( req->bits, bits, width * height * sizeof(req->bits[0]) );
    ret = android_ioctl( IOCTL_ANDROID_SET_WINDOW_ICON, req, size, NULL, NULL );
    HeapFree( GetProcessHeap(), 0, req );
    return ret;
}

int ioctl_set_window_rgn( HWND hwnd, HRGN rgn )
{
    struct ioctl_android_set_window_rgn req;

    req.hdr.hwnd   = HandleToLong( hwnd );
    req.has_region = (rgn != 0);
    return android_ioctl( IOCTL_ANDROID_SET_WINDOW_RGN, &req, sizeof(req), NULL, NULL );
}

int ioctl_set_window_layered( HWND hwnd, COLORREF key, BYTE alpha )
{
    struct ioctl_android_set_window_layered req;

    req.hdr.hwnd = HandleToLong( hwnd );
    req.key      = key;
    req.alpha    = alpha;
    return android_ioctl( IOCTL_ANDROID_SET_WINDOW_LAYERED, &req, sizeof(req), NULL, NULL );
}

int ioctl_set_surface_alpha( HWND hwnd, BOOL has_alpha )
{
    struct ioctl_android_set_surface_alpha req;

    req.hdr.hwnd  = HandleToLong( hwnd );
    req.has_alpha = has_alpha;
    return android_ioctl( IOCTL_ANDROID_SET_SURFACE_ALPHA, &req, sizeof(req), NULL, NULL );
}

int ioctl_set_capture( HWND hwnd )
{
    struct ioctl_android_set_capture req;

    req.hdr.hwnd  = HandleToLong( hwnd );
    return android_ioctl( IOCTL_ANDROID_SET_CAPTURE, &req, sizeof(req), NULL, NULL );
}

int ioctl_gamepad_query( int index, int device, void* data)
{
    struct ioctl_android_gamepad_value query;
    DWORD size = sizeof( query );
    int ret;

    query.index = index;
    query.device = device;
    ret = android_ioctl( IOCTL_ANDROID_GAMEPAD_QUERY, &query , sizeof(query), &query, &size );
    switch (index)
    {
        case 0:  /* Count */
            *(int*)data = query.data.count;
            break;
        case 1: /* Name */
            lstrcpynW((WCHAR*)data, query.data.name, DI_NAME_LENGTH);
            break;
        case 2: /* Values*/
            memcpy(data, query.data.value, sizeof(query.data.value));
            break;
    }
    return ret;
}

int ioctl_get_clipboard_formats( DWORD* seqno, UINT** formats, DWORD* num_formats )
{
    struct ioctl_android_clipboard_formats query;
    DWORD size = sizeof( query );
    int ret;

    query.count = 0;
    ret = android_ioctl( IOCTL_ANDROID_GET_CLIPBOARD_FORMATS, &query, sizeof(query), &query, &size );

    if (!ret)
    {
        if (query.count == 0)
        {
            *seqno = query.seqno;
            *formats = NULL;
            *num_formats = 0;
        }
        else
        {
            struct ioctl_android_clipboard_formats *dyn_query;

            size = offsetof( struct ioctl_android_clipboard_formats, formats[query.count] );
            dyn_query = HeapAlloc( GetProcessHeap(), 0, size );
            if (!dyn_query)
                return -ENOMEM;

            dyn_query->count = query.count;
            ret = android_ioctl( IOCTL_ANDROID_GET_CLIPBOARD_FORMATS, dyn_query, size, dyn_query, &size );

            if (!ret)
            {
                *formats = HeapAlloc( GetProcessHeap(), 0, dyn_query->count * sizeof(UINT) );
                if (*formats)
                {
                    *seqno = dyn_query->seqno;
                    memcpy( *formats, dyn_query->formats, dyn_query->count * sizeof(UINT) );
                    *num_formats = dyn_query->count;
                }
                else
                    ret = -ENOMEM;
            }

            HeapFree( GetProcessHeap(), 0, dyn_query );
        }
    }
    return ret;
}

int ioctl_get_clipboard_data( UINT format, HGLOBAL* result, BOOL* pending )
{
    struct ioctl_android_clipboard_data query;
    DWORD size = sizeof( query );
    int ret;

    *result = NULL;
    *pending = FALSE;

    query.pending = 0;
    query.format_present = 0;
    query.format = format;
    query.size = 0;
    ret = android_ioctl( IOCTL_ANDROID_GET_CLIPBOARD_DATA, &query, sizeof(query), &query, &size );

    if (!ret)
    {
        if (query.pending)
        {
            *pending = TRUE;
        }
        else if (query.format_present && query.size == 0)
        {
            *result = GlobalAlloc( GMEM_MOVEABLE, 0 );
        }
        else if (query.format_present)
        {
            struct ioctl_android_clipboard_data *dyn_query;

            size = offsetof( struct ioctl_android_clipboard_data, data[query.size] );
            dyn_query = HeapAlloc( GetProcessHeap(), 0, size );
            if (!dyn_query)
                return -ENOMEM;

            dyn_query->pending = 0;
            dyn_query->format_present = 0;
            dyn_query->format = format;
            dyn_query->size = query.size;
            ret = android_ioctl( IOCTL_ANDROID_GET_CLIPBOARD_DATA, dyn_query, size, dyn_query, &size );

            if (!ret && !dyn_query->pending && dyn_query->format_present)
            {
                *result = GlobalAlloc( GMEM_MOVEABLE, dyn_query->size );
                if (*result)
                {
                    void *lock = GlobalLock( *result );
                    memcpy( lock, dyn_query->data, dyn_query->size );
                    GlobalUnlock( *result );
                }
                else
                    ret = -ENOMEM;
            }

            HeapFree( GetProcessHeap(), 0, dyn_query );
        }
    }
    return ret;
}

int ioctl_render_clipboard_data( int android_format )
{
    struct ioctl_android_render_clipboard_data req;

    req.android_format = android_format;
    return android_ioctl( IOCTL_ANDROID_RENDER_CLIPBOARD_DATA, &req, sizeof(req), NULL, NULL );
}

int ioctl_empty_clipboard( void )
{
    struct ioctl_android_empty_clipboard req;

    return android_ioctl( IOCTL_ANDROID_EMPTY_CLIPBOARD, &req, sizeof(req), NULL, NULL );
}

int ioctl_set_clipboard_data( UINT format, BOOL format_present, BYTE* buffer, DWORD buffer_size )
{
    struct ioctl_android_clipboard_data *req;
    DWORD size = offsetof( struct ioctl_android_clipboard_data, data[buffer_size] );

    req = HeapAlloc( GetProcessHeap(), 0, size );
    if (!req) return -ENOMEM;

    req->pending = 0;
    req->format_present = format_present;
    req->format = format;
    req->size = buffer_size;
    memcpy( req->data, buffer, buffer_size );

    return android_ioctl( IOCTL_ANDROID_SET_CLIPBOARD_DATA, req, size, NULL, NULL );
}

int ioctl_end_clipboard_update( void )
{
    struct ioctl_android_end_clipboard_update req;

    return android_ioctl( IOCTL_ANDROID_END_CLIPBOARD_UPDATE, &req, sizeof(req), NULL, NULL );
}

int ioctl_acquire_clipboard( void )
{
    struct ioctl_android_acquire_clipboard req;

    return android_ioctl( IOCTL_ANDROID_ACQUIRE_CLIPBOARD, &req, sizeof(req), NULL, NULL );
}

int ioctl_export_clipboard_data( int android_format, BYTE* buffer, DWORD buffer_size )
{
    struct ioctl_android_clipboard_data *req;
    DWORD size = offsetof( struct ioctl_android_clipboard_data, data[buffer_size] );

    req = HeapAlloc( GetProcessHeap(), 0, size );
    if (!req) return -ENOMEM;

    req->pending = 0;
    req->format_present = TRUE;
    req->format = android_format;
    req->size = buffer_size;
    memcpy( req->data, buffer, buffer_size );

    return android_ioctl( IOCTL_ANDROID_EXPORT_CLIPBOARD_DATA, req, size, NULL, NULL );
}


struct ANativeWindow *create_ioctl_window( HWND hwnd )
{
    struct ioctl_android_create_window req;
    struct native_win_wrapper *win = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*win) );

    if (!win) return NULL;

    win->win.common.magic             = ANDROID_NATIVE_WINDOW_MAGIC;
    win->win.common.version           = sizeof(ANativeWindow);
    win->win.common.incRef            = win_incRef;
    win->win.common.decRef            = win_decRef;
    win->win.setSwapInterval          = setSwapInterval;
    win->win.dequeueBuffer_DEPRECATED = dequeueBuffer_DEPRECATED;
    win->win.lockBuffer_DEPRECATED    = lockBuffer_DEPRECATED;
    win->win.queueBuffer_DEPRECATED   = queueBuffer_DEPRECATED;
    win->win.query                    = query;
    win->win.perform                  = perform;
    win->win.cancelBuffer_DEPRECATED  = cancelBuffer_DEPRECATED;
    win->win.dequeueBuffer            = dequeueBuffer;
    win->win.queueBuffer              = queueBuffer;
    win->win.cancelBuffer             = cancelBuffer;
    win->ref  = 1;
    win->hwnd = hwnd;
    TRACE( "-> %p %p\n", win, win->hwnd );

    req.hdr.hwnd = HandleToLong( win->hwnd );
    android_ioctl( IOCTL_ANDROID_CREATE_WINDOW, &req, sizeof(req), NULL, NULL );

    return &win->win;
}

void destroy_ioctl_window( HWND hwnd )
{
    struct ioctl_android_destroy_window req;

    req.hdr.hwnd = HandleToLong( hwnd );
    android_ioctl( IOCTL_ANDROID_DESTROY_WINDOW, &req, sizeof(req), NULL, NULL );
}

void grab_ioctl_window( struct ANativeWindow *window )
{
    struct native_win_wrapper *win = (struct native_win_wrapper *)window;
    InterlockedIncrement( &win->ref );
}

void release_ioctl_window( struct ANativeWindow *window )
{
    struct native_win_wrapper *win = (struct native_win_wrapper *)window;
    unsigned int i;

    if (InterlockedDecrement( &win->ref ) > 0) return;

    TRACE( "%p %p\n", win, win->hwnd );
    for (i = 0; i < sizeof(win->buffers)/sizeof(win->buffers[0]); i++)
        if (win->buffers[i]) win->buffers[i]->buffer.common.decRef( &win->buffers[i]->buffer.common );

    destroy_ioctl_window( win->hwnd );
    HeapFree( GetProcessHeap(), 0, win );
}

int ioctl_imeText( int target, int *cursor, int *length, WCHAR* string)
{
    struct ioctl_android_ime_text *query;
    DWORD size = sizeof( *query ) + ((*length - 1) * sizeof(WCHAR));
    int ret;

    query = HeapAlloc(GetProcessHeap(), 0, size);

    query->length = (*length);
    query->target = target;
    ret = android_ioctl( IOCTL_ANDROID_IMETEXT, query , size, query, &size );
    lstrcpynW(string, query->text, query->length);
    *length = query->length;
    *cursor = query->cursor;

    HeapFree(GetProcessHeap(), 0, query);

    return ret;
}

int ioctl_imeFinish( int target )
{
    struct ioctl_android_ime_finish *query;
    DWORD size = sizeof( *query );
    int ret;

    query = HeapAlloc(GetProcessHeap(), 0, size);
    query->target = target;
    ret = android_ioctl( IOCTL_ANDROID_IMEFINISH, query , size, query, &size );
    HeapFree(GetProcessHeap(), 0, query);

    return ret;
}
