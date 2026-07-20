// SPDX-License-Identifier: Apache-2.0
#include <net/posix/UnixListener.h>

#ifndef _WIN32

    #include <sys/socket.h>
    #include <sys/stat.h>
    #include <sys/un.h>

    #include <cerrno>
    #include <cstring>

    #include <fcntl.h>
    #include <unistd.h>

    #include <net/posix/PosixSocket.h>

namespace net
{

namespace
{
    /// Makes @p fd non-blocking and close-on-exec.
    /// @return True on success.
    [[nodiscard]] bool makeNonBlockingCloexec(int fd) noexcept
    {
        auto flags = ::fcntl(fd, F_GETFL, 0);
        if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
            return false;
        auto fdFlags = ::fcntl(fd, F_GETFD, 0);
        return fdFlags >= 0 && ::fcntl(fd, F_SETFD, fdFlags | FD_CLOEXEC) >= 0;
    }
} // namespace

std::expected<void, NetError> ensureOwnedPrivateDirectory(std::filesystem::path const& directory)
{
    auto const dir = directory.string();

    // Create with owner-only rwx; an existing directory falls through to the checks.
    if (::mkdir(dir.c_str(), S_IRWXU) != 0 && errno != EEXIST)
        return std::unexpected(makeNetError(NetErrorCode::Other, errno, "mkdir " + dir));

    // lstat, not stat: a symlink planted at the path must not launder the checks
    // through its (attacker-chosen) target.
    struct stat info {};
    if (::lstat(dir.c_str(), &info) != 0)
        return std::unexpected(makeNetError(NetErrorCode::Other, errno, "lstat " + dir));

    if (!S_ISDIR(info.st_mode))
        return std::unexpected(makeNetError(NetErrorCode::Other, ENOTDIR, dir + " is not a directory"));
    if (info.st_uid != ::getuid())
        return std::unexpected(
            makeNetError(NetErrorCode::Other, EACCES, dir + " is not owned by the current user"));
    // Refuse world access (tmux's TMUX_SOCK_PERM mask: o+rwx); group access is
    // permitted, mirroring the reference behaviour.
    if ((info.st_mode & S_IRWXO) != 0)
        return std::unexpected(
            makeNetError(NetErrorCode::Other, EACCES, dir + " has unsafe (world-accessible) permissions"));

    return {};
}

UnixListener::UnixListener(EventLoop& loop, int fd, std::filesystem::path path) noexcept:
    _loop(loop), _fd(fd), _path(std::move(path))
{
}

UnixListener::~UnixListener()
{
    close();
}

void UnixListener::close() noexcept
{
    if (_closed)
        return;
    _closed = true;
    if (_fd >= 0)
    {
        ::close(_fd);
        _fd = -1;
    }
    auto ec = std::error_code {};
    std::filesystem::remove(_path, ec); // best effort; the path may already be gone
}

std::expected<std::unique_ptr<UnixListener>, NetError> UnixListener::bind(EventLoop& loop,
                                                                          std::filesystem::path const& path,
                                                                          int backlog)
{
    if (auto directory = ensureOwnedPrivateDirectory(path.parent_path()); !directory)
        return std::unexpected(directory.error());

    auto const pathString = path.string();
    auto address = sockaddr_un {};
    if (pathString.size() >= sizeof(address.sun_path))
        return std::unexpected(
            makeNetError(NetErrorCode::AddressError, ENAMETOOLONG, "socket path too long"));
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, pathString.c_str(), pathString.size() + 1);

    // Unlink a stale socket file: a previous server that crashed leaves one
    // behind, and bind() refuses an existing path.
    ::unlink(pathString.c_str());

    auto const fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return std::unexpected(makeNetError(NetErrorCode::Other, errno, "socket"));

    if (::bind(fd, reinterpret_cast<sockaddr const*>(&address), sizeof(address)) != 0)
    {
        auto const err = errno;
        ::close(fd);
        return std::unexpected(
            makeNetError(err == EADDRINUSE ? NetErrorCode::AddressInUse : NetErrorCode::Other, err, "bind"));
    }

    // Owner-only access to the socket itself; the hardened directory is the
    // outer wall, this is the inner one.
    ::chmod(pathString.c_str(), S_IRUSR | S_IWUSR);

    if (::listen(fd, backlog) != 0 || !makeNonBlockingCloexec(fd))
    {
        auto const err = errno;
        ::close(fd);
        ::unlink(pathString.c_str());
        return std::unexpected(makeNetError(NetErrorCode::Other, err, "listen"));
    }

    return std::unique_ptr<UnixListener>(new UnixListener(loop, fd, path));
}

coro::Task<AcceptResult> UnixListener::accept()
{
    while (true)
    {
        if (_closed || _fd < 0)
            co_return std::unexpected(makeNetError(NetErrorCode::Cancelled, 0, "accept on closed listener"));

    #ifdef __linux__
        auto const conn = ::accept4(_fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
    #else
        auto const conn = ::accept(_fd, nullptr, nullptr);
    #endif
        if (conn >= 0)
        {
    #ifndef __linux__
            // Portable fallback: set non-blocking + cloexec explicitly.
            if (auto const flags = ::fcntl(conn, F_GETFL, 0); flags >= 0)
                ::fcntl(conn, F_SETFL, flags | O_NONBLOCK);
            if (auto const fdFlags = ::fcntl(conn, F_GETFD, 0); fdFlags >= 0)
                ::fcntl(conn, F_SETFD, fdFlags | FD_CLOEXEC);
    #endif
            co_return std::unique_ptr<ISocket>(new PosixSocket(_loop, conn));
        }

        auto const err = errno;
        if (err == EAGAIN || err == EWOULDBLOCK)
        {
            // Park until the listener fd is readable (a connection is pending). A
            // cancelled wait (listener closed / stop requested) throws
            // OperationCancelled, which the accept loop turns into Cancelled.
            try
            {
                co_await _loop.waitReadable(_fd);
            }
            catch (coro::OperationCancelled const&)
            {
                co_return std::unexpected(makeNetError(NetErrorCode::Cancelled, 0, "accept cancelled"));
            }
            continue;
        }
        if (err == EINTR || err == ECONNABORTED)
            continue;
        co_return std::unexpected(makeNetError(NetErrorCode::Other, err, "accept"));
    }
}

} // namespace net

#endif // !_WIN32
