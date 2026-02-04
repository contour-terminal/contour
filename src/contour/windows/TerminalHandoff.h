// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/windows/ITerminalHandoff.h>

#include <atomic>

// {B178D323-E77D-4C67-AF21-AE2B81F269F0}
static const CLSID CLSID_ContourTerminalHandoff = {
    0xB178D323, 0xE77D, 0x4C67, { 0xAF, 0x21, 0xAE, 0x2B, 0x81, 0xF2, 0x69, 0xF0 }
};

// {E686C757-9A35-4A1C-B3CE-0BCC8B5C69F4} - The Mystery IID from Logs
static const IID IID_ITerminalHandoff_Unknown = {
    0xE686C757, 0x9A35, 0x4A1C, { 0xB3, 0xCE, 0x0B, 0xCC, 0x8B, 0x5C, 0x69, 0xF4 }
};

// V1 Signature (By Value)
struct ITerminalHandoffV1: public IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE EstablishPtyHandoff(
        HANDLE in, HANDLE out, HANDLE signal, HANDLE reference, HANDLE server, HANDLE client) = 0;
};

class TerminalHandoff: public ITerminalHandoff3, public ITerminalHandoffV1
{
  public:
    TerminalHandoff();

    // IUnknown methods
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    // ITerminalHandoff3 methods (By Reference - We create pipes)
    HRESULT STDMETHODCALLTYPE EstablishPtyHandoff(HANDLE* in,
                                                  HANDLE* out,
                                                  HANDLE signal,
                                                  HANDLE reference,
                                                  HANDLE server,
                                                  HANDLE client,
                                                  const TERMINAL_STARTUP_INFO* startupInfo) override;

    // ITerminalHandoffV1 methods (By Value - Caller created pipes)
    HRESULT STDMETHODCALLTYPE EstablishPtyHandoff(
        HANDLE in, HANDLE out, HANDLE signal, HANDLE reference, HANDLE server, HANDLE client) override;

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
