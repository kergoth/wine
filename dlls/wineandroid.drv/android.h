/*
 * Android driver definitions
 *
 * Copyright 2013 Alexandre Julliard
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

#ifndef __WINE_ANDROID_H
#define __WINE_ANDROID_H

#include <limits.h>
#include <stdarg.h>
#include <stdlib.h>
#include <android/log.h>
#include <android/configuration.h>
#include <android/input.h>
#include <android/native_window_jni.h>

#undef SendMessage /* conflicts with SLMIDIMessageItf_::SendMessage */
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>

#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "winuser.h"
#include "wine/gdi_driver.h"
#include "android_native.h"
#include "wine/list.h"


/**************************************************************************
 * Android interface
 */
#define DECL_FUNCPTR(f) extern typeof(f) * p##f DECLSPEC_HIDDEN
DECL_FUNCPTR( __android_log_print );
DECL_FUNCPTR( ANativeWindow_fromSurface );
DECL_FUNCPTR( ANativeWindow_release );
DECL_FUNCPTR( slCreateEngine );
DECL_FUNCPTR( SL_IID_ANDROIDSIMPLEBUFFERQUEUE );
DECL_FUNCPTR( SL_IID_ENGINE );
DECL_FUNCPTR( SL_IID_PLAY );
DECL_FUNCPTR( SL_IID_PLAYBACKRATE );
DECL_FUNCPTR( SL_IID_RECORD );
#undef DECL_FUNCPTR

/**************************************************************************
 * GDI driver
 */

typedef struct
{
    struct gdi_physdev dev;
} ANDROID_PDEVICE;

static inline ANDROID_PDEVICE *get_android_dev( PHYSDEV dev )
{
    return (ANDROID_PDEVICE *)dev;
}

static inline void reset_bounds( RECT *bounds )
{
    bounds->left = bounds->top = INT_MAX;
    bounds->right = bounds->bottom = INT_MIN;
}

static inline void add_bounds_rect( RECT *bounds, const RECT *rect )
{
    if (rect->left >= rect->right || rect->top >= rect->bottom) return;
    bounds->left   = min( bounds->left, rect->left );
    bounds->top    = min( bounds->top, rect->top );
    bounds->right  = max( bounds->right, rect->right );
    bounds->bottom = max( bounds->bottom, rect->bottom );
}

enum android_pixel_format
{
    PF_RGBA_8888 = 1,
    PF_RGBX_8888 = 2,
    PF_RGB_888   = 3,
    PF_RGB_565   = 4,
    PF_BGRA_8888 = 5,
    PF_RGBA_5551 = 6,
    PF_RGBA_4444 = 7
};

extern void destroy_gl_drawable( HWND hwnd ) DECLSPEC_HIDDEN;
extern struct opengl_funcs *get_wgl_driver( UINT version ) DECLSPEC_HIDDEN;


/**************************************************************************
 * Android pseudo-device
 */

extern void start_android_device(void) DECLSPEC_HIDDEN;
extern void register_native_window( HWND hwnd, struct ANativeWindow *win ) DECLSPEC_HIDDEN;
extern int ioctl_window_pos_changed( HWND hwnd, const RECT *rect, UINT style, UINT flags,
                                     HWND after, HWND owner ) DECLSPEC_HIDDEN;
extern int ioctl_set_window_focus( HWND hwnd ) DECLSPEC_HIDDEN;
extern int ioctl_set_window_text( HWND hwnd, const WCHAR *text ) DECLSPEC_HIDDEN;
extern int ioctl_set_window_icon( HWND hwnd, int width, int height,
                                  const unsigned int *bits ) DECLSPEC_HIDDEN;
extern int ioctl_set_window_rgn( HWND hwnd, HRGN rgn ) DECLSPEC_HIDDEN;
extern int ioctl_set_window_layered( HWND hwnd, COLORREF key, BYTE alpha ) DECLSPEC_HIDDEN;
extern int ioctl_set_surface_alpha( HWND hwnd, BOOL has_alpha ) DECLSPEC_HIDDEN;
extern int ioctl_set_capture( HWND hwnd ) DECLSPEC_HIDDEN;
extern struct ANativeWindow *create_ioctl_window( HWND hwnd ) DECLSPEC_HIDDEN;
extern void destroy_ioctl_window( HWND hwnd ) DECLSPEC_HIDDEN;
extern void grab_ioctl_window( struct ANativeWindow *window ) DECLSPEC_HIDDEN;
extern void release_ioctl_window( struct ANativeWindow *window ) DECLSPEC_HIDDEN;
extern int ioctl_gamepad_query( int index, int device, void* data) DECLSPEC_HIDDEN;
extern int ioctl_imeText( int target, int *cursor, int *length, WCHAR* string ) DECLSPEC_HIDDEN;
extern int ioctl_imeFinish( int target ) DECLSPEC_HIDDEN;
extern int ioctl_get_clipboard_formats( DWORD* seqno, UINT** formats, DWORD* num_formats ) DECLSPEC_HIDDEN;
extern int ioctl_get_clipboard_data( UINT format, HGLOBAL* result, BOOL* pending ) DECLSPEC_HIDDEN;
extern int ioctl_render_clipboard_data( int android_format ) DECLSPEC_HIDDEN;
extern int ioctl_empty_clipboard( void ) DECLSPEC_HIDDEN;
extern int ioctl_set_clipboard_data( UINT format, BOOL format_present, BYTE* buffer, DWORD size ) DECLSPEC_HIDDEN;
extern int ioctl_end_clipboard_update( void ) DECLSPEC_HIDDEN;
extern int ioctl_acquire_clipboard( void ) DECLSPEC_HIDDEN;
extern int ioctl_export_clipboard_data( int android_format, BYTE* data, DWORD size ) DECLSPEC_HIDDEN;


/**************************************************************************
 * USER driver
 */

extern unsigned int screen_width DECLSPEC_HIDDEN;
extern unsigned int screen_height DECLSPEC_HIDDEN;
extern unsigned int screen_bpp DECLSPEC_HIDDEN;
extern unsigned int screen_dpi DECLSPEC_HIDDEN;
extern RECT virtual_screen_rect DECLSPEC_HIDDEN;
extern MONITORINFOEXW default_monitor DECLSPEC_HIDDEN;

enum android_window_messages
{
    WM_ANDROID_REFRESH = 0x80001000,
    WM_ANDROID_IME_CONTROL,
    WM_ANDROID_RENDERFORMAT,
    WM_ANDROID_CLIPBOARD_REQUEST,
};

/* private window data */
struct android_win_data
{
    HWND           hwnd;           /* hwnd that this private data belongs to */
    RECT           window_rect;    /* USER window rectangle relative to parent */
    RECT           whole_rect;     /* X window rectangle for the whole window relative to parent */
    RECT           client_rect;    /* client area relative to parent */
    ANativeWindow *window;
    struct window_surface *surface;
};

extern struct android_win_data *get_win_data( HWND hwnd ) DECLSPEC_HIDDEN;
extern void release_win_data( struct android_win_data *data ) DECLSPEC_HIDDEN;
extern struct ANativeWindow *get_ioctl_window( HWND hwnd ) DECLSPEC_HIDDEN;
extern HWND get_capture_window(void) DECLSPEC_HIDDEN;
extern void set_screen_dpi( DWORD dpi, BOOL force ) DECLSPEC_HIDDEN;
extern void init_monitors( int width, int height ) DECLSPEC_HIDDEN;
extern void handle_run_cmdline( LPWSTR cmdline, LPWSTR* wineEnv ) DECLSPEC_HIDDEN;
extern void handle_clear_meta_key_states( int states ) DECLSPEC_HIDDEN;
extern void handle_clipboard_changed( void ) DECLSPEC_HIDDEN;
extern void handle_import_clipboard_data( INT android_format, BYTE* data, DWORD len ) DECLSPEC_HIDDEN;
extern int get_clipboard_formats( DWORD* seqno, UINT* formats, UINT* num_formats ) DECLSPEC_HIDDEN;
extern void get_exported_formats( BOOL* formats, int num_formats ) DECLSPEC_HIDDEN;
extern BOOL handle_ioctl_get_clipboard_data( UINT format, BOOL* format_present, BOOL* pending, BYTE* data, DWORD* size ) DECLSPEC_HIDDEN;
extern void handle_ioctl_empty_clipboard( void ) DECLSPEC_HIDDEN;
extern void handle_ioctl_set_clipboard_data( UINT format, BOOL format_present, BYTE* data, DWORD size ) DECLSPEC_HIDDEN;
extern void handle_ioctl_end_clipboard_update( void ) DECLSPEC_HIDDEN;
extern void handle_clipboard_request( int android_format ) DECLSPEC_HIDDEN;
extern void update_keyboard_lock_state( WORD vkey, UINT state ) DECLSPEC_HIDDEN;

/* JNI entry points */
extern jboolean keyboard_event( JNIEnv *env, jobject obj, jint win, jint action, jint keycode, jint scancode, jint state ) DECLSPEC_HIDDEN;
extern jboolean clear_meta_key_states( JNIEnv *env, jobject obj, jint states ) DECLSPEC_HIDDEN;
extern jboolean motion_event( JNIEnv *env, jobject obj, jint win, jint action, jint x, jint y, jint state, jint vscroll ) DECLSPEC_HIDDEN;
extern void surface_changed( JNIEnv *env, jobject obj, jint win, jobject surface ) DECLSPEC_HIDDEN;
extern void desktop_changed( JNIEnv *env, jobject obj, jint width, jint height ) DECLSPEC_HIDDEN;
extern void config_changed( JNIEnv *env, jobject obj, jint dpi, jboolean force ) DECLSPEC_HIDDEN;
extern void clipboard_changed( JNIEnv *env, jobject obj, jbooleanArray formats_present ) DECLSPEC_HIDDEN;
extern void import_clipboard_data( JNIEnv *env, jobject obj, jint android_format, jbyteArray data ) DECLSPEC_HIDDEN;
extern void clipboard_request( JNIEnv *env, jobject obj, jint android_format ) DECLSPEC_HIDDEN;
extern void run_commandline( JNIEnv *env, jobject obj, jobject _cmdline, jobjectArray _wineEnv ) DECLSPEC_HIDDEN;;

/* IME entry points */
extern void IME_UpdateAssociation(HWND focus) DECLSPEC_HIDDEN;
extern void ime_text( JNIEnv *env, jobject obj, jstring text, jint length, jint cursor) DECLSPEC_HIDDEN;
extern void ime_finish( JNIEnv *env, jobject obj) DECLSPEC_HIDDEN;
extern void ime_cancel( JNIEnv *env, jobject obj) DECLSPEC_HIDDEN;
extern void ime_start( JNIEnv *env, jobject obj) DECLSPEC_HIDDEN;
extern void handle_IME_TEXT(int target, int length) DECLSPEC_HIDDEN;
extern void handle_IME_FINISH(int target, int length) DECLSPEC_HIDDEN;
extern void handle_IME_CANCEL(void) DECLSPEC_HIDDEN;
extern void handle_IME_START(void) DECLSPEC_HIDDEN;
extern LRESULT Ime_Control(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) DECLSPEC_HIDDEN;

/* GAMEPAD entry points and DATA*/
extern void gamepad_count(JNIEnv *env, jobject obj, jint count) DECLSPEC_HIDDEN;
extern void gamepad_data(JNIEnv *env, jobject obj, jint index, jint id, jstring name) DECLSPEC_HIDDEN;
extern void gamepad_sendaxis(JNIEnv *env, jobject obj, jint device, jfloatArray axis) DECLSPEC_HIDDEN;
extern void gamepad_sendbutton(JNIEnv *env, jobject obj, jint device, jint element, jint value) DECLSPEC_HIDDEN;

#define DI_AXIS_COUNT 8
#define DI_POV_COUNT 1  /* POVs take 2 axis  */
#define DI_AXIS_DATA_COUNT (DI_AXIS_COUNT + DI_POV_COUNT*2)
#define DI_BUTTON_COUNT 30
#define DI_DATASIZE  (DI_AXIS_DATA_COUNT + DI_BUTTON_COUNT)
#define DI_BUTTON_DATA_OFFSET DI_AXIS_DATA_COUNT
#define DI_NAME_LENGTH 255

typedef int di_value_set[DI_DATASIZE];
typedef WCHAR di_name[DI_NAME_LENGTH];

extern di_value_set *di_value;
extern di_name      *di_names;
extern int          di_controllers;

enum event_type
{
    HARDWARE_INPUT,
    SURFACE_CHANGED,
    DESKTOP_CHANGED,
    CONFIG_CHANGED,
    CLIPBOARD_CHANGED,
    IMPORT_CLIPBOARD_DATA,
    CLIPBOARD_REQUEST,
    IME_TEXT,
    IME_FINISH,
    IME_CANCEL,
    IME_START,
    RUN_CMDLINE,
    CLEAR_META
};

union event_data
{
    enum event_type type;
    struct
    {
        enum event_type type;
        HWND            hwnd;
        INPUT           input;
    } hw;
    struct
    {
        enum event_type type;
        HWND            hwnd;
        ANativeWindow *window;
        unsigned int width;
        unsigned int height;
    } surface;
    struct
    {
        enum event_type type;
        HWND            hwnd;
        RECT            rect;
    } flush;
    struct
    {
        enum event_type type;
        unsigned int    width;
        unsigned int    height;
    } desktop;
    struct
    {
        enum event_type type;
        unsigned int    dpi;
        BOOL            force;
    } cfg;
    struct
    {
        enum event_type type;
        INT             android_format;
        DWORD           len;
        BYTE*           data;
    } clipdata;
    struct
    {
        enum event_type type;
        WORD            target;
        WORD            length;
    } ime_text;
    struct
    {
        enum event_type type;
        WORD            target;
        WORD            length;
    } ime_finish;
    struct
    {
        enum event_type type;
        LPWSTR          cmdline;
        LPWSTR*         env;
    } runcmd;
    struct
    {
        enum event_type type;
        int             states;
    } clearmeta;
};

struct android_thread_data
{
    union event_data *current_event;
    int               event_pipe[2];
    struct list       event_queue;
};

extern struct android_thread_data *android_init_thread_data(void) DECLSPEC_HIDDEN;
extern DWORD thread_data_tls_index DECLSPEC_HIDDEN;

int send_event( struct android_thread_data *thread, const union event_data *data );
extern struct android_thread_data *desktop_thread;

static inline struct android_thread_data *android_thread_data(void)
{
    return TlsGetValue( thread_data_tls_index );
}

extern JavaVM *wine_get_java_vm(void);
extern jobject wine_get_java_object(void);

extern struct gralloc_module_t *gralloc_module;

extern ANativeWindow *main_window DECLSPEC_HIDDEN;

extern HANDLE g_timer_q;

typedef struct _s_ime_text {
    WCHAR *text;
    INT    length;
    INT    cursor_pos;
} s_ime_text;

extern s_ime_text **java_ime_text;
extern INT java_ime_active_target;
extern INT java_ime_count;


#endif  /* __WINE_ANDROID_H */
