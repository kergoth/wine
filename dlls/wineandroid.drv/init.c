/*
 * Android driver initialisation functions
 *
 * Copyright 1996, 2013 Alexandre Julliard
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

#define NONAMELESSSTRUCT
#define NONAMELESSUNION
#include "config.h"
#include "wine/port.h"

#include <stdarg.h>
#include <string.h>
#include <SLES/OpenSLES.h>

#include "windef.h"
#include "winbase.h"
#include "winreg.h"
#include "android.h"
#include "wine/server.h"
#include "wine/library.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(android);

unsigned int screen_width = 0;
unsigned int screen_height = 0;
unsigned int screen_bpp = 32;
unsigned int screen_dpi = 0;
RECT virtual_screen_rect = { 0, 0, 0, 0 };

MONITORINFOEXW default_monitor =
{
    sizeof(default_monitor),    /* cbSize */
    { 0, 0, 0, 0 },             /* rcMonitor */
    { 0, 0, 0, 0 },             /* rcWork */
    MONITORINFOF_PRIMARY,       /* dwFlags */
    { '\\','\\','.','\\','D','I','S','P','L','A','Y','1',0 }   /* szDevice */
};

static int device_init_done;

static const WCHAR dpi_key_name[] = {'S','o','f','t','w','a','r','e','\\','F','o','n','t','s','\0'};
static const WCHAR dpi_value_name[] = {'L','o','g','P','i','x','e','l','s','\0'};

static const struct gdi_dc_funcs android_drv_funcs;

DWORD thread_data_tls_index = TLS_OUT_OF_INDEXES;
static JavaVM *java_vm;


/******************************************************************************
 *      set_screen_dpi
 */
void set_screen_dpi( DWORD dpi, BOOL force )
{
    HKEY hkey;

    if (!dpi) return;
    if (screen_dpi) return;  /* already set */

    TRACE( "setting to %u force %d\n", dpi, force );
    if (!force && RegOpenKeyW(HKEY_CURRENT_CONFIG, dpi_key_name, &hkey) == ERROR_SUCCESS)
    {
        DWORD tmp, type, size = sizeof(tmp);
        if (RegQueryValueExW(hkey, dpi_value_name, NULL, &type, (void *)&tmp, &size) == ERROR_SUCCESS)
        {
            if (type == REG_DWORD)
            {
                RegCloseKey(hkey);
                goto exit;
            }
        }
        RegCloseKey(hkey);
    }
    if (!RegCreateKeyW( HKEY_CURRENT_CONFIG, dpi_key_name, &hkey ))
    {
        RegSetValueExW( hkey, dpi_value_name, 0, REG_DWORD, (void *)&dpi, sizeof(DWORD) );
        RegCloseKey( hkey );
    }
 exit:
    screen_dpi = dpi;
}

/******************************************************************************
 *      get_dpi
 *
 * get the dpi from the registry
 */
static DWORD get_dpi(void)
{
    HKEY hkey;
    DWORD dpi = 0;

    if (RegOpenKeyW(HKEY_CURRENT_CONFIG, dpi_key_name, &hkey) == ERROR_SUCCESS)
    {
        DWORD type, size = sizeof(dpi);
        if (RegQueryValueExW(hkey, dpi_value_name, NULL, &type, (void *)&dpi, &size) == ERROR_SUCCESS)
        {
            if (type != REG_DWORD) dpi = 0;
        }
        RegCloseKey(hkey);
    }
    if (!dpi) dpi = 96;
    return dpi;
}

/******************************************************************************
 *      init_monitors
 */
void init_monitors( int width, int height )
{
    static const WCHAR trayW[] = {'S','h','e','l','l','_','T','r','a','y','W','n','d',0};
    RECT rect;
    HWND hwnd = FindWindowW( trayW, NULL );

    virtual_screen_rect.right = width;
    virtual_screen_rect.bottom = height;
    default_monitor.rcMonitor = default_monitor.rcWork = virtual_screen_rect;

    if (!hwnd || !IsWindowVisible( hwnd )) return;
    if (!GetWindowRect( hwnd, &rect )) return;
    if (rect.top) default_monitor.rcWork.bottom = rect.top;
    else default_monitor.rcWork.top = rect.bottom;
    TRACE( "found tray %p %s work area %s\n", hwnd,
           wine_dbgstr_rect( &rect ), wine_dbgstr_rect( &default_monitor.rcWork ));
}

void handle_run_cmdline( LPWSTR cmdline, LPWSTR* wineEnv )
{
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    UNICODE_STRING var, val;
    WCHAR *env = NULL;

    ZeroMemory( &si, sizeof(STARTUPINFOW) );
    TRACE( "Running windows cmd: : %s\n", debugstr_w( cmdline ) );

    if (wineEnv && !RtlCreateEnvironment( TRUE, &env ))
    {
        while (*wineEnv)
        {
            RtlInitUnicodeString( &var, *wineEnv++ );
            RtlInitUnicodeString( &val, *wineEnv++ );
            RtlSetEnvironmentVariable( &env, &var, &val );
        }
    }

    if (!CreateProcessW( NULL, cmdline, NULL, NULL, FALSE,
                         DETACHED_PROCESS | CREATE_UNICODE_ENVIRONMENT, env, NULL, &si, &pi ))
        ERR( "Failed to run cmd : Error %d\n", GetLastError() );

    if (env) RtlDestroyEnvironment( env );
}

static void fetch_display_metrics(void)
{
    if (java_vm) return;  /* for Java threads it will be set when the surface is created */

    SERVER_START_REQ( get_window_rectangles )
    {
        req->handle = wine_server_user_handle( GetDesktopWindow() );
        req->relative = COORDS_CLIENT;
        if (!wine_server_call( req ))
        {
            screen_width  = reply->window.right;
            screen_height = reply->window.bottom;
        }
    }
    SERVER_END_REQ;

    init_monitors( screen_width, screen_height );
    screen_dpi = get_dpi();
    TRACE( "%ux%u %u dpi\n", screen_width, screen_height, screen_dpi );
}

/**********************************************************************
 *	     device_init
 *
 * Perform initializations needed upon creation of the first device.
 */
static void device_init(void)
{
    device_init_done = TRUE;
    fetch_display_metrics();
}


static ANDROID_PDEVICE *create_android_physdev(void)
{
    ANDROID_PDEVICE *physdev;

    if (!device_init_done) device_init();

    if (!(physdev = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*physdev) ))) return NULL;
    return physdev;
}

/**********************************************************************
 *	     ANDROID_CreateDC
 */
static BOOL ANDROID_CreateDC( PHYSDEV *pdev, LPCWSTR driver, LPCWSTR device,
                              LPCWSTR output, const DEVMODEW* initData )
{
    ANDROID_PDEVICE *physdev = create_android_physdev();

    if (!physdev) return FALSE;

    push_dc_driver( pdev, &physdev->dev, &android_drv_funcs );
    return TRUE;
}


/**********************************************************************
 *	     ANDROID_CreateCompatibleDC
 */
static BOOL ANDROID_CreateCompatibleDC( PHYSDEV orig, PHYSDEV *pdev )
{
    ANDROID_PDEVICE *physdev = create_android_physdev();

    if (!physdev) return FALSE;

    push_dc_driver( pdev, &physdev->dev, &android_drv_funcs );
    return TRUE;
}


/**********************************************************************
 *	     ANDROID_DeleteDC
 */
static BOOL ANDROID_DeleteDC( PHYSDEV dev )
{
    ANDROID_PDEVICE *physdev = get_android_dev( dev );

    HeapFree( GetProcessHeap(), 0, physdev );
    return TRUE;
}


/***********************************************************************
 *           GetDeviceCaps
 */
static INT ANDROID_GetDeviceCaps( PHYSDEV dev, INT cap )
{
    switch(cap)
    {
    case DRIVERVERSION:
        return 0x300;
    case TECHNOLOGY:
        return DT_RASDISPLAY;
    case HORZSIZE:
        return MulDiv( screen_width, 254, screen_dpi * 10 );
    case VERTSIZE:
        return MulDiv( screen_height, 254, screen_dpi * 10 );
    case HORZRES:
        return screen_width;
    case VERTRES:
        return screen_height;
    case DESKTOPHORZRES:
        return virtual_screen_rect.right - virtual_screen_rect.left;
    case DESKTOPVERTRES:
        return virtual_screen_rect.bottom - virtual_screen_rect.top;
    case BITSPIXEL:
        return screen_bpp;
    case PLANES:
        return 1;
    case NUMBRUSHES:
        return -1;
    case NUMPENS:
        return -1;
    case NUMMARKERS:
        return 0;
    case NUMFONTS:
        return 0;
    case NUMCOLORS:
        /* MSDN: Number of entries in the device's color table, if the device has
         * a color depth of no more than 8 bits per pixel.For devices with greater
         * color depths, -1 is returned. */
        return -1;
    case PDEVICESIZE:
        return sizeof(ANDROID_PDEVICE);
    case CURVECAPS:
        return (CC_CIRCLES | CC_PIE | CC_CHORD | CC_ELLIPSES | CC_WIDE |
                CC_STYLED | CC_WIDESTYLED | CC_INTERIORS | CC_ROUNDRECT);
    case LINECAPS:
        return (LC_POLYLINE | LC_MARKER | LC_POLYMARKER | LC_WIDE |
                LC_STYLED | LC_WIDESTYLED | LC_INTERIORS);
    case POLYGONALCAPS:
        return (PC_POLYGON | PC_RECTANGLE | PC_WINDPOLYGON | PC_SCANLINE |
                PC_WIDE | PC_STYLED | PC_WIDESTYLED | PC_INTERIORS);
    case TEXTCAPS:
        return (TC_OP_CHARACTER | TC_OP_STROKE | TC_CP_STROKE |
                TC_CR_ANY | TC_SF_X_YINDEP | TC_SA_DOUBLE | TC_SA_INTEGER |
                TC_SA_CONTIN | TC_UA_ABLE | TC_SO_ABLE | TC_RA_ABLE | TC_VA_ABLE);
    case CLIPCAPS:
        return CP_REGION;
    case COLORRES:
        /* The observed correspondence between BITSPIXEL and COLORRES is:
         * BITSPIXEL: 8  -> COLORRES: 18
         * BITSPIXEL: 16 -> COLORRES: 16
         * BITSPIXEL: 24 -> COLORRES: 24
         * BITSPIXEL: 32 -> COLORRES: 24 */
        return (screen_bpp <= 8) ? 18 : min( 24, screen_bpp );
    case RASTERCAPS:
        return (RC_BITBLT | RC_BANDING | RC_SCALING | RC_BITMAP64 | RC_DI_BITMAP |
                RC_DIBTODEV | RC_BIGFONT | RC_STRETCHBLT | RC_STRETCHDIB | RC_DEVBITS);
    case SHADEBLENDCAPS:
        return (SB_GRAD_RECT | SB_GRAD_TRI | SB_CONST_ALPHA | SB_PIXEL_ALPHA);
    case ASPECTX:
    case ASPECTY:
        return 36;
    case ASPECTXY:
        return 51;
    case LOGPIXELSX:
        return screen_dpi;
    case LOGPIXELSY:
        return screen_dpi;
    case CAPS1:
        FIXME("(%p): CAPS1 is unimplemented, will return 0\n", dev->hdc );
        /* please see wingdi.h for the possible bit-flag values that need
           to be returned. */
        return 0;
    case SIZEPALETTE:
        return 0;
    case NUMRESERVED:
    case PHYSICALWIDTH:
    case PHYSICALHEIGHT:
    case PHYSICALOFFSETX:
    case PHYSICALOFFSETY:
    case SCALINGFACTORX:
    case SCALINGFACTORY:
    case VREFRESH:
    case BLTALIGNMENT:
        return 0;
    default:
        FIXME("(%p): unsupported capability %d, will return 0\n", dev->hdc, cap );
        return 0;
    }
}


/***********************************************************************
 *		ANDROID_ChangeDisplaySettingsEx
 */
LONG CDECL ANDROID_ChangeDisplaySettingsEx( LPCWSTR devname, LPDEVMODEW devmode,
                                            HWND hwnd, DWORD flags, LPVOID lpvoid )
{
    FIXME( "(%s,%p,%p,0x%08x,%p)\n", debugstr_w( devname ), devmode, hwnd, flags, lpvoid );
    return DISP_CHANGE_SUCCESSFUL;
}


/***********************************************************************
 *		ANDROID_GetMonitorInfo
 */
BOOL CDECL ANDROID_GetMonitorInfo( HMONITOR handle, LPMONITORINFO info )
{
    if (handle != (HMONITOR)1)
    {
        SetLastError( ERROR_INVALID_HANDLE );
        return FALSE;
    }
    info->rcMonitor = default_monitor.rcMonitor;
    info->rcWork = default_monitor.rcWork;
    info->dwFlags = default_monitor.dwFlags;
    if (info->cbSize >= sizeof(MONITORINFOEXW))
        lstrcpyW( ((MONITORINFOEXW *)info)->szDevice, default_monitor.szDevice );
    return TRUE;
}


/***********************************************************************
 *		ANDROID_EnumDisplayMonitors
 */
BOOL CDECL ANDROID_EnumDisplayMonitors( HDC hdc, LPRECT rect, MONITORENUMPROC proc, LPARAM lp )
{
    if (hdc)
    {
        POINT origin;
        RECT limit, monrect;

        if (!GetDCOrgEx( hdc, &origin )) return FALSE;
        if (GetClipBox( hdc, &limit ) == ERROR) return FALSE;

        if (rect && !IntersectRect( &limit, &limit, rect )) return TRUE;

        monrect = default_monitor.rcMonitor;
        OffsetRect( &monrect, -origin.x, -origin.y );
        if (IntersectRect( &monrect, &monrect, &limit ))
            if (!proc( (HMONITOR)1, hdc, &monrect, lp ))
                return FALSE;
    }
    else
    {
        RECT unused;
        if (!rect || IntersectRect( &unused, &default_monitor.rcMonitor, rect ))
            if (!proc( (HMONITOR)1, 0, &default_monitor.rcMonitor, lp ))
                return FALSE;
    }
    return TRUE;
}


/***********************************************************************
 *		ANDROID_EnumDisplaySettingsEx
 */
BOOL CDECL ANDROID_EnumDisplaySettingsEx( LPCWSTR name, DWORD n, LPDEVMODEW devmode, DWORD flags)
{
    static const WCHAR dev_name[CCHDEVICENAME] =
        { 'W','i','n','e',' ','A','n','d','r','o','i','d',' ','d','r','i','v','e','r',0 };

    devmode->dmSize = offsetof( DEVMODEW, dmICMMethod );
    devmode->dmSpecVersion = DM_SPECVERSION;
    devmode->dmDriverVersion = DM_SPECVERSION;
    memcpy( devmode->dmDeviceName, dev_name, sizeof(dev_name) );
    devmode->dmDriverExtra = 0;
    devmode->u2.dmDisplayFlags = 0;
    devmode->dmDisplayFrequency = 0;
    devmode->u1.s2.dmPosition.x = 0;
    devmode->u1.s2.dmPosition.y = 0;
    devmode->u1.s2.dmDisplayOrientation = 0;
    devmode->u1.s2.dmDisplayFixedOutput = 0;

    if (n == ENUM_CURRENT_SETTINGS)
    {
        n = 0;
    }
    if (n == ENUM_REGISTRY_SETTINGS)
    {
        n = 0;
    }
    if (n == 0)
    {
        devmode->dmPelsWidth = screen_width;
        devmode->dmPelsHeight = screen_height;
        devmode->dmBitsPerPel = screen_bpp;
        devmode->dmDisplayFrequency = 60;
        devmode->dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL | DM_DISPLAYFLAGS;
        if (devmode->dmDisplayFrequency)
        {
            devmode->dmFields |= DM_DISPLAYFREQUENCY;
            TRACE("mode %d -- %dx%dx%dbpp @%d Hz\n", n,
                  devmode->dmPelsWidth, devmode->dmPelsHeight, devmode->dmBitsPerPel,
                  devmode->dmDisplayFrequency);
        }
        else
        {
            TRACE("mode %d -- %dx%dx%dbpp\n", n,
                  devmode->dmPelsWidth, devmode->dmPelsHeight, devmode->dmBitsPerPel);
        }
        return TRUE;
    }
    TRACE("mode %d -- not present\n", n);
    SetLastError(ERROR_NO_MORE_FILES);
    return FALSE;
}


/**********************************************************************
 *           ANDROID_wine_get_wgl_driver
 */
static struct opengl_funcs * ANDROID_wine_get_wgl_driver( PHYSDEV dev, UINT version )
{
    struct opengl_funcs *ret;

    if (!(ret = get_wgl_driver( version )))
    {
        dev = GET_NEXT_PHYSDEV( dev, wine_get_wgl_driver );
        ret = dev->funcs->wine_get_wgl_driver( dev, version );
    }
    return ret;
}


static const struct gdi_dc_funcs android_drv_funcs =
{
    NULL,                               /* pAbortDoc */
    NULL,                               /* pAbortPath */
    NULL,                               /* pAlphaBlend */
    NULL,                               /* pAngleArc */
    NULL,                               /* pArc */
    NULL,                               /* pArcTo */
    NULL,                               /* pBeginPath */
    NULL,                               /* pBlendImage */
    NULL,                               /* pChord */
    NULL,                               /* pCloseFigure */
    ANDROID_CreateCompatibleDC,         /* pCreateCompatibleDC */
    ANDROID_CreateDC,                   /* pCreateDC */
    ANDROID_DeleteDC,                   /* pDeleteDC */
    NULL,                               /* pDeleteObject */
    NULL,                               /* pDeviceCapabilities */
    NULL,                               /* pEllipse */
    NULL,                               /* pEndDoc */
    NULL,                               /* pEndPage */
    NULL,                               /* pEndPath */
    NULL,                               /* pEnumFonts */
    NULL,                               /* pEnumICMProfiles */
    NULL,                               /* pExcludeClipRect */
    NULL,                               /* pExtDeviceMode */
    NULL,                               /* pExtEscape */
    NULL,                               /* pExtFloodFill */
    NULL,                               /* pExtSelectClipRgn */
    NULL,                               /* pExtTextOut */
    NULL,                               /* pFillPath */
    NULL,                               /* pFillRgn */
    NULL,                               /* pFlattenPath */
    NULL,                               /* pFontIsLinked */
    NULL,                               /* pFrameRgn */
    NULL,                               /* pGdiComment */
    NULL,                               /* pGetBoundsRect */
    NULL,                               /* pGetCharABCWidths */
    NULL,                               /* pGetCharABCWidthsI */
    NULL,                               /* pGetCharWidth */
    ANDROID_GetDeviceCaps,              /* pGetDeviceCaps */
    NULL,                               /* pGetDeviceGammaRamp */
    NULL,                               /* pGetFontData */
    NULL,                               /* pGetFontRealizationInfo */
    NULL,                               /* pGetFontUnicodeRanges */
    NULL,                               /* pGetGlyphIndices */
    NULL,                               /* pGetGlyphOutline */
    NULL,                               /* pGetICMProfile */
    NULL,                               /* pGetImage */
    NULL,                               /* pGetKerningPairs */
    NULL,                               /* pGetNearestColor */
    NULL,                               /* pGetOutlineTextMetrics */
    NULL,                               /* pGetPixel */
    NULL,                               /* pGetSystemPaletteEntries */
    NULL,                               /* pGetTextCharsetInfo */
    NULL,                               /* pGetTextExtentExPoint */
    NULL,                               /* pGetTextExtentExPointI */
    NULL,                               /* pGetTextFace */
    NULL,                               /* pGetTextMetrics */
    NULL,                               /* pGradientFill */
    NULL,                               /* pIntersectClipRect */
    NULL,                               /* pInvertRgn */
    NULL,                               /* pLineTo */
    NULL,                               /* pModifyWorldTransform */
    NULL,                               /* pMoveTo */
    NULL,                               /* pOffsetClipRgn */
    NULL,                               /* pOffsetViewportOrg */
    NULL,                               /* pOffsetWindowOrg */
    NULL,                               /* pPaintRgn */
    NULL,                               /* pPatBlt */
    NULL,                               /* pPie */
    NULL,                               /* pPolyBezier */
    NULL,                               /* pPolyBezierTo */
    NULL,                               /* pPolyDraw */
    NULL,                               /* pPolyPolygon */
    NULL,                               /* pPolyPolyline */
    NULL,                               /* pPolygon */
    NULL,                               /* pPolyline */
    NULL,                               /* pPolylineTo */
    NULL,                               /* pPutImage */
    NULL,                               /* pRealizeDefaultPalette */
    NULL,                               /* pRealizePalette */
    NULL,                               /* pRectangle */
    NULL,                               /* pResetDC */
    NULL,                               /* pRestoreDC */
    NULL,                               /* pRoundRect */
    NULL,                               /* pSaveDC */
    NULL,                               /* pScaleViewportExt */
    NULL,                               /* pScaleWindowExt */
    NULL,                               /* pSelectBitmap */
    NULL,                               /* pSelectBrush */
    NULL,                               /* pSelectClipPath */
    NULL,                               /* pSelectFont */
    NULL,                               /* pSelectPalette */
    NULL,                               /* pSelectPen */
    NULL,                               /* pSetArcDirection */
    NULL,                               /* pSetBkColor */
    NULL,                               /* pSetBkMode */
    NULL,                               /* pSetBoundsRect */
    NULL,                               /* pSetDCBrushColor */
    NULL,                               /* pSetDCPenColor */
    NULL,                               /* pSetDIBitsToDevice */
    NULL,                               /* pSetDeviceClipping */
    NULL,                               /* pSetDeviceGammaRamp */
    NULL,                               /* pSetLayout */
    NULL,                               /* pSetMapMode */
    NULL,                               /* pSetMapperFlags */
    NULL,                               /* pSetPixel */
    NULL,                               /* pSetPolyFillMode */
    NULL,                               /* pSetROP2 */
    NULL,                               /* pSetRelAbs */
    NULL,                               /* pSetStretchBltMode */
    NULL,                               /* pSetTextAlign */
    NULL,                               /* pSetTextCharacterExtra */
    NULL,                               /* pSetTextColor */
    NULL,                               /* pSetTextJustification */
    NULL,                               /* pSetViewportExt */
    NULL,                               /* pSetViewportOrg */
    NULL,                               /* pSetWindowExt */
    NULL,                               /* pSetWindowOrg */
    NULL,                               /* pSetWorldTransform */
    NULL,                               /* pStartDoc */
    NULL,                               /* pStartPage */
    NULL,                               /* pStretchBlt */
    NULL,                               /* pStretchDIBits */
    NULL,                               /* pStrokeAndFillPath */
    NULL,                               /* pStrokePath */
    NULL,                               /* pUnrealizePalette */
    NULL,                               /* pWidenPath */
    ANDROID_wine_get_wgl_driver,        /* wine_get_wgl_driver */
    GDI_PRIORITY_GRAPHICS_DRV           /* priority */
};


/******************************************************************************
 *      ANDROID_get_gdi_driver
 */
const struct gdi_dc_funcs * CDECL ANDROID_get_gdi_driver( unsigned int version )
{
    if (version != WINE_GDI_DRIVER_VERSION)
    {
        ERR( "version mismatch, gdi32 wants %u but winex11 has %u\n", version, WINE_GDI_DRIVER_VERSION );
        return NULL;
    }
    return &android_drv_funcs;
}

static int create_event_pipe( int fd[2] )
{
    int ret;
#ifdef HAVE_PIPE2
    if (!(ret = pipe2( fd, O_CLOEXEC | O_NONBLOCK ))) return ret;
#endif
    if (!(ret = pipe( fd )))
    {
        fcntl( fd[0], F_SETFD, FD_CLOEXEC );
        fcntl( fd[1], F_SETFD, FD_CLOEXEC );
        fcntl( fd[0], F_SETFL, O_NONBLOCK );
        fcntl( fd[1], F_SETFL, O_NONBLOCK );
    }
    return ret;
}

static void set_queue_event_fd( int fd )
{
    HANDLE handle;
    int ret;

    if (wine_server_fd_to_handle( fd, GENERIC_READ | SYNCHRONIZE, 0, &handle ))
    {
        ERR( "Can't allocate handle for event fd\n" );
        ExitProcess(1);
    }
    SERVER_START_REQ( set_queue_fd )
    {
        req->handle = wine_server_obj_handle( handle );
        ret = wine_server_call( req );
    }
    SERVER_END_REQ;
    if (ret)
    {
        ERR( "Can't store handle for event fd %x\n", ret );
        ExitProcess(1);
    }
    CloseHandle( handle );
}

struct android_thread_data *android_init_thread_data(void)
{
    struct android_thread_data *data = android_thread_data();

    if (data) return data;

    if (!(data = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*data) )) ||
        create_event_pipe( data->event_pipe ) == -1)
    {
        ERR( "could not create data\n" );
        ExitProcess(1);
    }
    TlsSetValue( thread_data_tls_index, data );
    set_queue_event_fd( data->event_pipe[0] );
    list_init( &data->event_queue );

    return data;
}

static void thread_detach(void)
{
    struct android_thread_data *data = TlsGetValue( thread_data_tls_index );

    if (data)
    {
        close( data->event_pipe[0] );
        close( data->event_pipe[1] );
        HeapFree( GetProcessHeap(), 0, data );
    }
}

static const JNINativeMethod methods[] =
{
    { "wine_keyboard_event", "(IIIII)Z", keyboard_event },
    { "wine_clear_meta_key_states", "(I)V", clear_meta_key_states },
    { "wine_motion_event", "(IIIIII)Z", motion_event },
    { "wine_surface_changed", "(ILandroid/view/Surface;)V", surface_changed },
    { "wine_desktop_changed", "(II)V", desktop_changed },
    { "wine_config_changed", "(IZ)V", config_changed },
    { "wine_clipboard_changed", "([Z)V", clipboard_changed },
    { "wine_import_clipboard_data", "(I[B)V", import_clipboard_data },
    { "wine_clipboard_request", "(I)V", clipboard_request },
    { "wine_ime_settext", "(Ljava/lang/String;II)V", ime_text},
    { "wine_ime_finishtext", "()V", ime_finish},
    { "wine_ime_canceltext", "()V", ime_cancel},
    { "wine_ime_start", "()V", ime_start},

    { "wine_send_gamepad_count", "(I)V", gamepad_count},
    { "wine_send_gamepad_data", "(IILjava/lang/String;)V", gamepad_data},
    { "wine_send_gamepad_axis", "(I[F)V", gamepad_sendaxis},
    { "wine_send_gamepad_button", "(III)V", gamepad_sendbutton},

    { "wine_run_commandline", "(Ljava/lang/String;[Ljava/lang/String;)V", run_commandline }
};

#define DECL_FUNCPTR(f) typeof(f) * p##f = NULL
#define LOAD_FUNCPTR(lib, func) do { \
    if ((p##func = wine_dlsym( lib, #func, NULL, 0 )) == NULL) \
        { ERR( "can't find symbol %s\n", #func); return; } \
    } while(0)

DECL_FUNCPTR( __android_log_print );
DECL_FUNCPTR( ANativeWindow_fromSurface );
DECL_FUNCPTR( ANativeWindow_release );
DECL_FUNCPTR( hw_get_module );
DECL_FUNCPTR( slCreateEngine );
DECL_FUNCPTR( SL_IID_ANDROIDSIMPLEBUFFERQUEUE );
DECL_FUNCPTR( SL_IID_ENGINE );
DECL_FUNCPTR( SL_IID_PLAY );
DECL_FUNCPTR( SL_IID_PLAYBACKRATE );
DECL_FUNCPTR( SL_IID_RECORD );

void run_commandline( JNIEnv *env, jobject obj, jobject _cmdline, jobjectArray _wineEnv )
{
    union event_data data;
    const jchar* cmdline = (*env)->GetStringChars(env, _cmdline, 0);
    jsize len = (*env)->GetStringLength( env, _cmdline );
    int j;
    
    memset( &data, 0, sizeof(data) );
    data.type = RUN_CMDLINE;
    data.runcmd.cmdline = malloc( sizeof(WCHAR) * (len + 1) );
    lstrcpynW( data.runcmd.cmdline, (WCHAR*)cmdline, len + 1 );
    (*env)->ReleaseStringChars(env, _cmdline, cmdline);

    if (_wineEnv)
    {
        int count = (*env)->GetArrayLength( env, _wineEnv );

        data.runcmd.env = malloc( sizeof(LPWSTR*) * (count + 1) );
        for (j = 0; j < count; j++)
        {
            jobject s = (*env)->GetObjectArrayElement( env, _wineEnv, j );
            const jchar *key_val_str = (*env)->GetStringChars( env, s, NULL );
            len = (*env)->GetStringLength( env, s );
            data.runcmd.env[j] = malloc( sizeof(WCHAR) * (len + 1) );
            lstrcpynW( data.runcmd.env[j], (WCHAR*)key_val_str, len + 1 );
            (*env)->ReleaseStringChars( env, s, key_val_str );
        }
        data.runcmd.env[j] = 0;
    }

    send_event( desktop_thread, &data );
}

struct gralloc_module_t *gralloc_module = NULL;

static void load_hardware_libs(void)
{
    const struct hw_module_t *module;
    void *libhardware;
    char error[256];

    if (!(libhardware = wine_dlopen( "libhardware.so", RTLD_GLOBAL, error, sizeof(error) )))
    {
        ERR( "failed to load libhardware.so: %s\n", error );
        return;
    }
    LOAD_FUNCPTR( libhardware, hw_get_module );

    if (phw_get_module( GRALLOC_HARDWARE_MODULE_ID, &module ) == 0)
        gralloc_module = (struct gralloc_module_t *)module;
    else
        ERR( "failed to load gralloc module\n" );
}

static void load_android_libs(void)
{
    void *libandroid, *liblog;
    char error[1024];

    if (!(libandroid = wine_dlopen( "libandroid.so", RTLD_GLOBAL, error, sizeof(error) )))
    {
        ERR( "failed to load libandroid.so: %s\n", error );
        return;
    }
    if (!(liblog = wine_dlopen( "liblog.so", RTLD_GLOBAL, error, sizeof(error) )))
    {
        ERR( "failed to load liblog.so: %s\n", error );
        return;
    }
    LOAD_FUNCPTR( liblog, __android_log_print );
    LOAD_FUNCPTR( libandroid, ANativeWindow_fromSurface );
    LOAD_FUNCPTR( libandroid, ANativeWindow_release );
}

static void load_opensles_libs(void)
{
    void *libopensles;
    char error[1024];

    if (!(libopensles = wine_dlopen( "libOpenSLES.so", RTLD_GLOBAL, error, sizeof(error) )))
    {
        ERR( "failed to load libOpenSLES.so: %s\n", error );
        return;
    }
    LOAD_FUNCPTR( libopensles, slCreateEngine );
    LOAD_FUNCPTR( libopensles, SL_IID_ANDROIDSIMPLEBUFFERQUEUE );
    LOAD_FUNCPTR( libopensles, SL_IID_ENGINE );
    LOAD_FUNCPTR( libopensles, SL_IID_PLAY );
    LOAD_FUNCPTR( libopensles, SL_IID_PLAYBACKRATE );
    LOAD_FUNCPTR( libopensles, SL_IID_RECORD );
}

#undef DECL_FUNCPTR
#undef LOAD_FUNCPTR

static BOOL process_attach(void)
{
    jclass class;
    jobject object = wine_get_java_object();
    JNIEnv *jni_env;

    if ((thread_data_tls_index = TlsAlloc()) == TLS_OUT_OF_INDEXES) return FALSE;

    load_hardware_libs();

    load_opensles_libs();
    g_timer_q = CreateTimerQueue();

    if ((java_vm = wine_get_java_vm()))  /* running under Java */
    {
#ifdef __i386__
        WORD old_fs = wine_get_fs();
#endif

        load_android_libs();
        (*java_vm)->AttachCurrentThread( java_vm, &jni_env, 0 );
        class = (*jni_env)->GetObjectClass( jni_env, object );
        (*jni_env)->RegisterNatives( jni_env, class, methods, sizeof(methods)/sizeof(methods[0]) );
        (*jni_env)->DeleteLocalRef( jni_env, class );

#ifdef __i386__
        wine_set_fs( old_fs );  /* the Java VM hijacks %fs for its own purposes, restore it */
#endif
    }
    return TRUE;
}

/***********************************************************************
 *       dll initialisation routine
 */
BOOL WINAPI DllMain( HINSTANCE inst, DWORD reason, LPVOID reserved )
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        return process_attach();
    case DLL_THREAD_DETACH:
        thread_detach();
        break;
    }
    return TRUE;
}

