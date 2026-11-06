// SPDX-License-Identifier: Apache-2.0
#include <vtpty/ConPty.hpp>

#include <vtpty/Win32Handle.hpp>

#include <crispy/BufferObject.hpp>

#include <core/WideString.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <new>
#include <ranges>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

#include <Windows.h>
#include <winternl.h>

using namespace std;

namespace vtpty
{

// Function pointer types for ConPTY API
using CreatePseudoConsoleFn = HRESULT(WINAPI*)(COORD, HANDLE, HANDLE, DWORD, HPCON*);
using ResizePseudoConsoleFn = HRESULT(WINAPI*)(HPCON, COORD);
using ClosePseudoConsoleFn = void(WINAPI*)(HPCON);
using PackPseudoConsoleFn = HRESULT(WINAPI*)(HANDLE, HANDLE, HANDLE, HPCON*);

namespace
{
    /// The pseudoconsole as the bundled conpty.dll lays it out behind an HPCON (OpenConsole's
    /// src/winconpty/winconpty.h, `PseudoConsole`): allocated on the process heap, which ClosePseudoConsole()
    /// frees it from. Known for that conpty.dll alone, which is why only it crosses to another process. Read
    /// only to hand its handles over: the adopter builds its own with the DLL's ConptyPackPseudoConsole().
    struct BundledPseudoConsole
    {
        HANDLE signal;    ///< The signal pipe: resizes travel through it, and closing it ends the host.
        HANDLE reference; ///< The reference handle, which keeps the console session open.
        HANDLE process;   ///< The console host process.
    };

    /// @return The conpty.dll beside this process's executable; empty where its path cannot be told.
    [[nodiscard]] std::wstring bundledConptyPath()
    {
        auto path = std::wstring(MAX_PATH, L'\0');
        auto const length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0 || length >= path.size())
            return {};
        path.resize(length);
        auto const directory = path.find_last_of(L'\\');
        if (directory == std::wstring::npos)
            return {};
        path.resize(directory + 1);
        return path + L"conpty.dll";
    }
} // namespace

// Implementation of the ConPTY API
class ConPty::ConptyApiImpl
{
  public:
    CreatePseudoConsoleFn createPseudoConsole;
    ResizePseudoConsoleFn resizePseudoConsole;
    ClosePseudoConsoleFn closePseudoConsole;
    /// Builds an HPCON around the handles of a pseudoconsole another process made (OpenConsole's
    /// ConptyPackPseudoConsole()): the bundled conpty.dll's alone; null for any other.
    PackPseudoConsoleFn packPseudoConsole = nullptr;
    ConsoleHost host = ConsoleHost::System; ///< Which implementation the functions above are.

    /// @return This process's implementation: found, and its library loaded, once.
    [[nodiscard]] static ConptyApiImpl const& instance()
    {
        static auto const api = ConptyApiImpl {};
        return api;
    }

    // Constructor loads the appropriate API implementation
    ConptyApiImpl()
    {
        // First the conpty.dll beside our executable, then one on the PATH.
        auto const bundled = bundledConptyPath();
        if (!bundled.empty() && load(LoadLibraryW(bundled.c_str()), ConsoleHost::Bundled))
        {
            ptyLog()("Using the conpty.dll in the executable's directory: {}", core::toUtf8(bundled));
            return;
        }
        if (load(LoadLibraryW(L"conpty.dll"), ConsoleHost::OnPath))
        {
            ptyLog()("Using the conpty.dll found on the PATH");
            return;
        }

        // Use default Windows API functions from kernel32.dll
        ptyLog()("Using Windows system API for pseudoconsole");
        createPseudoConsole = ::CreatePseudoConsole;
        resizePseudoConsole = ::ResizePseudoConsole;
        closePseudoConsole = ::ClosePseudoConsole;
    }

  private:
    /// Takes the pseudoconsole functions from @p module, a conpty.dll, where it exports all three.
    /// @return Whether it did; a module that does not is freed.
    [[nodiscard]] bool load(HMODULE module, ConsoleHost from)
    {
        if (module == nullptr)
            return false;
        auto* const create =
            reinterpret_cast<CreatePseudoConsoleFn>(GetProcAddress(module, "CreatePseudoConsole"));
        auto* const resize =
            reinterpret_cast<ResizePseudoConsoleFn>(GetProcAddress(module, "ResizePseudoConsole"));
        auto* const close =
            reinterpret_cast<ClosePseudoConsoleFn>(GetProcAddress(module, "ClosePseudoConsole"));
        if (create == nullptr || resize == nullptr || close == nullptr)
        {
            ptyLog()("conpty.dll was found but missing required functions");
            FreeLibrary(module);
            return false;
        }
        createPseudoConsole = create;
        resizePseudoConsole = resize;
        closePseudoConsole = close;
        if (from == ConsoleHost::Bundled)
            packPseudoConsole =
                reinterpret_cast<PackPseudoConsoleFn>(GetProcAddress(module, "ConptyPackPseudoConsole"));
        host = from;
        return true;
    }
};

namespace
{
    /// How much either side of a pseudoconsole's pipes may write ahead of the other's reads: Windows
    /// Terminal's size for the same pipes.
    constexpr auto PipeBufferSize = DWORD { 128 * 1024 };

    /// Which way a pipe between this process and the pseudoconsole carries the bytes.
    enum class PipeFlow : uint8_t
    {
        FromPseudoconsole = 0, ///< The program's output: this process reads.
        ToPseudoconsole,       ///< The program's input: this process writes.
    };

    /// How a pipe of one flow is opened at either end.
    struct PipeFlowRow
    {
        PipeFlow flow;
        DWORD ourAccess;           ///< This process's end, as CreateNamedPipeW() opens it.
        DWORD pseudoconsoleAccess; ///< The pseudoconsole's end, as CreateFileW() opens it.
        std::wstring_view tag;     ///< Part of the pipe's name, which tells the two of a pseudoconsole apart.
    };

    /// One row per flow, in enumerator order.
    constexpr auto PipeFlows = std::array {
        PipeFlowRow { .flow = PipeFlow::FromPseudoconsole,
                      .ourAccess = PIPE_ACCESS_INBOUND,
                      .pseudoconsoleAccess = GENERIC_WRITE | FILE_READ_ATTRIBUTES,
                      .tag = L"out" },
        // Duplex, though nothing comes back: an outbound end may not read its own attributes, and this one
        // asks how much room the pipe has (writableWithoutWaiting()).
        PipeFlowRow { .flow = PipeFlow::ToPseudoconsole,
                      .ourAccess = PIPE_ACCESS_DUPLEX,
                      .pseudoconsoleAccess = GENERIC_READ | FILE_WRITE_ATTRIBUTES,
                      .tag = L"in" },
    };

    static_assert(std::ranges::all_of(std::views::iota(std::size_t { 0 }, PipeFlows.size()),
                                      [](std::size_t index) {
                                          return std::to_underlying(PipeFlows.at(index).flow) == index;
                                      }),
                  "PipeFlows must list every flow in enumerator order");

    /// @return When this process started, in 100 ns ticks: what tells it apart from a later process given the
    ///         same id. A pipe outlives the process that named it once a live upgrade hands it on, and the
    ///         name must not then be asked for again.
    [[nodiscard]] std::uint64_t processStartTime() noexcept
    {
        static auto const started = [] {
            auto creation = FILETIME {};
            auto exit = FILETIME {};
            auto kernel = FILETIME {};
            auto user = FILETIME {};
            if (GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user) == FALSE)
                return std::uint64_t { 0 };
            return (std::uint64_t { creation.dwHighDateTime } << 32) | creation.dwLowDateTime;
        }();
        return started;
    }

    /// Creates a pipe between this process and the pseudoconsole. A named one, so that our end can be
    /// opened for overlapped I/O, which alone can be bounded by a timeout and ended early: CreatePipe() takes
    /// no flags, and its ends block until the other side reads or writes. The pseudoconsole's end stays
    /// synchronous, as CreatePseudoConsole() requires of it.
    /// @param flow Which way it carries the bytes.
    /// @return Our end, then the pseudoconsole's; or why the pipe could not be created.
    [[nodiscard]] std::expected<std::pair<HANDLE, HANDLE>, DWORD> createPipe(PipeFlow flow)
    {
        static auto pipesCreated = std::atomic<unsigned> { 0 };
        auto const& row = PipeFlows.at(std::to_underlying(flow));
        auto const name = std::format(L"\\\\.\\pipe\\contour-conpty-{}-{}-{}-{}",
                                      GetCurrentProcessId(),
                                      processStartTime(),
                                      pipesCreated++,
                                      row.tag);

        // One instance, ours, and only for a client on this machine.
        auto* const ours =
            CreateNamedPipeW(name.c_str(),
                             row.ourAccess | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                             PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                             1,
                             PipeBufferSize,
                             PipeBufferSize,
                             0,
                             nullptr);
        if (ours == INVALID_HANDLE_VALUE)
            return std::unexpected(GetLastError());

        // Connected as it is opened: a client that connects before ConnectNamedPipe() is called leaves the
        // pipe connected, and the call is then not needed.
        auto* const theirs = CreateFileW(
            name.c_str(), row.pseudoconsoleAccess, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (theirs == INVALID_HANDLE_VALUE)
        {
            auto const error = GetLastError();
            CloseHandle(ours);
            return std::unexpected(error);
        }
        return std::pair { ours, theirs };
    }

    /// FILE_PIPE_LOCAL_INFORMATION (ntifs.h): what NtQueryInformationFile() tells of one end of a pipe.
    struct PipeLocalInformation
    {
        ULONG namedPipeType;
        ULONG namedPipeConfiguration;
        ULONG maximumInstances;
        ULONG currentInstances;
        ULONG inboundQuota;
        ULONG readDataAvailable;
        ULONG outboundQuota;
        ULONG writeQuotaAvailable; ///< What a write takes now without waiting for the reader.
        ULONG namedPipeState;
        ULONG namedPipeEnd;
    };

    /// FilePipeLocalInformation, of FILE_INFORMATION_CLASS (ntifs.h).
    constexpr auto FilePipeLocalInformation = static_cast<FILE_INFORMATION_CLASS>(24);

    using NtQueryInformationFileFn =
        LONG(NTAPI*)(HANDLE, IO_STATUS_BLOCK*, void*, ULONG, FILE_INFORMATION_CLASS);

    /// @return How many bytes a write to @p pipe, a pipe end open for reading its attributes as well as for
    ///         writing, takes now without waiting for the reader; 0 while it is full, or where that cannot be
    ///         told.
    [[nodiscard]] std::size_t writableWithoutWaiting(HANDLE pipe) noexcept
    {
        static auto* const query = ntdllFunction<NtQueryInformationFileFn>("NtQueryInformationFile");
        auto information = PipeLocalInformation {};
        auto status = IO_STATUS_BLOCK {};
        if (query == nullptr
            || query(pipe, &status, &information, sizeof(information), FilePipeLocalInformation) != 0)
            return 0;
        return information.writeQuotaAvailable;
    }

    /// @return Whether @p handle names a handle open in this process.
    [[nodiscard]] bool isOpenHandle(CrossingHandle handle) noexcept
    {
        auto flags = DWORD {};
        return !handle.isNone() && GetHandleInformation(handle.native(), &flags) != FALSE;
    }

    /// The pseudoconsole API reports a failure in the HRESULT it returns, not in the thread's last error.
    /// @return What @p result says, as the system spells it, and its code.
    [[nodiscard]] std::string resultMessage(HRESULT result)
    {
        return std::format("{} (HRESULT 0x{:08X})",
                           core::systemErrorMessage(static_cast<DWORD>(result)),
                           static_cast<DWORD>(result));
    }

    /// What ConPty::write() returns when the pipe failed under it: what it wrote before, else -1 with errno
    /// EPIPE.
    /// @param size What write() was asked to write.
    /// @param written What it wrote before the failure.
    [[nodiscard]] int writeFailed(std::size_t size, std::size_t written)
    {
        ptyOutLog()("PTY write of {} bytes failed after {}.", size, written);
        if (written > 0)
            return static_cast<int>(written);
        errno = EPIPE; // the pseudoconsole is gone, and reads no more
        return -1;
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
    _writeCompleted { CreateEventW(nullptr, TRUE, FALSE, nullptr) },
    _writesInterrupted { CreateEventW(nullptr, TRUE, FALSE, nullptr) },
    _wakeup { CreateEventW(nullptr, FALSE, FALSE, nullptr) },
    _closing { CreateEventW(nullptr, TRUE, FALSE, nullptr) },
    _closed { CreateEventW(nullptr, TRUE, FALSE, nullptr) },
    _slave { std::make_unique<ConPtySlave>() },
    _conptyApi { &ConptyApiImpl::instance() }
{
}

std::expected<void, AdoptError> ConPty::canAdoptAny()
{
    if (auto const& api = ConptyApiImpl::instance();
        api.host != ConsoleHost::Bundled || !api.packPseudoConsole)
        return std::unexpected(AdoptError::ForeignConsoleHost);
    return {};
}

std::expected<void, AdoptError> ConPty::canAdopt(InheritedPty const& inherited)
{
    if (!inherited.conpty || !std::ranges::all_of(inherited.conpty->handles(), isOpenHandle))
        return std::unexpected(AdoptError::BadDescriptor);
    return canAdoptAny();
}

std::expected<std::unique_ptr<ConPty>, AdoptError> ConPty::adopt(InheritedPty const& inherited)
{
    if (auto const adoptable = canAdopt(inherited); !adoptable)
        return std::unexpected(adoptable.error());
    auto const& handles = *inherited.conpty;
    auto pty = std::make_unique<ConPty>(inherited.pageSize);
    if (pty->_conptyApi->packPseudoConsole(
            handles.consoleHost.native(), handles.reference.native(), handles.signal.native(), &pty->_master)
        != S_OK)
    {
        pty->_master = INVALID_HANDLE_VALUE;
        return std::unexpected(AdoptError::BadDescriptor);
    }
    pty->_pixels = inherited.pixels;
    pty->_input = handles.outputReader.native();
    pty->_output = handles.inputWriter.native();

    // Crossed for this pseudoconsole alone: no program this process starts inherits them.
    for (auto const handle: handles.handles())
        if (auto const set = setInheritance(handle.native(), HandleInheritance::NotInherited); !set)
            ptyLog()("An adopted pseudoconsole's handle stays inheritable: {}", set.error().message());
    return pty;
}

ConPty::~ConPty()
{
    ptyLog()("~ConPty()");
    {
        auto const _ = std::lock_guard { _mutex };
        closeLocked();
    }
    for (auto* const event:
         { _readCompleted, _writeCompleted, _writesInterrupted, _wakeup, _closing, _closed })
        if (event != nullptr)
            CloseHandle(event);
}

bool ConPty::isClosed() const noexcept
{
    return _master == INVALID_HANDLE_VALUE;
}

ConsoleHost ConPty::consoleHost() const noexcept
{
    return _conptyApi->host;
}

std::optional<ImageSize> ConPty::pixelSize() const noexcept
{
    return _pixels;
}

std::expected<InheritedPty, AdoptError> ConPty::handover() const
{
    // Not while close() frees the pseudoconsole on another thread: as a program's exit watcher does.
    auto const _ = std::lock_guard { _mutex };
    return handoverLocked();
}

std::expected<InheritedPty, AdoptError> ConPty::handoverLocked() const
{
    if (consoleHost() != ConsoleHost::Bundled)
        return std::unexpected(AdoptError::ForeignConsoleHost);
    if (isClosed() || _input == INVALID_HANDLE_VALUE || _output == INVALID_HANDLE_VALUE)
        return std::unexpected(AdoptError::BadDescriptor);
    auto const* const console = reinterpret_cast<BundledPseudoConsole const*>(_master);
    return InheritedPty { .pageSize = _size,
                          .pixels = _pixels,
                          .conpty = InheritedConPty { .outputReader = CrossingHandle::of(_input),
                                                      .inputWriter = CrossingHandle::of(_output),
                                                      .signal = CrossingHandle::of(console->signal),
                                                      .reference = CrossingHandle::of(console->reference),
                                                      .consoleHost = CrossingHandle::of(console->process) } };
}

std::expected<InheritedPty, AdoptError> ConPty::releaseHandover()
{
    auto const _ = std::lock_guard { _mutex };
    auto inherited = handoverLocked();
    if (!inherited)
        return inherited;
    _closeDeferral = CloseDeferral::None; // nothing is this process's to close any more

    // A read or a write under way is ended first, and each handle forgotten once nothing uses it, as close()
    // closes it; but nothing is closed.
    SetEvent(_closing);
    interruptWritesLocked();
    {
        auto const reading = std::scoped_lock { _readMutex };
        _input = INVALID_HANDLE_VALUE;
    }
    {
        auto const writing = std::scoped_lock { _writeMutex };
        _output = INVALID_HANDLE_VALUE;
    }
    if (_master != INVALID_HANDLE_VALUE)
    {
        // Only the struct this process allocated around the handles: an adopter allocates its own.
        HeapFree(GetProcessHeap(), 0, reinterpret_cast<BundledPseudoConsole*>(_master));
        _master = INVALID_HANDLE_VALUE;
    }
    if (_closed != nullptr)
        SetEvent(_closed);
    return inherited;
}

StartResult ConPty::start()
{
    // Running already: an adopted pseudoconsole, or a second call. Another one would replace it.
    if (!isClosed())
        return {};
    ptyLog()("Starting ConPTY");

    auto const failed = [](std::string detail) {
        return std::unexpected(
            StartFailure { .error = StartError::PtyAllocationFailed, .detail = std::move(detail) });
    };

    if (_readCompleted == nullptr || _writeCompleted == nullptr || _writesInterrupted == nullptr
        || _wakeup == nullptr || _closing == nullptr || _closed == nullptr)
        return failed("Could not create the events this PTY waits on.");

    // Create the pipes to which the ConPty will connect to
    auto const inputPipe = createPipe(PipeFlow::ToPseudoconsole);
    if (!inputPipe)
        return failed(core::systemErrorMessage(inputPipe.error()));
    auto* const hPipePTYIn = inputPipe->second;
    _output = inputPipe->first;

    auto const outputPipe = createPipe(PipeFlow::FromPseudoconsole);
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
    // Quiesced for a live upgrade that may yet hand this pseudoconsole on, by handles a capture names: closed
    // now, they would be freed for whatever this process opens next -- the handover's own mapping and events
    // among them -- and the successor would adopt those under their names. Held until the upgrade rolls back
    // (allowWrites()), or the handles are handed on (releaseHandover()), or this ConPty goes.
    if (_writeInterruption.load(std::memory_order_acquire) == WriteInterruption::Interrupted && !isClosed())
    {
        ptyLog()("ConPty.close() held until writes are allowed again");
        _closeDeferral = CloseDeferral::Pending;
        return;
    }
    closeLocked();
}

void ConPty::closeLocked()
{
    _closeDeferral = CloseDeferral::None;

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
    // Serialized against close(): a handle closed under a write may be handed to whatever is opened
    // next -- another pane's pipe, a file -- and the write would go there.
    auto const _ = std::scoped_lock { _writeMutex };
    if (_output == INVALID_HANDLE_VALUE)
    {
        errno = EPIPE;
        return -1;
    }

    // Never more at once than the pipe takes without waiting, which a write completes with then and there;
    // only while it is full, a single byte, which waits until the pseudoconsole reads it -- or until writes
    // are interrupted, which cancels it. A cancelled write of one byte was read or was not; one of more may
    // have been read in part, and the pipe then reports none of it written, so what was written would be
    // written again.
    //
    // The room is asked of the pipe only when a write needs more than is known to be free: this is the pipe's
    // one writer, and its reader only ever frees room, so what was free less what was written since is free
    // still. A keystroke asks nothing.
    auto written = std::size_t { 0 };
    while (written < data.size())
    {
        if (_writeInterruption.load(std::memory_order_acquire) == WriteInterruption::Interrupted)
        {
            errno = EINTR;
            return static_cast<int>(written);
        }
        auto const left = data.size() - written;
        if (left > _knownRoom && left > 1)
            _knownRoom = writableWithoutWaiting(_output);
        auto const chunk = data.substr(written, std::clamp<std::size_t>(_knownRoom, 1, left));
        auto overlapped = OVERLAPPED {};
        overlapped.hEvent = _writeCompleted;
        if (!WriteFile(_output, chunk.data(), static_cast<DWORD>(chunk.size()), nullptr, &overlapped))
        {
            if (GetLastError() != ERROR_IO_PENDING)
                return writeFailed(data.size(), written);
            // Only a single byte waits for the reader. A write that completes as writes are interrupted
            // counts as completed: the wait names the lowest of the two.
            auto const events = std::array { _writeCompleted, _writesInterrupted };
            if (chunk.size() == 1
                && WaitForMultipleObjects(static_cast<DWORD>(events.size()), events.data(), FALSE, INFINITE)
                       != WAIT_OBJECT_0)
                CancelIoEx(_output, &overlapped);
        }
        auto nwritten = DWORD {};
        if (!GetOverlappedResult(_output, &overlapped, &nwritten, TRUE))
        {
            if (GetLastError() != ERROR_OPERATION_ABORTED)
                return writeFailed(data.size(), written);
            errno = EINTR;
            return static_cast<int>(written);
        }
        if (ptyOutLog)
            ptyOutLog()("Sending bytes: \"{}\"", core::escape(chunk.substr(0, nwritten)));
        _knownRoom -= std::min<std::size_t>(_knownRoom, nwritten);
        written += nwritten;
    }
    return static_cast<int>(written);
}

void ConPty::interruptWrites()
{
    // The flag for a write about to start, the event for one already waiting. Under the lock, so that a
    // close() either finished before it or finds writes interrupted, and is held (@see close()).
    auto const _ = std::lock_guard { _mutex };
    interruptWritesLocked();
}

void ConPty::interruptWritesLocked()
{
    _writeInterruption.store(WriteInterruption::Interrupted, std::memory_order_release);
    SetEvent(_writesInterrupted);
}

void ConPty::allowWrites()
{
    auto const _ = std::lock_guard { _mutex };
    ResetEvent(_writesInterrupted);
    _writeInterruption.store(WriteInterruption::Allowed, std::memory_order_release);
    // A close() asked for while the upgrade was under way, which has rolled back.
    if (_closeDeferral == CloseDeferral::Pending)
        closeLocked();
}

PageSize ConPty::pageSize() const noexcept
{
    return _size;
}

void ConPty::resizeScreen(PageSize cells, std::optional<ImageSize> pixels)
{
    // The pseudoconsole takes no pixel size; it is kept for a live upgrade, whose successor restores the
    // terminal's cell size from it before any client says it again.
    if (pixels)
        _pixels = pixels;

    // No live pseudoconsole to resize: remember the size rather than drop it, so a subsequent
    // start() creates the pseudoconsole at it. attachDisplay() is what first tells a PTY its size,
    // and it now runs before start() -- dropping the size here left the child born at whatever the
    // constructor was handed (the profile's configured terminal size), so the shell's first prompt,
    // and any program launched before the first window-geometry change, wrapped at the wrong column.
    // UnixPty stashes the same way.
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
