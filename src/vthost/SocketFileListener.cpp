// SPDX-License-Identifier: Apache-2.0
#include <vthost/SocketFileListener.hpp>

#include <core/Utf8Path.hpp>
#include <core/net/Sockets.hpp>

#include <cerrno>
#include <cstring>
#include <format>
#include <string>
#include <system_error>
#include <tuple>
#include <utility>

#ifndef _WIN32
    #include <core/net/posix/FdUtils.hpp>

    #include <sys/socket.h>
    #include <sys/stat.h>
    #include <sys/un.h>

    #include <unistd.h>
#endif

namespace vthost
{

namespace
{
#ifndef _WIN32
    /// @param path A socket file path.
    /// @return It as an AF_UNIX address; nullopt where it does not fit one.
    [[nodiscard]] std::optional<sockaddr_un> unixAddressOf(std::string const& path)
    {
        auto address = sockaddr_un {};
        if (path.size() >= sizeof(address.sun_path))
            return std::nullopt;
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
        return address;
    }

    /// Whether a live server answers at @p address, as core::net::listenUnix() asks before it binds (tmux
    /// does the same): a connect, non-blocking, so that a server with a full backlog answers too.
    /// @return Nothing where none does: the path is absent, or a crashed server's stale file. AddressInUse
    ///         where one answers; SystemError where the probe failed for a reason that must not replace it.
    [[nodiscard]] std::expected<void, core::net::NetError> probeSocketOwner(sockaddr_un const& address,
                                                                            std::string const& path)
    {
        auto const fd = core::net::makeStreamSocket(AF_UNIX, 0);
        if (fd < 0)
            return std::unexpected(
                core::net::makeNetError(core::net::NetErrorCode::SystemError, errno, "probe"));
        auto const connected = ::connect(fd, reinterpret_cast<sockaddr const*>(&address), sizeof(address));
        auto const error = errno;
        ::close(fd);
        if (connected == 0 || error == EINPROGRESS || error == EAGAIN || error == EWOULDBLOCK)
            return std::unexpected(core::net::makeNetError(
                core::net::NetErrorCode::AddressInUse, EADDRINUSE, "a daemon is already serving " + path));
        if (error == ECONNREFUSED || error == ENOENT)
            return {};
        return std::unexpected(
            core::net::makeNetError(core::net::NetErrorCode::SystemError, error, "probe connect " + path));
    }
#endif
} // namespace

std::optional<SocketFileIdentity> socketFileIdentityOf(std::filesystem::path const& path)
{
#ifndef _WIN32
    struct stat status {};
    if (::stat(path.c_str(), &status) != 0)
        return std::nullopt;
    return SocketFileIdentity { .device = static_cast<std::uint64_t>(status.st_dev),
                                .inode = static_cast<std::uint64_t>(status.st_ino) };
#else
    std::ignore = path;
    return std::nullopt;
#endif
}

SocketFileListener::SocketFileListener(std::unique_ptr<core::net::IListener> inner,
                                       std::filesystem::path path,
                                       SocketFileRemoval removal):
    _inner { std::move(inner) },
    _path { std::move(path) },
    _removal { removal },
    _identity { socketFileIdentityOf(_path) }
{
}

bool SocketFileListener::isStillOurs() const
{
#ifndef _WIN32
    return _identity && socketFileIdentityOf(_path) == _identity;
#else
    return true; // no file identity to tell another's file by: removed as before
#endif
}

SocketFileListener::~SocketFileListener()
{
    close();
}

void SocketFileListener::doClose() noexcept
{
    // Removed before the listener closes: until then no connect is refused, so no other daemon takes the path
    // for a stale one and binds it in between. A file that is not the one it listened on is another's.
    if (_removal == SocketFileRemoval::OnClose && isStillOurs())
    {
        auto ignored = std::error_code {};
        std::filesystem::remove(_path, ignored);
    }
    _inner->close();
}

std::expected<std::unique_ptr<core::net::IListener>, core::net::NetError> listenUnixLeavingFile(
    core::net::EventLoop& loop, std::filesystem::path const& path)
{
#ifndef _WIN32
    auto const pathText = core::pathToUtf8(path);
    // Named after this process: a daemon on another label binds beside it at the same time, and the one
    // that stopped on this label removes its own such name as it closes, never this one.
    auto const aside = path.parent_path() / std::format(".bind-{}", ::getpid());
    auto const address = unixAddressOf(pathText);
    if (!address || !unixAddressOf(core::pathToUtf8(aside)))
        return core::net::listenUnix(loop, pathText);
    if (auto const probed = probeSocketOwner(*address, pathText); !probed)
        return std::unexpected(probed.error());
    return core::net::listenUnix(loop, core::pathToUtf8(aside))
        .and_then([&](std::unique_ptr<core::net::IListener> bound)
                      -> std::expected<std::unique_ptr<core::net::IListener>, core::net::NetError> {
            auto renamed = std::error_code {};
            std::filesystem::rename(aside, path, renamed);
            if (renamed)
                return std::unexpected(core::net::makeNetError(
                    core::net::NetErrorCode::SystemError, renamed.value(), "rename onto " + pathText));
            return std::make_unique<SocketFileListener>(
                std::move(bound), path, SocketFileRemoval::LeftToKeeper);
        });
#else
    return core::net::listenUnix(loop, core::pathToUtf8(path));
#endif
}

} // namespace vthost
