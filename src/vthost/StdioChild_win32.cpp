// SPDX-License-Identifier: Apache-2.0
#include <vthost/StdioChild.hpp>

#ifdef _WIN32

    #include <core/WideString.hpp>
    #include <core/net/Sockets.hpp>

    #include <array>
    #include <format>
    #include <initializer_list>
    #include <string_view>
    #include <system_error>
    #include <thread>
    #include <utility>
    #include <vector>

    #include <vthost/ByteStream.hpp>
    #include <vthost/Daemon.hpp>
    #include <vthost/PlatformStreams.hpp>
    #include <vthost/StderrTail.hpp>

// clang-format off
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <windows.h>
// clang-format on

namespace vthost
{

namespace
{
    [[nodiscard]] StdioSpawnError systemError(std::string_view what, DWORD error)
    {
        return StdioSpawnError {
            .kind = SpawnFailure::SystemError,
            .message = std::format("{}: {}", what, std::system_category().message(static_cast<int>(error)))
        };
    }

    /// Closes a handle once, when it goes out of scope.
    class Handle
    {
      public:
        explicit Handle(HANDLE handle = nullptr) noexcept: _handle { handle } {}
        ~Handle() { reset(); }
        Handle(Handle const&) = delete;
        Handle& operator=(Handle const&) = delete;
        Handle(Handle&& other) noexcept: _handle { other.release() } {}
        Handle& operator=(Handle&& other) noexcept
        {
            reset(other.release());
            return *this;
        }

        [[nodiscard]] HANDLE get() const noexcept { return _handle; }
        [[nodiscard]] HANDLE release() noexcept { return std::exchange(_handle, nullptr); }
        void reset(HANDLE handle = nullptr) noexcept
        {
            if (auto const old = std::exchange(_handle, handle);
                old != nullptr && old != INVALID_HANDLE_VALUE)
                CloseHandle(old);
        }

      private:
        HANDLE _handle;
    };

    /// One end of an anonymous pipe as a stream: the child's stdin (written) or stdout (read).
    class PipeStream final: public IByteStream
    {
      public:
        PipeStream(Handle read, Handle write): _read { std::move(read) }, _write { std::move(write) } {}

        [[nodiscard]] std::expected<std::size_t, std::string> readSome(std::span<std::byte> buffer) override
        {
            auto count = DWORD { 0 };
            if (ReadFile(_read.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr))
                return static_cast<std::size_t>(count);
            if (auto const error = GetLastError(); error != ERROR_BROKEN_PIPE)
                return std::unexpected { std::system_category().message(static_cast<int>(error)) };
            return 0;
        }

        [[nodiscard]] std::expected<void, std::string> writeAll(std::span<std::byte const> bytes) override
        {
            while (!bytes.empty())
            {
                auto count = DWORD { 0 };
                if (_write.get() == nullptr
                    || !WriteFile(
                        _write.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr))
                    return std::unexpected { std::string { "the child's stdin is closed" } };
                bytes = bytes.subspan(count);
            }
            return {};
        }

        void closeWrite() noexcept override { _write.reset(); }

      private:
        Handle _read;
        Handle _write;
    };

    /// A blocking loopback TCP pair: what the reactor end is adopted from and the bridge's end.
    struct LoopbackPair
    {
        SOCKET reactorEnd = INVALID_SOCKET;
        SOCKET bridgeEnd = INVALID_SOCKET;
    };

    [[nodiscard]] std::expected<LoopbackPair, StdioSpawnError> makeLoopbackPair()
    {
        // Once per process, as PlatformStreams does: every WSAStartup would need its own WSACleanup.
        static auto const startupError = []() {
            auto data = WSADATA {};
            return WSAStartup(MAKEWORD(2, 2), &data);
        }();
        if (startupError != 0)
            return std::unexpected { systemError("WSAStartup", static_cast<DWORD>(startupError)) };
        auto const listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener == INVALID_SOCKET)
            return std::unexpected { systemError("socket", static_cast<DWORD>(WSAGetLastError())) };
        auto const closeListener = [listener]() {
            closesocket(listener);
        };

        auto address = sockaddr_in {};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        auto length = static_cast<int>(sizeof(address));
        if (::bind(listener, reinterpret_cast<sockaddr const*>(&address), sizeof(address)) == SOCKET_ERROR
            || ::listen(listener, 1) == SOCKET_ERROR
            || ::getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length) == SOCKET_ERROR)
        {
            auto const error = static_cast<DWORD>(WSAGetLastError());
            closeListener();
            return std::unexpected { systemError("cannot listen on the loopback", error) };
        }

        auto pair = LoopbackPair {};
        pair.bridgeEnd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (pair.bridgeEnd == INVALID_SOCKET
            || ::connect(pair.bridgeEnd, reinterpret_cast<sockaddr const*>(&address), sizeof(address))
                   == SOCKET_ERROR)
        {
            auto const error = static_cast<DWORD>(WSAGetLastError());
            if (pair.bridgeEnd != INVALID_SOCKET)
                closesocket(pair.bridgeEnd);
            closeListener();
            return std::unexpected { systemError("cannot connect on the loopback", error) };
        }
        pair.reactorEnd = ::accept(listener, nullptr, nullptr);
        auto const error = static_cast<DWORD>(WSAGetLastError());
        closeListener();
        if (pair.reactorEnd == INVALID_SOCKET)
        {
            closesocket(pair.bridgeEnd);
            return std::unexpected { systemError("cannot accept on the loopback", error) };
        }
        return pair;
    }

    /// Copies the child's stderr into @p tail until it ends. Detached, as on POSIX.
    void drainInto(Handle read, std::shared_ptr<StderrTail> tail)
    {
        std::thread { [read = std::move(read), tail = std::move(tail)]() {
            auto buffer = std::array<char, 1024> {};
            auto count = DWORD { 0 };
            while (ReadFile(read.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr)
                   && count > 0)
                tail->append(std::string_view { buffer.data(), count });
            tail->end();
        } }.detach();
    }

    class Win32StdioChild final: public IStdioChild
    {
      public:
        Win32StdioChild(Handle process, std::shared_ptr<StderrTail> tail):
            _process { std::move(process) }, _tail { std::move(tail) }
        {
        }
        ~Win32StdioChild() override { terminate(); }
        Win32StdioChild(Win32StdioChild const&) = delete;
        Win32StdioChild& operator=(Win32StdioChild const&) = delete;
        Win32StdioChild(Win32StdioChild&&) = delete;
        Win32StdioChild& operator=(Win32StdioChild&&) = delete;

        [[nodiscard]] std::string stderrTail() const override { return _tail->text(); }

        [[nodiscard]] std::optional<int> exitCode(std::chrono::milliseconds wait) override
        {
            auto const deadline = std::chrono::steady_clock::now() + wait;
            if (WaitForSingleObject(_process.get(), static_cast<DWORD>(wait.count())) != WAIT_OBJECT_0)
                return std::nullopt;
            auto code = DWORD { 0 };
            GetExitCodeProcess(_process.get(), &code);
            _tail->awaitEnd(deadline);
            return static_cast<int>(code);
        }

        void terminate() noexcept override
        {
            if (WaitForSingleObject(_process.get(), 0) == WAIT_OBJECT_0)
                return;
            TerminateProcess(_process.get(), 1);
            WaitForSingleObject(_process.get(), INFINITE);
        }

      private:
        Handle _process;
        std::shared_ptr<StderrTail> _tail;
    };

    class Win32StdioSpawner final: public IStdioSpawner
    {
      public:
        explicit Win32StdioSpawner(vtpty::ProgramSearch search): _search { std::move(search) } {}

        [[nodiscard]] std::expected<SpawnedStdioChild, StdioSpawnError> spawn(
            core::net::EventLoop& loop, std::span<std::string const> argv) override
        {
            if (argv.empty())
                return std::unexpected { StdioSpawnError { .kind = SpawnFailure::SystemError,
                                                           .message = "no program to start" } };
            auto const image = vtpty::findProgram(argv.front(), _search);
            if (!image)
                return std::unexpected { StdioSpawnError {
                    .kind = SpawnFailure::ProgramNotFound,
                    .message = std::format("{} was not found on PATH ({})", argv.front(), _search.path) } };

            // Anonymous pipes for the child's three streams; only the child's ends are inheritable.
            auto inheritable = SECURITY_ATTRIBUTES { .nLength = sizeof(SECURITY_ATTRIBUTES),
                                                     .lpSecurityDescriptor = nullptr,
                                                     .bInheritHandle = TRUE };
            auto pipes = std::array<std::array<HANDLE, 2>, 3> {};
            for (auto& pipe: pipes)
                if (!CreatePipe(&pipe[0], &pipe[1], &inheritable, 0))
                    return std::unexpected { systemError("CreatePipe", GetLastError()) };
            auto stdinRead = Handle { pipes[0][0] };
            auto stdinWrite = Handle { pipes[0][1] };
            auto stdoutRead = Handle { pipes[1][0] };
            auto stdoutWrite = Handle { pipes[1][1] };
            auto stderrRead = Handle { pipes[2][0] };
            auto stderrWrite = Handle { pipes[2][1] };
            for (auto const parentEnd: { stdinWrite.get(), stdoutRead.get(), stderrRead.get() })
                SetHandleInformation(parentEnd, HANDLE_FLAG_INHERIT, 0);

            auto commandLine = core::toUtf16(joinCommandLine(argv));
            auto const program = core::toUtf16(*image);
            if (!commandLine || !program)
                return std::unexpected { StdioSpawnError {
                    .kind = SpawnFailure::SystemError, .message = "the command line is not valid UTF-8" } };
            auto startup = STARTUPINFOW {};
            startup.cb = sizeof(startup);
            startup.dwFlags = STARTF_USESTDHANDLES;
            startup.hStdInput = stdinRead.get();
            startup.hStdOutput = stdoutWrite.get();
            startup.hStdError = stderrWrite.get();
            auto info = PROCESS_INFORMATION {};
            // CREATE_NO_WINDOW: a GUI parent must not flash a console for ssh.
            if (!CreateProcessW(program->c_str(),
                                commandLine->data(),
                                nullptr,
                                nullptr,
                                TRUE,
                                CREATE_NO_WINDOW,
                                nullptr,
                                nullptr,
                                &startup,
                                &info))
                return std::unexpected { systemError(std::format("cannot start {}", *image),
                                                     GetLastError()) };
            CloseHandle(info.hThread);
            auto process = Handle { info.hProcess };
            // The child holds its own copies now; the parent's would keep its streams from ever ending.
            stdinRead.reset();
            stdoutWrite.reset();
            stderrWrite.reset();

            auto tail = std::make_shared<StderrTail>(StderrTailLimit);
            drainInto(std::move(stderrRead), tail);
            auto child = std::make_unique<Win32StdioChild>(std::move(process), std::move(tail));

            auto pair = makeLoopbackPair();
            if (!pair)
                return std::unexpected { pair.error() };
            auto transport = core::net::adoptSocket(
                loop, reinterpret_cast<core::platform::NativeHandle>(pair->reactorEnd), "stdio-child");
            if (!transport)
            {
                closesocket(pair->bridgeEnd);
                return std::unexpected { StdioSpawnError {
                    .kind = SpawnFailure::SystemError,
                    .message =
                        std::format("cannot adopt the child's socket: {}", transport.error().toString()) } };
            }

            // The bridge: socket→stdin and stdout→socket, each a pumpBytes on a thread of its own that
            // co-owns both streams; each ends when its source does. Neither is joined: the last owner to
            // drop closes the socket, and a child that never writes stdout keeps the stdout→socket thread
            // until terminate() ends the child and with it the pipe's write end.
            auto socket = makeSocketStream(reinterpret_cast<core::platform::NativeHandle>(pair->bridgeEnd));
            auto pipe = std::make_shared<PipeStream>(std::move(stdoutRead), std::move(stdinWrite));
            std::thread { [socket, pipe]() { (void) pumpBytes(*socket, *pipe); } }.detach();
            std::thread { [socket, pipe]() { (void) pumpBytes(*pipe, *socket); } }.detach();
            return SpawnedStdioChild { .transport = std::move(*transport), .process = std::move(child) };
        }

      private:
        vtpty::ProgramSearch _search;
    };
} // namespace

std::unique_ptr<IStdioSpawner> makeSystemStdioSpawner(vtpty::ProgramSearch search)
{
    return std::make_unique<Win32StdioSpawner>(std::move(search));
}

} // namespace vthost

#endif // _WIN32
