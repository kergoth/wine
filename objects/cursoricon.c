/*
 * Cursor and icon support
 *
 * Copyright 1995 Alexandre Julliard
 */

/*
 * Theory:
 *
 * Cursors and icons are stored in a global heap block, with the
 * following layout:
 *
 * CURSORICONINFO info;
 * BYTE[]         ANDbits;
 * BYTE[]         XORbits;
 *
 * The bits structures are in the format of a device-dependent bitmap.
 *
 * This layout is very sub-optimal, as the bitmap bits are stored in
 * the X client instead of in the server like other bitmaps; however,
 * some programs (notably Paint Brush) expect to be able to manipulate
 * the bits directly :-(
 */

#include <string.h>
#include <stdlib.h>
#include "windows.h"
#include "bitmap.h"
#include "callback.h"
#include "cursoricon.h"
#include "sysmetrics.h"
#include "win.h"
#include "stddebug.h"
#include "debug.h"


Cursor CURSORICON_XCursor = None;  /* Current X cursor */
static HCURSOR hActiveCursor = 0;  /* Active cursor */
static int CURSOR_ShowCount = 0;   /* Cursor display count */
static RECT CURSOR_ClipRect;       /* Cursor clipping rect */

/**********************************************************************
 *	    CURSORICON_FindBestIcon
 *
 * Find the icon closest to the requested size and number of colors.
 */
static ICONDIRENTRY *CURSORICON_FindBestIcon( CURSORICONDIR *dir, int width,
                                              int height, int colors )
{
    int i, maxcolors, maxwidth, maxheight;
    ICONDIRENTRY *entry, *bestEntry = NULL;

    if (dir->idCount < 1)
    {
        fprintf( stderr, "Icon: empty directory!\n" );
        return NULL;
    }
    if (dir->idCount == 1) return &dir->idEntries[0].icon;  /* No choice... */

    /* First find the exact size with less colors */

    maxcolors = 0;
    for (i = 0, entry = &dir->idEntries[0].icon; i < dir->idCount; i++,entry++)
        if ((entry->bWidth == width) && (entry->bHeight == height) &&
            (entry->bColorCount <= colors) && (entry->bColorCount > maxcolors))
        {
            bestEntry = entry;
            maxcolors = entry->bColorCount;
        }
    if (bestEntry) return bestEntry;

    /* First find the exact size with more colors */

    maxcolors = 255;
    for (i = 0, entry = &dir->idEntries[0].icon; i < dir->idCount; i++,entry++)
        if ((entry->bWidth == width) && (entry->bHeight == height) &&
            (entry->bColorCount > colors) && (entry->bColorCount <= maxcolors))
        {
            bestEntry = entry;
            maxcolors = entry->bColorCount;
        }
    if (bestEntry) return bestEntry;

    /* Now find a smaller one with less colors */

    maxcolors = maxwidth = maxheight = 0;
    for (i = 0, entry = &dir->idEntries[0].icon; i < dir->idCount; i++,entry++)
        if ((entry->bWidth <= width) && (entry->bHeight <= height) &&
            (entry->bWidth >= maxwidth) && (entry->bHeight >= maxheight) &&
            (entry->bColorCount <= colors) && (entry->bColorCount > maxcolors))
        {
            bestEntry = entry;
            maxwidth  = entry->bWidth;
            maxheight = entry->bHeight;
            maxcolors = entry->bColorCount;
        }
    if (bestEntry) return bestEntry;

    /* Now find a smaller one with more colors */

    maxcolors = 255;
    maxwidth = maxheight = 0;
    for (i = 0, entry = &dir->idEntries[0].icon; i < dir->idCount; i++,entry++)
        if ((entry->bWidth <= width) && (entry->bHeight <= height) &&
            (entry->bWidth >= maxwidth) && (entry->bHeight >= maxheight) &&
            (entry->bColorCount > colors) && (entry->bColorCount <= maxcolors))
        {
            bestEntry = entry;
            maxwidth  = entry->bWidth;
            maxheight = entry->bHeight;
            maxcolors = entry->bColorCount;
        }
    if (bestEntry) return bestEntry;

    /* Now find a larger one with less colors */

    maxcolors = 0;
    maxwidth = maxheight = 255;
    for (i = 0, entry = &dir->idEntries[0].icon; i < dir->idCount; i++,entry++)
        if ((entry->bWidth <= maxwidth) && (entry->bHeight <= maxheight) &&
            (entry->bColorCount <= colors) && (entry->bColorCount > maxcolors))
        {
            bestEntry = entry;
            maxwidth  = entry->bWidth;
            maxheight = entry->bHeight;
            maxcolors = entry->bColorCount;
        }
    if (bestEntry) return bestEntry;

    /* Now find a larger one with more colors */

    maxcolors = maxwidth = maxheight = 255;
    for (i = 0, entry = &dir->idEntries[0].icon; i < dir->idCount; i++,entry++)
        if ((entry->bWidth <= maxwidth) && (entry->bHeight <= maxheight) &&
            (entry->bColorCount > colors) && (entry->bColorCount <= maxcolors))
        {
            bestEntry = entry;
            maxwidth  = entry->bWidth;
            maxheight = entry->bHeight;
            maxcolors = entry->bColorCount;
        }

    return bestEntry;
}


/**********************************************************************
 *	    CURSORICON_FindBestCursor
 *
 * Find the cursor closest to the requested size.
 */
static CURSORDIRENTRY *CURSORICON_FindBestCursor( CURSORICONDIR *dir,
                                                  int width, int height )
{
    int i, maxwidth, maxheight;
    CURSORDIRENTRY *entry, *bestEntry = NULL;

    if (dir->idCount < 1)
    {
        fprintf( stderr, "Cursor: empty directory!\n" );
        return NULL;
    }
    if (dir->idCount == 1) return &dir->idEntries[0].cursor; /* No choice... */

    /* First find the largest one smaller than or equal to the requested size*/

    maxwidth = maxheight = 0;
    for(i = 0,entry = &dir->idEntries[0].cursor; i < dir->idCount; i++,entry++)
        if ((entry->wWidth <= width) && (entry->wHeight <= height) &&
            (entry->wWidth > maxwidth) && (entry->wHeight > maxheight))
        {
            bestEntry = entry;
            maxwidth  = entry->wWidth;
            maxheight = entry->wHeight;
        }
    if (bestEntry) return bestEntry;

    /* Now find the smallest one larger than the requested size */

    maxwidth = maxheight = 255;
    for(i = 0,entry = &dir->idEntries[0].cursor; i < dir->idCount; i++,entry++)
        if ((entry->wWidth < maxwidth) && (entry->wHeight < maxheight))
        {
            bestEntry = entry;
            maxwidth  = entry->wWidth;
            maxheight = entry->wHeight;
        }

    return bestEntry;
}


/**********************************************************************
 *	    CURSORICON_LoadDirEntry
 *
 * Load the icon/cursor directory for a given resource name and find the
 * best matching entry.
 */
static BOOL CURSORICON_LoadDirEntry(HANDLE hInstance, SEGPTR name,
                                    int width, int height, int colors,
                                    BOOL fCursor, CURSORICONDIRENTRY *dirEntry)
{
    HRSRC hRsrc;
    HANDLE hMem;
    CURSORICONDIR *dir;
    CURSORICONDIRENTRY *entry = NULL;

    if (!(hRsrc = FindResource( hInstance, name,
                                fCursor ? RT_GROUP_CURSOR : RT_GROUP_ICON )))
        return FALSE;
    if (!(hMem = LoadResource( hInstance, hRsrc ))) return FALSE;
    if ((dir = (CURSORICONDIR *)LockResource( hMem )))
    {
        if (fCursor)
            entry = (CURSORICONDIRENTRY *)CURSORICON_FindBestCursor( dir,
                                                               width, height );
        else
            entry = (CURSORICONDIRENTRY *)CURSORICON_FindBestIcon( dir,
                                                       width, height, colors );
        if (entry) *dirEntry = *entry;
    }
    FreeResource( hMem );
    return (entry != NULL);
}


/**********************************************************************
 *	    CURSORICON_Load
 *
 * Load a cursor or icon.
 */
static HANDLE CURSORICON_Load( HANDLE hInstance, SEGPTR name, int width,
                               int height, int colors, BOOL fCursor )
{
    HANDLE handle, hAndBits, hXorBits;
    HRSRC hRsrc;
    HDC hdc;
    int size, sizeAnd, sizeXor;
    POINT hotspot = { 0 ,0 };
    BITMAPOBJ *bmpXor, *bmpAnd;
    BITMAPINFO *bmi, *pInfo;
    CURSORICONINFO *info;
    CURSORICONDIRENTRY dirEntry;
    char *bits;

    if (!hInstance)  /* OEM cursor/icon */
    {
        if (HIWORD(name))  /* Check for '#xxx' name */
        {
            char *ptr = PTR_SEG_TO_LIN( name );
            if (ptr[0] != '#') return 0;
            if (!(name = (SEGPTR)atoi( ptr + 1 ))) return 0;
        }
        return OBM_LoadCursorIcon( LOWORD(name), fCursor );
    }

    /* Find the best entry in the directory */

    if (!CURSORICON_LoadDirEntry( hInstance, name, width, height,
                                  colors, fCursor, &dirEntry )) return 0;

    /* Load the resource */

    if (!(hRsrc = FindResource( hInstance,
                                MAKEINTRESOURCE( dirEntry.icon.wResId ),
                                fCursor ? RT_CURSOR : RT_ICON ))) return 0;
    if (!(handle = LoadResource( hInstance, hRsrc ))) return 0;

    if (fCursor)  /* If cursor, get the hotspot */
    {
        POINT *pt = (POINT *)LockResource( handle );
        hotspot = *pt;
        bmi = (BITMAPINFO *)(pt + 1);
    }
    else bmi = (BITMAPINFO *)LockResource( handle );

    /* Create a copy of the bitmap header */

    size = DIB_BitmapInfoSize( bmi, DIB_RGB_COLORS );
    /* Make sure we have room for the monochrome bitmap later on */
    size = max( size, sizeof(BITMAPINFOHEADER) + 2*sizeof(RGBQUAD) );
    pInfo = (BITMAPINFO *)malloc( size );
    memcpy( pInfo, bmi, size );

    if (pInfo->bmiHeader.biSize == sizeof(BITMAPINFOHEADER))
    {
        if (pInfo->bmiHeader.biCompression != BI_RGB)
        {
            fprintf(stderr,"Unknown size for compressed icon bitmap.\n");
            FreeResource( handle );
            free( pInfo );
            return 0;
        }
        pInfo->bmiHeader.biHeight /= 2;
    }
    else if (pInfo->bmiHeader.biSize == sizeof(BITMAPCOREHEADER))
    {
        BITMAPCOREHEADER *core = (BITMAPCOREHEADER *)pInfo;
        core->bcHeight /= 2;
    }
    else
    {
        fprintf( stderr, "CURSORICON_Load: Unknown bitmap length %ld!\n",
                 pInfo->bmiHeader.biSize );
        FreeResource( handle );
        free( pInfo );
        return 0;
    }

    /* Create the XOR bitmap */

    if (!(hdc = GetDC( 0 )))
    {
        FreeResource( handle );
        free( pInfo );
        return 0;
    }

    hXorBits = CreateDIBitmap( hdc, &pInfo->bmiHeader, CBM_INIT,
                               (char*)bmi + size, pInfo, DIB_RGB_COLORS );

    /* Fix the bitmap header to load the monochrome mask */

    if (pInfo->bmiHeader.biSize == sizeof(BITMAPINFOHEADER))
    {
        BITMAPINFOHEADER *bih = &pInfo->bmiHeader;
        RGBQUAD *rgb = pInfo->bmiColors;
        bits = (char *)bmi + size +
            DIB_GetImageWidthBytes(bih->biWidth,bih->biBitCount)*bih->biHeight;
        bih->biBitCount = 1;
        bih->biClrUsed = bih->biClrImportant = 2;
        rgb[0].rgbBlue = rgb[0].rgbGreen = rgb[0].rgbRed = 0x00;
        rgb[1].rgbBlue = rgb[1].rgbGreen = rgb[1].rgbRed = 0xff;
        rgb[0].rgbReserved = rgb[1].rgbReserved = 0;
    }
    else
    {
        BITMAPCOREHEADER *bch = (BITMAPCOREHEADER *)pInfo;
        RGBTRIPLE *rgb = (RGBTRIPLE *)(bch + 1);
        bits = (char *)bmi + size +
            DIB_GetImageWidthBytes(bch->bcWidth,bch->bcBitCount)*bch->bcHeight;
        bch->bcBitCount = 1;
        rgb[0].rgbtBlue = rgb[0].rgbtGreen = rgb[0].rgbtRed = 0x00;
        rgb[1].rgbtBlue = rgb[1].rgbtGreen = rgb[1].rgbtRed = 0xff;
    }

    /* Create the AND bitmap */

    hAndBits = CreateDIBitmap( hdc, &pInfo->bmiHeader, CBM_INIT,
                               bits, pInfo, DIB_RGB_COLORS );
    ReleaseDC( 0, hdc );
    FreeResource( handle );

    /* Now create the CURSORICONINFO structure */

    bmpXor = (BITMAPOBJ *) GDI_GetObjPtr( hXorBits, BITMAP_MAGIC );
    bmpAnd = (BITMAPOBJ *) GDI_GetObjPtr( hAndBits, BITMAP_MAGIC );
    sizeXor = bmpXor->bitmap.bmHeight * bmpXor->bitmap.bmWidthBytes;
    sizeAnd = bmpAnd->bitmap.bmHeight * bmpAnd->bitmap.bmWidthBytes;

    if (!(handle = GlobalAlloc( GMEM_MOVEABLE,
                                sizeof(CURSORICONINFO) + sizeXor + sizeAnd)))
    {
        DeleteObject( hXorBits );
        DeleteObject( hAndBits );
        return 0;
    }
    /* Make it owned by the module */
    FarSetOwner( handle, GetExePtr( hInstance ) );
    info = (CURSORICONINFO *)GlobalLock( handle );
    info->ptHotSpot.x   = hotspot.x;
    info->ptHotSpot.y   = hotspot.y;
    info->nWidth        = bmpXor->bitmap.bmWidth;
    info->nHeight       = bmpXor->bitmap.bmHeight;
    info->nWidthBytes   = bmpXor->bitmap.bmWidthBytes;
    info->bPlanes       = bmpXor->bitmap.bmPlanes;
    info->bBitsPerPixel = bmpXor->bitmap.bmBitsPixel;

    /* Transfer the bitmap bits to the CURSORICONINFO structure */

    GetBitmapBits( hAndBits, sizeAnd, (char *)(info + 1) );
    GetBitmapBits( hXorBits, sizeXor, (char *)(info + 1) + sizeAnd );
    DeleteObject( hXorBits );
    DeleteObject( hAndBits );
    GlobalUnlock( handle );
    return handle;
}


/***********************************************************************
 *           CURSORICON_Copy
 *
 * Make a copy of a cursor or icon.
 */
static HANDLE CURSORICON_Copy( HANDLE hInstance, HANDLE handle )
{
    char *ptrOld, *ptrNew;
    int size;
    HANDLE hNew;

    if (!(ptrOld = (char *)GlobalLock( handle ))) return 0;
    if (!(hInstance = GetExePtr( hInstance ))) return 0;
    size = GlobalSize( handle );
    hNew = GlobalAlloc( GMEM_MOVEABLE, size );
    FarSetOwner( hNew, hInstance );
    ptrNew = (char *)GlobalLock( hNew );
    memcpy( ptrNew, ptrOld, size );
    GlobalUnlock( handle );
    GlobalUnlock( hNew );
    return hNew;
}


/***********************************************************************
 *           LoadCursor    (USER.173)
 */
HCURSOR LoadCursor( HANDLE hInstance, SEGPTR name )
{
    if (HIWORD(name))
        dprintf_cursor( stddeb, "LoadCursor: "NPFMT" '%s'\n",
                        hInstance, (char *)PTR_SEG_TO_LIN( name ) );
    else
        dprintf_cursor( stddeb, "LoadCursor: "NPFMT" %04x\n",
                        hInstance, LOWORD(name) );

    return CURSORICON_Load( hInstance, name,
                            SYSMETRICS_CXCURSOR, SYSMETRICS_CYCURSOR, 1, TRUE);
}


/***********************************************************************
 *           LoadIcon    (USER.174)
 */
HICON LoadIcon( HANDLE hInstance, SEGPTR name )
{
    if (HIWORD(name))
        dprintf_icon( stddeb, "LoadIcon: "NPFMT" '%s'\n",
                      hInstance, (char *)PTR_SEG_TO_LIN( name ) );
    else
        dprintf_icon( stddeb, "LoadIcon: "NPFMT" %04x\n",
                      hInstance, LOWORD(name) );

    return CURSORICON_Load( hInstance, name,
                            SYSMETRICS_CXICON, SYSMETRICS_CYICON,
                            min( 16, 1 << screenDepth ), FALSE );
}


/***********************************************************************
 *           CreateCursor    (USER.406)
 */
HICON CreateCursor( HANDLE hInstance, INT xHotSpot, INT yHotSpot,
                    INT nWidth, INT nHeight, LPSTR lpANDbits, LPSTR lpXORbits)
{
    CURSORICONINFO info = { { xHotSpot, yHotSpot }, nWidth, nHeight, 1, 1 };

    dprintf_cursor( stddeb, "CreateCursor: %dx%d spot=%d,%d xor=%p and=%p\n",
                    nWidth, nHeight, xHotSpot, yHotSpot, lpXORbits, lpANDbits);
    return CreateCursorIconIndirect( hInstance, &info, lpANDbits, lpXORbits );
}


/***********************************************************************
 *           CreateIcon    (USER.407)
 */
HICON CreateIcon( HANDLE hInstance, INT nWidth, INT nHeight, BYTE bPlanes,
                  BYTE bBitsPixel, LPSTR lpANDbits, LPSTR lpXORbits)
{
    CURSORICONINFO info = { { 0, 0 }, nWidth, nHeight, bPlanes, bBitsPixel };

    dprintf_icon( stddeb, "CreateIcon: %dx%dx%d, xor=%p, and=%p\n",
                  nWidth, nHeight, bPlanes * bBitsPixel, lpXORbits, lpANDbits);
    return CreateCursorIconIndirect( hInstance, &info, lpANDbits, lpXORbits );
}


/***********************************************************************
 *           CreateCursorIconIndirect    (USER.408)
 */
HANDLE CreateCursorIconIndirect( HANDLE hInstance, CURSORICONINFO *info,
                                 LPSTR lpANDbits, LPSTR lpXORbits )
{
    HANDLE handle;
    char *ptr;
    int sizeAnd, sizeXor;

    hInstance = GetExePtr( hInstance );  /* Make it a module handle */
    if (!hInstance || !lpXORbits || !lpANDbits || info->bPlanes != 1) return 0;
    info->nWidthBytes = (info->nWidth * info->bBitsPerPixel + 15) / 16 * 2;
    sizeXor = info->nHeight * info->nWidthBytes;
    sizeAnd = info->nHeight * ((info->nWidth + 15) / 16 * 2);
    if (!(handle = DirectResAlloc(hInstance, 0x10,
                                  sizeof(CURSORICONINFO) + sizeXor + sizeAnd)))
        return 0;
    ptr = (char *)GlobalLock( handle );
    memcpy( ptr, info, sizeof(*info) );
    memcpy( ptr + sizeof(CURSORICONINFO), lpANDbits, sizeAnd );
    memcpy( ptr + sizeof(CURSORICONINFO) + sizeAnd, lpXORbits, sizeXor );
    GlobalUnlock( handle );
    return handle;
}


/***********************************************************************
 *           CopyIcon    (USER.368)
 */
HICON CopyIcon( HANDLE hInstance, HICON hIcon )
{
    dprintf_icon( stddeb, "CopyIcon: %04x %04x\n", hInstance, hIcon );
    return CURSORICON_Copy( hInstance, hIcon );
}


/***********************************************************************
 *           CopyCursor    (USER.369)
 */
HCURSOR CopyCursor( HANDLE hInstance, HCURSOR hCursor )
{
    dprintf_cursor( stddeb, "CopyCursor: %04x %04x\n", hInstance, hCursor );
    return CURSORICON_Copy( hInstance, hCursor );
}


/***********************************************************************
 *           DestroyIcon    (USER.457)
 */
BOOL DestroyIcon( HICON hIcon )
{
    dprintf_icon( stddeb, "DestroyIcon: %04x\n", hIcon );
    return GlobalFree( hIcon );
}


/***********************************************************************
 *           DestroyCursor    (USER.458)
 */
BOOL DestroyCursor( HCURSOR hCursor )
{
    dprintf_cursor( stddeb, "DestroyCursor: %04x\n", hCursor );
    return GlobalFree( hCursor );
}


/***********************************************************************
 *           DrawIcon    (USER.84)
 */
BOOL DrawIcon( HDC hdc, short x, short y, HICON hIcon )
{
    CURSORICONINFO *ptr;
    HDC hMemDC;
    HBITMAP hXorBits, hAndBits;
    COLORREF oldFg, oldBg;

    if (!(ptr = (CURSORICONINFO *)GlobalLock( hIcon ))) return FALSE;
    if (!(hMemDC = CreateCompatibleDC( hdc ))) return FALSE;
    hAndBits = CreateBitmap( ptr->nWidth, ptr->nHeight, 1, 1, (char *)(ptr+1));
    hXorBits = CreateBitmap( ptr->nWidth, ptr->nHeight, ptr->bPlanes,
                             ptr->bBitsPerPixel, (char *)(ptr + 1)
                              + ptr->nHeight * ((ptr->nWidth + 15) / 16 * 2) );
    oldFg = SetTextColor( hdc, RGB(0,0,0) );
    oldBg = SetBkColor( hdc, RGB(255,255,255) );

    if (hXorBits && hAndBits)
    {
        HBITMAP hBitTemp = SelectObject( hMemDC, hAndBits );
        BitBlt( hdc, x, y, ptr->nWidth, ptr->nHeight, hMemDC, 0, 0, SRCAND );
        SelectObject( hMemDC, hXorBits );
        BitBlt( hdc, x, y, ptr->nWidth, ptr->nHeight, hMemDC, 0, 0, SRCINVERT);
        SelectObject( hMemDC, hBitTemp );
    }
    DeleteDC( hMemDC );
    if (hXorBits) DeleteObject( hXorBits );
    if (hAndBits) DeleteObject( hAndBits );
    GlobalUnlock( hIcon );
    SetTextColor( hdc, oldFg );
    SetBkColor( hdc, oldBg );
    return TRUE;
}


/***********************************************************************
 *           DumpIcon    (USER.459)
 */
DWORD DumpIcon( CURSORICONINFO *info, WORD *lpLen,
                LPSTR *lpXorBits, LPSTR *lpAndBits )
{
    int sizeAnd, sizeXor;

    if (!info) return 0;
    sizeXor = info->nHeight * info->nWidthBytes;
    sizeAnd = info->nHeight * ((info->nWidth + 15) / 16 * 2);
    if (lpAndBits) *lpAndBits = (LPSTR)(info + 1);
    if (lpXorBits) *lpXorBits = (LPSTR)(info + 1) + sizeAnd;
    if (lpLen) *lpLen = sizeof(CURSORICONINFO) + sizeAnd + sizeXor;
    return MAKELONG( sizeXor, sizeXor );
}


/***********************************************************************
 *           CURSORICON_SetCursor
 *
 * Change the X cursor. Helper function for SetCursor() and ShowCursor().
 */
static BOOL CURSORICON_SetCursor( HCURSOR hCursor )
{
    Pixmap pixmapBits, pixmapMask, pixmapAll;
    XColor fg, bg;
    Cursor cursor = None;

    if (!hCursor)  /* Create an empty cursor */
    {
        static const char data[] = { 0 };

        bg.red = bg.green = bg.blue = 0x0000;
        pixmapBits = XCreateBitmapFromData( display, rootWindow, data, 1, 1 );
        if (pixmapBits)
        {
            cursor = XCreatePixmapCursor( display, pixmapBits, pixmapBits,
                                          &bg, &bg, 0, 0 );
            XFreePixmap( display, pixmapBits );
        }
    }
    else  /* Create the X cursor from the bits */
    {
        CURSORICONINFO *ptr;
        XImage *image;

        if (!(ptr = (CURSORICONINFO*)GlobalLock( hCursor ))) return FALSE;
        if (ptr->bPlanes * ptr->bBitsPerPixel != 1)
        {
            fprintf( stderr, "Cursor %04x has more than 1 bpp!\n", hCursor );
            return FALSE;
        }

        /* Create a pixmap and transfer all the bits to it */

        pixmapAll = XCreatePixmap( display, rootWindow,
                                   ptr->nWidth, ptr->nHeight * 2, 1 );
        image = XCreateImage( display, DefaultVisualOfScreen(screen),
                              1, ZPixmap, 0, (char *)(ptr + 1), ptr->nWidth,
                              ptr->nHeight * 2, 16, ptr->nWidthBytes);
        if (image)
        {
            extern void _XInitImageFuncPtrs( XImage* );
            image->byte_order = MSBFirst;
            image->bitmap_bit_order = MSBFirst;
            image->bitmap_unit = 16;
            _XInitImageFuncPtrs(image);
            if (pixmapAll)
                CallTo32_LargeStack( XPutImage, 10,
                                     display, pixmapAll, BITMAP_monoGC, image,
                                     0, 0, 0, 0, ptr->nWidth, ptr->nHeight*2 );
            image->data = NULL;
            XDestroyImage( image );
        }

        /* Now create the 2 pixmaps for bits and mask */

        pixmapBits = XCreatePixmap( display, rootWindow,
                                    ptr->nWidth, ptr->nHeight, 1 );
        pixmapMask = XCreatePixmap( display, rootWindow,
                                    ptr->nWidth, ptr->nHeight, 1 );

        /* Make sure everything went OK so far */

        if (pixmapBits && pixmapMask && pixmapAll)
        {
            /* We have to do some magic here, as cursors are not fully
             * compatible between Windows and X11. Under X11, there
             * are only 3 possible color cursor: black, white and
             * masked. So we map the 4th Windows color (invert the
             * bits on the screen) to black. This require some boolean
             * arithmetic:
             *
             *   Windows                        X11
             * Xor    And      Result      Bits     Mask
             *  0      0     black          0        1
             *  0      1     no change      X        0
             *  1      0     white          1        1
             *  1      1     inverted       0        1  (=black)
             *
             * which gives:
             *  Bits = 'Xor' xor 'And'
             *  Mask = 'Xor' or not 'And'
             */
            XCopyArea( display, pixmapAll, pixmapBits, BITMAP_monoGC,
                       0, 0, ptr->nWidth, ptr->nHeight, 0, 0 );
            XCopyArea( display, pixmapAll, pixmapMask, BITMAP_monoGC,
                       0, 0, ptr->nWidth, ptr->nHeight, 0, 0 );
            XSetFunction( display, BITMAP_monoGC, GXxor );
            XCopyArea( display, pixmapAll, pixmapBits, BITMAP_monoGC,
                       0, ptr->nHeight, ptr->nWidth, ptr->nHeight, 0, 0 );
            XSetFunction( display, BITMAP_monoGC, GXorReverse );
            XCopyArea( display, pixmapAll, pixmapMask, BITMAP_monoGC,
                       0, ptr->nHeight, ptr->nWidth, ptr->nHeight, 0, 0 );
            XSetFunction( display, BITMAP_monoGC, GXcopy );
            fg.red = fg.green = fg.blue = 0xffff;
            bg.red = bg.green = bg.blue = 0x0000;
            cursor = XCreatePixmapCursor( display, pixmapBits, pixmapMask,
                                &fg, &bg, ptr->ptHotSpot.x, ptr->ptHotSpot.y );
        }

        /* Now free everything */

        if (pixmapAll) XFreePixmap( display, pixmapAll );
        if (pixmapBits) XFreePixmap( display, pixmapBits );
        if (pixmapMask) XFreePixmap( display, pixmapMask );
        GlobalUnlock( hCursor );
    }

    if (cursor == None) return FALSE;
    if (CURSORICON_XCursor != None) XFreeCursor( display, CURSORICON_XCursor );
    CURSORICON_XCursor = cursor;

    if (rootWindow != DefaultRootWindow(display))
    {
        /* Set the cursor on the desktop window */
        XDefineCursor( display, rootWindow, cursor );
    }
    else
    {
        /* Set the same cursor for all top-level windows */
        HWND hwnd = GetWindow( GetDesktopWindow(), GW_CHILD );
        while(hwnd)
        {
            Window win = WIN_GetXWindow( hwnd );
            if (win) XDefineCursor( display, win, cursor );
            hwnd = GetWindow( hwnd, GW_HWNDNEXT );
        }
    }
    return TRUE;
}


/***********************************************************************
 *           SetCursor    (USER.69)
 */
HCURSOR SetCursor( HCURSOR hCursor )
{
    HCURSOR hOldCursor;

    if (hCursor == hActiveCursor) return hActiveCursor;  /* No change */
    dprintf_cursor( stddeb, "SetCursor: %04x\n", hCursor );
    hOldCursor = hActiveCursor;
    hActiveCursor = hCursor;
    /* Change the cursor shape only if it is visible */
    if (CURSOR_ShowCount >= 0) CURSORICON_SetCursor( hActiveCursor );
    return hOldCursor;
}


/***********************************************************************
 *           SetCursorPos    (USER.70)
 */
void SetCursorPos( short x, short y )
{
    dprintf_cursor( stddeb, "SetCursorPos: x=%d y=%d\n", x, y );
    XWarpPointer( display, None, rootWindow, 0, 0, 0, 0, x, y );
}


/***********************************************************************
 *           ShowCursor    (USER.71)
 */
int ShowCursor( BOOL bShow )
{
    dprintf_cursor( stddeb, "ShowCursor: %d, count=%d\n",
                    bShow, CURSOR_ShowCount );

    if (bShow)
    {
        if (++CURSOR_ShowCount == 0)
            CURSORICON_SetCursor( hActiveCursor );  /* Show it */
    }
    else
    {
        if (--CURSOR_ShowCount == -1)
            CURSORICON_SetCursor( 0 );  /* Hide it */
    }
    return CURSOR_ShowCount;
}


/***********************************************************************
 *           GetCursor    (USER.247)
 */
HCURSOR GetCursor(void)
{
    return hActiveCursor;
}


/***********************************************************************
 *           ClipCursor    (USER.16)
 */
void ClipCursor( RECT *rect )
{
    if (!rect) SetRectEmpty( &CURSOR_ClipRect );
    else CopyRect( &CURSOR_ClipRect, rect );
}


/***********************************************************************
 *           GetCursorPos    (USER.17)
 */
void GetCursorPos( POINT *pt )
{
    Window root, child;
    int rootX, rootY, childX, childY;
    unsigned int mousebut;

    if (!pt) return;
    if (!XQueryPointer( display, rootWindow, &root, &child,
		        &rootX, &rootY, &childX, &childY, &mousebut ))
	pt->x = pt->y = 0;
    else
    {
	pt->x = rootX + desktopX;
	pt->y = rootY + desktopY;
    }
    dprintf_cursor(stddeb, "GetCursorPos: ret=%d,%d\n", pt->x, pt->y );
}


/***********************************************************************
 *           GetClipCursor    (USER.309)
 */
void GetClipCursor( RECT *rect )
{
    if (rect) CopyRect( rect, &CURSOR_ClipRect );
}


/**********************************************************************
 *	    GetIconID    (USER.455)
 */
WORD GetIconID( HANDLE hResource, DWORD resType )
{
    fprintf( stderr, "GetIconId(%04x,%ld): empty stub!\n",
             hResource, resType );
    return 0;
}


/**********************************************************************
 *	    LoadIconHandler    (USER.456)
 */
HICON LoadIconHandler( HANDLE hResource, BOOL bNew )
{
    fprintf( stderr, "LoadIconHandle(%04x,%d): empty stub!\n",
             hResource, bNew );
    return 0;
}
