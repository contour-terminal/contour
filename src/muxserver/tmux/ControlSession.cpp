// SPDX-License-Identifier: Apache-2.0
#include <muxserver/tmux/ControlSession.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <format>
#include <ranges>
#include <utility>

#include <muxserver/tmux/LayoutString.h>
#include <vtmux/Pane.h>
#include <vtmux/Tab.h>

namespace muxserver::tmux
{

using vtmux::Pane;
using vtmux::PaneId;
using vtmux::SessionId;
using vtmux::SplitState;
using vtmux::Tab;
using vtmux::TabId;
using vtmux::WindowId;

std::vector<std::string> splitCommandLine(std::string_view line)
{
    auto arguments = std::vector<std::string> {};
    auto current = std::string {};
    auto inWord = false;
    auto quote = '\0';

    for (std::size_t i = 0; i < line.size(); ++i)
    {
        auto const ch = line[i];
        if (quote != '\0')
        {
            if (ch == quote)
                quote = '\0';
            else if (quote == '"' && ch == '\\' && i + 1 < line.size()
                     && (line[i + 1] == '"' || line[i + 1] == '\\'))
                current += line[++i];
            else
                current += ch;
            continue;
        }
        if (ch == '\'' || ch == '"')
        {
            quote = ch;
            inWord = true;
            continue;
        }
        if (ch == ' ' || ch == '\t')
        {
            if (inWord)
            {
                arguments.push_back(std::move(current));
                current.clear();
                inWord = false;
            }
            continue;
        }
        current += ch;
        inWord = true;
    }
    if (inWord)
        arguments.push_back(std::move(current));
    return arguments;
}

std::chrono::milliseconds pauseAfterFromSeconds(std::uint64_t seconds) noexcept
{
    // Cap at the largest whole-second duration that fits milliseconds' signed rep,
    // THEN multiply — so an overflowing value clamps to a huge positive threshold
    // instead of wrapping negative (which would pause all output immediately).
    constexpr auto MaxSeconds = static_cast<std::uint64_t>(std::chrono::milliseconds::max().count() / 1000);
    return std::chrono::milliseconds { static_cast<std::chrono::milliseconds::rep>(
        std::min(seconds, MaxSeconds) * 1000) };
}

namespace
{
    /// Extracts the value of a `-t <target>` option, if present.
    [[nodiscard]] std::optional<std::string_view> targetOf(std::vector<std::string> const& arguments)
    {
        for (std::size_t i = 1; i + 1 < arguments.size(); ++i)
            if (arguments[i] == "-t")
                return arguments[i + 1];
        return std::nullopt;
    }

    [[nodiscard]] bool hasFlag(std::vector<std::string> const& arguments, std::string_view flag)
    {
        return std::ranges::find(arguments, flag) != arguments.end();
    }

    /// Collects every value of a repeatable value-carrying option (`-A x -A y`).
    [[nodiscard]] std::vector<std::string_view> valuesOf(std::vector<std::string> const& arguments,
                                                         std::string_view flag)
    {
        auto values = std::vector<std::string_view> {};
        for (std::size_t i = 1; i + 1 < arguments.size(); ++i)
            if (arguments[i] == flag)
                values.emplace_back(arguments[++i]);
        return values;
    }

    /// Parses a full decimal number; anything else (partial parse included)
    /// yields nullopt.
    [[nodiscard]] std::optional<std::uint64_t> parseNumber(std::string_view text)
    {
        auto value = std::uint64_t {};
        auto const [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (ec != std::errc {} || ptr != text.data() + text.size() || text.empty())
            return std::nullopt;
        return value;
    }

    [[nodiscard]] std::optional<std::uint64_t> parseIdSuffix(std::string_view target, char sigil)
    {
        if (target.size() < 2 || target[0] != sigil)
            return std::nullopt;
        return parseNumber(target.substr(1));
    }

    /// The tiny send-keys key-name table; everything else is sent literally.
    struct NamedKey
    {
        std::string_view name;
        std::string_view bytes;
    };

    constexpr auto NamedKeys = std::array {
        NamedKey { "Enter", "\r" },    NamedKey { "Escape", "\x1b" },  NamedKey { "Tab", "\t" },
        NamedKey { "Space", " " },     NamedKey { "BSpace", "\x7f" },  NamedKey { "Up", "\x1b[A" },
        NamedKey { "Down", "\x1b[B" }, NamedKey { "Right", "\x1b[C" }, NamedKey { "Left", "\x1b[D" },
    };

    [[nodiscard]] std::string translateKey(std::string_view key)
    {
        for (auto const& entry: NamedKeys)
            if (entry.name == key)
                return { std::string_view { entry.bytes }.begin(), std::string_view { entry.bytes }.end() };
        // C-x control chords.
        if (key.size() == 3 && key.starts_with("C-"))
        {
            auto const base = static_cast<char>(std::tolower(static_cast<unsigned char>(key[2])));
            if (base >= 'a' && base <= 'z')
                return { 1, static_cast<char>(base - 'a' + 1) };
        }
        return { key.begin(), key.end() };
    }
} // namespace

ControlSession::ControlSession(net::EventLoop& loop,
                               SessionHost& host,
                               std::unique_ptr<net::ISocket> connection,
                               std::function<std::int64_t()> wallClock,
                               Options options):
    _loop(loop),
    _host(host),
    _connection(std::move(connection)),
    _wallClock(std::move(wallClock)),
    _options(options),
    _writer(loop, _connection.get(), _options.writeQueueMaxBytes),
    _output([this](std::string line) {
        if (_peerLost)
            return; // the client is already gone; further lines drop silently
        if (!_writer.enqueue(std::move(line)))
            handlePeerLost(); // queue overflow/failure: disconnect, never drop-and-continue
    })
{
    _host.subscribe(this);
}

ControlSession::~ControlSession()
{
    *_alive = false; // any posted drain continuation now resumes into a no-op
    _host.unsubscribe(this);
}

vtpty::PageSize ControlSession::pageSize() const noexcept
{
    // The host owns the authoritative client area (refresh-client -C updates
    // it); every layout projection derives from it.
    return _host.pageSize();
}

coro::Task<void> ControlSession::run()
{
    // The implicit initial command's empty guard pair, then the session state —
    // the preamble iTerm2-style clients gate their notification handling on.
    // The imsg path stamps it flag 0 (the MSG_COMMAND-originated attach).
    emitGuarded(HandlerResult { std::vector<std::string> {} }, _options.initialGuardFlag);
    _output.enqueueNotification("%session-changed $0 0");

    auto reader = net::AsyncBufferedReader { _connection.get() };
    while (true)
    {
        auto line = co_await reader.readLine();
        if (!line.has_value())
            break; // disconnect or poisoned stream
        if (line->empty())
            break; // an empty line detaches (control.c:547)
        dispatch(*line);
    }

    // The tmux client binary prints its own %exit; the imsg path suppresses ours.
    if (_options.emitExitLine)
        _output.enqueueNotification("%exit");
    // Drain the ordering queue first — a burst still being paced out (and the
    // notifications gated behind it) must fully reach the writer; a lost peer
    // skips that wait. Then let the write queue flush %exit and any trailing
    // replies before the connection dies.
    co_await net::pollUntil(&_loop, [this] { return _peerLost || !_output.hasPending(); });
    co_await _writer.flushThenClose();
    _connection->close();
}

void ControlSession::dispatch(std::string_view line)
{
    auto const arguments = splitCommandLine(line);
    if (arguments.empty())
    {
        emitGuarded(std::unexpected("empty command"));
        return;
    }

    auto const& catalog = commandCatalog();
    auto const entry = std::ranges::find_if(
        catalog, [&](CommandEntry const& command) { return command.name == arguments.front(); });
    if (entry == catalog.end())
    {
        emitGuarded(std::unexpected(std::format("unknown command: {}", arguments.front())));
        return;
    }

    emitGuarded((this->*(entry->handler))(arguments));
}

void ControlSession::emitGuarded(HandlerResult const& result, int flags)
{
    auto const time = _wallClock();
    auto const number = _commandNumber++;
    // flags bit 0 (client-originated) is SET on guard blocks answering client
    // commands (cmd-queue.c:591); spontaneous output never uses guards. Only
    // the preamble may carry 0 (see ControlSessionOptions::initialGuardFlag).
    _output.enqueueNotification(std::format("%begin {} {} {}", time, number, flags));
    if (result.has_value())
        for (auto const& line: *result)
            _output.enqueueNotification(line);
    else
        _output.enqueueNotification(result.error());
    _output.enqueueNotification(
        std::format("{} {} {} {}", result.has_value() ? "%end" : "%error", time, number, flags));
}

void ControlSession::sessionOutput(SessionId session, std::string const& bytes)
{
    if (_noOutput || _peerLost)
        return; // no-output: the client wants notifications only; lost peer: nothing to send
    // Map the session to its hosting leaf pane (the %N the client knows).
    auto* window = _host.model().window(_host.windowId());
    for (auto const tabIndex: std::views::iota(0, window->tabCount()))
    {
        auto* tab = window->tabAt(tabIndex);
        auto paneId = std::optional<std::uint64_t> {};
        tab->rootPane()->walkTree([&](Pane& pane) {
            if (pane.isLeaf() && pane.session() == session)
                paneId = pane.id().value;
        });
        if (paneId)
        {
            _output.enqueueOutput(*paneId, bytes, std::chrono::steady_clock::now());
            pumpOutput();
            return;
        }
    }
}

void ControlSession::pumpOutput()
{
    if (_peerLost)
        return;
    _output.pump(_writer.queuedBytes(), std::chrono::steady_clock::now());
    // A burst clipped by this pass's byte budget must keep draining even if the
    // PTY now goes silent (scheduleOutputDrain itself backs out on a lost peer).
    if (_output.hasPending())
        scheduleOutputDrain();
}

void ControlSession::scheduleOutputDrain()
{
    if (_outputDrainScheduled || _peerLost)
        return;
    _outputDrainScheduled = true;
    // post() runs on the loop thread on the next pump: the drain is self-sustaining
    // without new PTY output. `alive` keeps the flag object alive so a continuation
    // that resumes after this session is destroyed reads it and no-ops.
    _loop.post([this, alive = _alive] {
        if (!*alive)
            return; // the session was torn down while this continuation was queued
        _outputDrainScheduled = false;
        pumpOutput();
    });
}

void ControlSession::handlePeerLost()
{
    if (_peerLost)
        return;
    _peerLost = true;
    // The client cannot keep up (or its transport failed): drop it the way a read
    // EOF would. Closing the queue and connection wakes run()'s parked reader with
    // BadHandle, so it unwinds through the normal teardown epilogue.
    _writer.close();
    _connection->close();
}

// ---------------------------------------------------------------------------
// Notifications

void ControlSession::tabAdded(WindowId /*window*/, TabId tab, int /*index*/)
{
    _output.enqueueNotification(std::format("%window-add @{}", tab.value));
    notifyLayoutChanged(tab);
}

void ControlSession::tabClosed(WindowId /*window*/, TabId tab, int /*index*/)
{
    _output.enqueueNotification(std::format("%window-close @{}", tab.value));
}

void ControlSession::tabMoved(WindowId /*window*/, TabId /*tab*/, int /*fromIndex*/, int /*toIndex*/)
{
    _output.enqueueNotification("%sessions-changed");
}

void ControlSession::activeTabChanged(WindowId /*window*/, TabId tab, int /*index*/)
{
    _output.enqueueNotification(std::format("%session-window-changed $0 @{}", tab.value));
}

void ControlSession::paneSplit(TabId tab, PaneId /*splitNode*/, PaneId /*newLeaf*/)
{
    notifyLayoutChanged(tab);
}

void ControlSession::paneClosed(TabId tab, PaneId /*closed*/, PaneId /*survivor*/)
{
    notifyLayoutChanged(tab);
}

void ControlSession::activePaneChanged(TabId /*tab*/, PaneId /*leaf*/)
{
    // Focus is client-local in control mode; nothing to notify.
}

void ControlSession::paneRatioChanged(TabId tab, PaneId /*splitNode*/, double /*ratio*/)
{
    notifyLayoutChanged(tab);
}

void ControlSession::tabTitleChanged(TabId tab)
{
    auto const* tabPtr = _host.model().findTab(tab);
    auto const title = tabPtr != nullptr ? tabPtr->runtimeTitle().value_or("") : std::string {};
    _output.enqueueNotification(std::format("%window-renamed @{} {}", tab.value, title));
}

void ControlSession::tabColorChanged(TabId /*tab*/)
{
    // No control-mode notification exists for window colors.
}

void ControlSession::paneTreeRestructured(TabId tab)
{
    notifyLayoutChanged(tab);
}

void ControlSession::paneZoomChanged(TabId tab, std::optional<PaneId> /*zoomedLeaf*/)
{
    notifyLayoutChanged(tab);
}

void ControlSession::notifyLayoutChanged(TabId tab)
{
    auto const* tabPtr = _host.model().findTab(tab);
    if (tabPtr == nullptr)
        return;
    auto const layout = encodeLayout(*tabPtr->rootPane(), pageSize());
    auto const visible = encodeLayout(*tabPtr->layoutRoot(), pageSize());
    // The trailing #{window_raw_flags} expands empty here — the trailing space
    // is part of the wire format (control-notify.c:77-78).
    _output.enqueueNotification(std::format("%layout-change @{} {} {} ", tab.value, layout, visible));
}

// ---------------------------------------------------------------------------
// Commands

std::vector<ControlSession::CommandEntry> const& ControlSession::commandCatalog()
{
    static auto const catalog = std::vector<CommandEntry> {
        { "capture-pane", &ControlSession::commandCapturePane },
        { "display-message", &ControlSession::commandDisplayMessage },
        { "kill-pane", &ControlSession::commandKillPane },
        { "list-panes", &ControlSession::commandListPanes },
        { "list-sessions", &ControlSession::commandListSessions },
        { "list-windows", &ControlSession::commandListWindows },
        { "new-window", &ControlSession::commandNewWindow },
        { "refresh-client", &ControlSession::commandRefreshClient },
        { "rename-window", &ControlSession::commandRenameWindow },
        { "resize-pane", &ControlSession::commandResizePane },
        { "select-pane", &ControlSession::commandSelectPane },
        { "select-window", &ControlSession::commandSelectWindow },
        { "send-keys", &ControlSession::commandSendKeys },
        { "split-window", &ControlSession::commandSplitWindow },
    };
    return catalog;
}

std::expected<Tab*, std::string> ControlSession::resolveTab(std::vector<std::string> const& arguments) const
{
    auto* window = _host.model().window(_host.windowId());
    if (auto const target = targetOf(arguments))
    {
        auto const id = parseIdSuffix(*target, '@');
        if (!id)
            return std::unexpected(std::format("bad window target: {}", *target));
        auto* tab = _host.model().findTab(TabId { *id });
        if (tab == nullptr)
            return std::unexpected(std::format("window not found: {}", *target));
        return tab;
    }
    auto* active = window->activeTab();
    if (active == nullptr)
        return std::unexpected("no current window");
    return active;
}

std::expected<Pane*, std::string> ControlSession::resolvePane(std::vector<std::string> const& arguments) const
{
    if (auto const target = targetOf(arguments))
    {
        auto const id = parseIdSuffix(*target, '%');
        if (!id)
            return std::unexpected(std::format("bad pane target: {}", *target));
        auto* window = _host.model().window(_host.windowId());
        for (auto const tabIndex: std::views::iota(0, window->tabCount()))
        {
            Pane* found = nullptr;
            window->tabAt(tabIndex)->rootPane()->walkTree([&](Pane& pane) {
                if (pane.isLeaf() && pane.id().value == *id)
                    found = &pane;
            });
            if (found != nullptr)
                return found;
        }
        return std::unexpected(std::format("pane not found: {}", *target));
    }
    auto* active = _host.model().window(_host.windowId())->activeTab();
    if (active == nullptr || active->activePane() == nullptr)
        return std::unexpected("no current pane");
    return active->activePane();
}

ControlSession::HandlerResult ControlSession::commandListSessions(std::vector<std::string> const& /*args*/)
{
    auto const* window = _host.model().window(_host.windowId());
    return std::vector<std::string> { std::format("0: 0 [{} windows] (attached)", window->tabCount()) };
}

ControlSession::HandlerResult ControlSession::commandListWindows(std::vector<std::string> const& arguments)
{
    auto const formats = valuesOf(arguments, "-F");
    auto lines = std::vector<std::string> {};
    auto const* window = _host.model().window(_host.windowId());
    for (auto const tabIndex: std::views::iota(0, window->tabCount()))
    {
        auto const* tab = window->tabAt(tabIndex);
        if (formats.empty())
        {
            lines.push_back(std::format("{}: @{} {} [{}]",
                                        tabIndex,
                                        tab->id().value,
                                        tab->runtimeTitle().value_or(""),
                                        encodeLayout(*tab->rootPane(), pageSize())));
            continue;
        }

        // Minimal #{...} format support: one row per known variable.
        struct FormatVariable
        {
            std::string_view name;
            std::string value;
        };
        auto const variables = std::array {
            FormatVariable { "#{window_id}", std::format("@{}", tab->id().value) },
            FormatVariable { "#{window_index}", std::format("{}", tabIndex) },
            FormatVariable { "#{window_name}", tab->runtimeTitle().value_or("") },
            FormatVariable { "#{window_layout}", encodeLayout(*tab->rootPane(), pageSize()) },
        };
        auto line = std::string { formats.front() };
        for (auto const& [name, value]: variables)
            for (auto at = line.find(name); at != std::string::npos; at = line.find(name, at))
            {
                line.replace(at, name.size(), value);
                at += value.size();
            }
        lines.push_back(std::move(line));
    }
    return lines;
}

ControlSession::HandlerResult ControlSession::commandListPanes(std::vector<std::string> const& arguments)
{
    auto tab = resolveTab(arguments);
    if (!tab)
        return std::unexpected(tab.error());
    auto lines = std::vector<std::string> {};
    auto index = 0;
    (*tab)->rootPane()->walkTree([&](Pane& pane) {
        if (pane.isLeaf())
            lines.push_back(std::format("{}: %{}", index++, pane.id().value));
    });
    return lines;
}

ControlSession::HandlerResult ControlSession::commandNewWindow(std::vector<std::string> const& /*args*/)
{
    auto* tab = _host.createTab();
    if (tab == nullptr)
        return std::unexpected("create window failed");
    return std::vector<std::string> {};
}

ControlSession::HandlerResult ControlSession::commandSplitWindow(std::vector<std::string> const& arguments)
{
    auto tab = resolveTab(arguments);
    if (!tab)
        return std::unexpected(tab.error());
    // tmux: -h splits left|right (our Vertical), -v (default) stacks (Horizontal).
    auto const orientation = hasFlag(arguments, "-h") ? SplitState::Vertical : SplitState::Horizontal;
    auto const panesBefore = (*tab)->paneCount();
    _host.splitActivePane((*tab)->id(), orientation, 0.5);
    if ((*tab)->paneCount() == panesBefore)
        return std::unexpected("split failed");
    return std::vector<std::string> {};
}

ControlSession::HandlerResult ControlSession::commandKillPane(std::vector<std::string> const& arguments)
{
    auto pane = resolvePane(arguments);
    if (!pane)
        return std::unexpected(pane.error());
    _host.handleSessionExit((*pane)->session());
    return std::vector<std::string> {};
}

ControlSession::HandlerResult ControlSession::commandSelectPane(std::vector<std::string> const& arguments)
{
    auto pane = resolvePane(arguments);
    if (!pane)
        return std::unexpected(pane.error());
    auto* window = _host.model().window(_host.windowId());
    for (auto const tabIndex: std::views::iota(0, window->tabCount()))
    {
        auto* tab = window->tabAt(tabIndex);
        auto contains = false;
        tab->rootPane()->walkTree([&](Pane& node) {
            if (&node == *pane)
                contains = true;
        });
        if (contains)
        {
            _host.model().setActivePane(tab->id(), (*pane)->id());
            return std::vector<std::string> {};
        }
    }
    return std::unexpected("pane not found");
}

ControlSession::HandlerResult ControlSession::commandSelectWindow(std::vector<std::string> const& arguments)
{
    auto tab = resolveTab(arguments);
    if (!tab)
        return std::unexpected(tab.error());
    _host.model().activateTab(_host.windowId(), (*tab)->id());
    return std::vector<std::string> {};
}

ControlSession::HandlerResult ControlSession::commandSendKeys(std::vector<std::string> const& arguments)
{
    auto pane = resolvePane(arguments);
    if (!pane)
        return std::unexpected(pane.error());
    auto* terminal = _host.terminal((*pane)->session());
    if (terminal == nullptr)
        return std::unexpected("pane has no live session");

    auto const literal = hasFlag(arguments, "-l");
    auto const hex = hasFlag(arguments, "-H");
    auto bytes = std::string {};
    auto skipNext = false;
    for (auto const& argument: arguments | std::views::drop(1))
    {
        if (skipNext)
        {
            skipNext = false;
            continue;
        }
        if (argument == "-t")
        {
            skipNext = true;
            continue;
        }
        if (argument == "-l" || argument == "-H")
            continue;
        if (argument == "--")
            continue; // end-of-options, as tmux's argument parser accepts
        if (hex)
        {
            // -H: each argument is one byte as a hexadecimal number.
            auto value = unsigned {};
            auto const [ptr, ec] =
                std::from_chars(argument.data(), argument.data() + argument.size(), value, 16);
            if (ec != std::errc {} || ptr != argument.data() + argument.size() || value > 0xFF)
                return std::unexpected(std::format("bad hex byte: {}", argument));
            bytes += static_cast<char>(value);
            continue;
        }
        bytes += literal ? argument : translateKey(argument);
    }

    if (!bytes.empty())
        std::ignore = terminal->device().write(bytes);
    return std::vector<std::string> {};
}

ControlSession::HandlerResult ControlSession::commandCapturePane(std::vector<std::string> const& arguments)
{
    auto pane = resolvePane(arguments);
    if (!pane)
        return std::unexpected(pane.error());
    auto* terminal = _host.terminal((*pane)->session());
    if (terminal == nullptr)
        return std::unexpected("pane has no live session");

    auto lines = std::vector<std::string> {};
    auto const text = terminal->primaryScreen().renderMainPageText();
    for (auto const line: std::views::split(text, '\n'))
        lines.emplace_back(std::string_view { line });
    // renderMainPageText ends each row with LF; drop the trailing empty piece.
    if (!lines.empty() && lines.back().empty())
        lines.pop_back();
    return lines;
}

ControlSession::HandlerResult ControlSession::commandRenameWindow(std::vector<std::string> const& arguments)
{
    auto tab = resolveTab(arguments);
    if (!tab)
        return std::unexpected(tab.error());
    auto name = std::string {};
    auto skipNext = false;
    for (auto const& argument: arguments | std::views::drop(1))
    {
        if (skipNext)
        {
            skipNext = false;
            continue;
        }
        if (argument == "-t")
        {
            skipNext = true;
            continue;
        }
        name = argument;
    }
    if (name.empty())
        return std::unexpected("usage: rename-window [-t @N] name");
    _host.model().setTabTitle((*tab)->id(), name);
    return std::vector<std::string> {};
}

ControlSession::HandlerResult ControlSession::commandResizePane(std::vector<std::string> const& arguments)
{
    if (hasFlag(arguments, "-Z"))
    {
        auto tab = resolveTab(arguments);
        if (!tab)
            return std::unexpected(tab.error());
        _host.model().toggleActivePaneZoom((*tab)->id());
        return std::vector<std::string> {};
    }
    return std::unexpected("only resize-pane -Z is supported");
}

ControlSession::HandlerResult ControlSession::commandDisplayMessage(std::vector<std::string> const& arguments)
{
    auto message = std::string {};
    for (auto const& argument: arguments | std::views::drop(1))
    {
        if (argument == "-p")
            continue;
        if (!message.empty())
            message += ' ';
        message += argument;
    }
    return std::vector<std::string> { message };
}

ControlSession::HandlerResult ControlSession::commandRefreshClient(std::vector<std::string> const& arguments)
{
    for (auto const size: valuesOf(arguments, "-C"))
    {
        // The client's size proposal, "WxH" or "W,H" (cmd-refresh-client.c
        // accepts both). The @W: per-window forms are not supported.
        if (size.starts_with('@'))
            return std::unexpected("per-window sizes are not supported");
        auto const separator = size.find_first_of("x,");
        if (separator == std::string_view::npos)
            return std::unexpected("bad size argument");
        auto const width = parseNumber(size.substr(0, separator));
        auto const height = parseNumber(size.substr(separator + 1));
        if (!width || !height)
            return std::unexpected("bad size argument");
        // WINDOW_MINIMUM (1) / WINDOW_MAXIMUM (10000), tmux.h:110-115.
        if (*width < 1 || *width > 10000 || *height < 1 || *height > 10000)
            return std::unexpected("size too small or too big");
        _host.applyClientSize(vtpty::PageSize { .lines = vtpty::LineCount(static_cast<int>(*height)),
                                                .columns = vtpty::ColumnCount(static_cast<int>(*width)) });
        // The server decides: answer the proposal with authoritative layouts.
        auto const* window = _host.model().window(_host.windowId());
        for (auto const tabIndex: std::views::iota(0, window->tabCount()))
            notifyLayoutChanged(window->tabAt(tabIndex)->id());
    }

    for (auto const value: valuesOf(arguments, "-A"))
    {
        // "%N:on|off|continue|pause"; malformed values are silently ignored,
        // exactly like cmd_refresh_client_update_offset.
        auto const colon = value.find(':');
        if (colon == std::string_view::npos)
            continue;
        auto const pane = parseIdSuffix(value.substr(0, colon), '%');
        if (!pane)
            continue;
        auto const state = value.substr(colon + 1);
        if (state == "on")
            _output.setPaneEnabled(*pane, true);
        else if (state == "off")
            _output.setPaneEnabled(*pane, false);
        else if (state == "continue")
            _output.continuePane(*pane);
        else if (state == "pause")
            _output.pausePane(*pane);
    }

    // -B subscriptions would need the format engine; the syntax is accepted so
    // probing clients don't fail, but no %subscription-changed is produced.

    for (auto const* option: { "-f", "-F" }) // -F is an alias for -f
        for (auto const flags: valuesOf(arguments, option))
            for (auto const flag: std::views::split(flags, ','))
                applyClientFlag(std::string_view { flag });

    return std::vector<std::string> {};
}

void ControlSession::applyClientFlag(std::string_view flag)
{
    // server_client_control_flags semantics: a "!" prefix clears the flag, an
    // unrecognized flag changes nothing.
    auto const negated = flag.starts_with('!');
    if (negated)
        flag.remove_prefix(1);

    if (flag == "pause-after" || flag.starts_with("pause-after="))
    {
        if (negated)
        {
            _output.setPauseAfter(std::nullopt);
            _output.setExtendedOutput(false);
            return;
        }
        auto seconds = std::uint64_t { 0 };
        if (auto const equals = flag.find('='); equals != std::string_view::npos)
        {
            auto const parsed = parseNumber(flag.substr(equals + 1));
            if (!parsed)
                return;
            seconds = *parsed;
        }
        _output.setPauseAfter(pauseAfterFromSeconds(seconds));
        // Every output line now carries its age (%extended-output) so the
        // client can judge staleness itself — control.c:653-658.
        _output.setExtendedOutput(true);
        return;
    }
    if (flag == "no-output")
        _noOutput = !negated;
    // wait-exit and the non-control-mode client flags are accepted unchanged:
    // run() already drains all pending replies before closing the connection.
}

// ---------------------------------------------------------------------------

namespace
{
    /// One control client's whole lifetime, as a free coroutine (a capturing
    /// lambda coroutine would dangle its closure; pointers live in the frame).
    coro::Task<void> serveControlClient(net::EventLoop* loop,
                                        SessionHost* host,
                                        std::unique_ptr<net::ISocket> connection)
    {
        auto session = std::make_unique<ControlSession>(*loop, *host, std::move(connection), [] {
            return std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                .count();
        });

        // Subscribe the byte tap for the lifetime of this connection; any
        // number of clients may be attached concurrently.
        auto const subscription = ScopedStreamSubscription { *host, *session };
        co_await session->run();
    }
} // namespace

std::function<coro::Task<void>(std::unique_ptr<net::ISocket>)> makeControlModeHandler(net::EventLoop& loop,
                                                                                      SessionHost& host)
{
    // NOT a coroutine itself: it merely constructs the free coroutine's task,
    // so the captures never outlive an activation frame.
    return [&loop, &host](std::unique_ptr<net::ISocket> connection) {
        return serveControlClient(&loop, &host, std::move(connection));
    };
}

} // namespace muxserver::tmux
