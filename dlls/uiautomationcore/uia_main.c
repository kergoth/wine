/*
 * Copyright 2017 Jacek Caban for CodeWeavers
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

#include "uiautomationcore.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(uiautomation);

static BOOL disable_dll_hack(void)
{
    HKEY key;
    DWORD res;
    static const WCHAR enable_keyW[] =
        {'S','o','f','t','w','a','r','e',
         '\\','W','i','n','e',
         '\\','E','n','a','b','l','e','U','I','A','u','t','o','m','a','t','i','o','n','C','o','r','e',0};

    res = RegOpenKeyW(HKEY_CURRENT_USER, enable_keyW, &key);
    if(res == ERROR_SUCCESS) {
        RegCloseKey(key);
        return FALSE;
    }

    FIXME("CXHACK: Disabling uiautomationcore.dll.");
    return TRUE;
}

BOOL WINAPI DllMain(HINSTANCE hInstDLL, DWORD fdwReason, void *lpv)
{
    TRACE("(%p %d %p)\n", hInstDLL, fdwReason, lpv);

    switch(fdwReason) {
    case DLL_WINE_PREATTACH:
        return FALSE;  /* prefer native version */
    case DLL_PROCESS_ATTACH:
        if(disable_dll_hack())
            return FALSE;
        DisableThreadLibraryCalls(hInstDLL);
        break;
    }

    return TRUE;
}

/***********************************************************************
 *          UiaClientsAreListening (uiautomationcore.@)
 */
BOOL WINAPI UiaClientsAreListening(void)
{
    FIXME("()\n");
    return FALSE;
}
