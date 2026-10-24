// SPDX-License-Identifier: Apache-2.0
#include <vthost/PlatformStreams.hpp>

#ifndef _WIN32

    #include <core/Utf8Path.hpp>

    #include <sys/socket.h>
    #include <sys/un.h>

    #include <algorithm>
    #include <cerrno>
    #include <cstring>
    #include <format>
    #include <iostream>
    #include <system_error>
    #include <utility>

    #include <fcntl.h>
    #include <unistd.h>

namespace vthost
{

namespace
{
    /// @return The text of @p error, the errno a call just failed with.
    [[nodiscard]] std::string describeErrno(int error)
    {
        return std::system_category().message(error);
    }

    /// How a stream ends its writing side.
    enum class HalfClose : std::uint8_t
    {
        CloseDescriptor = 0, ///< Closes the write descriptor (a pipe, a terminal: stdout).
        ShutdownSocket,      ///< shutdown(SHUT_WR) on the one socket that reads and writes.
    };

    /// A stream over POSIX descriptors, read with read(2) and written with write(2) or send(2).
    class DescriptorStream final: public IByteStream
    {
      public:
        /// @param readFd What is read; owned only as the socket of HalfClose::ShutdownSocket.
        /// @param writeFd What is written; owned.
        /// @param halfClose How closeWrite() ends the writing side.
        DescriptorStream(int readFd, int writeFd, HalfClose halfClose):
            _readFd { readFd }, _writeFd { writeFd }, _halfClose { halfClose }
        {
        }

        ~DescriptorStream() override
        {
            if (_halfClose == HalfClose::ShutdownSocket)
                ::close(_readFd);
            else if (_writeFd >= 0)
                ::close(_writeFd);
        }

        DescriptorStream(DescriptorStream const&) = delete;
        DescriptorStream& operator=(DescriptorStream const&) = delete;
        DescriptorStream(DescriptorStream&&) = delete;
        DescriptorStream& operator=(DescriptorStream&&) = delete;

        [[nodiscard]] std::expected<std::size_t, std::string> readSome(std::span<std::byte> buffer) override
        {
            while (true)
            {
                auto const count = ::read(_readFd, buffer.data(), buffer.size());
                if (count >= 0)
                    return static_cast<std::size_t>(count);
                if (errno != EINTR)
                    return std::unexpected { describeErrno(errno) };
            }
        }

        [[nodiscard]] std::expected<void, std::string> writeAll(std::span<std::byte const> bytes) override
        {
            if (_writeFd < 0)
                return std::unexpected { std::string { "the stream is closed for writing" } };
            while (!bytes.empty())
            {
                auto const count = writeSome(bytes);
                if (count < 0)
                {
                    if (errno == EINTR)
                        continue;
                    return std::unexpected { describeErrno(errno) };
                }
                bytes = bytes.subspan(static_cast<std::size_t>(count));
            }
            return {};
        }

        void closeWrite() noexcept override
        {
            switch (_halfClose)
            {
                case HalfClose::ShutdownSocket: ::shutdown(_writeFd, SHUT_WR); break;
                case HalfClose::CloseDescriptor:
                    if (auto const fd = std::exchange(_writeFd, -1); fd >= 0)
                        ::close(fd);
                    break;
            }
        }

      private:
        [[nodiscard]] ssize_t writeSome(std::span<std::byte const> bytes) const noexcept
        {
    #ifdef MSG_NOSIGNAL
            // A peer that went away is an error to report, not a SIGPIPE that ends the process.
            if (_halfClose == HalfClose::ShutdownSocket)
                return ::send(_writeFd, bytes.data(), bytes.size(), MSG_NOSIGNAL);
    #endif
            return ::write(_writeFd, bytes.data(), bytes.size());
        }

        int _readFd;
        int _writeFd;
        HalfClose _halfClose;
    };
} // namespace

std::shared_ptr<IByteStream> makeProcessStdioStream()
{
    // Whatever std::cout holds already goes out now, before stdout is taken over.
    std::cout.flush();
    auto const stdoutCopy = ::fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
    if (stdoutCopy < 0)
        return std::make_shared<DescriptorStream>(STDIN_FILENO, STDOUT_FILENO, HalfClose::CloseDescriptor);
    ::dup2(STDERR_FILENO, STDOUT_FILENO);
    return std::make_shared<DescriptorStream>(STDIN_FILENO, stdoutCopy, HalfClose::CloseDescriptor);
}

std::expected<std::shared_ptr<IByteStream>, std::string> connectUnixStream(std::filesystem::path const& path)
{
    auto const text = core::pathToUtf8(path);
    auto address = sockaddr_un {};
    address.sun_family = AF_UNIX;
    if (text.size() >= sizeof(address.sun_path))
        return std::unexpected { std::format("the socket path is too long: {}", text) };
    std::ranges::copy(text, std::begin(address.sun_path));

    auto const fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return std::unexpected { std::format("cannot create a socket: {}", describeErrno(errno)) };
    // Kept as a stream at once, so every return below closes it.
    auto stream = makeSocketStream(fd);
    while (::connect(fd, reinterpret_cast<sockaddr const*>(&address), sizeof(address)) < 0)
    {
        // An interrupted connect goes on in the kernel; asking again says when it is done.
        if (errno == EISCONN)
            break;
        if (errno != EINTR && errno != EALREADY)
            return std::unexpected { std::format("cannot connect to {}: {}", text, describeErrno(errno)) };
    }
    return stream;
}

std::shared_ptr<IByteStream> makeSocketStream(core::platform::NativeHandle handle)
{
    #ifdef SO_NOSIGPIPE
    // Where send(2) has no MSG_NOSIGNAL, the socket itself is told not to raise SIGPIPE.
    auto const on = 1;
    ::setsockopt(handle, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
    #endif
    return std::make_shared<DescriptorStream>(handle, handle, HalfClose::ShutdownSocket);
}

} // namespace vthost

#endif // !_WIN32
