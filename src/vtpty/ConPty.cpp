// SPDX-License-Identifier: Apache-2.0
#include <vtpty/ConPty.h>

#include <crispy/BufferObject.h>

#include <memory>
#include <utility>

#include <Windows.h>

using namespace std;

namespace vtpty
{

// Function pointer types for ConPTY API
using CreatePseudoConsoleFn = HRESULT(WINAPI*)(COORD, HANDLE, HANDLE, DWORD, HPCON*);
using ResizePseudoConsoleFn = HRESULT(WINAPI*)(HPCON, COORD);
using ClosePseudoConsoleFn = void(WINAPI*)(HPCON);

// Implementation of the ConPTY API
class ConPty::ConptyApiImpl
{
  public:
    CreatePseudoConsoleFn createPseudoConsole;
    ResizePseudoConsoleFn resizePseudoConsole;
    ClosePseudoConsoleFn closePseudoConsole;

    // Constructor loads the appropriate API implementation
    ConptyApiImpl()
    {
        // First try to load conpty.dll from the same directory as our executable
        wchar_t exePath[MAX_PATH];
        DWORD pathLen = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        HMODULE hModule = nullptr;

        if (pathLen > 0 && pathLen < MAX_PATH)
        {
            // Find the last backslash to get the directory
            wchar_t* lastBackslash = wcsrchr(exePath, L'\\');
            if (lastBackslash != nullptr)
            {
                // Replace executable name with conpty.dll
                wcscpy_s(lastBackslash + 1, MAX_PATH - (lastBackslash - exePath) - 1, L"conpty.dll");

                // Try loading from this path
                hModule = LoadLibraryW(exePath);

                if (hModule != nullptr)
                {
                    // Convert wide string to narrow string for logging using proper Windows API
                    int sizeNeeded = WideCharToMultiByte(CP_UTF8, 0, exePath, -1, NULL, 0, NULL, NULL);
                    std::string path(sizeNeeded, 0);
                    WideCharToMultiByte(CP_UTF8, 0, exePath, -1, &path[0], sizeNeeded, NULL, NULL);
                    // Remove the null terminator included in the conversion
                    path.resize(path.size() - 1);
                    ptyLog()("Found conpty.dll in executable directory: {}", path);
                }
            }
        }

        // If not found in executable directory, try loading from PATH
        if (hModule == nullptr)
        {
            hModule = LoadLibraryW(L"conpty.dll");
            if (hModule != nullptr)
            {
                ptyLog()("Found conpty.dll in system PATH");
            }
        }

        if (hModule != nullptr)
        {
            // Found conpty.dll, load functions from it
            createPseudoConsole =
                reinterpret_cast<CreatePseudoConsoleFn>(GetProcAddress(hModule, "CreatePseudoConsole"));
            resizePseudoConsole =
                reinterpret_cast<ResizePseudoConsoleFn>(GetProcAddress(hModule, "ResizePseudoConsole"));
            closePseudoConsole =
                reinterpret_cast<ClosePseudoConsoleFn>(GetProcAddress(hModule, "ClosePseudoConsole"));

            // Verify all functions were found
            if (createPseudoConsole && resizePseudoConsole && closePseudoConsole)
            {
                ptyLog()("Using conpty.dll for pseudoconsole - this should improve mouse input handling on "
                         "Windows 10");
                return;
            }

            // If any function is missing, fall back to kernel32
            ptyLog()(
                "conpty.dll was found but missing required functions, falling back to system implementation");
            FreeLibrary(hModule);
        }

        // Use default Windows API functions from kernel32.dll
        ptyLog()("Using Windows system API for pseudoconsole");
        createPseudoConsole = ::CreatePseudoConsole;
        resizePseudoConsole = ::ResizePseudoConsole;
        closePseudoConsole = ::ClosePseudoConsole;
    }
};

namespace
{
    string GetLastErrorAsString()
    {
        DWORD errorMessageID = GetLastError();
        if (errorMessageID == 0)
            return "";

        LPSTR messageBuffer = nullptr;
        size_t size = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
                                         | FORMAT_MESSAGE_IGNORE_INSERTS,
                                     nullptr,
                                     errorMessageID,
                                     MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                                     (LPSTR) &messageBuffer,
                                     0,
                                     nullptr);

        string message(messageBuffer, size);

        LocalFree(messageBuffer);

        return message;
    }
} // anonymous namespace

struct ConPtySlave: public PtySlaveDummy
{
    HANDLE _output;

    explicit ConPtySlave(HANDLE output): _output { output } {}

    int write(std::string_view text) noexcept override
    {
        DWORD nwritten {};
        if (WriteFile(_output, text.data(), static_cast<DWORD>(text.size()), &nwritten, nullptr))
            return static_cast<int>(nwritten);
        else
            return -1;
    }
};

ConPty::ConPty(PageSize const& windowSize): _size { windowSize }
{
    _master = INVALID_HANDLE_VALUE;
    _input = INVALID_HANDLE_VALUE;
    _output = INVALID_HANDLE_VALUE;
    _buffer.resize(10240);

    // Create the ConptyApi implementation
    _conptyApi = std::make_unique<ConptyApiImpl>();
}

ConPty::~ConPty()
{
    ptyLog()("~ConPty()");
    close();
}

bool ConPty::isClosed() const noexcept
{
    return _master == INVALID_HANDLE_VALUE;
}

void ConPty::start()
{
    ptyLog()("Starting ConPTY");
    assert(!_slave);

    _slave = make_unique<ConPtySlave>(_output);

    HANDLE hPipePTYIn { INVALID_HANDLE_VALUE };
    HANDLE hPipePTYOut { INVALID_HANDLE_VALUE };

    // Create the pipes to which the ConPty will connect to
    if (!CreatePipe(&hPipePTYIn, &_output, NULL, 0))
        throw runtime_error { GetLastErrorAsString() };

    if (!CreatePipe(&_input, &hPipePTYOut, NULL, 0))
    {
        CloseHandle(hPipePTYIn);
        throw runtime_error { GetLastErrorAsString() };
    }

    // Create the Pseudo Console of the required size, attached to the PTY-end of the pipes
    HRESULT hr = _conptyApi->createPseudoConsole(
        { unbox<SHORT>(_size.columns), unbox<SHORT>(_size.lines) }, hPipePTYIn, hPipePTYOut, 0, &_master);

    if (hPipePTYIn != INVALID_HANDLE_VALUE)
        CloseHandle(hPipePTYIn);

    if (hPipePTYOut != INVALID_HANDLE_VALUE)
        CloseHandle(hPipePTYOut);

    if (hr != S_OK)
        throw runtime_error { GetLastErrorAsString() };
}

void ConPty::waitForClosed()
{
    while (!isClosed())
        Sleep(1000);
}

void ConPty::close()
{
    ptyLog()("ConPty.close()");
    auto const _ = std::lock_guard { _mutex };

    if (_master != INVALID_HANDLE_VALUE)
    {
        _conptyApi->closePseudoConsole(_master);
        _master = INVALID_HANDLE_VALUE;
    }

    if (_input != INVALID_HANDLE_VALUE)
    {
        CloseHandle(_input);
        _input = INVALID_HANDLE_VALUE;
    }

    if (_output != INVALID_HANDLE_VALUE)
    {
        CloseHandle(_output);
        _output = INVALID_HANDLE_VALUE;
    }
}

std::optional<Pty::ReadResult> ConPty::read(crispy::buffer_object<char>& buffer,
                                            std::optional<std::chrono::milliseconds> timeout,
                                            size_t size)
{
    // TODO: wait for timeout time at most AND got woken up upon wakeupReader() invokcation.
    (void) timeout;

    auto const n = static_cast<DWORD>(std::min(size, buffer.bytesAvailable()));

    DWORD nread {};
    if (!ReadFile(_input, buffer.hotEnd(), n, &nread, nullptr))
        return nullopt;

    if (ptyInLog)
        ptyInLog()("{} received: \"{}\"", "master", crispy::escape(buffer.hotEnd(), buffer.hotEnd() + nread));

    return ReadResult { .data = string_view { buffer.hotEnd(), nread }, .fromStdoutFastPipe = false };
}

void ConPty::wakeupReader()
{
    // TODO: Windows ConPTY does *NOT* support non-blocking / overlapped I/O.
    // How can we make ReadFile() return early? We could maybe WriteFile() to it?
}

int ConPty::write(std::string_view data)
{
    auto const* buf = data.data();
    auto const size = data.size();

    DWORD nwritten {};
    if (WriteFile(_output, buf, static_cast<DWORD>(size), &nwritten, nullptr))
    {
        ptyOutLog()("Sending bytes: \"{}\"", crispy::escape(data.data(), data.data() + nwritten));
        return static_cast<int>(nwritten);
    }
    else
    {
        ptyOutLog()("PTY write of {} bytes failed.\n", size);
        return -1;
    }
}

PageSize ConPty::pageSize() const noexcept
{
    return _size;
}

void ConPty::resizeScreen(PageSize cells, std::optional<ImageSize> pixels)
{
    if (!_slave)
        return;

    (void) pixels; // TODO Can we pass that information, too?

    COORD coords;
    coords.X = unbox<unsigned short>(cells.columns);
    coords.Y = unbox<unsigned short>(cells.lines);

    HRESULT const result = _conptyApi->resizePseudoConsole(_master, coords);
    if (result != S_OK)
        throw runtime_error { GetLastErrorAsString() };

    _size = cells;
}

PtySlave& ConPty::slave() noexcept
{
    return *_slave;
}

} // namespace vtpty
