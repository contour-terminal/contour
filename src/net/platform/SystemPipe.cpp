// SPDX-License-Identifier: Apache-2.0

// On Windows, winsock2.h MUST precede windows.h (which net/platform/SystemPipe.h
// pulls in via net/platform/NativeHandle.h), so this block comes before any other
// include.
// clang-format off
#ifdef _WIN32
    #include <winsock2.h>
    #include <windows.h>
#endif
// clang-format on

#include <net/platform/SystemPipe.h>

#include <array>
#include <cerrno>

#include <net/platform/WindowsLoopback.h>
#include <net/platform/WinsockInit.h>

#ifndef _WIN32
    #include <sys/socket.h>

    #include <unistd.h>
#endif

namespace net
{

namespace
{

#ifndef _WIN32

    /// POSIX SystemPipe: a connected AF_UNIX socketpair. The read fd polls directly,
    /// so waitHandle() == readFd().
    class PosixSystemPipe final: public SystemPipe
    {
      public:
        PosixSystemPipe(int readFd, int writeFd) noexcept: _readFd(readFd), _writeFd(writeFd) {}

        ~PosixSystemPipe() override
        {
            if (_readFd >= 0)
                ::close(_readFd);
            if (_writeFd >= 0)
                ::close(_writeFd);
        }

        PosixSystemPipe(PosixSystemPipe const&) = delete;
        PosixSystemPipe& operator=(PosixSystemPipe const&) = delete;
        PosixSystemPipe(PosixSystemPipe&&) = delete;
        PosixSystemPipe& operator=(PosixSystemPipe&&) = delete;

        [[nodiscard]] NativeHandle waitHandle() const noexcept override { return _readFd; }

        [[nodiscard]] NativeHandle readFd() const noexcept override { return _readFd; }

        [[nodiscard]] NativeHandle writeFd() const noexcept override { return _writeFd; }

        [[nodiscard]] IoResult write(void const* data, std::size_t size) override
        {
            auto const n = ::send(_writeFd, data, size, 0);
            if (n < 0)
                return std::unexpected(makeNetError(NetErrorCode::Other, errno, "SystemPipe send"));
            return static_cast<std::size_t>(n);
        }

        [[nodiscard]] IoResult read(void* data, std::size_t size) override
        {
            auto const n = ::recv(_readFd, data, size, 0);
            if (n < 0)
                return std::unexpected(makeNetError(NetErrorCode::Other, errno, "SystemPipe recv"));
            return static_cast<std::size_t>(n);
        }

        [[nodiscard]] bool good() const noexcept override { return _readFd >= 0 && _writeFd >= 0; }

      private:
        int _readFd;
        int _writeFd;
    };

#else // _WIN32

    /// Windows SystemPipe: a loopback TCP socket pair with the read socket mapped to a
    /// waitable WSAEVENT via WSAEventSelect, so waitHandle() (the event) integrates
    /// with WaitForMultipleObjects while readFd()/writeFd() carry the bytes.
    class WindowsSystemPipe final: public SystemPipe
    {
      public:
        WindowsSystemPipe(SOCKET readSock, SOCKET writeSock, WSAEVENT event) noexcept:
            _readSock(readSock), _writeSock(writeSock), _event(event)
        {
        }

        ~WindowsSystemPipe() override
        {
            if (_event != WSA_INVALID_EVENT)
                WSACloseEvent(_event);
            if (_readSock != INVALID_SOCKET)
                closesocket(_readSock);
            if (_writeSock != INVALID_SOCKET)
                closesocket(_writeSock);
        }

        WindowsSystemPipe(WindowsSystemPipe const&) = delete;
        WindowsSystemPipe& operator=(WindowsSystemPipe const&) = delete;
        WindowsSystemPipe(WindowsSystemPipe&&) = delete;
        WindowsSystemPipe& operator=(WindowsSystemPipe&&) = delete;

        [[nodiscard]] NativeHandle waitHandle() const noexcept override
        {
            return static_cast<HANDLE>(_event);
        }

        [[nodiscard]] NativeHandle readFd() const noexcept override
        {
            return reinterpret_cast<HANDLE>(_readSock);
        }

        [[nodiscard]] NativeHandle writeFd() const noexcept override
        {
            return reinterpret_cast<HANDLE>(_writeSock);
        }

        [[nodiscard]] IoResult write(void const* data, std::size_t size) override
        {
            auto const n = ::send(_writeSock, static_cast<char const*>(data), static_cast<int>(size), 0);
            if (n == SOCKET_ERROR)
                return std::unexpected(
                    makeNetError(NetErrorCode::Other, WSAGetLastError(), "SystemPipe send"));
            return static_cast<std::size_t>(n);
        }

        [[nodiscard]] IoResult read(void* data, std::size_t size) override
        {
            // Reset the readiness event before draining: WSAEventSelect is
            // edge-triggered per event type, and recv re-arms FD_READ if data remains.
            WSAResetEvent(_event);
            auto const n = ::recv(_readSock, static_cast<char*>(data), static_cast<int>(size), 0);
            if (n == SOCKET_ERROR)
                return std::unexpected(
                    makeNetError(NetErrorCode::Other, WSAGetLastError(), "SystemPipe recv"));
            return static_cast<std::size_t>(n);
        }

        [[nodiscard]] bool good() const noexcept override
        {
            return _readSock != INVALID_SOCKET && _writeSock != INVALID_SOCKET && _event != WSA_INVALID_EVENT;
        }

      private:
        SOCKET _readSock;
        SOCKET _writeSock;
        WSAEVENT _event;
    };

#endif // _WIN32

} // namespace

std::expected<std::unique_ptr<SystemPipe>, NetError> createSystemPipe()
{
    ensureWinsockInitialized(); // no-op on POSIX; required before any socket call on Windows
#ifndef _WIN32
    auto fds = std::array<int, 2> {};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds.data()) != 0)
        return std::unexpected(makeNetError(NetErrorCode::Other, errno, "socketpair"));
    return std::make_unique<PosixSystemPipe>(fds[0], fds[1]);
#else
    auto pair = std::array<SOCKET, 2> {};
    if (!makeLoopbackPair(pair))
        return std::unexpected(makeNetError(NetErrorCode::Other, WSAGetLastError(), "loopback pair"));

    auto const event = WSACreateEvent();
    if (event == WSA_INVALID_EVENT)
    {
        closesocket(pair[0]);
        closesocket(pair[1]);
        return std::unexpected(makeNetError(NetErrorCode::Other, WSAGetLastError(), "WSACreateEvent"));
    }
    // Watch the read socket for incoming data and peer close. WSAEventSelect also
    // puts the socket into non-blocking mode, which is what we want for the
    // reactor-driven drain in read().
    if (WSAEventSelect(pair[0], event, FD_READ | FD_CLOSE) == SOCKET_ERROR)
    {
        WSACloseEvent(event);
        closesocket(pair[0]);
        closesocket(pair[1]);
        return std::unexpected(makeNetError(NetErrorCode::Other, WSAGetLastError(), "WSAEventSelect"));
    }
    return std::make_unique<WindowsSystemPipe>(pair[0], pair[1], event);
#endif
}

} // namespace net
