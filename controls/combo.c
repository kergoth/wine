/*
 * Combo controls
 * 
 * Copyright 1993 Martin Ayotte
 * Copyright 1995 Bernd Schmidt
 * 
 */

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "windows.h"
#include "sysmetrics.h"
#include "combo.h"
#include "stackframe.h"
#include "user.h"
#include "win.h"
#include "graphics.h"
#include "listbox.h"
#include "dos_fs.h"
#include "stddebug.h"
#include "debug.h"

 /*
  * Note: Combos are probably implemented in a different way by Windows.
  * Using a message spy for Windows, you can see some undocumented
  * messages being passed between ComboBox and ComboLBox.
  * I hope no programs rely on the implementation of combos.
  */

static HBITMAP hComboBit = 0;
static WORD CBitHeight, CBitWidth;

static int COMBO_Init()
{
  BITMAP bm;
  
  dprintf_combo(stddeb, "COMBO_Init\n");
  hComboBit = LoadBitmap(0, MAKEINTRESOURCE(OBM_COMBO));
  GetObject(hComboBit, sizeof(BITMAP), (LPSTR)&bm);
  CBitHeight = bm.bmHeight;
  CBitWidth = bm.bmWidth;
  return 0;
}

LPHEADCOMBO ComboGetStorageHeader(HWND hwnd)
{
  return (LPHEADCOMBO)GetWindowLong(hwnd,4);
}

LPHEADLIST ComboGetListHeader(HWND hwnd)
{
  return (LPHEADLIST)GetWindowLong(hwnd,0);
}

int CreateComboStruct(HWND hwnd, LONG style)
{
  LPHEADCOMBO lphc;

  lphc = (LPHEADCOMBO)malloc(sizeof(HEADCOMBO));
  SetWindowLong(hwnd,4,(LONG)lphc);
  lphc->hWndEdit = 0;
  lphc->hWndLBox = 0;
  lphc->dwState = 0;
  lphc->LastSel = -1;
  lphc->dwStyle = style;
  lphc->DropDownVisible = FALSE;
  return TRUE;
}

void ComboUpdateWindow(HWND hwnd, LPHEADLIST lphl, LPHEADCOMBO lphc, BOOL repaint)
{
  SetScrollRange(lphc->hWndLBox, SB_VERT, 0, ListMaxFirstVisible(lphl), TRUE);
  if (repaint && lphl->bRedrawFlag) {
    InvalidateRect(hwnd, NULL, TRUE);
  }
}

/***********************************************************************
 *           CBNCCreate
 */
static LONG CBNCCreate(HWND hwnd, WORD wParam, LONG lParam)
{
  CREATESTRUCT *createStruct;

  if (!hComboBit) COMBO_Init();

  createStruct = (CREATESTRUCT *)PTR_SEG_TO_LIN(lParam);
  createStruct->style &= ~(WS_VSCROLL | WS_HSCROLL);
  SetWindowLong(hwnd, GWL_STYLE, createStruct->style);

  dprintf_combo(stddeb,"ComboBox WM_NCCREATE!\n");
  return DefWindowProc(hwnd, WM_NCCREATE, wParam, lParam);

}

/***********************************************************************
 *           CBCreate
 */
static LONG CBCreate(HWND hwnd, WORD wParam, LONG lParam)
{
  LPHEADLIST   lphl;
  LPHEADCOMBO  lphc;
  LONG         style = 0;
  LONG         cstyle = GetWindowLong(hwnd,GWL_STYLE);
  RECT         rect,lboxrect;
  char className[] = "COMBOLBOX";  /* Hack so that class names are > 0x10000 */
  char editName[] = "EDIT";

  /* translate combo into listbox styles */
  if (cstyle & CBS_OWNERDRAWFIXED) style |= LBS_OWNERDRAWFIXED;
  if (cstyle & CBS_OWNERDRAWVARIABLE) style |= LBS_OWNERDRAWVARIABLE;
  if (cstyle & CBS_SORT) style |= LBS_SORT;
  if (cstyle & CBS_HASSTRINGS) style |= LBS_HASSTRINGS;
  style |= LBS_NOTIFY;
  CreateListBoxStruct(hwnd, ODT_COMBOBOX, style, GetParent(hwnd));
  CreateComboStruct(hwnd,cstyle);
  lphl = ComboGetListHeader(hwnd);
  lphc = ComboGetStorageHeader(hwnd);

  GetClientRect(hwnd,&rect);
  GetWindowRect(hwnd,&lboxrect);
  /* FIXME: combos with edit controls are broken. */
  switch(cstyle & 3) {
   case CBS_SIMPLE:            /* edit control, list always visible  */
    dprintf_combo(stddeb,"CBS_SIMPLE\n");
    SetRectEmpty(&lphc->RectButton);
    lphc->LBoxTop = lphl->StdItemHeight;
    lphc->hWndEdit = CreateWindow(MAKE_SEGPTR(editName), (SEGPTR)0, 
				  WS_CHILD | WS_CLIPCHILDREN | WS_VISIBLE | SS_LEFT,
				  0, 0, rect.right, lphl->StdItemHeight,
				  hwnd, (HMENU)1, WIN_GetWindowInstance(hwnd), 0L);
    break;
   case CBS_DROPDOWN:          /* edit control, dropdown listbox     */
    dprintf_combo(stddeb,"CBS_DROPDOWN\n");
    lphc->RectButton = rect;
    lphc->RectButton.left = lphc->RectButton.right - 6 - CBitWidth;
    lphc->RectButton.bottom = lphc->RectButton.top + lphl->StdItemHeight;
    lphc->LBoxTop = lphl->StdItemHeight;
    SetWindowPos(hwnd, 0, 0, 0, rect.right - rect.left + 2*SYSMETRICS_CXBORDER,
		 lphl->StdItemHeight + 2*SYSMETRICS_CYBORDER,
		 SWP_NOMOVE | SWP_NOZORDER);
    lphc->hWndEdit = CreateWindow(MAKE_SEGPTR(editName), (SEGPTR)0,
				  WS_CHILD | WS_CLIPCHILDREN | WS_VISIBLE | SS_LEFT,
				  0, 0, lphc->RectButton.left, lphl->StdItemHeight,
				  hwnd, (HMENU)1, WIN_GetWindowInstance(hwnd), 0L);
    break;
   case CBS_DROPDOWNLIST:      /* static control, downdown listbox   */
    dprintf_combo(stddeb,"CBS_DROPDOWNLIST\n");
    lphc->RectButton = rect;
    lphc->RectButton.left = lphc->RectButton.right - 6 - CBitWidth;
    lphc->RectButton.bottom = lphc->RectButton.top + lphl->StdItemHeight;
    lphc->LBoxTop = lphl->StdItemHeight;
    SetWindowPos(hwnd, 0, 0, 0, rect.right - rect.left + 2*SYSMETRICS_CXBORDER,
		 lphl->StdItemHeight + 2*SYSMETRICS_CYBORDER,
		 SWP_NOMOVE | SWP_NOZORDER);
    break;
  }
  lboxrect.top += lphc->LBoxTop;
  /* FIXME: WinSight says these should be CHILD windows with the TOPMOST flag
   * set. Wine doesn't support TOPMOST, and simply setting the WS_CHILD
   * flag doesn't work. */
  lphc->hWndLBox = CreateWindow(MAKE_SEGPTR(className), (SEGPTR)0, 
				WS_POPUP | WS_BORDER | WS_VSCROLL,
				lboxrect.left, lboxrect.top,
				lboxrect.right - lboxrect.left, 
				lboxrect.bottom - lboxrect.top,
				0, 0, WIN_GetWindowInstance(hwnd),
				(SEGPTR)hwnd );
  ShowWindow(lphc->hWndLBox, SW_HIDE);
  dprintf_combo(stddeb,"Combo Creation LBox="NPFMT"!\n", lphc->hWndLBox);
  return 0;
}

/***********************************************************************
 *           CBDestroy
 */
static LONG CBDestroy(HWND hwnd, WORD wParam, LONG lParam)
{
  LPHEADLIST lphl = ComboGetListHeader(hwnd);

  ListBoxResetContent(lphl);
  DestroyListBoxStruct(lphl);
  dprintf_combo(stddeb,"Combo WM_DESTROY %p !\n", lphl);
  return 0;
}

/***********************************************************************
 *           CBPaint
 */
static LONG CBPaint(HWND hwnd, WORD wParam, LONG lParam)
{
  LPHEADLIST lphl = ComboGetListHeader(hwnd);
  LPHEADCOMBO lphc = ComboGetStorageHeader(hwnd);
  LPLISTSTRUCT lpls;
  PAINTSTRUCT  ps;
  HBRUSH hBrush;
  HFONT  hOldFont;
  HDC  hdc;
  RECT rect;
  int height;
  
  hdc = BeginPaint(hwnd, &ps);

  if (hComboBit != 0) {
    GRAPH_DrawReliefRect(hdc, &lphc->RectButton, 2, 2, FALSE);
    GRAPH_DrawBitmap(hdc, hComboBit,
		     lphc->RectButton.left + 3,lphc->RectButton.top + 2,
		     0, 0, CBitWidth, CBitHeight );
  }
  if (!IsWindowVisible(hwnd) || !lphl->bRedrawFlag 
      || (lphc->dwStyle & 3) != CBS_DROPDOWNLIST) 
  {
    /* we don't want to draw an entry when there is an edit control */
    EndPaint(hwnd, &ps);
    return 0;
  }

  hOldFont = SelectObject(hdc, lphl->hFont);

#ifdef WINELIB32
  hBrush = (HBRUSH) SendMessage(lphl->hParent, WM_CTLCOLORLISTBOX, (WPARAM)hdc,
				(LPARAM)hwnd);
#else
  hBrush = SendMessage(lphl->hParent, WM_CTLCOLOR, hdc,
		       MAKELONG(hwnd, CTLCOLOR_LISTBOX));
#endif
  if (hBrush == 0) hBrush = GetStockObject(WHITE_BRUSH);

  GetClientRect(hwnd, &rect);
  rect.right -= (lphc->RectButton.right - lphc->RectButton.left);
  FillRect(hdc, &rect, hBrush);

  lpls = ListBoxGetItem(lphl,lphl->ItemFocused);
  if (lpls != NULL) {  
    height = lpls->mis.itemHeight;
    rect.bottom = rect.top + height;

    if (lphl->OwnerDrawn) {
      ListBoxDrawItem (hwnd, lphl, hdc, lpls, &rect, ODA_DRAWENTIRE, 0);
    } else {
      ListBoxDrawItem (hwnd, lphl, hdc, lpls, &rect, ODA_DRAWENTIRE, 0);
    }
    if (GetFocus() == hwnd)
    ListBoxDrawItem (hwnd,lphl, hdc, lpls, &rect, ODA_FOCUS, ODS_FOCUS);
  }
  SelectObject(hdc,hOldFont);
  EndPaint(hwnd, &ps);
  return 0;
}

/***********************************************************************
 *           CBGetDlgCode
 */
static LONG CBGetDlgCode(HWND hwnd, WORD wParam, LONG lParam)
{
  return DLGC_WANTARROWS | DLGC_WANTCHARS;
}

/***********************************************************************
 *           CBLButtonDown
 */
static LONG CBLButtonDown(HWND hwnd, WORD wParam, LONG lParam)
{
  LPHEADCOMBO lphc = ComboGetStorageHeader(hwnd);
  SendMessage(hwnd,CB_SHOWDROPDOWN,!lphc->DropDownVisible,0);
  return 0;
}

/***********************************************************************
 *           CBKeyDown
 */
static LONG CBKeyDown(HWND hwnd, WORD wParam, LONG lParam)
{
  LPHEADLIST lphl = ComboGetListHeader(hwnd);
  WORD       newFocused = lphl->ItemFocused;

  switch(wParam) {
  case VK_HOME:
    newFocused = 0;
    break;
  case VK_END:
    newFocused = lphl->ItemsCount - 1;
    break;
  case VK_UP:
    if (newFocused > 0) newFocused--;
    break;
  case VK_DOWN:
    newFocused++;
    break;
  default:
    return 0;
  }

  if (newFocused >= lphl->ItemsCount)
    newFocused = lphl->ItemsCount - 1;
  
  ListBoxSetCurSel(lphl, newFocused);
  ListBoxSendNotification(lphl, hwnd, CBN_SELCHANGE);

  lphl->ItemFocused = newFocused;
  ListBoxScrollToFocus(lphl);
/*  SetScrollPos(hwnd, SB_VERT, lphl->FirstVisible, TRUE);*/
  InvalidateRect(hwnd, NULL, TRUE);

  return 0;
}

/***********************************************************************
 *           CBChar
 */
static LONG CBChar(HWND hwnd, WORD wParam, LONG lParam)
{
  LPHEADLIST lphl = ComboGetListHeader(hwnd);
  WORD       newFocused;

  newFocused = ListBoxFindNextMatch(lphl, wParam);
  if (newFocused == (WORD)LB_ERR) return 0;

  if (newFocused >= lphl->ItemsCount)
    newFocused = lphl->ItemsCount - 1;
  
  ListBoxSetCurSel(lphl, newFocused);
  ListBoxSendNotification(lphl, hwnd, CBN_SELCHANGE);
  lphl->ItemFocused = newFocused;
  ListBoxScrollToFocus(lphl);
  
/*  SetScrollPos(hwnd, SB_VERT, lphl->FirstVisible, TRUE);*/
  InvalidateRect(hwnd, NULL, TRUE);

  return 0;
}

/***********************************************************************
 *           CBKillFocus
 */
static LONG CBKillFocus(HWND hwnd, WORD wParam, LONG lParam)
{
  return 0;
}

/***********************************************************************
 *           CBSetFocus
 */
static LONG CBSetFocus(HWND hwnd, WORD wParam, LONG lParam)
{
  return 0;
}

/***********************************************************************
 *           CBResetContent
 */
static LONG CBResetContent(HWND hwnd, WORD wParam, LONG lParam)
{
  LPHEADLIST lphl = ComboGetListHeader(hwnd);
  LPHEADCOMBO lphc = ComboGetStorageHeader(hwnd);

  ListBoxResetContent(lphl);
  ComboUpdateWindow(hwnd, lphl, lphc, TRUE);
  return 0;
}

/***********************************************************************
 *           CBDir
 */
static LONG CBDir(HWND hwnd, WORD wParam, LONG lParam)
{
  WORD wRet;
  LPHEADLIST lphl = ComboGetListHeader(hwnd);
  LPHEADCOMBO lphc = ComboGetStorageHeader(hwnd);

  wRet = ListBoxDirectory(lphl, wParam, (LPSTR)PTR_SEG_TO_LIN(lParam));
  ComboUpdateWindow(hwnd, lphl, lphc, TRUE);
  return wRet;
}

/***********************************************************************
 *           CBInsertString
 */
static LONG CBInsertString(HWND hwnd, WORD wParam, LONG lParam)
{
  WORD  wRet;
  LPHEADLIST lphl = ComboGetListHeader(hwnd);
  LPHEADCOMBO lphc = ComboGetStorageHeader(hwnd);

  if (lphl->HasStrings)
    wRet = ListBoxInsertString(lphl, wParam, (LPSTR)PTR_SEG_TO_LIN(lParam));
  else
    wRet = ListBoxInsertString(lphl, wParam, (LPSTR)lParam);

  ComboUpdateWindow(hwnd, lphl, lphc, TRUE);
  return wRet;
}

/***********************************************************************
 *           CBAddString
 */
static LONG CBAddString(HWND hwnd, WORD wParam, LONG lParam)
{
  WORD  wRet;
  LPHEADLIST lphl = ComboGetListHeader(hwnd);
  LPHEADCOMBO lphc = ComboGetStorageHeader(hwnd);

  if (lphl->HasStrings)
    wRet = ListBoxAddString(lphl, (LPSTR)PTR_SEG_TO_LIN(lParam));
  else
    wRet = ListBoxAddString(lphl, (LPSTR)lParam);

  ComboUpdateWindow(hwnd, lphl, lphc, TRUE);
  return wRet;
}

/***********************************************************************
 *           CBDeleteString
 */
static LONG CBDeleteString(HWND hwnd, WORD wParam, LONG lParam)
{
  LPHEADLIST lphl = ComboGetListHeader(hwnd);
  LPHEADCOMBO lphc = ComboGetStorageHeader(hwnd);
  LONG lRet = ListBoxDeleteString(lphl,wParam);
  
  ComboUpdateWindow(hwnd, lphl, lphc, TRUE);
  return lRet;
}

/***********************************************************************
 *           CBSelectString
 */
static LONG CBSelectString(HWND hwnd, WORD wParam, LONG lParam)
{
  LPHEADLIST lphl = ComboGetListHeader(hwnd);
  WORD  wRet;

  wRet = ListBoxFindString(lphl, wParam, lParam);

  /* XXX add functionality here */

  return 0;
}

/***********************************************************************
 *           CBFindString
 */
static LONG CBFindString(HWND hwnd, WORD wParam, LONG lParam)
{
  LPHEADLIST lphl = ComboGetListHeader(hwnd);
  return ListBoxFindString(lphl, wParam, lParam);
}

/***********************************************************************
 *           CBGetCount
 */
static LONG CBGetCount(HWND hwnd, WORD wParam, LONG lParam)
{
  LPHEADLIST lphl = ComboGetListHeader(hwnd);
  return lphl->ItemsCount;
}

/***********************************************************************
 *           CBSetCurSel
 */
static LONG CBSetCurSel(HWND hwnd, WORD wParam, LONG lParam)
{
  LPHEADLIST lphl = ComboGetListHeader(hwnd);
  WORD  wRet;

  wRet = ListBoxSetCurSel(lphl, wParam);

/*  SetScrollPos(hwnd, SB_VERT, lphl->FirstVisible, TRUE);*/
  InvalidateRect(hwnd, NULL, TRUE);

  return wRet;
}

/***********************************************************************
 *           CBGetCurSel
 */
static LONG CBGetCurSel(HWND hwnd, WORD wParam, LONG lParam)
{
  LPHEADLIST lphl = ComboGetListHeader(hwnd);
  return lphl->ItemFocused;
}

/***********************************************************************
 *           CBGetItemHeight
 */
static LONG CBGetItemHeight(HWND hwnd, WORD wParam, LONG lParam)
{
  LPHEADLIST lphl = ComboGetListHeader(hwnd);
  LPLISTSTRUCT lpls = ListBoxGetItem (lphl, wParam);

  if (lpls == NULL) return LB_ERR;
  return lpls->mis.itemHeight;
}

/***********************************************************************
 *           CBSetItemHeight
 */
static LONG CBSetItemHeight(HWND hwnd, WORD wParam, LONG lParam)
{
  LPHEADLIST lphl = ComboGetListHeader(hwnd);
  return ListBoxSetItemHeight(lphl, wParam, lParam);
}

/***********************************************************************
 *           CBSetRedraw
 */
static LONG CBSetRedraw(HWND hwnd, WORD wParam, LONG lParam)
{
  LPHEADLIST lphl = ComboGetListHeader(hwnd);
  lphl->bRedrawFlag = wParam;
  return 0;
}

/***********************************************************************
 *           CBSetFont
 */
static LONG CBSetFont(HWND hwnd, WPARAM wParam, LPARAM lParam)
{
  LPHEADLIST  lphl = ComboGetListHeader(hwnd);

  if (wParam == 0)
    lphl->hFont = GetStockObject(SYSTEM_FONT);
  else
    lphl->hFont = (HFONT)wParam;

  return 0;
}

/***********************************************************************
 *           CBGetLBTextLen
 */
static LONG CBGetLBTextLen(HWND hwnd, WORD wParam, LONG lParam)
{
  LPHEADLIST   lphl = ComboGetListHeader(hwnd);
  LPLISTSTRUCT lpls = ListBoxGetItem(lphl,wParam);

  if (lpls == NULL || !lphl->HasStrings) return LB_ERR;
  return strlen(lpls->itemText);
}

/***********************************************************************
 *           CBGetLBText
 */
static LONG CBGetLBText(HWND hwnd, WORD wParam, LONG lParam)
{
  LPHEADLIST lphl = ComboGetListHeader(hwnd);
  return ListBoxGetText(lphl, wParam, (LPSTR)PTR_SEG_TO_LIN(lParam));
}

/***********************************************************************
 *           CBGetItemData
 */
static LONG CBGetItemData(HWND hwnd, WORD wParam, LONG lParam)
{
  LPHEADLIST lphl = ComboGetListHeader(hwnd);
  return ListBoxGetItemData(lphl, wParam);
}

/***********************************************************************
 *           CBSetItemData
 */
static LONG CBSetItemData(HWND hwnd, WORD wParam, LONG lParam)
{
  LPHEADLIST lphl = ComboGetListHeader(hwnd);
  return ListBoxSetItemData(lphl, wParam, lParam);
}

/***********************************************************************
 *           CBShowDropDown
 */
static LONG CBShowDropDown(HWND hwnd, WORD wParam, LONG lParam)
{
  LPHEADCOMBO lphc = ComboGetStorageHeader(hwnd);
  RECT rect;
  
  if (lphc->dwStyle & 3 == CBS_SIMPLE) return LB_ERR;
  
  wParam = !!wParam;
  if (wParam != lphc->DropDownVisible) {
    lphc->DropDownVisible = wParam;
    GetWindowRect(hwnd,&rect);
    SetWindowPos(lphc->hWndLBox, 0, rect.left, rect.top+lphc->LBoxTop, 0, 0,
		 SWP_NOSIZE | (wParam ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
    if (!wParam) SetFocus(hwnd);
  }
  return 0;
}


/***********************************************************************
 *           ComboWndProc
 */
LRESULT ComboBoxWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch(message) {	
     case WM_NCCREATE: return CBNCCreate(hwnd, wParam, lParam);
     case WM_CREATE: return CBCreate(hwnd, wParam, lParam);
     case WM_DESTROY: return CBDestroy(hwnd, wParam, lParam);
     case WM_GETDLGCODE: return CBGetDlgCode(hwnd, wParam, lParam);
     case WM_KEYDOWN: return CBKeyDown(hwnd, wParam, lParam);
     case WM_CHAR: return CBChar(hwnd, wParam, lParam);
     case WM_SETFONT: return CBSetFont(hwnd, wParam, lParam);
     case WM_SETREDRAW: return CBSetRedraw(hwnd, wParam, lParam);
     case WM_PAINT: return CBPaint(hwnd, wParam, lParam);
     case WM_LBUTTONDOWN: return CBLButtonDown(hwnd, wParam, lParam);
     case WM_SETFOCUS: return CBSetFocus(hwnd, wParam, lParam);
     case WM_KILLFOCUS: return CBKillFocus(hwnd, wParam, lParam);
     case CB_RESETCONTENT: return CBResetContent(hwnd, wParam, lParam);
     case CB_DIR: return CBDir(hwnd, wParam, lParam);
     case CB_ADDSTRING: return CBAddString(hwnd, wParam, lParam);
     case CB_INSERTSTRING: return CBInsertString(hwnd, wParam, lParam);
     case CB_DELETESTRING: return CBDeleteString(hwnd, wParam, lParam);
     case CB_FINDSTRING: return CBFindString(hwnd, wParam, lParam);
     case CB_GETCOUNT: return CBGetCount(hwnd, wParam, lParam);
     case CB_GETCURSEL: return CBGetCurSel(hwnd, wParam, lParam);
     case CB_GETITEMDATA: return CBGetItemData(hwnd, wParam, lParam);
     case CB_GETITEMHEIGHT: return CBGetItemHeight(hwnd, wParam, lParam);
     case CB_GETLBTEXT: return CBGetLBText(hwnd, wParam, lParam);
     case CB_GETLBTEXTLEN: return CBGetLBTextLen(hwnd, wParam, lParam);
     case CB_SELECTSTRING: return CBSelectString(hwnd, wParam, lParam);
     case CB_SETITEMDATA: return CBSetItemData(hwnd, wParam, lParam);
     case CB_SETCURSEL: return CBSetCurSel(hwnd, wParam, lParam);
     case CB_SETITEMHEIGHT: return CBSetItemHeight(hwnd, wParam, lParam);
     case CB_SHOWDROPDOWN: return CBShowDropDown(hwnd, wParam, lParam);
    }
    return DefWindowProc(hwnd, message, wParam, lParam);
}

/*--------------------------------------------------------------------*/
/* ComboLBox code starts here */

HWND CLBoxGetCombo(HWND hwnd)
{
#ifdef WINELIB32
  return (HWND)GetWindowLong(hwnd,0);
#else
  return (HWND)GetWindowWord(hwnd,0);
#endif
}

LPHEADLIST CLBoxGetListHeader(HWND hwnd)
{
  return ComboGetListHeader(CLBoxGetCombo(hwnd));
}

/***********************************************************************
 *           CBLCreate
 */
static LONG CBLCreate( HWND hwnd, WORD wParam, LONG lParam )
{
  CREATESTRUCT *createStruct = (CREATESTRUCT *)PTR_SEG_TO_LIN(lParam);
  SetWindowWord(hwnd,0,LOWORD(createStruct->lpCreateParams));
  return 0;
}

/***********************************************************************
 *           CBLGetDlgCode
 */
static LONG CBLGetDlgCode( HWND hwnd, WORD wParam, LONG lParam )
{
  return DLGC_WANTARROWS | DLGC_WANTCHARS;
}

/***********************************************************************
 *           CBLKeyDown
 */
static LONG CBLKeyDown( HWND hwnd, WORD wParam, LONG lParam ) 
{
  LPHEADLIST lphl = CLBoxGetListHeader(hwnd);
  WORD newFocused = lphl->ItemFocused;

  switch(wParam) {
  case VK_HOME:
    newFocused = 0;
    break;
  case VK_END:
    newFocused = lphl->ItemsCount - 1;
    break;
  case VK_UP:
    if (newFocused > 0) newFocused--;
    break;
  case VK_DOWN:
    newFocused++;
    break;
  case VK_PRIOR:
    if (newFocused > lphl->ItemsVisible) {
      newFocused -= lphl->ItemsVisible;
    } else {
      newFocused = 0;
    }
    break;
  case VK_NEXT:
    newFocused += lphl->ItemsVisible;
    break;
  default:
    return 0;
  }

  if (newFocused >= lphl->ItemsCount)
    newFocused = lphl->ItemsCount - 1;
  
  ListBoxSetCurSel(lphl, newFocused);
  ListBoxSendNotification(lphl, hwnd, CBN_SELCHANGE);

  lphl->ItemFocused = newFocused;
  ListBoxScrollToFocus(lphl);
  SetScrollPos(hwnd, SB_VERT, lphl->FirstVisible, TRUE);
  InvalidateRect(hwnd, NULL, TRUE);  
  return 0;
}

/***********************************************************************
 *           CBLChar
 */
static LONG CBLChar( HWND hwnd, WORD wParam, LONG lParam )
{
  return 0;
}

/***********************************************************************
 *           CBLPaint
 */
static LONG CBLPaint( HWND hwnd, WORD wParam, LONG lParam )
{
  LPHEADLIST   lphl = CLBoxGetListHeader(hwnd);
  LPLISTSTRUCT lpls;
  PAINTSTRUCT  ps;
  HBRUSH       hBrush;
  HFONT        hOldFont;
  HWND  combohwnd = CLBoxGetCombo(hwnd);
  HDC 	hdc;
  RECT 	rect;
  int   i, top, height;

  top = 0;
  hdc = BeginPaint( hwnd, &ps );

  if (!IsWindowVisible(hwnd) || !lphl->bRedrawFlag) {
    EndPaint(hwnd, &ps);
    return 0;
  }

  hOldFont = SelectObject(hdc, lphl->hFont);
#ifdef WINELIB32
  hBrush = (HBRUSH) SendMessage(lphl->hParent, WM_CTLCOLORLISTBOX, (WPARAM)hdc,
				(LPARAM)hwnd);
#else
  hBrush = SendMessage(lphl->hParent, WM_CTLCOLOR, hdc,
		       MAKELONG(hwnd, CTLCOLOR_LISTBOX));
#endif

  if (hBrush == 0) hBrush = GetStockObject(WHITE_BRUSH);

  GetClientRect(hwnd, &rect);
  FillRect(hdc, &rect, hBrush);

  lpls = lphl->lpFirst;

  lphl->ItemsVisible = 0;
  for(i = 0; i < lphl->ItemsCount; i++) {
    if (lpls == NULL) break;

    if (i >= lphl->FirstVisible) {
      height = lpls->mis.itemHeight;

      if (top > rect.bottom) break;
      lpls->itemRect.top    = top;
      lpls->itemRect.bottom = top + height;
      lpls->itemRect.left   = rect.left;
      lpls->itemRect.right  = rect.right;

      dprintf_listbox(stddeb,"drawing item: %d %d %d %d %d\n",rect.left,top,rect.right,top+height,lpls->itemState);
      if (lphl->OwnerDrawn) {
	ListBoxDrawItem (combohwnd, lphl, hdc, lpls, &lpls->itemRect, ODA_DRAWENTIRE, 0);
	if (lpls->itemState)
	  ListBoxDrawItem (combohwnd, lphl, hdc, lpls, &lpls->itemRect, ODA_SELECT, ODS_SELECTED);
      } else {
	ListBoxDrawItem (combohwnd, lphl, hdc, lpls, &lpls->itemRect, ODA_DRAWENTIRE, 
			 lpls->itemState);
      }
      if ((lphl->ItemFocused == i) && GetFocus() == hwnd)
	ListBoxDrawItem (combohwnd, lphl, hdc, lpls, &lpls->itemRect, ODA_FOCUS, ODS_FOCUS);

      top += height;
      lphl->ItemsVisible++;
    }

    lpls = lpls->lpNext;
  }
  SelectObject(hdc,hOldFont);
  EndPaint( hwnd, &ps );
  return 0;

}

/***********************************************************************
 *           CBLKillFocus
 */
static LONG CBLKillFocus( HWND hwnd, WORD wParam, LONG lParam )
{
/*  SendMessage(CLBoxGetCombo(hwnd),CB_SHOWDROPDOWN,0,0);*/
  return 0;
}

/***********************************************************************
 *           CBLActivate
 */
static LONG CBLActivate( HWND hwnd, WORD wParam, LONG lParam )
{
  if (wParam == WA_INACTIVE)
    SendMessage(CLBoxGetCombo(hwnd),CB_SHOWDROPDOWN,0,0);
  return 0;
}

/***********************************************************************
 *           CBLLButtonDown
 */
static LONG CBLLButtonDown( HWND hwnd, WORD wParam, LONG lParam )
{
  LPHEADLIST lphl = CLBoxGetListHeader(hwnd);
  int        y;
  RECT       rectsel;

  SetFocus(hwnd);
  SetCapture(hwnd);

  lphl->PrevFocused = lphl->ItemFocused;

  y = ListBoxFindMouse(lphl, LOWORD(lParam), HIWORD(lParam));
  if (y == -1)
    return 0;

  ListBoxSetCurSel(lphl, y);
  ListBoxGetItemRect(lphl, y, &rectsel);

  InvalidateRect(hwnd, NULL, TRUE);
  return 0;
}

/***********************************************************************
 *           CBLLButtonUp
 */
static LONG CBLLButtonUp( HWND hwnd, WORD wParam, LONG lParam )
{
  LPHEADLIST lphl = CLBoxGetListHeader(hwnd);

  if (GetCapture() == hwnd) ReleaseCapture();

  if(!lphl)
     {
      fprintf(stdnimp,"CBLLButtonUp: CLBoxGetListHeader returned NULL!\n");
     }
  else if (lphl->PrevFocused != lphl->ItemFocused) 
          {
      		SendMessage(CLBoxGetCombo(hwnd),CB_SETCURSEL,lphl->ItemFocused,0);
      		ListBoxSendNotification(lphl, CLBoxGetCombo(hwnd), CBN_SELCHANGE);
     	  }

  SendMessage(CLBoxGetCombo(hwnd),CB_SHOWDROPDOWN,0,0);

  return 0;
}

/***********************************************************************
 *           CBLMouseMove
 */
static LONG CBLMouseMove( HWND hwnd, WORD wParam, LONG lParam )
{
  LPHEADLIST lphl = CLBoxGetListHeader(hwnd);
  int  y;
  WORD wRet;
  RECT rect, rectsel;   /* XXX Broken */

  if ((wParam & MK_LBUTTON) != 0) {
    y = SHIWORD(lParam);
    if (y < 0) {
      if (lphl->FirstVisible > 0) {
	lphl->FirstVisible--;
	SetScrollPos(hwnd, SB_VERT, lphl->FirstVisible, TRUE);
	InvalidateRect(hwnd, NULL, TRUE);
	return 0;
      }
    }
    GetClientRect(hwnd, &rect);
    if (y >= rect.bottom) {
      if (lphl->FirstVisible < ListMaxFirstVisible(lphl)) {
	lphl->FirstVisible++;
	SetScrollPos(hwnd, SB_VERT, lphl->FirstVisible, TRUE);
	InvalidateRect(hwnd, NULL, TRUE);
	return 0;
      }
    }
    if ((y > 0) && (y < (rect.bottom - 4))) {
      if ((y < rectsel.top) || (y > rectsel.bottom)) {
	wRet = ListBoxFindMouse(lphl, LOWORD(lParam), HIWORD(lParam));
	if (wRet == lphl->ItemFocused) return 0;
	ListBoxSetCurSel(lphl, wRet);
	ListBoxGetItemRect(lphl, wRet, &rectsel);
	InvalidateRect(hwnd, NULL, TRUE);
      }
    }
  }

  return 0;
}

/***********************************************************************
 *           CBLVScroll
 */
static LONG CBLVScroll( HWND hwnd, WORD wParam, LONG lParam )
{
  LPHEADLIST lphl = CLBoxGetListHeader(hwnd);
  int  y;

  y = lphl->FirstVisible;

  switch(wParam) {
  case SB_LINEUP:
    if (lphl->FirstVisible > 0)
      lphl->FirstVisible--;
    break;

  case SB_LINEDOWN:
    lphl->FirstVisible++;
    break;

  case SB_PAGEUP:
    if (lphl->FirstVisible > lphl->ItemsVisible) {
      lphl->FirstVisible -= lphl->ItemsVisible;
    } else {
      lphl->FirstVisible = 0;
    }
    break;

  case SB_PAGEDOWN:
    lphl->FirstVisible += lphl->ItemsVisible;
    break;

  case SB_THUMBTRACK:
    lphl->FirstVisible = LOWORD(lParam);
    break;
  }

  if (lphl->FirstVisible > ListMaxFirstVisible(lphl))
    lphl->FirstVisible = ListMaxFirstVisible(lphl);

  if (y != lphl->FirstVisible) {
    SetScrollPos(hwnd, SB_VERT, lphl->FirstVisible, TRUE);
    InvalidateRect(hwnd, NULL, TRUE);
  }

  return 0;
}

/***********************************************************************
 *           ComboLBoxWndProc
 */
LRESULT ComboLBoxWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch(message) {	
     case WM_CREATE: return CBLCreate(hwnd, wParam, lParam);
     case WM_GETDLGCODE: return CBLGetDlgCode(hwnd, wParam, lParam);
     case WM_KEYDOWN: return CBLKeyDown(hwnd, wParam, lParam);
     case WM_CHAR: return CBLChar(hwnd, wParam, lParam);
     case WM_PAINT: return CBLPaint(hwnd, wParam, lParam);
     case WM_KILLFOCUS: return CBLKillFocus(hwnd, wParam, lParam);
     case WM_ACTIVATE: return CBLActivate(hwnd, wParam, lParam);
     case WM_LBUTTONDOWN: return CBLLButtonDown(hwnd, wParam, lParam);
     case WM_LBUTTONUP: return CBLLButtonUp(hwnd, wParam, lParam);
     case WM_MOUSEMOVE: return CBLMouseMove(hwnd, wParam, lParam);
     case WM_VSCROLL: return CBLVScroll(hwnd, wParam, lParam);
    }
    return DefWindowProc(hwnd, message, wParam, lParam);
}

/************************************************************************
 * 			       	DlgDirSelectComboBox	[USER.194]
 */
BOOL DlgDirSelectComboBox(HWND hDlg, LPSTR lpStr, int nIDLBox)
{
	fprintf(stdnimp,"DlgDirSelectComboBox("NPFMT", '%s', %d) \n",
				hDlg, lpStr, nIDLBox);
	return TRUE;
}


/************************************************************************
 * 					DlgDirListComboBox     [USER.195]
 */
int DlgDirListComboBox(HWND hDlg, SEGPTR PathSpec,
		       int nIDLBox, int nIDStat, WORD wType)
{
  HWND	hWnd;
  int ret;
  LPSTR lpPathSpec = PTR_SEG_TO_LIN(PathSpec);

  dprintf_combo(stddeb,"DlgDirListComboBox("NPFMT", '%s', %d, %d, %04X) \n",
		  hDlg, lpPathSpec, nIDLBox, nIDStat, wType);
  if (nIDLBox) {
    LPHEADLIST lphl;
    LPHEADCOMBO lphc;
    hWnd = GetDlgItem(hDlg, nIDLBox);
    lphl = ComboGetListHeader(hWnd);
    lphc = ComboGetStorageHeader(hWnd);
    ListBoxResetContent(lphl);
    ret = ListBoxDirectory(lphl, wType, lpPathSpec);
    ComboUpdateWindow(hWnd, lphl, lphc, TRUE);
  } else {
    ret = 0;
  }
  if (nIDStat) {
      int drive;
      HANDLE hTemp;
      char *temp;
      drive = DOS_GetDefaultDrive();
      hTemp = USER_HEAP_ALLOC( 256 );
      temp = (char *) USER_HEAP_LIN_ADDR( hTemp );
      strcpy( temp+3, DOS_GetCurrentDir(drive) );
      if( temp[3] == '\\' ) {
	temp[1] = 'A'+drive;
	temp[2] = ':';
	SendDlgItemMessage( hDlg, nIDStat, WM_SETTEXT, 0,
                            (LPARAM)(USER_HEAP_SEG_ADDR(hTemp) + 1) );
      } else {
	temp[0] = 'A'+drive;
	temp[1] = ':';
	temp[2] = '\\';
	SendDlgItemMessage( hDlg, nIDStat, WM_SETTEXT, 0,
                            (LPARAM)USER_HEAP_SEG_ADDR(hTemp) );
      }
      USER_HEAP_FREE( hTemp );
  } 
  return ret;
}
