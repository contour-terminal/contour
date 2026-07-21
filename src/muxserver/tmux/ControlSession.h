// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `ControlSession` — one tmux-control-mode client connection.
///
/// Speaks the line protocol pinned to tmux 3.7b semantics: guarded command
/// responses (`%begin/%end/%error <time> <number> <flags>`, flags bit 0 =
/// client-originated), asynchronous notifications, and byte-exact %output via
/// the ControlOutput ordering queue. The id mapping is fixed: the host's single
/// window is session `$0`, a vtmux Tab is a window `@N` (its TabId), a vtmux
/// leaf Pane is `%N` (its PaneId).
///
/// Commands are a data-driven table (the Actions.h catalog idiom): one row per
/// verb naming its handler; adding a verb is adding a row.

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <coro/Task.hpp>
#include <coro/WhenAll.hpp>
#include <muxserver/SessionHost.h>
#include <muxserver/tmux/ControlOutput.h>
#include <net/AsyncBufferedReader.h>
#include <net/EventLoop.h>
#include <net/ISocket.h>
#include <net/WriteQueue.h>
#include <vtmux/ModelEvents.h>

namespace muxserver::tmux
{

/// Splits a command line into arguments, honouring single and double quotes
/// (double quotes keep backslash escapes for `\"` and `\\`).
/// @param line The raw command line.
/// @return The argument vector (possibly empty).
[[nodiscard]] std::vector<std::string> splitCommandLine(std::string_view line);

/// One connected control-mode client.
class ControlSession final: public vtmux::ModelEvents, public SessionStreamEvents
{
  public:
    /// @param loop The event loop everything runs on.
    /// @param host The session host commands act upon (not owned; outlives this).
    /// @param connection The client transport (owned).
    /// @param wallClock Seconds-since-epoch source for guard timestamps
    ///        (injected so tests are deterministic).
    ControlSession(net::EventLoop& loop,
                   SessionHost& host,
                   std::unique_ptr<net::ISocket> connection,
                   std::function<std::int64_t()> wallClock);
    ~ControlSession() override;

    ControlSession(ControlSession const&) = delete;
    ControlSession& operator=(ControlSession const&) = delete;
    ControlSession(ControlSession&&) = delete;
    ControlSession& operator=(ControlSession&&) = delete;

    /// The connection flow: emits the initial guard pair and session state,
    /// then serves commands until the peer disconnects or sends an empty line.
    [[nodiscard]] coro::Task<void> run();

    // vtmux::ModelEvents — model changes become control-mode notifications.
    void tabAdded(vtmux::WindowId window, vtmux::TabId tab, int index) override;
    void tabClosed(vtmux::WindowId window, vtmux::TabId tab, int index) override;
    void tabMoved(vtmux::WindowId window, vtmux::TabId tab, int fromIndex, int toIndex) override;
    void activeTabChanged(vtmux::WindowId window, vtmux::TabId tab, int index) override;
    void paneSplit(vtmux::TabId tab, vtmux::PaneId splitNode, vtmux::PaneId newLeaf) override;
    void paneClosed(vtmux::TabId tab, vtmux::PaneId closed, vtmux::PaneId survivor) override;
    void activePaneChanged(vtmux::TabId tab, vtmux::PaneId leaf) override;
    void paneRatioChanged(vtmux::TabId tab, vtmux::PaneId splitNode, double ratio) override;
    void tabTitleChanged(vtmux::TabId tab) override;
    void tabColorChanged(vtmux::TabId tab) override;
    void paneTreeRestructured(vtmux::TabId tab) override;
    void paneZoomChanged(vtmux::TabId tab, std::optional<vtmux::PaneId> zoomedLeaf) override;

    /// Feeds one session's raw PTY bytes into the %output queue (the
    /// connection subscribes itself to the host's stream fan-out).
    void sessionOutput(vtmux::SessionId session, std::string const& bytes) override;

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
    void emitGuarded(HandlerResult const& result);

    /// Emits the current layout of @p tab as a %layout-change notification.
    void notifyLayoutChanged(vtmux::TabId tab);

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
    [[nodiscard]] std::expected<vtmux::Tab*, std::string> resolveTab(
        std::vector<std::string> const& arguments) const;
    [[nodiscard]] std::expected<vtmux::Pane*, std::string> resolvePane(
        std::vector<std::string> const& arguments) const;

    /// @return The page size used for layout projection (the host's settings).
    [[nodiscard]] vtpty::PageSize pageSize() const noexcept;

    /// Applies one `refresh-client -f` flag ("pause-after=5", "!no-output", …).
    void applyClientFlag(std::string_view flag);

    net::EventLoop& _loop;
    SessionHost& _host;
    std::unique_ptr<net::ISocket> _connection;
    std::function<std::int64_t()> _wallClock;
    net::WriteQueue _writer;
    ControlOutput _output;
    std::uint32_t _commandNumber = 0;
    bool _noOutput = false; ///< refresh-client -f no-output: suppress %output entirely.
};

/// The daemon's connection-handler factory for control-mode clients.
/// @param loop The event loop.
/// @param host The session host (not owned; must outlive the daemon's serving).
/// @return A handler suitable for MuxServer's constructor.
[[nodiscard]] std::function<coro::Task<void>(std::unique_ptr<net::ISocket>)> makeControlModeHandler(
    net::EventLoop& loop, SessionHost& host);

} // namespace muxserver::tmux
