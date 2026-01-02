// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/windows/ITerminalHandoff.h>

#include <atomic>

// {B178D323-E77D-4C67-AF21-AE2B81F269F0}
static const CLSID CLSID_ContourTerminalHandoff = {
    0xB178D323, 0xE77D, 0x4C67, { 0xAF, 0x21, 0xAE, 0x2B, 0x81, 0xF2, 0x69, 0xF0 }
};

class TerminalHandoff: public ITerminalHandoff3
{
  public:
    TerminalHandoff();

    // IUnknown methods
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    // ITerminalHandoff3 methods
    HRESULT STDMETHODCALLTYPE EstablishPtyHandoff(HANDLE* in,
                                                  HANDLE* out,
                                                  HANDLE signal,
                                                  HANDLE reference,
                                                  HANDLE server,
                                                  HANDLE client,
                                                  const TERMINAL_STARTUP_INFO* startupInfo) override;

  private:
    std::atomic<ULONG> _refCount = 1;
};

class TerminalHandoffFactory: public IClassFactory
{
  public:
    // IUnknown methods
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    // IClassFactory methods
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppvObject) override;
    HRESULT STDMETHODCALLTYPE LockServer(BOOL fLock) override;

  private:
    std::atomic<ULONG> _refCount = 1;
};
