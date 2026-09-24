// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/async/Task.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/IListener.hpp>

#include <expected>
#include <filesystem>
#include <memory>

namespace core::net
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
    /// first, unless @p path names no directory at all (a bare filename, i.e. the
    /// current working directory — which is the caller's own and not ours to
    /// create or re-permission). The path is then probed the way tmux does — a client connect() — so
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

    [[nodiscard]] async::Task<AcceptResult> accept() override;

    /// AF_UNIX endpoints have no port; always 0.
    [[nodiscard]] std::uint16_t boundPort() const noexcept override { return 0; }

    /// @return The socket file path this listener is bound to.
    [[nodiscard]] std::filesystem::path const& path() const noexcept { return _path; }

    /// Stops accepting and unlinks the socket file.
    void close() noexcept override;

  private:
    UnixListener(EventLoop& loop, int fd, std::filesystem::path path) noexcept;

    /// Closes the listening fd and unlinks the socket file, telling the loop first
    /// so a parked accept is resumed rather than left waiting on a descriptor the
    /// poller can no longer report.
    /// @param policy How a parked accept observes the close. @c close() passes
    ///        @c Resume — this listener is alive, so acceptOne may re-read the @c _fd /
    ///        @c _closed it holds pointers to, once its lifetime token says the listener
    ///        still exists (an owner may destroy it before the loop resumes the accept).
    ///        The destructor passes @c Cancel, since those pointers are about to dangle.
    void close(FdWakePolicy policy) noexcept;

    EventLoop& _loop;
    int _fd;
    std::filesystem::path _path;
    bool _closed = false;
    /// Expires with this listener. A parked accept is resumed by the loop a turn after `close()`,
    /// and an owner may destroy the listener in between (`listener->close(); listener.reset();`),
    /// so the accept asks this -- never the listener -- whether there is still one to read.
    std::shared_ptr<void const> _lifetime = std::make_shared<char const>('\0');
};

} // namespace core::net
