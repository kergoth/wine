/*
 * 16-bit mode callback functions
 *
 * Copyright 1995 Alexandre Julliard
 */

#ifndef WINE_CALLBACK_H
#define WINE_CALLBACK_H

#include <stdlib.h>
#include <stdarg.h>

#include "stackframe.h"

extern int CallTo32_LargeStack( int (*func)(), int nbargs, ... );


/* List of the 16-bit callback functions. This list is used  */
/* by the build program to generate the file if1632/call16.S */

                               /* func     ds    parameters */
extern WORD CallTo16_word_     ( FARPROC, WORD );

#ifndef WINELIB

extern WORD CallTo16_word_ww   ( FARPROC, WORD, WORD, WORD );
extern WORD CallTo16_word_wl   ( FARPROC, WORD, WORD, LONG );
extern WORD CallTo16_word_ll   ( FARPROC, WORD, LONG, LONG );
extern WORD CallTo16_word_www  ( FARPROC, WORD, WORD, WORD, WORD );
extern WORD CallTo16_word_wwl  ( FARPROC, WORD, WORD, WORD, LONG );
extern WORD CallTo16_word_wlw  ( FARPROC, WORD, WORD, LONG, WORD );
extern LONG CallTo16_long_wwl  ( FARPROC, WORD, WORD, WORD, LONG );
extern WORD CallTo16_word_llwl ( FARPROC, WORD, LONG, LONG, WORD, LONG );
extern LONG CallTo16_long_wwwl ( FARPROC, WORD, WORD, WORD, WORD, LONG );
extern WORD CallTo16_word_wllwl( FARPROC, WORD, WORD, LONG, LONG, WORD, LONG );
extern WORD CallTo16_word_wwlll( FARPROC, WORD, WORD, WORD, LONG, LONG, LONG );

extern WORD CallTo16_regs_( FARPROC func, WORD ds, WORD es, WORD bp, WORD ax,
                            WORD bx, WORD cx, WORD dx, WORD si, WORD di );

#define CallEnumChildProc( func, hwnd, lParam ) \
    CallTo16_word_wl( func, CURRENT_DS, hwnd, lParam )
#define CallEnumFontFamProc( func, lpfont, lpmetric, type, lParam ) \
    CallTo16_word_llwl( func, CURRENT_DS, lpfont, lpmetric, type, lParam )
#define CallEnumFontsProc( func, lpfont, lpmetric, type, lParam ) \
    CallTo16_word_llwl( func, CURRENT_DS, lpfont, lpmetric, type, lParam )
#define CallEnumMetafileProc( func, hdc, lptable, lprecord, objs, lParam ) \
    CallTo16_word_wllwl(func, CURRENT_DS, hdc, lptable, lprecord, objs, lParam)
#define CallEnumObjectsProc( func, lpobj, lParam ) \
    CallTo16_word_ll( func, CURRENT_DS, lpobj, lParam )
#define CallEnumPropProc( func, hwnd, lpstr, data ) \
    CallTo16_word_wlw( func, CURRENT_DS, hwnd, lpstr, data )
#define CallEnumTaskWndProc( func, hwnd, lParam ) \
    CallTo16_word_wl( func, CURRENT_DS, hwnd, lParam )
#define CallEnumWindowsProc( func, hwnd, lParam ) \
    CallTo16_word_wl( func, CURRENT_DS, hwnd, lParam )
#define CallLineDDAProc( func, xPos, yPos, lParam ) \
    CallTo16_word_wwl( func, CURRENT_DS, xPos, yPos, lParam )
#define CallGrayStringProc( func, hdc, lParam, cch ) \
    CallTo16_word_wlw( func, CURRENT_DS, hdc, lParam, cch )
#define CallHookProc( func, code, wParam, lParam ) \
    CallTo16_long_wwl( func, CURRENT_DS, code, wParam, lParam )
#define CallTimeFuncProc( func, id, msg, dwUser, dw1, dw2 ) \
    CallTo16_word_wwlll( func, CURRENT_DS, id, msg, dwUser, dw1, dw2 )
#define CallWndProc( func, ds, hwnd, msg, wParam, lParam ) \
    CallTo16_long_wwwl( func, ds, hwnd, msg, wParam, lParam )

#else  /* WINELIB */

#define CallEnumChildProc( func, hwnd, lParam ) \
    (*func)( hwnd, lParam )
#define CallEnumFontFamProc( func, lpfont, lpmetric, type, lParam ) \
    (*func)( lpfont, lpmetric, type, lParam )
#define CallEnumFontsProc( func, lpfont, lpmetric, type, lParam ) \
    (*func)( lpfont, lpmetric, type, lParam )
#define CallEnumMetafileProc( func, hdc, lptable, lprecord, objs, lParam ) \
    (*func)( hdc, lptable, lprecord, objs, lParam)
#define CallEnumObjectsProc( func, lpobj, lParam ) \
    (*func)( lpobj, lParam )
#define CallEnumPropProc( func, hwnd, lpstr, data ) \
    (*func)( hwnd, lpstr, data )
#define CallEnumTaskWndProc( func, hwnd, lParam ) \
    (*func)( hwnd, lParam )
#define CallEnumWindowsProc( func, hwnd, lParam ) \
    (*func)( hwnd, lParam )
#define CallLineDDAProc( func, xPos, yPos, lParam ) \
    (*func)( xPos, yPos, lParam )
#define CallGrayStringProc( func, hdc, lParam, cch ) \
    (*func)( hdc, lParam, cch )
#define CallHookProc( func, code, wParam, lParam ) \
    (*func)( code, wParam, lParam )
#define CallTimeFuncProc( func, id, msg, dwUser, dw1, dw2 ) \
    (*func)( id, msg, dwUser, dw1, dw2 )
#define CallWndProc( func, ds, hwnd, msg, wParam, lParam ) \
    (*func)( hwnd, msg, wParam, lParam )

#endif  /* WINELIB */


#endif /* WINE_CALLBACK_H */
