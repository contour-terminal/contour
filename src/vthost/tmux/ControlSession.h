// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `ControlSession` — one tmux-control-mode client connection.
///
/// Speaks the line protocol pinned to tmux 3.7b semantics: guarded command
/// responses (`%begin/%end/%error <time> <number> <flags>`, flags bit 0 =
/// client-originated), asynchronous notifications, and byte-exact %output via
/// the ControlOutput ordering queue. The id mapping is fixed: the host's single
/// window is session `$0`, a vtworkspace Tab is a window `@N` (its TabId), a vtworkspace
/// leaf Pane is `%N` (its PaneId).
///
/// Commands are a data-driven table (the Actions.h catalog idiom): one row per
/// verb naming its handler; adding a verb is adding a row.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <coro/Task.hpp>
#include <net/AsyncBufferedReader.h>
#include <net/EventLoop.h>
#include <net/ISocket.h>
#include <net/WriteQueue.h>
#include <vthost/SessionHost.h>
#include <vthost/tmux/ControlOutput.h>
#include <vtworkspace/ModelEvents.h>

namespace vthost::tmux
{

/// Splits a command line into arguments, honouring single and double quotes
/// (double quotes keep backslash escapes for `\"` and `\\`).
/// @param line The raw command line.
/// @return The argument vector (possibly empty).
[[nodiscard]] std::vector<std::string> splitCommandLine(std::string_view line);

/// Converts a client-supplied `pause-after` value in SECONDS to a millisecond
/// threshold, clamped so a value large enough to overflow the signed rep cannot
/// wrap to a NEGATIVE ("already past due") duration that would pause all output —
/// the exact opposite of the long grace period the client asked for.
/// @param seconds The client-supplied whole-second value (unbounded).
/// @return The clamped millisecond duration (always >= 0).
[[nodiscard]] std::chrono::milliseconds pauseAfterFromSeconds(std::uint64_t seconds) noexcept;

/// Per-transport dialect knobs for a ControlSession. The raw line-protocol
/// socket keeps the defaults; the imsg path (a real tmux binary relaying to
/// its user) deviates exactly where the real server does.
struct ControlSessionOptions
{
    /// Whether run() emits its own `%exit` line — the tmux client binary
    /// prints `%exit` itself, so the imsg path suppresses ours.
    bool emitExitLine = true;

    /// The flags field of the PREAMBLE guard pair: 1 for a line-protocol
    /// peer (client-originated), 0 for the MSG_COMMAND-originated attach
    /// (cmd-queue.c stamps only stdin-line commands as client-originated).
    int initialGuardFlag = 1;

    /// Byte bound of the per-connection write queue. Enqueue is refused once the
    /// queued-but-unsent total would exceed this; the session treats that refusal
    /// as a lost peer (a client too slow to keep up is disconnected, never
    /// buffered without limit). Tests shrink it to force the overflow path.
    std::size_t writeQueueMaxBytes = std::size_t { 256 } * 1024;
};

/// One connected control-mode client.
class ControlSession final: public vtworkspace::ModelEvents, public SessionStreamEvents
{
  public:
    using Options = ControlSessionOptions;

    /// @param loop The event loop everything runs on.
    /// @param host The session host commands act upon (not owned; outlives this).
    /// @param connection The client transport (owned).
    /// @param wallClock Seconds-since-epoch source for guard timestamps
    ///        (injected so tests are deterministic).
    /// @param options Transport-dialect knobs (defaults = line protocol).
    ControlSession(net::EventLoop& loop,
                   SessionHost& host,
                   std::unique_ptr<net::ISocket> connection,
                   std::function<std::int64_t()> wallClock,
                   Options options = {});
    ~ControlSession() override;

    ControlSession(ControlSession const&) = delete;
    ControlSession& operator=(ControlSession const&) = delete;
    ControlSession(ControlSession&&) = delete;
    ControlSession& operator=(ControlSession&&) = delete;

    /// The connection flow: emits the initial guard pair and session state,
    /// then serves commands until the peer disconnects or sends an empty line.
    [[nodiscard]] coro::Task<void> run();

    /// @return True once the client was dropped because the write queue
    ///         overflowed (or its transport failed): the session is tearing down.
    ///         Diagnostics/testing. See ControlSessionOptions::writeQueueMaxBytes.
    [[nodiscard]] bool peerLost() const noexcept { return _peerLost; }

    // vtworkspace::ModelEvents — model changes become control-mode notifications.
    void tabAdded(vtworkspace::WindowId window, vtworkspace::TabId tab, int index) override;
    void tabClosed(vtworkspace::WindowId window, vtworkspace::TabId tab, int index) override;
    void tabMoved(vtworkspace::WindowId window, vtworkspace::TabId tab, int fromIndex, int toIndex) override;
    void activeTabChanged(vtworkspace::WindowId window, vtworkspace::TabId tab, int index) override;
    void paneSplit(vtworkspace::TabId tab,
                   vtworkspace::PaneId splitNode,
                   vtworkspace::PaneId newLeaf) override;
    void paneClosed(vtworkspace::TabId tab,
                    vtworkspace::PaneId closed,
                    vtworkspace::PaneId survivor) override;
    void activePaneChanged(vtworkspace::TabId tab, vtworkspace::PaneId leaf) override;
    void paneRatioChanged(vtworkspace::TabId tab, vtworkspace::PaneId splitNode, double ratio) override;
    void tabTitleChanged(vtworkspace::TabId tab) override;
    void tabColorChanged(vtworkspace::TabId tab) override;
    void paneTreeRestructured(vtworkspace::TabId tab) override;
    void paneZoomChanged(vtworkspace::TabId tab, std::optional<vtworkspace::PaneId> zoomedLeaf) override;

    /// Feeds one session's raw PTY bytes into the %output queue (the
    /// connection subscribes itself to the host's stream fan-out).
    void sessionOutput(vtworkspace::SessionId session, std::string const& bytes) override;

  private:
    /// A command handler: returns response body lines, or an error message.
    using HandlerResult = std::expected<std::vector<std::string>, std::string>;
    using Handler = HandlerResult (ControlSession::*)(std::vector<std::string> const& arguments);

    /// One row of the command catalog.
    struct CommandEntry
    {
        std::string_view name;
        Handler handler;
    };

    /// @return The command catalog (one row per verb; sorted by name).
    [[nodiscard]] static std::vector<CommandEntry> const& commandCatalog();

    /// Executes one received command line inside a guard block.
    void dispatch(std::string_view line);

    /// Emits `%begin/%end` (or `%error`) around @p bodyLines.
    void emitGuarded(HandlerResult const& result, int flags = 1);

    /// Emits the current layout of @p tab as a %layout-change notification.
    void notifyLayoutChanged(vtworkspace::TabId tab);

    // Command handlers (the catalog's rows).
    HandlerResult commandListSessions(std::vector<std::string> const& arguments);
    HandlerResult commandListWindows(std::vector<std::string> const& arguments);
    HandlerResult commandListPanes(std::vector<std::string> const& arguments);
    HandlerResult commandNewWindow(std::vector<std::string> const& arguments);
    HandlerResult commandSplitWindow(std::vector<std::string> const& arguments);
    HandlerResult commandKillPane(std::vector<std::string> const& arguments);
    HandlerResult commandSelectPane(std::vector<std::string> const& arguments);
    HandlerResult commandSelectWindow(std::vector<std::string> const& arguments);
    HandlerResult commandSendKeys(std::vector<std::string> const& arguments);
    HandlerResult commandCapturePane(std::vector<std::string> const& arguments);
    HandlerResult commandRenameWindow(std::vector<std::string> const& arguments);
    HandlerResult commandResizePane(std::vector<std::string> const& arguments);
    HandlerResult commandDisplayMessage(std::vector<std::string> const& arguments);
    HandlerResult commandRefreshClient(std::vector<std::string> const& arguments);

    // Target resolution ("-t %N" / "-t @N"; defaults to the active pane/tab).
    [[nodiscard]] std::expected<vtworkspace::Tab*, std::string> resolveTab(
        std::vector<std::string> const& arguments) const;
    [[nodiscard]] std::expected<vtworkspace::Pane*, std::string> resolvePane(
        std::vector<std::string> const& arguments) const;

    /// @return The page size used for layout projection (the host's settings).
    [[nodiscard]] vtpty::PageSize pageSize() const noexcept;

    /// Applies one `refresh-client -f` flag ("pause-after=5", "!no-output", …).
    void applyClientFlag(std::string_view flag);

    /// Runs one fair pass of the %output/notification ordering queue, then
    /// schedules a continuation if backlog remains — so a burst larger than one
    /// pass's byte budget keeps draining even after the PTY falls silent, and the
    /// notifications gated behind it are never stranded.
    void pumpOutput();

    /// Posts a single self-perpetuating drain continuation, unless one is already
    /// in flight or the peer is gone. The continuation captures a shared alive
    /// flag owned by this session, so one that resumes after teardown is a no-op.
    void scheduleOutputDrain();

    /// Drops the control client: closes the write queue and the connection so
    /// run()'s parked reader unwinds (BadHandle) through the normal teardown.
    /// Idempotent. Invoked when a write is refused — WriteQueue's disconnect
    /// contract (WriteQueue.h) — rather than silently dropping data.
    void handlePeerLost();

    net::EventLoop& _loop;
    SessionHost& _host;
    std::unique_ptr<net::ISocket> _connection;
    std::function<std::int64_t()> _wallClock;
    Options _options;
    net::WriteQueue _writer;
    ControlOutput _output;
    std::uint32_t _commandNumber = 0;
    bool _noOutput = false; ///< refresh-client -f no-output: suppress %output entirely.

    /// Kept alive by any posted drain continuation; the destructor clears the
    /// pointee so a continuation resuming after teardown observes a dead session
    /// and does nothing (guards against a use-after-free on `this`).
    std::shared_ptr<bool> _alive = std::make_shared<bool>(true);
    bool _outputDrainScheduled = false; ///< A drain continuation is posted (at most one in flight).
    bool _peerLost = false;             ///< The client was dropped (write refused / transport failed).
};

/// The daemon's connection-handler factory for control-mode clients.
/// @param loop The event loop.
/// @param host The session host (not owned; must outlive the daemon's serving).
/// @return A handler suitable for ConnectionAcceptor's constructor.
[[nodiscard]] std::function<coro::Task<void>(std::unique_ptr<net::ISocket>)> makeControlModeHandler(
    net::EventLoop& loop, SessionHost& host);

} // namespace vthost::tmux
