// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Unknwn.h>
#include <Windows.h>

struct TERMINAL_STARTUP_INFO
{
    BSTR pszTitle;
    BSTR pszIconPath;
    LONG iconIndex;
    DWORD dwX;
    DWORD dwY;
    DWORD dwXSize;
    DWORD dwYSize;
    DWORD dwXCountChars;
    DWORD dwYCountChars;
    DWORD dwFillAttribute;
    DWORD dwFlags;
    WORD wShowWindow;
};

// Interface ITerminalHandoff3
// {6F23DA90-15C5-4203-9DB0-64E73F1B1B00}
static const IID IID_ITerminalHandoff3 = {
    0x6F23DA90, 0x15C5, 0x4203, { 0x9D, 0xB0, 0x64, 0xE7, 0x3F, 0x1B, 0x1B, 0x00 }
};

struct ITerminalHandoff3: public IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE EstablishPtyHandoff(
        /* [out] */ HANDLE* in,
        /* [out] */ HANDLE* out,
        /* [in] */ HANDLE signal,
        /* [in] */ HANDLE reference,
        /* [in] */ HANDLE server,
        /* [in] */ HANDLE client,
        /* [in] */ const TERMINAL_STARTUP_INFO* startupInfo) = 0;
};
