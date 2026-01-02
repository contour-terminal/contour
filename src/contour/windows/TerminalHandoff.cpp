// SPDX-License-Identifier: Apache-2.0
#include <contour/windows/TerminalHandoff.h>

#include <atomic>
#include <format>
#include <string>
#include <utility>

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
        DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &dup, 0, FALSE, DUPLICATE_SAME_ACCESS);
    }
    return dup;
}

// --- TerminalHandoff ---

TerminalHandoff::TerminalHandoff() = default;

HRESULT STDMETHODCALLTYPE TerminalHandoff::QueryInterface(REFIID riid, void** ppvObject)
{
    if (!ppvObject)
        return E_POINTER;
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITerminalHandoff3))
    {
        *ppvObject = static_cast<ITerminalHandoff3*>(this);
        AddRef();
        return S_OK;
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
    // Generate unique pipe names
    static std::atomic<int> pipeCounter { 0 };
    int id = pipeCounter++;
    DWORD pid = GetCurrentProcessId();

    // Pipe In: Server writes (Outbound), Client reads.
    std::wstring pipeInName = std::format(L"\\\\.\\pipe\\contour_in_{}_{}", pid, id);
    HANDLE hInWrite = CreateNamedPipeW(pipeInName.c_str(),
                                       PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED,
                                       PIPE_TYPE_BYTE | PIPE_WAIT,
                                       1,
                                       4096,
                                       4096,
                                       0,
                                       nullptr);

    if (hInWrite == INVALID_HANDLE_VALUE)
        return E_FAIL;

    HANDLE hInRead = CreateFileW(pipeInName.c_str(),
                                 GENERIC_READ,
                                 0, // No sharing? or FILE_SHARE_READ?
                                 nullptr,
                                 OPEN_EXISTING,
                                 0, // Default attributes
                                 nullptr);

    if (hInRead == INVALID_HANDLE_VALUE)
    {
        CloseHandle(hInWrite);
        return E_FAIL;
    }

    // Pipe Out: Server reads (Inbound), Client writes.
    std::wstring pipeOutName = std::format(L"\\\\.\\pipe\\contour_out_{}_{}", pid, id);
    HANDLE hOutRead = CreateNamedPipeW(pipeOutName.c_str(),
                                       PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
                                       PIPE_TYPE_BYTE | PIPE_WAIT,
                                       1,
                                       4096,
                                       4096,
                                       0,
                                       nullptr);

    if (hOutRead == INVALID_HANDLE_VALUE)
    {
        CloseHandle(hInRead);
        CloseHandle(hInWrite);
        return E_FAIL;
    }

    HANDLE hOutWrite = CreateFileW(pipeOutName.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);

    if (hOutWrite == INVALID_HANDLE_VALUE)
    {
        CloseHandle(hOutRead);
        CloseHandle(hInRead);
        CloseHandle(hInWrite);
        return E_FAIL;
    }

    *in = hInRead;
    *out = hOutWrite;

    // Pass the server-side handles to Contour app logic.
    // Ensure we duplicate or detach logic such that if this function returns, the handles are handled
    // correctly. The wrapper ContourHandleHandoff should take ownership.

    HANDLE hSignalDup = duplicateHandle(signal);
    HANDLE hReferenceDup = duplicateHandle(reference);
    HANDLE hServerDup = duplicateHandle(server);
    HANDLE hClientDup = INVALID_HANDLE_VALUE;

    DuplicateHandle(GetCurrentProcess(),
                    client,
                    GetCurrentProcess(),
                    &hClientDup,
                    PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_SET_INFORMATION | SYNCHRONIZE,
                    FALSE,
                    0);

    if (hClientDup == INVALID_HANDLE_VALUE)
        hClientDup = duplicateHandle(client);

    std::wstring title;
    if (startupInfo && startupInfo->pszTitle)
        title = startupInfo->pszTitle;

    ContourHandleHandoff(hInWrite, hOutRead, hSignalDup, hReferenceDup, hServerDup, hClientDup, title);

    return S_OK;
}

// --- TerminalHandoffFactory ---

HRESULT STDMETHODCALLTYPE TerminalHandoffFactory::QueryInterface(REFIID riid, void** ppvObject)
{
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
