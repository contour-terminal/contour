// SPDX-License-Identifier: Apache-2.0
#include <contour/windows/TerminalHandoff.h>

#include <atomic>
#include <format>
#include <string>
#include <utility>

extern void SimpleFileLogger(std::string const& msg);

// Forward declaration of handler in the main application
// Forward declaration of the global handler in ContourGuiApp.cpp
void ContourHandleHandoff(HANDLE hInput,
                          HANDLE hOutput,
                          HANDLE hSignal,
                          HANDLE hReference,
                          HANDLE hServer,
                          HANDLE hClient,
                          std::wstring const& title);

static HANDLE duplicateHandle(HANDLE h)
{
    HANDLE dup = INVALID_HANDLE_VALUE;
    if (h != INVALID_HANDLE_VALUE)
    {
        if (!DuplicateHandle(
                GetCurrentProcess(), h, GetCurrentProcess(), &dup, 0, FALSE, DUPLICATE_SAME_ACCESS))
        {
            SimpleFileLogger(std::format(
                "duplicateHandle: Failed to duplicate handle {:p}. Error: {}", h, GetLastError()));
            return INVALID_HANDLE_VALUE;
        }
    }
    return dup;
}

// --- TerminalHandoff ---

TerminalHandoff::TerminalHandoff() = default;

extern void SimpleFileLogger(std::string const& msg);

static std::string GuidToString(REFIID iid)
{
    LPOLESTR str = nullptr;
    if (StringFromIID(iid, &str) == S_OK)
    {
        std::wstring w(str);
        CoTaskMemFree(str);
        std::string s(w.begin(), w.end());
        return s;
    }
    return "{???}";
}

HRESULT STDMETHODCALLTYPE TerminalHandoff::QueryInterface(REFIID riid, void** ppvObject)
{
    SimpleFileLogger(std::format("TerminalHandoff::QueryInterface request: {}", GuidToString(riid)));
    if (!ppvObject)
        return E_POINTER;
    if (IsEqualIID(riid, IID_IUnknown))
    {
        *ppvObject = static_cast<ITerminalHandoffV1*>(this);
        AddRef();
        return S_OK;
    }

    // ITerminalHandoff (V1) {59D55CCE-FC8A-48B4-ACE8-0A9286C6557F}
    static const IID IID_ITerminalHandoffV1_Real = {
        0x59D55CCE, 0xFC8A, 0x48B4, { 0xAC, 0xE8, 0x0A, 0x92, 0x86, 0xC6, 0x55, 0x7F }
    };
    if (IsEqualIID(riid, IID_ITerminalHandoffV1_Real))
    {
        SimpleFileLogger("TerminalHandoff::QueryInterface: Accepting ITerminalHandoff (V1)");
        *ppvObject = static_cast<ITerminalHandoffV1*>(this);
        AddRef();
        return S_OK;
    }

    // ITerminalHandoff3 (V3) {6F23DA90-15C5-4203-9DB0-64E73F1B1B00}
    // Also matches ITerminalHandoff3 interface layout (HANDLE* for in/out)
    static const IID IID_ITerminalHandoff3_Real = {
        0x6F23DA90, 0x15C5, 0x4203, { 0x9D, 0xB0, 0x64, 0xE7, 0x3F, 0x1B, 0x1B, 0x00 }
    };

    if (IsEqualIID(riid, IID_ITerminalHandoff3) || IsEqualIID(riid, IID_ITerminalHandoff3_Real))
    {
        SimpleFileLogger("TerminalHandoff::QueryInterface: Accepting ITerminalHandoff3 (V3)");
        *ppvObject = static_cast<ITerminalHandoff3*>(this);
        AddRef();
        return S_OK;
    }

    // Explicitly Log and Reject IConsoleHandoff {E686C757...}
    static const IID IID_IConsoleHandoff = {
        0xE686C757, 0x9A35, 0x4A1C, { 0xB3, 0xCE, 0x0B, 0xCC, 0x8B, 0x5C, 0x69, 0xF4 }
    };
    if (IsEqualIID(riid, IID_IConsoleHandoff))
    {
        SimpleFileLogger(
            "TerminalHandoff::QueryInterface: Rejecting IConsoleHandoff (E686C...) - Wrong Interface!");
        *ppvObject = nullptr;
        return E_NOINTERFACE;
    }

    if (IsEqualIID(riid, IID_IMarshal))
    {
        // Standard marshalling request, log less verbosely
        // SimpleFileLogger("TerminalHandoff::QueryInterface: IID_IMarshal");
    }
    else
    {
        SimpleFileLogger("TerminalHandoff::QueryInterface: E_NOINTERFACE");
    }
    *ppvObject = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE TerminalHandoff::AddRef()
{
    return ++_refCount;
}

ULONG STDMETHODCALLTYPE TerminalHandoff::Release()
{
    ULONG count = --_refCount;
    if (count == 0)
        delete this;
    return count;
}

HRESULT STDMETHODCALLTYPE TerminalHandoff::EstablishPtyHandoff(HANDLE* in,
                                                               HANDLE* out,
                                                               HANDLE signal,
                                                               HANDLE reference,
                                                               HANDLE server,
                                                               HANDLE client,
                                                               const TERMINAL_STARTUP_INFO* startupInfo)
{
    SimpleFileLogger("TerminalHandoff::EstablishPtyHandoff (V3) called!");

    if (!in || !out)
        return E_POINTER;

    // Create In Pipe (Server Write, Client Read)
    SECURITY_ATTRIBUTES sa = { sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE }; // Inheritable
    HANDLE hInRead, hInWrite;
    if (!CreatePipe(&hInRead, &hInWrite, &sa, 0))
    {
        SimpleFileLogger("EstablishPtyHandoff: Failed to create IN pipe.");
        return E_FAIL;
    }

    // Create Out Pipe (Server Read, Client Write)
    HANDLE hOutRead, hOutWrite;
    if (!CreatePipe(&hOutRead, &hOutWrite, &sa, 0))
    {
        SimpleFileLogger("EstablishPtyHandoff: Failed to create OUT pipe.");
        CloseHandle(hInRead);
        CloseHandle(hInWrite);
        return E_FAIL;
    }

    // Assign to [out] params.
    // The COM stub will duplicate these handles for the caller and close the originals on our side.
    *in = hInRead;
    *out = hOutWrite;

    // We KEEP Server ends (InWrite, OutRead) for our usage.
    // Pass them to Contour handoff logic.

    HANDLE hSignalDup = duplicateHandle(signal);
    HANDLE hReferenceDup = duplicateHandle(reference);
    HANDLE hServerDup = duplicateHandle(server);
    HANDLE hClientDup = duplicateHandle(client);

    std::wstring title;
    if (startupInfo && startupInfo->pszTitle)
        title = startupInfo->pszTitle;

    SimpleFileLogger("TerminalHandoff: Calling ContourHandleHandoff...");

    // NOTE: 'hInWrite' is what I write to (Client Input). 'hOutRead' is where I read from (Client Output).
    ContourHandleHandoff(hInWrite, hOutRead, hSignalDup, hReferenceDup, hServerDup, hClientDup, title);

    SimpleFileLogger("TerminalHandoff: ContourHandleHandoff returned.");

    return S_OK;
}

// ITerminalHandoffV1 implementation
HRESULT STDMETHODCALLTYPE TerminalHandoff::EstablishPtyHandoff(
    HANDLE in, HANDLE out, HANDLE signal, HANDLE reference, HANDLE server, HANDLE client)
{
    SimpleFileLogger(std::format(
        "TerminalHandoff::EstablishPtyHandoff (V1 - By Value) called! in: {:p}, out: {:p}", in, out));

    // In V1, the caller provides the pipes. We just use them.
    HANDLE hSignalDup = duplicateHandle(signal);
    HANDLE hReferenceDup = duplicateHandle(reference);
    HANDLE hServerDup = duplicateHandle(server);
    HANDLE hClientDup = duplicateHandle(client);

    // We need to duplicate 'in' and 'out' as well because we might need to take ownership given they are
    // passed by value? Actually, usually in COM [in] handles are borrowed. We should duplicate them to be
    // safe.
    HANDLE hInKey = duplicateHandle(in);
    HANDLE hOutKey = duplicateHandle(out);

    SimpleFileLogger("TerminalHandoff: Calling ContourHandleHandoff (V1)...");
    ContourHandleHandoff(hInKey, hOutKey, hSignalDup, hReferenceDup, hServerDup, hClientDup, L"");
    SimpleFileLogger("TerminalHandoff: ContourHandleHandoff returned.");

    return S_OK;
}

// --- TerminalHandoffFactory ---

HRESULT STDMETHODCALLTYPE TerminalHandoffFactory::QueryInterface(REFIID riid, void** ppvObject)
{
    SimpleFileLogger(std::format("TerminalHandoffFactory::QueryInterface request: {}", GuidToString(riid)));
    if (!ppvObject)
        return E_POINTER;
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IClassFactory))
    {
        *ppvObject = static_cast<IClassFactory*>(this);
        AddRef();
        return S_OK;
    }
    *ppvObject = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE TerminalHandoffFactory::AddRef()
{
    return ++_refCount;
}

ULONG STDMETHODCALLTYPE TerminalHandoffFactory::Release()
{
    ULONG count = --_refCount;
    if (count == 0)
        delete this;
    return count;
}

HRESULT STDMETHODCALLTYPE TerminalHandoffFactory::CreateInstance(IUnknown* pUnkOuter,
                                                                 REFIID riid,
                                                                 void** ppvObject)
{
    SimpleFileLogger("TerminalHandoffFactory::CreateInstance called.");
    if (pUnkOuter)
        return CLASS_E_NOAGGREGATION;
    if (!ppvObject)
        return E_POINTER;

    TerminalHandoff* p = new TerminalHandoff();
    if (!p)
        return E_OUTOFMEMORY;

    HRESULT hr = p->QueryInterface(riid, ppvObject);
    p->Release();
    return hr;
}

HRESULT STDMETHODCALLTYPE TerminalHandoffFactory::LockServer(BOOL fLock)
{
    // Simplified lock handling
    return S_OK;
}
