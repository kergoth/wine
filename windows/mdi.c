/* MDI.C
 *
 * Copyright 1994, Bob Amstadt
 *
 * This file contains routines to support MDI features.
 */
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include "windows.h"
#include "win.h"
#include "nonclient.h"
#include "mdi.h"
#include "user.h"
#include "menu.h"
#include "sysmetrics.h"
#include "stddebug.h"
#include "debug.h"

/**********************************************************************
 *					MDIRecreateMenuList
 */
void MDIRecreateMenuList(MDICLIENTINFO *ci)
{
    HLOCAL hinfo;
    
    char buffer[128];
    int id, n, index;

    dprintf_mdi(stddeb, "MDIRecreateMenuList: hWindowMenu "NPFMT"\n", 
	    ci->hWindowMenu);
    
    id = ci->idFirstChild; 
    while (DeleteMenu(ci->hWindowMenu, id, MF_BYCOMMAND))
	id++;

    dprintf_mdi(stddeb, "MDIRecreateMenuList: id %04x, idFirstChild %04x\n", 
	    id, ci->idFirstChild);

    if (!ci->flagMenuAltered)
    {
	ci->flagMenuAltered = TRUE;
	AppendMenu(ci->hWindowMenu, MF_SEPARATOR, 0, NULL);
    }
    
    id = ci->idFirstChild;
    index = 1;
    for (hinfo = ci->infoActiveChildren; hinfo != 0;)
    {
	MDICHILDINFO *chi = USER_HEAP_LIN_ADDR(hinfo);
	
	n = sprintf(buffer, "%d ", index++);
	GetWindowText(chi->hwnd, buffer + n, sizeof(buffer) - n - 1);

	dprintf_mdi(stddeb, "MDIRecreateMenuList: id %04x, '%s'\n",
		id, buffer);

	AppendMenu(ci->hWindowMenu, MF_STRING, id++, buffer);
	hinfo = chi->next;
    }
}

/**********************************************************************
 *					MDISetMenu
 * FIXME: This is not complete.
 */
HMENU MDISetMenu(HWND hwnd, BOOL fRefresh, HMENU hmenuFrame, HMENU hmenuWindow)
{
    dprintf_mdi(stddeb, "WM_MDISETMENU: "NPFMT" %04x "NPFMT" "NPFMT"\n", hwnd, fRefresh, hmenuFrame, hmenuWindow);
    if (!fRefresh) {
	HWND hwndFrame = GetParent(hwnd);
	HMENU oldFrameMenu = GetMenu(hwndFrame);
	SetMenu(hwndFrame, hmenuFrame);
	return oldFrameMenu;
    }
    return 0;
}

/**********************************************************************
 *					MDIIconArrange
 */
WORD MDIIconArrange(HWND parent)
{
  return ArrangeIconicWindows(parent);		/* Any reason why the    */
						/* existing icon arrange */
						/* can't be used here?	 */
						/* -DRP			 */
}

/**********************************************************************
 *					MDICreateChild
 */
HWND MDICreateChild(WND *w, MDICLIENTINFO *ci, HWND parent, LPARAM lParam )
{
    MDICREATESTRUCT *cs = (MDICREATESTRUCT *)PTR_SEG_TO_LIN(lParam);
    HWND hwnd;
    int spacing;

    /*
     * Create child window
     */
    cs->style &= (WS_MINIMIZE | WS_MAXIMIZE | WS_HSCROLL | WS_VSCROLL);

				/* The child windows should probably  */
				/* stagger, shouldn't they? -DRP      */
    spacing = GetSystemMetrics(SM_CYCAPTION) + GetSystemMetrics(SM_CYFRAME);
    cs->x = ci->nActiveChildren * spacing;  
    cs->y = ci->nActiveChildren * spacing;

    hwnd = CreateWindow( cs->szClass, cs->szTitle,
			  WS_CHILD | WS_BORDER | WS_CAPTION | WS_CLIPSIBLINGS |
			  WS_MAXIMIZEBOX | WS_MINIMIZEBOX | WS_SYSMENU |
			  WS_THICKFRAME | WS_VISIBLE | cs->style,
			  cs->x, cs->y, cs->cx, cs->cy, parent, (HMENU) 0,
			  w->hInstance, (SEGPTR)lParam);

    if (hwnd)
    {
	HANDLE h = USER_HEAP_ALLOC( sizeof(MDICHILDINFO) );
	MDICHILDINFO *child_info = USER_HEAP_LIN_ADDR(h);
	
	if (!h)
	{
	    DestroyWindow(hwnd);
	    return 0;
	}

	ci->nActiveChildren++;

	child_info->next = ci->infoActiveChildren;
	child_info->prev = 0;
	child_info->hwnd = hwnd;

	if (ci->infoActiveChildren) {
	    MDICHILDINFO *nextinfo = USER_HEAP_LIN_ADDR(ci->infoActiveChildren);
	    nextinfo->prev = h;
	}

	ci->infoActiveChildren = h;

	SendMessage(parent, WM_CHILDACTIVATE, 0, 0);
    }
	
    return hwnd;
}

/**********************************************************************
 *					MDIDestroyChild
 */
HWND MDIDestroyChild(WND *w_parent, MDICLIENTINFO *ci, HWND parent,
		     HWND child, BOOL flagDestroy)
{
    MDICHILDINFO  *chi;
    HLOCAL hinfo;
    
    hinfo = ci->infoActiveChildren;
    while (hinfo != 0) {
	chi = (MDICHILDINFO *)USER_HEAP_LIN_ADDR(hinfo);
	if (chi->hwnd == child) break;
	hinfo = chi->next;
    }
    
    if (hinfo != 0)
    {
	if (chi->prev)
	    ((MDICHILDINFO *)USER_HEAP_LIN_ADDR(chi->prev))->next = chi->next;
	if (chi->next)
	    ((MDICHILDINFO *)USER_HEAP_LIN_ADDR(chi->next))->prev = chi->prev;
	if (ci->infoActiveChildren == hinfo)
	    ci->infoActiveChildren = chi->next;

	ci->nActiveChildren--;
	
	if (chi->hwnd == ci->hwndActiveChild)
	    SendMessage(parent, WM_CHILDACTIVATE, 0, 0);

	USER_HEAP_FREE(hinfo);
	
	if (flagDestroy)
	    DestroyWindow(child);
    }
    
    return 0;
}

/**********************************************************************
 *					MDIBringChildToTop
 */
void MDIBringChildToTop(HWND parent, WORD id, WORD by_id, BOOL send_to_bottom)
{
    HLOCAL hinfo;
    MDICHILDINFO  *chi;
    MDICLIENTINFO *ci;
    WND           *w;
    int            i;

    w  = WIN_FindWndPtr(parent);
    ci = (MDICLIENTINFO *) w->wExtra;
    
    dprintf_mdi(stddeb, "MDIBringToTop: id %04x, by_id %d\n", id, by_id);

    if (by_id)
	id -= ci->idFirstChild;
    if (!by_id || id < ci->nActiveChildren)
    {
	hinfo = ci->infoActiveChildren;

	if (by_id)
	{
	    for (i = 0; i < id; i++)
		hinfo = ((MDICHILDINFO *)USER_HEAP_LIN_ADDR(hinfo))->next;
	    chi = USER_HEAP_LIN_ADDR(hinfo);
	}
	else
	{
	    while (hinfo != 0) {
		chi = (MDICHILDINFO *)USER_HEAP_LIN_ADDR(hinfo);
		if (chi->hwnd == (HWND)id) break;
	        hinfo = chi->next;
	    }
	}

	if (hinfo == 0)
	    return;

	dprintf_mdi(stddeb, "MDIBringToTop: child "NPFMT"\n", chi->hwnd);
	if (hinfo != ci->infoActiveChildren)
	{
	    if (ci->flagChildMaximized)
	    {
		RECT rectOldRestore, rect;

		w = WIN_FindWndPtr(chi->hwnd);
		
		rectOldRestore = ci->rectRestore;
		GetWindowRect(chi->hwnd, &ci->rectRestore);

		rect.top    = (ci->rectMaximize.top -
			       (w->rectClient.top - w->rectWindow.top));
		rect.bottom = (ci->rectMaximize.bottom + 
			       (w->rectWindow.bottom - w->rectClient.bottom));
		rect.left   = (ci->rectMaximize.left - 
			       (w->rectClient.left - w->rectWindow.left));
		rect.right  = (ci->rectMaximize.right +
			       (w->rectWindow.right - w->rectClient.right));
		w->dwStyle |= WS_MAXIMIZE;
		SetWindowPos(chi->hwnd, HWND_TOP, rect.left, rect.top, 
			     rect.right - rect.left + 1,
			     rect.bottom - rect.top + 1, 0);
		SendMessage(chi->hwnd, WM_SIZE, SIZE_MAXIMIZED,
			    MAKELONG(w->rectClient.right-w->rectClient.left,
				     w->rectClient.bottom-w->rectClient.top));

		w = WIN_FindWndPtr(ci->hwndActiveChild);
		w->dwStyle &= ~WS_MAXIMIZE;
		SetWindowPos(ci->hwndActiveChild, HWND_BOTTOM, 
			     rectOldRestore.left, rectOldRestore.top, 
			     rectOldRestore.right - rectOldRestore.left + 1, 
			     rectOldRestore.bottom - rectOldRestore.top + 1,
			     SWP_NOACTIVATE | 
			     (send_to_bottom ? 0 : SWP_NOZORDER));
	    }
	    else
	    {
		SetWindowPos(chi->hwnd, HWND_TOP, 0, 0, 0, 0, 
			     SWP_NOMOVE | SWP_NOSIZE );
		if (send_to_bottom)
		{
		    SetWindowPos(ci->hwndActiveChild, HWND_BOTTOM, 0, 0, 0, 0, 
				 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
		}
	    }
		
	    if (chi->next)
		((MDICHILDINFO *)USER_HEAP_LIN_ADDR(chi->next))->prev = chi->prev;

	    if (chi->prev)
		((MDICHILDINFO *)USER_HEAP_LIN_ADDR(chi->prev))->next = chi->next;
	    
	    chi->prev              = 0;
	    chi->next              = ci->infoActiveChildren;
	    ((MDICHILDINFO *)USER_HEAP_LIN_ADDR(chi->next))->prev = hinfo;
	    ci->infoActiveChildren = hinfo;

	    SendMessage(parent, WM_CHILDACTIVATE, 0, 0);
	}
	
	dprintf_mdi(stddeb, "MDIBringToTop: pos %04x, hwnd "NPFMT"\n", 
		id, chi->hwnd);
    }
}

/**********************************************************************
 *					MDIMaximizeChild
 */
LONG MDIMaximizeChild(HWND parent, HWND child, MDICLIENTINFO *ci)
{
    WND *w = WIN_FindWndPtr(child);
    RECT rect;
    
    MDIBringChildToTop(parent, child, FALSE, FALSE);
    ci->rectRestore = w->rectWindow;

    rect.top    = (ci->rectMaximize.top -
		   (w->rectClient.top - w->rectWindow.top));
    rect.bottom = (ci->rectMaximize.bottom + 
		   (w->rectWindow.bottom - w->rectClient.bottom));
    rect.left   = (ci->rectMaximize.left - 
		   (w->rectClient.left - w->rectWindow.left));
    rect.right  = (ci->rectMaximize.right +
		   (w->rectWindow.right - w->rectClient.right));
    w->dwStyle |= WS_MAXIMIZE;
    SetWindowPos(child, 0, rect.left, rect.top, 
		 rect.right - rect.left + 1, rect.bottom - rect.top + 1,
		 SWP_NOACTIVATE | SWP_NOZORDER);
    
    ci->flagChildMaximized = TRUE;
    
    SendMessage(child, WM_SIZE, SIZE_MAXIMIZED,
		MAKELONG(w->rectClient.right-w->rectClient.left,
			 w->rectClient.bottom-w->rectClient.top));
    SendMessage(GetParent(parent), WM_NCPAINT, 0, 0);

    return 0;
}

/**********************************************************************
 *					MDIRestoreChild
 */
LONG MDIRestoreChild(HWND parent, MDICLIENTINFO *ci)
{
    HWND    child;

    dprintf_mdi(stddeb,"restoring mdi child\n");

    child = ci->hwndActiveChild;
    ci->flagChildMaximized = FALSE;

    ShowWindow(child, SW_RESTORE);		/* display the window */
    MDIBringChildToTop(parent, child, FALSE, FALSE);
    SendMessage(GetParent(parent), WM_NCPAINT, 0, 0);

    return 0;
}

/**********************************************************************
 *					MDIChildActivated
 */
LONG MDIChildActivated(WND *w, MDICLIENTINFO *ci, HWND parent)
{
    HLOCAL hinfo;
    MDICHILDINFO *chi;
    HWND          deact_hwnd;
    HWND          act_hwnd;
    LONG          lParam;

    dprintf_mdi(stddeb, "MDIChildActivate: top "NPFMT"\n", w->hwndChild);

    hinfo = ci->infoActiveChildren;
    if (hinfo)
    {
	chi = (MDICHILDINFO *)USER_HEAP_LIN_ADDR(hinfo);
	deact_hwnd = ci->hwndActiveChild;
	act_hwnd   = chi->hwnd;                /* FIX: Hack */
	lParam     = ((LONG) deact_hwnd << 16) | (LONG)act_hwnd;

	dprintf_mdi(stddeb, "MDIChildActivate: deact "NPFMT", act "NPFMT"\n",
	       deact_hwnd, act_hwnd);

	ci->hwndActiveChild = act_hwnd;

	if (deact_hwnd != act_hwnd)
	{
	    MDIRecreateMenuList(ci);
	    SendMessage(deact_hwnd,  WM_NCACTIVATE, FALSE, 0);
	    SendMessage(deact_hwnd, WM_MDIACTIVATE, FALSE, lParam);
	}
	
	SendMessage(act_hwnd,  WM_NCACTIVATE, TRUE, 0);
	SendMessage(act_hwnd, WM_MDIACTIVATE, TRUE, lParam);
    }

    if (hinfo || ci->nActiveChildren == 0)
    {
	MDIRecreateMenuList(ci);
	SendMessage(GetParent(parent), WM_NCPAINT, 0, 0);
    }
    
    return 0;
}

/**********************************************************************
 *					MDICascade
 */
LONG MDICascade(HWND parent, MDICLIENTINFO *ci)
{
    HLOCAL hinfo;
    MDICHILDINFO *chi;
    RECT          rect;
    int           spacing, xsize, ysize;
    int		  x, y;

    if (ci->flagChildMaximized)
	MDIRestoreChild(parent, ci);

    /* If there aren't any children, don't even bother.
     */
    if (ci->nActiveChildren == 0)
        return 0;

    GetClientRect(parent, &rect);
    spacing = GetSystemMetrics(SM_CYCAPTION) + GetSystemMetrics(SM_CYFRAME);
    ysize   = rect.bottom - 8 * spacing;
    xsize   = rect.right  - 8 * spacing;
    
    dprintf_mdi(stddeb, 
	    "MDICascade: Client wnd at (%d,%d) - (%d,%d), spacing %d\n", 
	    rect.left, rect.top, rect.right, rect.bottom, spacing);
    dprintf_mdi(stddeb, "MDICascade: searching for last child\n");
    hinfo = ci->infoActiveChildren;
    while(1) {
	chi = USER_HEAP_LIN_ADDR(hinfo);
	if (chi->next == 0) break;
	hinfo = chi->next;
    }
    
    dprintf_mdi(stddeb, "MDICascade: last child is "NPFMT"\n", chi->hwnd);
    x = 0;
    y = 0;
    while (hinfo != 0)
    {
	chi = USER_HEAP_LIN_ADDR(hinfo);
	dprintf_mdi(stddeb, "MDICascade: move "NPFMT" to (%d,%d) size [%d,%d]\n", 
		chi->hwnd, x, y, xsize, ysize);
        if (IsIconic(chi->hwnd)) continue;
	SetWindowPos(chi->hwnd, 0, x, y, xsize, ysize, 
		     SWP_DRAWFRAME | SWP_NOACTIVATE | SWP_NOZORDER);

	x += spacing;
	y += spacing;
	
	hinfo = chi->prev;
    }

    return 0;
}

/**********************************************************************
 *					MDITile
 */
LONG MDITile(HWND parent, MDICLIENTINFO *ci)
{
    HLOCAL hinfo;
    MDICHILDINFO *chi;
    RECT          rect;
    int           xsize, ysize;
    int		  x, y;
    int		  rows, columns;
    int           r, c;
    int           i;

    if (ci->flagChildMaximized)
	MDIRestoreChild(parent, ci);

    /* If there aren't any children, don't even bother.
     */
    if (ci->nActiveChildren == 0)
        return 0;

    GetClientRect(parent, &rect);
    rows    = (int) sqrt((double) ci->nActiveChildren);
    columns = ci->nActiveChildren / rows;
    ysize   = rect.bottom / rows;
    xsize   = rect.right  / columns;
    
    hinfo   = ci->infoActiveChildren;
    x       = 0;
    i       = 0;
    for (c = 1; c <= columns; c++)
    {
	if (c == columns)
	{
	    rows  = ci->nActiveChildren - i;
	    ysize = rect.bottom / rows;
	}

	y = 0;
	for (r = 1; r <= rows; r++, i++)
	{
	    chi = (MDICHILDINFO *)USER_HEAP_LIN_ADDR(hinfo);
	    SetWindowPos(chi->hwnd, 0, x, y, xsize, ysize, 
			 SWP_DRAWFRAME | SWP_NOACTIVATE | SWP_NOZORDER);

	    y += ysize;
	    hinfo = chi->next;
	}

	x += xsize;
    }
    

    return 0;
}

/**********************************************************************
 *					MDIHandleLButton
 */
BOOL MDIHandleLButton(HWND hwndFrame, HWND hwndClient, 
		      WORD wParam, LONG lParam)
{
    MDICLIENTINFO *ci;
    WND           *w;
    RECT           rect;
    WORD           x;

    w  = WIN_FindWndPtr(hwndClient);
    ci = (MDICLIENTINFO *) w->wExtra;

    if (wParam == HTMENU && ci->flagChildMaximized)
    {
	x = LOWORD(lParam);
	
	NC_GetInsideRect(hwndFrame, &rect);
	if (x < rect.left + SYSMETRICS_CXSIZE)
	{
	    SendMessage(ci->hwndActiveChild, WM_SYSCOMMAND, 
			SC_CLOSE, lParam);
	    return TRUE;
	}
	else if (x >= rect.right - SYSMETRICS_CXSIZE)
	{
	    SendMessage(ci->hwndActiveChild, WM_SYSCOMMAND, 
			SC_RESTORE, lParam);
	    return TRUE;
	}
    }

    return FALSE;
}

/**********************************************************************
 *					MDIPaintMaximized
 */
LONG MDIPaintMaximized(HWND hwndFrame, HWND hwndClient, WORD message,
		       WORD wParam, LONG lParam)
{
    static HBITMAP hbitmapClose     = 0;
    static HBITMAP hbitmapMaximized = 0;
    
    MDICLIENTINFO *ci;
    WND           *w;
    HDC           hdc, hdcMem;
    RECT          rect;
    WND           *wndPtr = WIN_FindWndPtr(hwndFrame);

    w  = WIN_FindWndPtr(hwndClient);
    ci = (MDICLIENTINFO *) w->wExtra;

    dprintf_mdi(stddeb, "MDIPaintMaximized: frame "NPFMT",  client "NPFMT
		",  max flag %d,  menu %04x\n", hwndFrame, hwndClient, 
		ci->flagChildMaximized, wndPtr ? wndPtr->wIDmenu : 0);

    if (ci->flagChildMaximized && wndPtr && wndPtr->wIDmenu != 0)
    {
	NC_DoNCPaint(hwndFrame, wParam, TRUE);

	hdc = GetDCEx(hwndFrame, 0, DCX_CACHE | DCX_WINDOW);
	if (!hdc) return 0;

	hdcMem = CreateCompatibleDC(hdc);

	if (hbitmapClose == 0)
	{
	    hbitmapClose     = LoadBitmap(0, MAKEINTRESOURCE(OBM_OLD_CLOSE));
	    hbitmapMaximized = LoadBitmap(0, MAKEINTRESOURCE(OBM_RESTORE));
	}

	dprintf_mdi(stddeb, 
		    "MDIPaintMaximized: hdcMem "NPFMT", close bitmap "NPFMT", "
		    "maximized bitmap "NPFMT"\n",
		    hdcMem, hbitmapClose, hbitmapMaximized);

	NC_GetInsideRect(hwndFrame, &rect);
	rect.top += (wndPtr->dwStyle & WS_CAPTION) ? SYSMETRICS_CYSIZE + 1 : 0;
	SelectObject(hdcMem, hbitmapClose);
	BitBlt(hdc, rect.left, rect.top + 1, 
	       SYSMETRICS_CXSIZE, SYSMETRICS_CYSIZE,
	       hdcMem, 1, 1, SRCCOPY);
	
	NC_GetInsideRect(hwndFrame, &rect);
	rect.top += (wndPtr->dwStyle & WS_CAPTION) ? SYSMETRICS_CYSIZE + 1 : 0;
	rect.left   = rect.right - SYSMETRICS_CXSIZE;
	SelectObject(hdcMem, hbitmapMaximized);
	BitBlt(hdc, rect.left, rect.top + 1, 
	       SYSMETRICS_CXSIZE, SYSMETRICS_CYSIZE,
	       hdcMem, 1, 1, SRCCOPY);
	
	NC_GetInsideRect(hwndFrame, &rect);
	rect.top += (wndPtr->dwStyle & WS_CAPTION) ? SYSMETRICS_CYSIZE + 1 : 0;
	rect.left += SYSMETRICS_CXSIZE;
	rect.right -= SYSMETRICS_CXSIZE;
	rect.bottom = rect.top + SYSMETRICS_CYMENU;

	MENU_DrawMenuBar(hdc, &rect, hwndFrame, FALSE);
	
	DeleteDC(hdcMem);
	ReleaseDC(hwndFrame, hdc);
    }
    else
	return DefWindowProc(hwndFrame, message, wParam, lParam);

    return 0;
}

/**********************************************************************
 *					MDIClientWndProc
 *
 * This function is the handler for all MDI requests.
 */
LRESULT MDIClientWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    LPCREATESTRUCT       cs;
    LPCLIENTCREATESTRUCT ccs;
    MDICLIENTINFO       *ci;
    WND                 *w;

    w  = WIN_FindWndPtr(hwnd);
    ci = (MDICLIENTINFO *) w->wExtra;
    
    switch (message)
    {
      case WM_CHILDACTIVATE:
	return MDIChildActivated(w, ci, hwnd);

      case WM_CREATE:
	cs                      = (LPCREATESTRUCT) PTR_SEG_TO_LIN(lParam);
	ccs                     = (LPCLIENTCREATESTRUCT) PTR_SEG_TO_LIN(cs->lpCreateParams);
	ci->hWindowMenu         = ccs->hWindowMenu;
	ci->idFirstChild        = ccs->idFirstChild;
	ci->infoActiveChildren  = 0;
	ci->flagMenuAltered     = FALSE;
	ci->flagChildMaximized  = FALSE;
	w->dwStyle             |= WS_CLIPCHILDREN;

	GetClientRect(w->hwndParent, &ci->rectMaximize);
	MoveWindow(hwnd, 0, 0, 
		   ci->rectMaximize.right, ci->rectMaximize.bottom, 1);

	return 0;

      case WM_MDIACTIVATE:
	MDIBringChildToTop(hwnd, wParam, FALSE, FALSE);
	return 0;

      case WM_MDICASCADE:
	return MDICascade(hwnd, ci);

      case WM_MDICREATE:
	return (LONG)MDICreateChild(w, ci, hwnd, lParam );

      case WM_MDIDESTROY:
	return MDIDestroyChild(w, ci, hwnd, wParam, TRUE);

      case WM_MDIGETACTIVE:
	return ((LONG) ci->hwndActiveChild | 
		((LONG) ci->flagChildMaximized << 16));

      case WM_MDIICONARRANGE:
	return MDIIconArrange(hwnd);
	
      case WM_MDIMAXIMIZE:
	return MDIMaximizeChild(hwnd, wParam, ci);

      case WM_MDINEXT:
	MDIBringChildToTop(hwnd, wParam, FALSE, TRUE);
	break;
	
      case WM_MDIRESTORE:
	return MDIRestoreChild(hwnd, ci);

      case WM_MDISETMENU:
	return MDISetMenu(hwnd, wParam, LOWORD(lParam), HIWORD(lParam));
	
      case WM_MDITILE:
	return MDITile(hwnd, ci);
	
      case WM_NCACTIVATE:
	SendMessage(ci->hwndActiveChild, message, wParam, lParam);
	break;
	
      case WM_PARENTNOTIFY:
	if (wParam == WM_DESTROY)
#ifdef WINELIB32
	    return MDIDestroyChild(w, ci, hwnd, lParam, FALSE);
#else
	    return MDIDestroyChild(w, ci, hwnd, LOWORD(lParam), FALSE);
#endif
	else if (wParam == WM_LBUTTONDOWN)
	    MDIBringChildToTop(hwnd, ci->hwndHitTest, FALSE, FALSE);
	break;

      case WM_SIZE:
	GetClientRect(w->hwndParent, &ci->rectMaximize);
	break;

    }
    
    return DefWindowProc(hwnd, message, wParam, lParam);
}

/**********************************************************************
 *					DefFrameProc (USER.445)
 *
 */
LRESULT DefFrameProc(HWND hwnd, HWND hwndMDIClient, UINT message, 
		     WPARAM wParam, LPARAM lParam)
{
    if (hwndMDIClient)
    {
	switch (message)
	{
	  case WM_COMMAND:
	    MDIBringChildToTop(hwndMDIClient, wParam, TRUE, FALSE);
	    break;

	  case WM_NCLBUTTONDOWN:
	    if (MDIHandleLButton(hwnd, hwndMDIClient, wParam, lParam))
		return 0;
	    break;
	    
	  case WM_NCACTIVATE:
	    SendMessage(hwndMDIClient, message, wParam, lParam);
	    return MDIPaintMaximized(hwnd, hwndMDIClient, 
				     message, wParam, lParam);

	  case WM_NCPAINT:
	    return MDIPaintMaximized(hwnd, hwndMDIClient, 
				     message, wParam, lParam);
	
	  case WM_SETFOCUS:
	    SendMessage(hwndMDIClient, WM_SETFOCUS, wParam, lParam);
	    break;

	  case WM_SIZE:
	    MoveWindow(hwndMDIClient, 0, 0, 
		       LOWORD(lParam), HIWORD(lParam), TRUE);
	    break;
	}
    }
    
    return DefWindowProc(hwnd, message, wParam, lParam);
}

/**********************************************************************
 *					DefMDIChildProc (USER.447)
 *
 */
#ifdef WINELIB32
LONG DefMDIChildProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
#else
LONG DefMDIChildProc(HWND hwnd, WORD message, WORD wParam, LONG lParam)
#endif
{
    MDICLIENTINFO       *ci;
    WND                 *w;

    w  = WIN_FindWndPtr(GetParent(hwnd));
    ci = (MDICLIENTINFO *) w->wExtra;
    
    switch (message)
    {
      case WM_NCHITTEST:
	ci->hwndHitTest = hwnd;
	break;
	
      case WM_NCPAINT:
	NC_DoNCPaint( hwnd, hwnd == ci->hwndActiveChild, FALSE );
        return 0;

      case WM_SYSCOMMAND:
	switch (wParam)
	{
	  case SC_MAXIMIZE:
	    return SendMessage(GetParent(hwnd), WM_MDIMAXIMIZE, (WPARAM)hwnd, 0);

	  case SC_RESTORE:
	    return SendMessage(GetParent(hwnd), WM_MDIRESTORE, (WPARAM)hwnd, 0);
	}
	break;
	
    }
	
    return DefWindowProc(hwnd, message, wParam, lParam);
}

/**********************************************************************
 *					TranslateMDISysAccel (USER.451)
 *
 */
BOOL TranslateMDISysAccel(HWND hwndClient, LPMSG msg)
{
    return 0;
}


/***********************************************************************
 *           CalcChildScroll   (USER.462)
 */
void CalcChildScroll( HWND hwnd, WORD scroll )
{
    RECT childRect, clientRect;
    HWND hwndChild;

    GetClientRect( hwnd, &clientRect );
    SetRectEmpty( &childRect );
    hwndChild = GetWindow( hwnd, GW_CHILD );
    while (hwndChild)
    {
        WND *wndPtr = WIN_FindWndPtr( hwndChild );
        UnionRect( &childRect, &wndPtr->rectWindow, &childRect );
        hwndChild = wndPtr->hwndNext;
    }
    UnionRect( &childRect, &clientRect, &childRect );

    if ((scroll == SB_HORZ) || (scroll == SB_BOTH))
    {
        SetScrollRange( hwnd, SB_HORZ, childRect.left,
                        childRect.right - clientRect.right, FALSE );
        SetScrollPos( hwnd, SB_HORZ, clientRect.left - childRect.left, TRUE );
    }
    if ((scroll == SB_VERT) || (scroll == SB_BOTH))
    {
        SetScrollRange( hwnd, SB_VERT, childRect.top,
                        childRect.bottom - clientRect.bottom, FALSE );
        SetScrollPos( hwnd, SB_HORZ, clientRect.top - childRect.top, TRUE );
    }
}
