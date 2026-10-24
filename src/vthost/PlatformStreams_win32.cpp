// SPDX-License-Identifier: Apache-2.0
#include <vthost/PlatformStreams.hpp>

#ifdef _WIN32

    #include <core/Utf8Path.hpp>

    #include <algorithm>
    #include <cstdio>
    #include <format>
    #include <iostream>
    #include <mutex>
    #include <system_error>
    #include <utility>

// clang-format off
    #include <winsock2.h>
    #include <afunix.h>
    #include <windows.h>
    #include <io.h>
// clang-format on

namespace vthost
{

namespace
{
    /// @return The text of @p error, a Win32 or Winsock error code.
    [[nodiscard]] std::string describeError(DWORD error)
    {
        return std::system_category().message(static_cast<int>(error));
    }

    /// This process's stdin and stdout as raw handles: ReadFile/WriteFile, never the CRT's text mode.
    class ProcessStdioStream final: public IByteStream
    {
      public:
        ProcessStdioStream(HANDLE input, HANDLE output): _input { input }, _output { output } {}

        [[nodiscard]] std::expected<std::size_t, std::string> readSome(std::span<std::byte> buffer) override
        {
            auto count = DWORD { 0 };
            if (ReadFile(_input, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr))
                return static_cast<std::size_t>(count);
            auto const error = GetLastError();
            if (error == ERROR_BROKEN_PIPE || error == ERROR_HANDLE_EOF)
                return 0;
            return std::unexpected { describeError(error) };
        }

        [[nodiscard]] std::expected<void, std::string> writeAll(std::span<std::byte const> bytes) override
        {
            if (_output == INVALID_HANDLE_VALUE)
                return std::unexpected { std::string { "the stream is closed for writing" } };
            while (!bytes.empty())
            {
                auto count = DWORD { 0 };
                if (!WriteFile(_output, bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr))
                    return std::unexpected { describeError(GetLastError()) };
                bytes = bytes.subspan(count);
            }
            return {};
        }

        void closeWrite() noexcept override
        {
            if (auto const output = std::exchange(_output, INVALID_HANDLE_VALUE);
                output != INVALID_HANDLE_VALUE)
                CloseHandle(output);
        }

      private:
        HANDLE _input;
        HANDLE _output;
    };

    /// A connected Winsock stream socket.
    class SocketStream final: public IByteStream
    {
      public:
        explicit SocketStream(SOCKET socket): _socket { socket } {}
        ~SocketStream() override { closesocket(_socket); }

        SocketStream(SocketStream const&) = delete;
        SocketStream& operator=(SocketStream const&) = delete;
        SocketStream(SocketStream&&) = delete;
        SocketStream& operator=(SocketStream&&) = delete;

        [[nodiscard]] std::expected<std::size_t, std::string> readSome(std::span<std::byte> buffer) override
        {
            auto const count =
                recv(_socket, reinterpret_cast<char*>(buffer.data()), static_cast<int>(buffer.size()), 0);
            if (count == SOCKET_ERROR)
                return std::unexpected { describeError(static_cast<DWORD>(WSAGetLastError())) };
            return static_cast<std::size_t>(count);
        }

        [[nodiscard]] std::expected<void, std::string> writeAll(std::span<std::byte const> bytes) override
        {
            while (!bytes.empty())
            {
                auto const count = send(
                    _socket, reinterpret_cast<char const*>(bytes.data()), static_cast<int>(bytes.size()), 0);
                if (count == SOCKET_ERROR)
                    return std::unexpected { describeError(static_cast<DWORD>(WSAGetLastError())) };
                bytes = bytes.subspan(static_cast<std::size_t>(count));
            }
            return {};
        }

        void closeWrite() noexcept override { shutdown(_socket, SD_SEND); }

      private:
        SOCKET _socket;
    };

    /// Initializes Winsock once for this process; core::net does the same for its own sockets.
    [[nodiscard]] bool winsockReady()
    {
        static auto const ready = []() {
            auto data = WSADATA {};
            return WSAStartup(MAKEWORD(2, 2), &data) == 0;
        }();
        return ready;
    }
} // namespace

std::shared_ptr<IByteStream> makeProcessStdioStream()
{
    std::cout.flush();
    auto const input = GetStdHandle(STD_INPUT_HANDLE);
    auto const output = GetStdHandle(STD_OUTPUT_HANDLE);
    // The stream writes a private, non-inheritable copy of stdout (POSIX's F_DUPFD_CLOEXEC): the _dup2
    // below closes fd 1's handle, which is the very handle GetStdHandle just returned.
    auto stdoutCopy = HANDLE { nullptr };
    if (!DuplicateHandle(GetCurrentProcess(),
                         output,
                         GetCurrentProcess(),
                         &stdoutCopy,
                         0,
                         /*bInheritHandle=*/FALSE,
                         DUPLICATE_SAME_ACCESS))
        return std::make_shared<ProcessStdioStream>(input, output);
    // stdout is the protocol's from now on: the CRT's stdout and the process's standard output handle
    // both go to stderr, so a stray write cannot reach the stream.
    SetStdHandle(STD_OUTPUT_HANDLE, GetStdHandle(STD_ERROR_HANDLE));
    _dup2(_fileno(stderr), _fileno(stdout));
    return std::make_shared<ProcessStdioStream>(input, stdoutCopy);
}

std::expected<std::shared_ptr<IByteStream>, std::string> connectUnixStream(std::filesystem::path const& path)
{
    if (!winsockReady())
        return std::unexpected { std::string { "cannot initialize Winsock" } };
    auto const text = core::pathToUtf8(path);
    auto address = sockaddr_un {};
    address.sun_family = AF_UNIX;
    if (text.size() >= sizeof(address.sun_path))
        return std::unexpected { std::format("the socket path is too long: {}", text) };
    std::ranges::copy(text, std::begin(address.sun_path));

    auto const socket = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (socket == INVALID_SOCKET)
        return std::unexpected { std::format("cannot create a socket: {}",
                                             describeError(static_cast<DWORD>(WSAGetLastError()))) };
    auto stream = std::make_shared<SocketStream>(socket);
    if (::connect(socket, reinterpret_cast<sockaddr const*>(&address), sizeof(address)) == SOCKET_ERROR)
        return std::unexpected { std::format(
            "cannot connect to {}: {}", text, describeError(static_cast<DWORD>(WSAGetLastError()))) };
    return stream;
}

std::shared_ptr<IByteStream> makeSocketStream(core::platform::NativeHandle handle)
{
    return std::make_shared<SocketStream>(reinterpret_cast<SOCKET>(handle));
}

} // namespace vthost

#endif // _WIN32
