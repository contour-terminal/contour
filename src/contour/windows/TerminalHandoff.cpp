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
    if (IsEqualIID(riid, IID_ITerminalHandoff3))
    {
        *ppvObject = static_cast<ITerminalHandoff3*>(this);
        AddRef();
        return S_OK;
    }
    // {E686C757-9A35-4A1C-B3CE-0BCC8B5C69F4} - Likely ITerminalHandoff or ITerminalHandoff2
    static const IID IID_UnknownHandoff = {
        0xE686C757, 0x9A35, 0x4A1C, { 0xB3, 0xCE, 0x0B, 0xCC, 0x8B, 0x5C, 0x69, 0xF4 }
    };
    if (IsEqualIID(riid, IID_UnknownHandoff))
    {
        SimpleFileLogger("TerminalHandoff::QueryInterface: Accepting IID_UnknownHandoff (E686C757...) as "
                         "ITerminalHandoffV1");
        *ppvObject = static_cast<ITerminalHandoffV1*>(this);
        AddRef();
        return S_OK;
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
    SimpleFileLogger("TerminalHandoff::EstablishPtyHandoff called!");
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
    {
        SimpleFileLogger(
            std::format("EstablishPtyHandoff: Failed to create Pipe In Write. Error: {}", GetLastError()));
        return E_FAIL;
    }

    HANDLE hInRead = CreateFileW(pipeInName.c_str(),
                                 GENERIC_READ,
                                 0, // No sharing? or FILE_SHARE_READ?
                                 nullptr,
                                 OPEN_EXISTING,
                                 0, // Default attributes
                                 nullptr);

    if (hInRead == INVALID_HANDLE_VALUE)
    {
        SimpleFileLogger(
            std::format("EstablishPtyHandoff: Failed to open Pipe In Read. Error: {}", GetLastError()));
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
        SimpleFileLogger(
            std::format("EstablishPtyHandoff: Failed to create Pipe Out Read. Error: {}", GetLastError()));
        CloseHandle(hInRead);
        CloseHandle(hInWrite);
        return E_FAIL;
    }

    HANDLE hOutWrite = CreateFileW(pipeOutName.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);

    if (hOutWrite == INVALID_HANDLE_VALUE)
    {
        SimpleFileLogger(
            std::format("EstablishPtyHandoff: Failed to open Pipe Out Write. Error: {}", GetLastError()));
        CloseHandle(hOutRead);
        CloseHandle(hInRead);
        CloseHandle(hInWrite);
        return E_FAIL;
    }

    SimpleFileLogger("EstablishPtyHandoff: Pipes created successfully.");

    SimpleFileLogger(
        std::format("EstablishPtyHandoff: Pointers - in: {:p}, out: {:p}", (void*) in, (void*) out));

    if (!in || !out)
    {
        SimpleFileLogger("EstablishPtyHandoff: 'in' or 'out' pointer is NULL! Returning E_POINTER.");
        CloseHandle(hInRead);
        CloseHandle(hInWrite);
        CloseHandle(hOutRead);
        CloseHandle(hOutWrite);
        return E_POINTER;
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

    SimpleFileLogger(std::format("EstablishPtyHandoff: Checking startupInfo: {:p}", (void*) startupInfo));
    std::wstring title;
    if (startupInfo)
    {
        SimpleFileLogger("EstablishPtyHandoff: startupInfo is valid.");
        if (startupInfo->pszTitle)
        {
            SimpleFileLogger(
                std::format("EstablishPtyHandoff: Title ptr: {:p}", (void*) startupInfo->pszTitle));
            title = startupInfo->pszTitle;
        }
    }
    else
    {
        SimpleFileLogger("EstablishPtyHandoff: startupInfo is NULL.");
    }

    SimpleFileLogger("TerminalHandoff: Calling ContourHandleHandoff...");
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
