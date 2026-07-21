// SPDX-License-Identifier: Apache-2.0
#pragma once

#ifndef _WIN32

    #include <expected>
    #include <filesystem>
    #include <memory>

    #include <coro/Task.hpp>
    #include <net/EventLoop.h>
    #include <net/IListener.h>

namespace net
{

/// Verifies (creating it if absent, mode 0700) that @p directory is safe to hold
/// a control socket, mirroring tmux's socket-directory checks: it must be a
/// directory owned by the calling user with no *world* access bits set (group
/// access is permitted, exactly like tmux's TMUX_SOCK_PERM mask — mirror the
/// reference behaviour rather than inventing a stricter one).
/// @param directory The directory to create/verify.
/// @return Nothing on success, or a @c NetError describing the refusal.
[[nodiscard]] std::expected<void, NetError> ensureOwnedPrivateDirectory(
    std::filesystem::path const& directory);

/// A reactor-driven, non-blocking AF_UNIX stream listener. accept() parks on the
/// loop's waitReadable until a connection is pending, then accepts it as a
/// non-blocking PosixSocket.
class UnixListener final: public IListener
{
  public:
    ~UnixListener() override;

    UnixListener(UnixListener const&) = delete;
    UnixListener& operator=(UnixListener const&) = delete;
    UnixListener(UnixListener&&) = delete;
    UnixListener& operator=(UnixListener&&) = delete;

    /// Binds and listens on the socket file @p path.
    ///
    /// The parent directory is created/verified via ensureOwnedPrivateDirectory
    /// first. The path is then probed the way tmux does — a client connect() — so
    /// a live daemon is never hijacked: if a server answers, the bind fails with
    /// @c NetErrorCode::AddressInUse and the existing socket is left intact; only a
    /// stale socket (a crashed server's leftover) or an absent path is reclaimed
    /// and rebound. The socket file itself is chmod'd 0600.
    /// @param loop The loop whose reactor drives accept readiness (not owned).
    /// @param path The socket file path (its parent is the hardened directory).
    /// @param backlog The listen backlog.
    /// @return The bound listener, or a @c NetError on failure
    ///         (@c NetErrorCode::AddressInUse when a live daemon already owns @p path).
    [[nodiscard]] static std::expected<std::unique_ptr<UnixListener>, NetError> bind(
        EventLoop& loop, std::filesystem::path const& path, int backlog = 128);

    [[nodiscard]] coro::Task<AcceptResult> accept() override;

    /// AF_UNIX endpoints have no port; always 0.
    [[nodiscard]] std::uint16_t localPort() const noexcept override { return 0; }

    /// @return The socket file path this listener is bound to.
    [[nodiscard]] std::filesystem::path const& path() const noexcept { return _path; }

    /// Stops accepting and unlinks the socket file.
    void close() noexcept override;

  private:
    UnixListener(EventLoop& loop, int fd, std::filesystem::path path) noexcept;

    EventLoop& _loop;
    int _fd;
    std::filesystem::path _path;
    bool _closed = false;
};

} // namespace net

#endif // !_WIN32
