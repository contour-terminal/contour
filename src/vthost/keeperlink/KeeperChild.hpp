// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtpty/ChildProcess.hpp>
#include <vtpty/OwnedFd.hpp>
#include <vtpty/Process.hpp>
#include <vtpty/ProcessPlacement.hpp>
#include <vtpty/Pty.hpp>
#include <vtpty/SandboxInfo.hpp>

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include <vthost/keeper/Messages.hpp>

namespace vthost::keeperlink
{

class KeeperLink;

/// How a child the keeper holds ended, as the keeper told it: written on the loop as its Exited arrives,
/// read from any thread.
class ChildExit
{
  public:
    /// Records @p status; the first record stands.
    void record(vtpty::ExitStatus status);

    /// @return How the child ended; nullopt while the keeper has not said.
    [[nodiscard]] std::optional<vtpty::ExitStatus> status() const;

  private:
    mutable std::mutex _lock;
    std::optional<vtpty::ExitStatus> _status;
};

/// Hands each Exited the keeper sends to the ChildExit of its session. Loop thread, but for watch()'s
/// returned slots, which any thread reads.
class KeeperExits
{
  public:
    /// @param session A session the keeper spawned.
    /// @return Where its exit is recorded; held by whoever asks after it, forgotten here once none does.
    [[nodiscard]] std::shared_ptr<ChildExit> watch(keeper::KeeperSessionId session);

    /// Records an Exited.
    /// @param session The session. @param status How its child ended.
    void exited(keeper::KeeperSessionId session, vtpty::ExitStatus status);

  private:
    std::mutex _lock;
    std::map<keeper::KeeperSessionId, std::weak_ptr<ChildExit>> _watched;
};

/// What a KeeperChild is configured with; fixed for its life.
struct KeeperChildConfig
{
    /// The program, its arguments, directory and environment.
    vtpty::Process::ExecInfo exec;
    /// Whether the program escapes the sandbox this process runs in.
    vtpty::SandboxEscape sandbox = vtpty::SandboxEscape::Contained;
    /// Places the program before it runs, and releases it; never null.
    std::shared_ptr<vtpty::ProcessPlacement> placement;
    /// How long the keeper gives the program to end on the hangup of close() before it kills it.
    std::chrono::milliseconds hangupGrace = vtpty::HangupGrace::Default;
};

/// A shell whose parent is the PTY keeper, not the daemon: the daemon prepares the spawn, the keeper forks
/// and execs the trampoline, and from then on holds the child and a duplicate of its PTY master, so that the
/// shell outlives a crash of the daemon.
///
/// Its status comes from the keeper's Exited, its spawn progress from the settle signal, its foreground
/// process from the terminal (tcgetpgrp(), which needs no parenthood), and every signal goes through the
/// keeper, which alone may signal a child it may have reaped. close() hangs it up: the daemon closes its
/// master and releases the session, after which the keeper closes its own and kills a child that is still
/// there after the grace. Nothing here ever waits for the keeper or the child.
class KeeperChild final: public vtpty::Pty, public vtpty::ChildProcess
{
  public:
    /// @param link The keeper's link (not owned; outlives this).
    /// @param exits Where the keeper's Exited for this child lands.
    /// @param config What to run, and how.
    /// @param pty The terminal to run it on, not yet started.
    KeeperChild(KeeperLink& link,
                std::shared_ptr<KeeperExits> exits,
                KeeperChildConfig config,
                std::unique_ptr<vtpty::Pty> pty);
    KeeperChild(KeeperChild const&) = delete;
    KeeperChild& operator=(KeeperChild const&) = delete;
    KeeperChild(KeeperChild&&) = delete;
    KeeperChild& operator=(KeeperChild&&) = delete;
    /// Closes, as close() does; never blocks.
    ~KeeperChild() override;

    /// @return The keeper's name for the child; nullopt before start() spawned it.
    [[nodiscard]] std::optional<keeper::KeeperSessionId> session() const noexcept;

    /// @return The child's process id, as the keeper reported it; 0 before start() spawned it.
    [[nodiscard]] int64_t pid() const noexcept { return _pid; }

    // ChildProcess
    [[nodiscard]] vtpty::SpawnProgress spawnProgress() const override;
    [[nodiscard]] std::optional<vtpty::ExitStatus> checkStatus() const override;
    [[nodiscard]] std::string workingDirectory() const override;
    [[nodiscard]] std::optional<std::string> foregroundProcessName() const override;
    /// Normal and Kill are sent through the keeper; Hangup is close().
    void terminate(vtpty::TerminationHint hint) override;
    /// @return CrossingKind::Keeper with the keeper's name for the child.
    [[nodiscard]] vtpty::ChildCrossing crossing() const override;

    // Pty
    /// Starts the terminal and spawns the program through the keeper: the one channel round trip a
    /// session's creation adds. Loop thread.
    [[nodiscard]] vtpty::StartResult start() override;
    /// Closes the daemon's master and releases the session to the keeper, once.
    void close() override;
    /// Returns at once: the child's end is the keeper's to wait for.
    void waitForClosed() override {}
    // clang-format off
    [[nodiscard]] vtpty::PtySlave& slave() noexcept override { return _pty->slave(); }
    [[nodiscard]] bool isClosed() const noexcept override { return _pty->isClosed(); }
    [[nodiscard]] std::optional<ReadResult> read(crispy::BufferObject<char>& storage, std::optional<std::chrono::milliseconds> timeout, size_t n) override { return _pty->read(storage, timeout, n); }
    void wakeupReader() override { _pty->wakeupReader(); }
    [[nodiscard]] int write(std::string_view data) override { return _pty->write(data); }
    [[nodiscard]] vtpty::PasteLineEndings pasteLineEndings() const noexcept override { return _pty->pasteLineEndings(); }
    void interruptWrites() override { _pty->interruptWrites(); }
    void allowWrites() override { _pty->allowWrites(); }
    [[nodiscard]] std::expected<vtpty::InheritedPty, vtpty::AdoptError> handover() const override { return _pty->handover(); }
    [[nodiscard]] std::expected<vtpty::InheritedPty, vtpty::AdoptError> releaseHandover() override { return _pty->releaseHandover(); }
    [[nodiscard]] std::optional<SpawnDescriptors> spawnDescriptors() const noexcept override { return _pty->spawnDescriptors(); }
    void closeSpawnDescriptors() override { _pty->closeSpawnDescriptors(); }
    [[nodiscard]] vtpty::PageSize pageSize() const noexcept override { return _pty->pageSize(); }
    void resizeScreen(vtpty::PageSize cells, std::optional<vtpty::ImageSize> pixels = std::nullopt) override { _pty->resizeScreen(cells, pixels); }
    [[nodiscard]] vtpty::InputEcho inputEcho() const noexcept override { return _pty->inputEcho(); }
    // clang-format on

  private:
    /// Spawns the program on the started terminal through the keeper.
    [[nodiscard]] vtpty::StartResult spawn();

    KeeperLink& _link;
    std::shared_ptr<KeeperExits> _exits;
    KeeperChildConfig _config;
    std::unique_ptr<vtpty::Pty> _pty;

    // Set once by start(), before any other thread reads them.
    uint64_t _session = 0; ///< The keeper's name for the child; 0 for none spawned.
    int64_t _pid = 0;
    int _master = -1; ///< The terminal's master, for tcgetpgrp(); this object's PTY owns it.
    std::shared_ptr<ChildExit> _exit;

    mutable std::mutex _settleLock;
    mutable vtpty::OwnedFd _settle; ///< Guarded by _settleLock; open until observed settled.

    std::once_flag _released; ///< Release is sent once.
};

} // namespace vthost::keeperlink
