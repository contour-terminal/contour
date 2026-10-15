// SPDX-License-Identifier: Apache-2.0
#include <vtpty/ConPty.hpp>

#include <crispy/BufferObject.hpp>

#include <core/WideString.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <expected>
#include <format>
#include <memory>
#include <string>
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
        DWORD const pathLen = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
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
                    ptyLog()("Found conpty.dll in executable directory: {}", core::toUtf8(exePath));
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
    /// How much the pseudoconsole may write ahead of our reads: Windows Terminal's size for the same pipe.
    constexpr auto OutputPipeBufferSize = DWORD { 128 * 1024 };

    /// Creates the pipe the pseudoconsole writes its output to. A named one, so that our end can be opened
    /// for overlapped reads, which alone can be bounded by a timeout and ended early: CreatePipe() takes no
    /// flags, and its read end blocks until something arrives. The pseudoconsole's end stays synchronous,
    /// as CreatePseudoConsole() requires of it.
    /// @return Our end, then the pseudoconsole's; or why the pipe could not be created.
    [[nodiscard]] std::expected<std::pair<HANDLE, HANDLE>, DWORD> createOutputPipe()
    {
        static auto pipesCreated = std::atomic<unsigned> { 0 };
        auto const name =
            std::format(L"\\\\.\\pipe\\contour-conpty-{}-{}", GetCurrentProcessId(), pipesCreated++);

        // One instance, ours, and only for a client on this machine.
        auto* const reader =
            CreateNamedPipeW(name.c_str(),
                             PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                             PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                             1,
                             OutputPipeBufferSize,
                             OutputPipeBufferSize,
                             0,
                             nullptr);
        if (reader == INVALID_HANDLE_VALUE)
            return std::unexpected(GetLastError());

        // Connected as it is opened: a client that connects before ConnectNamedPipe() is called leaves the
        // pipe connected, and the call is then not needed.
        auto* const writer = CreateFileW(name.c_str(),
                                         GENERIC_WRITE | FILE_READ_ATTRIBUTES,
                                         0,
                                         nullptr,
                                         OPEN_EXISTING,
                                         FILE_ATTRIBUTE_NORMAL,
                                         nullptr);
        if (writer == INVALID_HANDLE_VALUE)
        {
            auto const error = GetLastError();
            CloseHandle(reader);
            return std::unexpected(error);
        }
        return std::pair { reader, writer };
    }

    /// The pseudoconsole API reports a failure in the HRESULT it returns, not in the thread's last error.
    /// @return What @p result says, as the system spells it, and its code.
    [[nodiscard]] std::string resultMessage(HRESULT result)
    {
        return std::format("{} (HRESULT 0x{:08X})",
                           core::systemErrorMessage(static_cast<DWORD>(result)),
                           static_cast<DWORD>(result));
    }

    /// @return @p timeout as WaitForMultipleObjects() takes it: INFINITE for none.
    [[nodiscard]] DWORD waitTimeOf(std::optional<std::chrono::milliseconds> timeout) noexcept
    {
        if (!timeout)
            return INFINITE;
        return static_cast<DWORD>(std::clamp<std::chrono::milliseconds::rep>(
            timeout->count(), 0, std::chrono::milliseconds::rep { INFINITE - 1 }));
    }
} // namespace

namespace
{
    /// The slave end of a pseudoconsole, which is not this process's to write to: CreatePseudoConsole()
    /// hands the child's end to the console host, and this process closes its copy.
    class ConPtySlave final: public PtySlaveDummy
    {
      public:
        int write(std::string_view /*text*/) noexcept override
        {
            errno = EPIPE;
            return -1;
        }
    };
} // namespace

ConPty::ConPty(PageSize const& windowSize):
    _size { windowSize },
    _readCompleted { CreateEventW(nullptr, TRUE, FALSE, nullptr) },
    _wakeup { CreateEventW(nullptr, FALSE, FALSE, nullptr) },
    _closing { CreateEventW(nullptr, TRUE, FALSE, nullptr) },
    _closed { CreateEventW(nullptr, TRUE, FALSE, nullptr) },
    _slave { std::make_unique<ConPtySlave>() },
    _conptyApi { std::make_unique<ConptyApiImpl>() }
{
}

ConPty::~ConPty()
{
    ptyLog()("~ConPty()");
    close();
    for (auto* const event: { _readCompleted, _wakeup, _closing, _closed })
        if (event != nullptr)
            CloseHandle(event);
}

bool ConPty::isClosed() const noexcept
{
    return _master == INVALID_HANDLE_VALUE;
}

StartResult ConPty::start()
{
    ptyLog()("Starting ConPTY");
    assert(_master == INVALID_HANDLE_VALUE);

    auto const failed = [](std::string detail) {
        return std::unexpected(
            StartFailure { .error = StartError::PtyAllocationFailed, .detail = std::move(detail) });
    };

    if (_readCompleted == nullptr || _wakeup == nullptr || _closing == nullptr || _closed == nullptr)
        return failed("Could not create the events this PTY waits on.");

    HANDLE hPipePTYIn { INVALID_HANDLE_VALUE };

    // Create the pipes to which the ConPty will connect to
    if (!CreatePipe(&hPipePTYIn, &_output, nullptr, 0))
        return failed(core::systemErrorMessage(GetLastError()));

    auto const outputPipe = createOutputPipe();
    if (!outputPipe)
    {
        CloseHandle(hPipePTYIn);
        return failed(core::systemErrorMessage(outputPipe.error()));
    }
    auto* const hPipePTYOut = outputPipe->second;
    _input = outputPipe->first;

    // Create the Pseudo Console of the required size, attached to the PTY-end of the pipes
    HRESULT const hr = _conptyApi->createPseudoConsole(
        { unbox<SHORT>(_size.columns), unbox<SHORT>(_size.lines) }, hPipePTYIn, hPipePTYOut, 0, &_master);

    if (hPipePTYIn != INVALID_HANDLE_VALUE)
        CloseHandle(hPipePTYIn);

    if (hPipePTYOut != INVALID_HANDLE_VALUE)
        CloseHandle(hPipePTYOut);

    if (hr != S_OK)
        return failed(resultMessage(hr));

    return {};
}

void ConPty::waitForClosed()
{
    // Woken by close() itself, not found on a poll's next tick: freeing a session waits on whoever waits
    // here, and a closing tab or a quitting app would otherwise stall for up to that tick.
    if (isClosed())
        return;
    if (WaitForSingleObject(_closed, INFINITE) != WAIT_OBJECT_0)
        ptyLog()("Waiting for the ConPTY to close failed: {}", core::systemErrorMessage(GetLastError()));
}

void ConPty::close()
{
    ptyLog()("ConPty.close()");
    auto const _ = std::lock_guard { _mutex };

    // Our end of the output first, as ClosePseudoConsole() asks of a caller that does not go on reading it
    // until that returns: before Windows 11 24H2 it waits until the pseudoconsole's last output has been
    // read. A read under way is woken, and the handle is closed once it has let go of it. No read starts in
    // between: the reader that read again at once would wait for output an idle child never writes, and so
    // would this close, with the pseudoconsole that ends the child still open.
    if (_input != INVALID_HANDLE_VALUE)
    {
        SetEvent(_closing);
        auto const reading = std::scoped_lock { _readMutex };
        CloseHandle(_input);
        _input = INVALID_HANDLE_VALUE;
    }

    if (_master != INVALID_HANDLE_VALUE)
    {
        _conptyApi->closePseudoConsole(_master);
        _master = INVALID_HANDLE_VALUE;
    }

    // Last, once the pseudoconsole is gone: a write it held up by not reading has failed by then, and let go.
    if (_output != INVALID_HANDLE_VALUE)
    {
        auto const writing = std::scoped_lock { _writeMutex };
        CloseHandle(_output);
        _output = INVALID_HANDLE_VALUE;
    }

    if (_closed != nullptr)
        SetEvent(_closed);
}

std::optional<Pty::ReadResult> ConPty::read(crispy::BufferObject<char>& storage,
                                            std::optional<std::chrono::milliseconds> timeout,
                                            size_t size)
{
    // EPIPE once close() has begun, too: it waits for this lock, which a read would hold until output came.
    auto const _ = std::scoped_lock { _readMutex };
    if (_input == INVALID_HANDLE_VALUE || WaitForSingleObject(_closing, 0) == WAIT_OBJECT_0)
    {
        errno = EPIPE;
        return nullopt;
    }

    // A zero timeout asks "is anything waiting?": the terminal reads ahead this way while the render thread
    // holds its state (Terminal::processInputOnce). The pipe answers that at once, without a read started
    // only to be cancelled.
    if (timeout == std::chrono::milliseconds::zero())
    {
        DWORD available {};
        if (!PeekNamedPipe(_input, nullptr, 0, nullptr, &available, nullptr))
        {
            // ERROR_BROKEN_PIPE: the pseudoconsole has closed its end, and will write nothing more.
            errno = EPIPE;
            return nullopt;
        }
        if (available == 0)
        {
            errno = EAGAIN;
            return nullopt;
        }
    }

    auto overlapped = OVERLAPPED {};
    overlapped.hEvent = _readCompleted;
    auto const n = static_cast<DWORD>(std::min(size, storage.bytesAvailable()));
    if (!ReadFile(_input, storage.hotEnd(), n, nullptr, &overlapped) && GetLastError() != ERROR_IO_PENDING)
    {
        // ERROR_BROKEN_PIPE: the pseudoconsole has closed its end, and will write nothing more.
        errno = EPIPE;
        return nullopt;
    }

    auto const events = std::array { _readCompleted, _wakeup, _closing };
    auto const waited =
        WaitForMultipleObjects(static_cast<DWORD>(events.size()), events.data(), FALSE, waitTimeOf(timeout));
    if (waited != WAIT_OBJECT_0)
        CancelIoEx(_input, &overlapped);

    // Waits for a cancelled read too, which may have completed in the meantime: what it read is kept.
    auto nread = DWORD {};
    if (!GetOverlappedResult(_input, &overlapped, &nread, TRUE))
    {
        if (GetLastError() != ERROR_OPERATION_ABORTED)
            errno = EPIPE;
        else
            errno = waited == WAIT_TIMEOUT ? EAGAIN : EINTR;
        return nullopt;
    }

    if (ptyInLog)
        ptyInLog()("{} received: \"{}\"", "master", core::escape(storage.hotEnd(), storage.hotEnd() + nread));

    return ReadResult { .data = string_view { storage.hotEnd(), nread }, .fromStdoutFastPipe = false };
}

void ConPty::wakeupReader()
{
    SetEvent(_wakeup);
}

int ConPty::write(std::string_view data)
{
    auto const* buf = data.data();
    auto const size = data.size();

    // Serialized against close(): a handle closed under a write may be handed to whatever is opened
    // next -- another pane's pipe, a file -- and the write would go there.
    auto const _ = std::scoped_lock { _writeMutex };
    if (_output == INVALID_HANDLE_VALUE)
    {
        errno = EPIPE;
        return -1;
    }

    DWORD nwritten {};
    if (WriteFile(_output, buf, static_cast<DWORD>(size), &nwritten, nullptr))
    {
        ptyOutLog()("Sending bytes: \"{}\"", core::escape(data.substr(0, nwritten)));
        return static_cast<int>(nwritten);
    }
    else
    {
        ptyOutLog()("PTY write of {} bytes failed.\n", size);
        errno = EPIPE; // the pseudoconsole is gone, and reads no more
        return -1;
    }
}

PageSize ConPty::pageSize() const noexcept
{
    return _size;
}

void ConPty::resizeScreen(PageSize cells, std::optional<ImageSize> pixels)
{
    (void) pixels; // TODO Can we pass that information, too?

    // No live pseudoconsole to resize: remember the size rather than drop it, so a subsequent
    // start() creates the pseudoconsole at it. attachDisplay() is what first tells a PTY its size,
    // and it now runs before start() -- dropping the size here left the child born at whatever the
    // constructor was handed (the profile's configured terminal size), so the shell's first prompt,
    // and any program launched before the first window-geometry change, wrapped at the wrong column.
    // UnixPty stashes the same way; only the pixel half is ConPTY's to ignore.
    //
    // This must test the master handle, not _slave: close() invalidates _master but leaves _slave
    // alive, so a _slave-based guard let a post-close resize through and handed
    // INVALID_HANDLE_VALUE to ResizePseudoConsole, which dereferences it. That is a real crash on
    // the session-teardown path -- closing a pane makes QML re-run the Loader binding, which calls
    // setSession() -> attachDisplay() -> Terminal::resizeScreen() on the already-closed PTY.
    if (isClosed())
    {
        ptyLog()("Recording terminal size while no pseudoconsole is live: {}x{}", cells.columns, cells.lines);
        _size = cells;
        return;
    }

    auto const coords = COORD { unbox<SHORT>(cells.columns), unbox<SHORT>(cells.lines) };

    HRESULT const result = _conptyApi->resizePseudoConsole(_master, coords);
    if (result != S_OK)
        throw runtime_error { resultMessage(result) };

    _size = cells;
}

PtySlave& ConPty::slave() noexcept
{
    return *_slave;
}

} // namespace vtpty
