// SPDX-License-Identifier: Apache-2.0
#include <core/net/windows/UnixSocketPath.hpp>

#include <core/net/windows/InvalidSocket.hpp>

#include <cstring>
#include <string>

#include <windows.h>

// A bound AF_UNIX socket is a file-system reparse point carrying this tag. The
// constant lives in the WDK's ntifs.h; user-mode SDK headers omit it, so define
// it exactly as libuv and Go's runtime do for their AF_UNIX support.
#ifndef IO_REPARSE_TAG_AF_UNIX
    #define IO_REPARSE_TAG_AF_UNIX 0x80000023
#endif

namespace core::net::detail
{

namespace
{
    /// Probes whether the file at @p path may be reclaimed for binding — the
    /// Windows cousin of UnixListener's owner probe, differing deliberately in
    /// two ways: Windows gets an EXPLICIT file-type guard (only a reparse point
    /// tagged AF_UNIX is a socket file; the POSIX probe needs none because
    /// connect() to a regular file fails, which POSIX then reads as "stale"),
    /// and this connect blocks while the POSIX probe is non-blocking. A live
    /// daemon keeps its path (AddressInUse), and a non-socket file at the path
    /// is never touched — bindUnix must not delete user data that happens to
    /// sit at the chosen socket path.
    /// @param address The AF_UNIX address already populated for @p path.
    /// @param path The socket file path, for diagnostics.
    /// @return Nothing when the path is absent or a stale socket; a NetError otherwise.
    [[nodiscard]] std::expected<void, NetError> probeUnixSocketOwner(sockaddr_un const& address,
                                                                     std::string const& path)
    {
        // FindFirstFileA fails on an absent path and reports the reparse tag on a
        // present one — the single syscall answering both questions.
        auto findData = WIN32_FIND_DATAA {};
        auto const find = ::FindFirstFileA(path.c_str(), &findData);
        if (find == INVALID_HANDLE_VALUE)
        {
            auto const statError = ::GetLastError();
            if (statError == ERROR_FILE_NOT_FOUND || statError == ERROR_PATH_NOT_FOUND)
                return {}; // nothing at the path: safe to bind
            return std::unexpected(
                makeNetError(NetErrorCode::SystemError, static_cast<int>(statError), "probe stat " + path));
        }
        ::FindClose(find);

        // Only a reparse point tagged AF_UNIX is a socket file; anything else is
        // user data that must never be deleted.
        auto const isSocketFile = (findData.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0
                                  && findData.dwReserved0 == IO_REPARSE_TAG_AF_UNIX;
        if (!isSocketFile)
            return std::unexpected(makeNetError(
                NetErrorCode::AddressInUse, ERROR_FILE_EXISTS, path + " exists and is not a socket file"));

        // A socket file is there: reclaim it only when no live server answers.
        auto const probe = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (probe == InvalidSocket)
            return std::unexpected(
                makeNetError(NetErrorCode::SystemError, WSAGetLastError(), "probe socket"));
        auto const rc = ::connect(probe, reinterpret_cast<sockaddr const*>(&address), sizeof(address));
        auto const err = WSAGetLastError();
        ::closesocket(probe);
        if (rc == 0)
            return std::unexpected(makeNetError(
                NetErrorCode::AddressInUse, WSAEADDRINUSE, "a daemon is already serving " + path));
        if (err == WSAECONNREFUSED)
            return {}; // a crashed server's stale socket
        return std::unexpected(makeNetError(NetErrorCode::SystemError, err, "probe connect " + path));
    }
} // namespace

std::expected<sockaddr_un, NetError> claimUnixSocketPath(std::string_view path)
{
    auto address = sockaddr_un {};
    address.sun_family = AF_UNIX;
    if (path.size() >= sizeof(address.sun_path))
        return std::unexpected(makeNetError(NetErrorCode::AddressError, 0, "unix socket path too long"));
    std::memcpy(address.sun_path, path.data(), path.size());

    auto const pathString = std::string { path };
    if (auto const probe = probeUnixSocketOwner(address, pathString); !probe)
        return std::unexpected(probe.error());
    ::DeleteFileA(pathString.c_str()); // the probe proved this absent or a stale socket
    return address;
}

} // namespace core::net::detail
