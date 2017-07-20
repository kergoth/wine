/*
 * Clipboard related functions
 *
 * Copyright 1994 Martin Ayotte
 *	     1996 Alex Korobka
 *	     1999 Noel Borthwick
 *           2003 Ulrich Czekalla for CodeWeavers
 *           2014 Damjan Jovanovic
 *           2016 Vincent Povirk for CodeWeavers
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

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "winuser.h"
#include "wine/unicode.h"

#include "android.h"
#include "wine/debug.h"
#include "wine/server.h"

WINE_DEFAULT_DEBUG_CHANNEL(clipboard);

typedef HANDLE (*AndroidImportFunction)(BYTE* data, DWORD len);
typedef DWORD (*AndroidExportFunction)(HANDLE input, BYTE* output, DWORD len);

typedef struct android_clipformat {
    UINT uFormat;
    AndroidImportFunction import;
    AndroidExportFunction export;
    BOOL present;
    BOOL requested;
} android_clipformat;

static HANDLE ANDROID_CLIPBOARD_ImportText(BYTE* data, DWORD len);
static DWORD ANDROID_CLIPBOARD_ExportText(HANDLE input, BYTE* output, DWORD len);

/* Keep synced with TopView.clip_mimetypes */
static android_clipformat android_clipformats[] = {
    { CF_UNICODETEXT, ANDROID_CLIPBOARD_ImportText, ANDROID_CLIPBOARD_ExportText }, /* text/plain */
};

enum HANDLE_TYPE {
    HANDLE_TYPE_GLOBAL,
    HANDLE_TYPE_GDI,
    HANDLE_TYPE_EMF,
    HANDLE_TYPE_METAFILEPICT,
    HANDLE_TYPE_PRIVATE,
};

DWORD clipdata_seqno = 0;
DWORD clipdata_count = 0;

typedef struct clipdata {
    struct list entry;
    UINT format;
    HANDLE data;
    enum data_source {
        DATA_SOURCE_INPROCESS,
        DATA_SOURCE_SYNTHESIZE,
        DATA_SOURCE_RENDERFORMAT,
        DATA_SOURCE_DESKTOP,
        DATA_SOURCE_JAVA,
    } data_source;
    int android_format;
    BOOL waiting; /* TRUE if someone is waiting for this format's data */
} clipdata;

struct list clipdata_list = LIST_INIT(clipdata_list);

static CRITICAL_SECTION clipdata_section;
static CRITICAL_SECTION_DEBUG critsect_debug =
{
    0, 0, &clipdata_section,
    { &critsect_debug.ProcessLocksList, &critsect_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": clipdata_section") }
};
static CRITICAL_SECTION clipdata_section = { &critsect_debug, -1, 0, 0, 0, 0 };

static BOOL handling_clipboard_changed;

HANDLE data_update_event; /* Signaled if any format where waiting==TRUE has changed. */

static clipdata* ANDROID_CLIPBOARD_LookupData( UINT format )
{
    clipdata* entry;

    EnterCriticalSection( &clipdata_section );

    LIST_FOR_EACH_ENTRY(entry, &clipdata_list, clipdata, entry) {
        if (entry->format == format)
        {
            LeaveCriticalSection( &clipdata_section );
            return entry;
        }
    }

    LeaveCriticalSection( &clipdata_section );

    return NULL;
}

static enum HANDLE_TYPE format_handle_type( UINT format )
{
    if ((format >= CF_GDIOBJFIRST && format <= CF_GDIOBJLAST) ||
        format == CF_BITMAP ||
        format == CF_DIB ||
        format == CF_PALETTE)
        return HANDLE_TYPE_GDI;
    else if (format == CF_METAFILEPICT)
        return HANDLE_TYPE_METAFILEPICT;
    else if (format == CF_ENHMETAFILE)
        return HANDLE_TYPE_EMF;
    else if (format >= CF_PRIVATEFIRST && format <= CF_PRIVATELAST)
        return HANDLE_TYPE_PRIVATE;
    else
        return HANDLE_TYPE_GLOBAL;
}

static void ANDROID_CLIPBOARD_FreeData( clipdata *data )
{
    TRACE("%04X %p\n", data->format, data->data);

    switch (format_handle_type( data->format ))
    {
    case HANDLE_TYPE_GLOBAL:
        GlobalFree( data->data );
        break;
    case HANDLE_TYPE_GDI:
        DeleteObject( data->data );
        break;
    case HANDLE_TYPE_EMF:
        if (data->data)
            GlobalFree( data->data );
        break;
    case HANDLE_TYPE_METAFILEPICT:
        if (data->data)
        {
            DeleteMetaFile(((METAFILEPICT *)GlobalLock( data->data ))->hMF );
            GlobalFree(data->data);
        }
        break;
    case HANDLE_TYPE_PRIVATE:
        break;
    }

    data->data = 0;
}

static HANDLE get_data_update_event(void)
{
    if (!data_update_event)
    {
        HANDLE result = CreateEventA( NULL, TRUE, TRUE, "Global\\WineAndroidClipboardUpdate" );
        if (InterlockedCompareExchangePointer( (void*)&data_update_event, result, NULL ))
            CloseHandle( result );
    }
    return data_update_event;
}

static void ANDROID_CLIPBOARD_HandleJavaRequests(void);

static clipdata* ANDROID_CLIPBOARD_InsertData( UINT format, HANDLE data, enum data_source data_source )
{
    clipdata* result;
    BOOL was_waiting=FALSE;

    TRACE("%04X, %p, %u\n", format, data, data_source);

    EnterCriticalSection( &clipdata_section );

    if ((result = ANDROID_CLIPBOARD_LookupData( format )))
    {
        ANDROID_CLIPBOARD_FreeData( result );
        result->data = data;
        result->data_source = data_source;
        if (result->waiting)
        {
            was_waiting = TRUE;
            result->waiting = FALSE;
        }
    }
    else if ((result = HeapAlloc( GetProcessHeap(), 0, sizeof(*result) )))
    {
        result->format = format;
        result->data = data;
        result->data_source = data_source;
        result->waiting = FALSE;

        list_add_tail( &clipdata_list, &result->entry );
        clipdata_count++;
    }

    if (was_waiting)
    {
        SetEvent( get_data_update_event() );
        ANDROID_CLIPBOARD_HandleJavaRequests();
    }

    LeaveCriticalSection( &clipdata_section );

    return result;
}

static void ANDROID_CLIPBOARD_SynthesizeFormats( void )
{
    clipdata *entry;
    BOOL cf_text=FALSE, cf_oemtext=FALSE, cf_unicodetext=FALSE;

    EnterCriticalSection( &clipdata_section );

    LIST_FOR_EACH_ENTRY(entry, &clipdata_list, clipdata, entry) {
        switch (entry->format) {
        case CF_TEXT:
            cf_text = TRUE;
            break;
        case CF_OEMTEXT:
            cf_oemtext = TRUE;
            break;
        case CF_UNICODETEXT:
            cf_unicodetext = TRUE;
            break;
        }
    }

    if (cf_text || cf_oemtext || cf_unicodetext) {
        if (!cf_text) ANDROID_CLIPBOARD_InsertData( CF_TEXT, 0, DATA_SOURCE_SYNTHESIZE );
        if (!cf_oemtext) ANDROID_CLIPBOARD_InsertData( CF_OEMTEXT, 0, DATA_SOURCE_SYNTHESIZE );
        if (!cf_unicodetext) ANDROID_CLIPBOARD_InsertData( CF_UNICODETEXT, 0, DATA_SOURCE_SYNTHESIZE );
    }

    LeaveCriticalSection( &clipdata_section );
}

static BOOL is_desktop_process( void )
{
    /* FIXME: There must be a better way. */
    DWORD pid;
    GetWindowThreadProcessId( GetDesktopWindow(), &pid );
    return pid == GetCurrentProcessId();
}

void clipboard_changed( JNIEnv *env, jobject obj, jbooleanArray formats_present )
{
    union event_data data;
    jboolean* values;
    int i;

    values = (*env)->GetBooleanArrayElements( env, formats_present, NULL );
    for (i=0; i < sizeof(android_clipformats)/sizeof(android_clipformats[0]); i++)
    {
        android_clipformats[i].present = (values[i] != JNI_FALSE);
    }
    (*env)->ReleaseBooleanArrayElements( env, formats_present, values, JNI_ABORT );

    memset( &data, 0, sizeof(data) );
    data.type = CLIPBOARD_CHANGED;
    p__android_log_print( ANDROID_LOG_INFO, "wine", "clipboard_changed\n" );
    send_event( desktop_thread, &data );
}

void handle_clipboard_changed(void)
{
    int i;

    TRACE("\n");

    if (OpenClipboard( GetDesktopWindow() ))
    {
        handling_clipboard_changed = TRUE;

        EmptyClipboard();

        for (i=0; i < sizeof(android_clipformats)/sizeof(android_clipformats[0]); i++)
        {
            if (android_clipformats[i].import && android_clipformats[i].present)
            {
                clipdata *data = ANDROID_CLIPBOARD_InsertData( android_clipformats[i].uFormat, NULL, DATA_SOURCE_JAVA );
                if (data)
                    data->android_format = i;
            }
        }

        CloseClipboard();
    }
}

BOOL get_clipboard_data( UINT wFormat, HANDLE* data, BOOL may_block );

void clipboard_request( JNIEnv *env, jobject obj, jint format )
{
    union event_data data;

    memset( &data, 0, sizeof(data) );
    data.type = CLIPBOARD_REQUEST;
    data.clipdata.android_format = format;
    p__android_log_print( ANDROID_LOG_INFO, "wine", "clipboard_requested\n" );
    send_event( desktop_thread, &data );
}

void handle_clipboard_request( int format )
{
    HANDLE data;
    android_clipformat *clipformat = &android_clipformats[format];

    TRACE("%i\n", format);

    if (get_clipboard_data( clipformat->uFormat, &data, FALSE ))
    {
        BYTE* exported=NULL;
        DWORD exported_len=0;

        if (data)
        {
            exported_len = clipformat->export( data, NULL, 0 );
            if (exported_len)
            {
                exported = HeapAlloc( GetProcessHeap(), 0, exported_len );
                if (exported)
                    clipformat->export( data, exported, exported_len );
                else
                    exported_len = 0;
            }
        }

        clipformat->requested = FALSE;

        ioctl_export_clipboard_data( format, exported, exported_len );

        HeapFree( GetProcessHeap(), 0, exported );
    }
    else
        clipformat->requested = TRUE;
}

static void ANDROID_CLIPBOARD_HandleJavaRequests(void)
{
    int i;

    for (i=0; i<sizeof(android_clipformats)/sizeof(android_clipformats[0]); i++)
    {
        if (android_clipformats[i].requested)
        {
            /* This may be called from the device thread, in which case we'll
             * hang if we try an ioctl, so send it to the desktop thread */
            SendNotifyMessageW( GetDesktopWindow(), WM_ANDROID_CLIPBOARD_REQUEST, (WPARAM)i, 0 );
        }
    }
}

void get_exported_formats( BOOL* formats, int num_formats )
{
    int i;

    if (num_formats != sizeof(android_clipformats)/sizeof(android_clipformats[0]))
    {
        ERR( "get_exported_formats called with wrong size\n" );
        return;
    }

    EnterCriticalSection( &clipdata_section );

    for (i=0; i<num_formats; i++)
    {
        android_clipformat *clipformat = &android_clipformats[i];

        formats[i] = (clipformat->export != NULL &&
            ANDROID_CLIPBOARD_LookupData( clipformat->uFormat ) != NULL);
    }

    LeaveCriticalSection( &clipdata_section );
}

static void ANDROID_CLIPBOARD_EmptyClipData(void)
{
    clipdata *data, *next;
    BOOL any_waiting=FALSE;

    EnterCriticalSection( &clipdata_section );

    LIST_FOR_EACH_ENTRY_SAFE( data, next, &clipdata_list, clipdata, entry )
    {
        list_remove( &data->entry );
        ANDROID_CLIPBOARD_FreeData( data );
        if (data->waiting)
            any_waiting = TRUE;
        HeapFree( GetProcessHeap(), 0, data );
        clipdata_count--;
    }

    if (any_waiting)
    {
        SetEvent( get_data_update_event() );
        ANDROID_CLIPBOARD_HandleJavaRequests();
    }

    LeaveCriticalSection( &clipdata_section );
}

void CDECL ANDROID_EmptyClipboard(void)
{
    TRACE("\n");

    ANDROID_CLIPBOARD_EmptyClipData();

    if (!is_desktop_process())
        ioctl_empty_clipboard();
}

void CDECL ANDROID_EndClipboardUpdate(void)
{
    TRACE("\n");

    clipdata_seqno = GetClipboardSequenceNumber();

    if (!is_desktop_process())
        ioctl_end_clipboard_update();
    else if (handling_clipboard_changed)
        handling_clipboard_changed = FALSE;
    else
        ioctl_acquire_clipboard();

    ANDROID_CLIPBOARD_SynthesizeFormats();
}

static BOOL ANDROID_CLIPBOARD_IsClipDataCurrent(void)
{
    BOOL is_process_owner = FALSE;
    DWORD current_seqno = 0;

    if (is_desktop_process())
        return TRUE;

    SERVER_START_REQ( set_clipboard_info )
    {
        req->flags = 0;
        if (!wine_server_call_err( req ))
        {
            is_process_owner = (reply->flags & CB_PROCESS);
            current_seqno = reply->seqno;
        }
    }
    SERVER_END_REQ;

    return is_process_owner || current_seqno == clipdata_seqno;
}

static void ANDROID_CLIPBOARD_UpdateCache(void)
{
    if (!ANDROID_CLIPBOARD_IsClipDataCurrent())
    {
        DWORD current_seqno, current_count, i;
        UINT *current_formats;

        TRACE("updating\n");

        if (ioctl_get_clipboard_formats( &current_seqno, &current_formats, &current_count ))
            return;

        EnterCriticalSection( &clipdata_section );

        ANDROID_CLIPBOARD_EmptyClipData();
        /* FIXME: wineserver spontaneously updates seqno so the one we get from the desktop process may be outdated */
        clipdata_seqno = GetClipboardSequenceNumber();
        for (i=0; i<current_count; i++)
        {
            ANDROID_CLIPBOARD_InsertData( current_formats[i], NULL, DATA_SOURCE_DESKTOP );
        }

        ANDROID_CLIPBOARD_SynthesizeFormats();

        LeaveCriticalSection( &clipdata_section );
    }
}

UINT CDECL ANDROID_EnumClipboardFormats( UINT wFormat )
{
    struct list *ptr = NULL;
    UINT ret;

    TRACE("(%04X)\n", wFormat);

    ANDROID_CLIPBOARD_UpdateCache();

    EnterCriticalSection( &clipdata_section );

    if (!wFormat)
    {
        ptr = list_head( &clipdata_list );
    }
    else
    {
        clipdata *lpData = ANDROID_CLIPBOARD_LookupData( wFormat );
        if (lpData) ptr = list_next( &clipdata_list, &lpData->entry );
    }

    if (!ptr) ret = 0;
    else ret = LIST_ENTRY( ptr, clipdata, entry )->format;

    LeaveCriticalSection( &clipdata_section );

    return ret;
}

BOOL CDECL ANDROID_IsClipboardFormatAvailable( UINT wFormat )
{
    TRACE("(%04X)\n", wFormat);

    ANDROID_CLIPBOARD_UpdateCache();

    return (ANDROID_CLIPBOARD_LookupData( wFormat ) != NULL);
}

INT CDECL ANDROID_CountClipboardFormats(void)
{
    ANDROID_CLIPBOARD_UpdateCache();

    TRACE("count=%d\n", clipdata_count);

    return clipdata_count;
}

/* Import UTF-16 text, adding NULL terminator if necessary. */
static HANDLE ANDROID_CLIPBOARD_ImportText( BYTE* data, DWORD len )
{
    HANDLE result;
    BYTE* lock;
    DWORD alloc_len = len;

    if (len < 2 || data[len-2] != 0 || data[len-1] != 0)
        alloc_len += 2;

    result = GlobalAlloc( GMEM_MOVEABLE, alloc_len );
    if (result)
    {
        lock = GlobalLock( result );
        memcpy( lock, data, len );
        lock[alloc_len-2] = 0;
        lock[alloc_len-1] = 0;
        GlobalUnlock( result );
    }

    return result;
}

/* Export CF_UNICODE to utf8 with no NULL terminator */
static DWORD ANDROID_CLIPBOARD_ExportText(HANDLE input, BYTE* output, DWORD len)
{
    DWORD input_size, input_len, output_len;
    WCHAR *input_data, *input_end;

    input_size = GlobalSize( input );

    input_data = GlobalLock( input );

    input_end = memchrW( input_data, 0, input_size/sizeof(WCHAR) );
    if (input_end) input_len = input_end - input_data;
    else input_len = input_size/sizeof(WCHAR);

    output_len = WideCharToMultiByte( CP_UTF8, 0, input_data, input_len, NULL, 0, NULL, NULL );

    if (output != NULL && output_len <= len)
    {
        WideCharToMultiByte( CP_UTF8, 0, input_data, input_len, (LPSTR)output, output_len, NULL, NULL );
    }

    GlobalUnlock( input );

    return output_len;
}

static HANDLE ANDROID_CLIPBOARD_DeserializeHGlobal( UINT format, HGLOBAL data )
{
    HANDLE result = NULL;

    TRACE("%04X %p\n", format, data);

    switch (format_handle_type( format ))
    {
    case HANDLE_TYPE_GDI:
    case HANDLE_TYPE_EMF:
    case HANDLE_TYPE_METAFILEPICT:
        FIXME("%04X\n", format);
        GlobalFree( data );
        break;
    case HANDLE_TYPE_GLOBAL:
        return data;
        break;
    case HANDLE_TYPE_PRIVATE:
        ERR("shouldn't happen\n");
        break;
    }

    return result;
}

static DWORD ANDROID_CLIPBOARD_GetSerializedSize( UINT format, HANDLE handle )
{
    switch (format_handle_type( format ))
    {
    case HANDLE_TYPE_GDI:
    case HANDLE_TYPE_EMF:
    case HANDLE_TYPE_METAFILEPICT:
        FIXME("%04X\n", format);
        return 0;
    case HANDLE_TYPE_GLOBAL:
        return GlobalSize( handle );
    case HANDLE_TYPE_PRIVATE:
        ERR("shouldn't happen\n");
        return 0;
    }

    return 0;
}

static void ANDROID_CLIPBOARD_SerializeHandle( UINT format, HANDLE handle, BYTE* buffer, DWORD size )
{
    switch (format_handle_type( format ))
    {
    case HANDLE_TYPE_GDI:
    case HANDLE_TYPE_EMF:
    case HANDLE_TYPE_METAFILEPICT:
        FIXME("%04X\n", format);
        break;
    case HANDLE_TYPE_GLOBAL:
    {
        DWORD actual_size = GlobalSize( handle );
        if (actual_size <= size)
        {
            void* lock = GlobalLock( handle );
            if (lock)
            {
                memcpy( buffer, lock, actual_size );
                GlobalUnlock( handle );
            }
        }
        break;
    }
    case HANDLE_TYPE_PRIVATE:
        ERR("shouldn't happen\n");
        break;
    }
}

static BOOL ANDROID_CLIPBOARD_ShouldExportData( clipdata* clipdata )
{
    if (clipdata->data_source == DATA_SOURCE_SYNTHESIZE) {
        /* We have no way for the desktop process to ask a client to
         * render synthesized data on another client's behalf, so the
         * easiest thing is for every process to synthesize its own data. */
        return FALSE;
    }

    switch (format_handle_type( clipdata->format ))
    {
    case HANDLE_TYPE_GDI:
    case HANDLE_TYPE_EMF:
    case HANDLE_TYPE_METAFILEPICT:
    case HANDLE_TYPE_GLOBAL:
        return TRUE;
    case HANDLE_TYPE_PRIVATE:
    default:
        return FALSE;
    }
}

extern NTSTATUS get_clipboard_formats( DWORD* seqno, UINT* formats, UINT* num_formats )
{
    UINT buffer_length = *num_formats;
    clipdata* entry;
    int i;

    *seqno = clipdata_seqno;

    EnterCriticalSection( &clipdata_section );

    i = 0;
    LIST_FOR_EACH_ENTRY(entry, &clipdata_list, clipdata, entry) {
        if (ANDROID_CLIPBOARD_ShouldExportData( entry ))
        {
            if (formats && i < buffer_length)
                formats[i] = entry->format;
            i++;
        }
    }

    LeaveCriticalSection( &clipdata_section );

    *num_formats = i;

    return i <= buffer_length ? STATUS_SUCCESS : STATUS_BUFFER_OVERFLOW;
}

void import_clipboard_data( JNIEnv *env, jobject obj, jint android_format, jbyteArray data )
{
    union event_data event;
    jsize array_length;
    jbyte* array_values;
    BYTE* buffer = NULL;

    array_length = (*env)->GetArrayLength( env, (jarray)data );

    if (array_length)
    {
        buffer = malloc( array_length );

        if (!buffer) return;

        array_values = (*env)->GetByteArrayElements( env, data, NULL );
        memcpy( buffer, array_values, array_length );
        (*env)->ReleaseByteArrayElements( env, data, array_values, JNI_ABORT );
    }
    else
        buffer = NULL;

    memset( &event, 0, sizeof(event) );
    event.type = IMPORT_CLIPBOARD_DATA;
    event.clipdata.android_format = android_format;
    event.clipdata.len = array_length;
    event.clipdata.data = buffer;
    p__android_log_print( ANDROID_LOG_INFO, "wine", "import_clipboard_data\n" );
    send_event( desktop_thread, &event );
}

void handle_import_clipboard_data( INT android_format, BYTE* data, DWORD len )
{
    android_clipformat* clipformat = &android_clipformats[android_format];
    clipdata* clipdata;

    TRACE("%i\n", android_format);

    EnterCriticalSection( &clipdata_section );

    clipdata = ANDROID_CLIPBOARD_LookupData( clipformat->uFormat );

    if (clipdata && !clipdata->data && clipdata->data_source == DATA_SOURCE_JAVA)
    {
        ANDROID_CLIPBOARD_InsertData( clipformat->uFormat, clipformat->import( data, len ), DATA_SOURCE_INPROCESS );
    }

    LeaveCriticalSection( &clipdata_section );
}

enum RENDERFORMAT_RESULT {
    COMPLETE, /* clipboard format is rendered (handle may be NULL on failure) */
    BLOCK, /* caller should block on get_data_update_event() and retry */
    RETRY, /* caller should retry immediately */
};

static enum RENDERFORMAT_RESULT ANDROID_CLIPBOARD_SynthesizeAnsiText( clipdata* clip, HANDLE* data, BOOL block )
{
    clipdata *unicode_clipdata;
    HANDLE hsrc, hdst;
    WCHAR *psrc;
    char *pdst;
    UINT cp;
    INT src_len, dst_len;
    WCHAR *src_end;

    TRACE("\n");

    unicode_clipdata = ANDROID_CLIPBOARD_LookupData( CF_UNICODETEXT );

    if (!unicode_clipdata)
    {
        ERR("missing CF_UNICODETEXT\n");
        LeaveCriticalSection( &clipdata_section );
        *data = NULL;
        return COMPLETE;
    }

    if (!unicode_clipdata->data)
    {
        enum RENDERFORMAT_RESULT ret;
        HANDLE dummy;
        LeaveCriticalSection( &clipdata_section );
        ret = get_clipboard_data( CF_UNICODETEXT, &dummy, block );
        if (ret == BLOCK) return BLOCK;
        else return RETRY;
    }

    if (clip->format == CF_OEMTEXT)
        cp = CP_OEMCP;
    else
        cp = CP_ACP;

    hsrc = unicode_clipdata->data;
    src_len = GlobalSize( hsrc ) / sizeof(WCHAR);
    psrc = GlobalLock( hsrc );

    src_end = memchrW( psrc, 0, src_len );
    if (src_end) src_len = src_end - psrc;

    dst_len = WideCharToMultiByte( cp, 0, psrc, src_len, 0, 0, 0, 0 );

    hdst = GlobalAlloc( GMEM_MOVEABLE, dst_len + 1 );
    if (!hdst)
    {
        GlobalUnlock( hsrc );
        LeaveCriticalSection( &clipdata_section );
        *data = NULL;
        return COMPLETE;
    }

    pdst = GlobalLock( hdst );
    WideCharToMultiByte( cp, 0, psrc, src_len, pdst, dst_len, 0, 0 );
    pdst[dst_len] = 0;
    GlobalUnlock( hdst );
    GlobalUnlock( hsrc );

    clip->data = hdst;

    LeaveCriticalSection( &clipdata_section );

    *data = hdst;
    return COMPLETE;
}

static enum RENDERFORMAT_RESULT ANDROID_CLIPBOARD_SynthesizeUnicodeText( clipdata* clip, HANDLE* data, BOOL block )
{
    clipdata *ansi_clipdata;
    UINT src_format;
    HANDLE hsrc, hdst;
    char *psrc;
    WCHAR *pdst;
    UINT cp;
    INT src_len, dst_len;
    char *src_end;

    TRACE("\n");

    ansi_clipdata = ANDROID_CLIPBOARD_LookupData( CF_TEXT );

    if (ansi_clipdata && ansi_clipdata->data_source == DATA_SOURCE_SYNTHESIZE)
        ansi_clipdata = ANDROID_CLIPBOARD_LookupData( CF_OEMTEXT );

    if (!ansi_clipdata)
    {
        ERR("missing CF_TEXT\n");
        LeaveCriticalSection( &clipdata_section );
        *data = NULL;
        return COMPLETE;
    }

    src_format = ansi_clipdata->format;

    if (!ansi_clipdata->data)
    {
        enum RENDERFORMAT_RESULT ret;
        HANDLE dummy;
        LeaveCriticalSection( &clipdata_section );
        ret = get_clipboard_data( src_format, &dummy, block );
        if (ret == BLOCK) return BLOCK;
        else return RETRY;
    }

    if (ansi_clipdata->format == CF_OEMTEXT)
        cp = CP_OEMCP;
    else
        cp = CP_ACP;

    hsrc = ansi_clipdata->data;
    src_len = GlobalSize( hsrc );
    psrc = GlobalLock( hsrc );

    src_end = memchr( psrc, 0, src_len );
    if (src_end) src_len = src_end - psrc;

    dst_len = MultiByteToWideChar( cp, 0, psrc, src_len, 0, 0 );

    hdst = GlobalAlloc( GMEM_MOVEABLE, (dst_len + 1) * sizeof(WCHAR) );
    if (!hdst)
    {
        GlobalUnlock( hsrc );
        LeaveCriticalSection( &clipdata_section );
        *data = NULL;
        return COMPLETE;
    }

    pdst = GlobalLock( hdst );
    MultiByteToWideChar( cp, 0, psrc, src_len, pdst, dst_len );
    pdst[dst_len] = 0;
    GlobalUnlock( hdst );
    GlobalUnlock( hsrc );

    clip->data = hdst;

    LeaveCriticalSection( &clipdata_section );

    *data = hdst;
    return COMPLETE;
}

/* Caller must hold clipdata_section. clipdata_section is released by this function. */
static enum RENDERFORMAT_RESULT ANDROID_CLIPBOARD_RenderFormat( clipdata* clipdata, HANDLE* data, BOOL block )
{
    switch (clipdata->data_source)
    {
    case DATA_SOURCE_INPROCESS:
        *data = clipdata->data;
        LeaveCriticalSection( &clipdata_section );
        return COMPLETE;
    case DATA_SOURCE_SYNTHESIZE:
        TRACE("%04X synthesize\n", clipdata->format);

        if (clipdata->data)
        {
            *data = clipdata->data;
            LeaveCriticalSection( &clipdata_section );
            return COMPLETE;
        }

        switch (clipdata->format)
        {
        case CF_TEXT:
        case CF_OEMTEXT:
            return ANDROID_CLIPBOARD_SynthesizeAnsiText( clipdata, data, block );
        case CF_UNICODETEXT:
            return ANDROID_CLIPBOARD_SynthesizeUnicodeText( clipdata, data, block );
        }

        FIXME("can't synthesize format %04X", clipdata->format);
        *data = NULL;
        LeaveCriticalSection( &clipdata_section );
        return COMPLETE;
    case DATA_SOURCE_RENDERFORMAT:
    {
        TRACE("%04X renderformat\n", clipdata->format);
        if (block)
        {
            DWORD format = clipdata->format;
            LeaveCriticalSection( &clipdata_section );
            SendMessageW( GetClipboardOwner(), WM_RENDERFORMAT, format, 0 );
            return RETRY;
        }
        else
        {
            clipdata->waiting = TRUE;
            ResetEvent( get_data_update_event() );
            SendNotifyMessageW( GetClipboardOwner(), WM_RENDERFORMAT, clipdata->format, 0 );
            LeaveCriticalSection( &clipdata_section );
            return BLOCK;
        }
    }
    case DATA_SOURCE_DESKTOP:
    {
        HGLOBAL global_data=NULL, handle;
        BOOL pending=FALSE;

        TRACE("%04X desktop\n", clipdata->format);

        if (ioctl_get_clipboard_data( clipdata->format, &global_data, &pending ))
        {
            *data = NULL;
            LeaveCriticalSection( &clipdata_section );
            return COMPLETE;
        }
        if (pending) {
            LeaveCriticalSection( &clipdata_section );
            return BLOCK;
        }
        if (global_data)
        {
            handle = ANDROID_CLIPBOARD_DeserializeHGlobal( clipdata->format, global_data );

            ANDROID_CLIPBOARD_InsertData( clipdata->format, handle, DATA_SOURCE_INPROCESS );
        }
        *data = clipdata->data;
        LeaveCriticalSection( &clipdata_section );
        return COMPLETE;
    }
    case DATA_SOURCE_JAVA:
        TRACE("%04X java\n", clipdata->format);
        /* Can't call ioctl_render_clipboard_data here because we may be in an ioctl handler */
        SendNotifyMessageW( GetDesktopWindow(), WM_ANDROID_RENDERFORMAT, clipdata->android_format, 0 );
        clipdata->waiting = TRUE;
        ResetEvent( get_data_update_event() );
        LeaveCriticalSection( &clipdata_section );
        return BLOCK;
    }

    *data = NULL;
    LeaveCriticalSection( &clipdata_section );
    return COMPLETE;
}

/* Get clipboard data. Returns FALSE if blocking is required.
 * If may_block is TRUE, this function will eventually return TRUE.
 * clipdata_section should not be held when may_block is TRUE. */
BOOL get_clipboard_data( UINT wFormat, HANDLE* data, BOOL may_block )
{
    clipdata *clipdata;

    TRACE("(%04X)\n", wFormat);

retry:

    ANDROID_CLIPBOARD_UpdateCache();

    EnterCriticalSection( &clipdata_section );

    if ((clipdata = ANDROID_CLIPBOARD_LookupData( wFormat )))
    {
        enum RENDERFORMAT_RESULT res = ANDROID_CLIPBOARD_RenderFormat( clipdata, data, may_block );
        /* ANDROID_CLIPBOARD_RenderFormat leaves critical section. */

        switch (res)
        {
        case COMPLETE:
            TRACE("returning %p (type %04X)\n", *data, wFormat);
            return TRUE;
        case BLOCK:
            TRACE("pending (type %04X) %i\n", wFormat, may_block);
            if (may_block)
            {
                WaitForSingleObject( get_data_update_event(), INFINITE );
                goto retry;
            }
            return FALSE;
        case RETRY:
            TRACE("retry (type %04X)\n", wFormat);
            goto retry;
        }
    }
    else
        LeaveCriticalSection( &clipdata_section );

    TRACE("returning NULL (type %04x)\n", wFormat);
    *data = NULL;
    return TRUE;
}

NTSTATUS handle_ioctl_get_clipboard_data( UINT format, BOOL* format_present, BOOL* pending, BYTE* data, DWORD* size )
{
    HANDLE handle;
    DWORD out_size = *size;

    if (!get_clipboard_data( format, &handle, FALSE ))
    {
        *format_present = TRUE;
        *size = 0;
        *pending = TRUE;
        return STATUS_SUCCESS;
    }

    if (!handle)
    {
        *format_present = FALSE;
        *size = 0;
        *pending = FALSE;
        return STATUS_SUCCESS;
    }

    *format_present = TRUE;
    *size = ANDROID_CLIPBOARD_GetSerializedSize( format, handle );
    *pending = FALSE;

    if (out_size >= *size)
    {
        ANDROID_CLIPBOARD_SerializeHandle( format, handle, data, out_size );
        return STATUS_SUCCESS;
    }
    else if (out_size == 0)
        return STATUS_SUCCESS;
    else
        return STATUS_BUFFER_OVERFLOW;
}

void handle_ioctl_empty_clipboard( void )
{
    TRACE("\n");

    ANDROID_CLIPBOARD_EmptyClipData();
}

void handle_ioctl_set_clipboard_data( UINT format, BOOL format_present, BYTE* data, DWORD size )
{
    HANDLE hglobal, handle;
    void* lock;

    TRACE("%04X, %i\n", format, format_present);

    if (!format_present)
    {
        ANDROID_CLIPBOARD_InsertData( format, NULL, DATA_SOURCE_RENDERFORMAT );
        return;
    }

    hglobal = GlobalAlloc( GMEM_MOVEABLE, size );

    if (!hglobal)
        return;

    lock = GlobalLock( hglobal );
    memcpy( lock, data, size );
    GlobalUnlock( hglobal );

    handle = ANDROID_CLIPBOARD_DeserializeHGlobal( format, hglobal );

    ANDROID_CLIPBOARD_InsertData( format, handle, DATA_SOURCE_INPROCESS );
}

void handle_ioctl_end_clipboard_update( void )
{
    TRACE("\n");

    clipdata_seqno = GetClipboardSequenceNumber();

    ANDROID_CLIPBOARD_SynthesizeFormats();
}

HANDLE CDECL ANDROID_GetClipboardData( UINT wFormat )
{
    HANDLE data;

    get_clipboard_data( wFormat, &data, TRUE );

    return data;
}

BOOL CDECL ANDROID_SetClipboardData( UINT wFormat, HANDLE hData, BOOL owner )
{
    clipdata* clipdata=NULL;
    BOOL res=TRUE;

    TRACE("%04X, %p\n", wFormat, hData);

    ANDROID_CLIPBOARD_UpdateCache();

    EnterCriticalSection( &clipdata_section );

    /* FIXME: Should fail if !owner and non-NULL data was previously set by the owner. */

    if (res)
    {
        clipdata = ANDROID_CLIPBOARD_InsertData( wFormat, hData,
            hData ? DATA_SOURCE_INPROCESS : DATA_SOURCE_RENDERFORMAT );
        res = (clipdata != NULL);
    }

    LeaveCriticalSection( &clipdata_section );

    if (res && ANDROID_CLIPBOARD_ShouldExportData( clipdata ) && !is_desktop_process())
    {
        BYTE* buffer=NULL;
        DWORD size=0;

        if (hData)
        {
            size = ANDROID_CLIPBOARD_GetSerializedSize( wFormat, hData );

            if (size)
            {
                buffer = HeapAlloc( GetProcessHeap(), 0, size );
                if (buffer)
                    ANDROID_CLIPBOARD_SerializeHandle( wFormat, hData, buffer, size );
                else
                    res = FALSE;
            }
        }

        if (res)
            ioctl_set_clipboard_data( wFormat, hData != NULL, buffer, size );

        HeapFree( GetProcessHeap(), 0, buffer );
    }

    return res;
}
