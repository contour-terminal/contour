// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// A listener whose socket file is removed as it closes, or left to the PTY keeper, as decided when it is
/// made.

#include <core/async/Task.hpp>
#include <core/net/IListener.hpp>
#include <core/net/NetError.hpp>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>

namespace core::net
{
class EventLoop;
}

namespace vthost
{

/// Who removes a listener's socket file once it no longer serves.
enum class SocketFileRemoval : uint8_t
{
    /// The PTY keeper, which removes it at every ending of its daemon while it still holds the label lock: a
    /// keeper that starts on the label after it cannot have bound the path yet. The daemon never does.
    LeftToKeeper = 0,
    /// The listener, as it closes: the socket file goes with the daemon that served it.
    OnClose,
};

/// Whether a daemon runs behind a PTY keeper.
enum class KeeperPresence : uint8_t
{
    None = 0, ///< It runs on its own.
    Behind,   ///< A PTY keeper started it, and holds its listeners and programs.
};

/// @param presence Whether the daemon runs behind a PTY keeper.
/// @return Who removes the socket files of the listeners the daemon took over: its keeper, which does at
///         every ending under the label lock; for a daemon behind none, each listener as it closes.
[[nodiscard]] constexpr SocketFileRemoval socketFileRemovalFor(KeeperPresence presence) noexcept
{
    return presence == KeeperPresence::Behind ? SocketFileRemoval::LeftToKeeper : SocketFileRemoval::OnClose;
}

/// The identity of a file, as the file system names it: what tells the file a listener was bound to from one
/// another process bound at the same path since.
struct SocketFileIdentity
{
    std::uint64_t device = 0; ///< The device the file lies on (`st_dev`).
    std::uint64_t inode = 0;  ///< The file's number on that device (`st_ino`).
    bool operator==(SocketFileIdentity const&) const = default;
};

/// @param path A file.
/// @return Its identity; nullopt when it does not exist, or where file identities are not known (Windows).
[[nodiscard]] std::optional<SocketFileIdentity> socketFileIdentityOf(std::filesystem::path const& path);

/// Serves an inherited AF_UNIX listener -- adopted from a live upgrade's handover image, or from the PTY
/// keeper after a crash -- and settles its socket file as @ref SocketFileRemoval says. With OnClose the file
/// is removed as the listener closes or is destroyed, as a listener this process bound is; and only while the
/// path still names the file it listened on when it was adopted, so a socket another daemon bound there since
/// survives.
class SocketFileListener final: public core::net::IListener
{
  public:
    /// @param inner The adopted listener, which accepts.
    /// @param path The socket file @p inner listens on; its identity is recorded now.
    /// @param removal Who removes the socket file.
    SocketFileListener(std::unique_ptr<core::net::IListener> inner,
                       std::filesystem::path path,
                       SocketFileRemoval removal);
    SocketFileListener(SocketFileListener const&) = delete;
    SocketFileListener& operator=(SocketFileListener const&) = delete;
    SocketFileListener(SocketFileListener&&) = delete;
    SocketFileListener& operator=(SocketFileListener&&) = delete;
    /// Closes the listener, settling the socket file as close() does.
    ~SocketFileListener() override;

    [[nodiscard]] core::async::Task<core::net::AcceptResult> accept() override { return _inner->accept(); }
    [[nodiscard]] std::uint16_t boundPort() const noexcept override { return _inner->boundPort(); }
    /// @return The inner listener's handle: what a live upgrade hands over.
    [[nodiscard]] core::platform::NativeHandle nativeHandle() const noexcept override
    {
        return _inner->nativeHandle();
    }

  protected:
    /// Removes the socket file first, while the path is still this listener's to remove (OnClose only), then
    /// closes the listener.
    void doClose() noexcept override;

  private:
    /// @return Whether @ref _path still names the file it named when this was made (always, on Windows).
    [[nodiscard]] bool isStillOurs() const;

    std::unique_ptr<core::net::IListener> _inner;
    std::filesystem::path _path;
    SocketFileRemoval _removal;
    std::optional<SocketFileIdentity> _identity; ///< The file @ref _path named when this was made.
};

/// Binds an AF_UNIX listener at @p path, as core::net::listenUnix() does -- a live server there refuses it,
/// a stale socket file is replaced -- but one that leaves its socket file to the PTY keeper (LeftToKeeper):
/// a plain listener removes the file at its path as it closes, which, once its daemon stopped and another
/// bound the path, is the other daemon's.
///
/// It is bound at a name of its own beside @p path, which only this process uses, and renamed onto @p path:
/// the rename replaces a stale file at once, and what the inner listener removes as it closes is that name,
/// gone since. POSIX; where the name beside @p path would not fit a socket address, it is bound at @p path
/// as core::net::listenUnix() binds it, and removes its file as it closes.
/// @param loop The loop whose reactor drives accept readiness (not owned).
/// @param path The socket file.
/// @return The listener; NetErrorCode::AddressInUse when a live server answers at @p path, or why it could
///         not be bound.
[[nodiscard]] std::expected<std::unique_ptr<core::net::IListener>, core::net::NetError> listenUnixLeavingFile(
    core::net::EventLoop& loop, std::filesystem::path const& path);

} // namespace vthost
