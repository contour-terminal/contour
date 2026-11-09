// SPDX-License-Identifier: Apache-2.0
#include <vthost/tmux/ControlSession.hpp>

#include <core/Utf8Path.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <format>
#include <mutex>
#include <ranges>
#include <span>
#include <utility>

#include <vthost/Logging.hpp>
#include <vthost/tmux/LayoutString.hpp>
#include <vtworkspace/Pane.hpp>
#include <vtworkspace/Tab.hpp>

namespace vthost::tmux
{

using vtworkspace::Pane;
using vtworkspace::PaneId;
using vtworkspace::SessionId;
using vtworkspace::SplitState;
using vtworkspace::Tab;
using vtworkspace::TabId;
using vtworkspace::WorkspaceId;

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

    /// Whether the boolean short-flag @p flag is present, honouring tmux's BUNDLED form: `-peqJ` sets
    /// p, e, q and J. A lone value option in @p valueOptions (e.g. `-S`, `-E`, `-t`) consumes the next
    /// token as its value, so that value (which may itself begin with `-`, like `-S -`) is not mistaken
    /// for a flag bundle.
    /// @param arguments The split command line.
    /// @param flag The boolean flag character (e.g. 'e').
    /// @param valueOptions The single-char options that take a following value.
    [[nodiscard]] bool hasBooleanFlag(std::vector<std::string> const& arguments,
                                      char flag,
                                      std::string_view valueOptions)
    {
        for (std::size_t i = 1; i < arguments.size(); ++i)
        {
            auto const& arg = arguments[i];
            if (arg.size() < 2 || arg[0] != '-')
                continue; // a value or bare token
            if (arg.size() == 2 && valueOptions.contains(arg[1]))
            {
                ++i; // a lone value option: skip its value
                continue;
            }
            if (arg.find(flag, 1) != std::string::npos)
                return true; // present in this bundle of boolean flags
        }
        return false;
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

    /// One `#{...}` format variable and the text it expands to.
    struct FormatVariable
    {
        std::string_view name;
        std::string value;
    };

    /// Expands every occurrence of each of @p variables in @p text; anything else passes as it is.
    /// @param text The format text.
    /// @param variables The variables the format may name.
    /// @return The expanded text.
    [[nodiscard]] std::string expandFormat(std::string text, std::span<FormatVariable const> variables)
    {
        for (auto const& [name, value]: variables)
        {
            auto at = text.find(name);
            while (at != std::string::npos)
            {
                text.replace(at, name.size(), value);
                at = text.find(name, at + value.size());
            }
        }
        return text;
    }

    /// The pane-level format variables of @p text. The directory is asked of the host only when the text
    /// names `#{pane_current_path}`, since it can mean a query of the shell's process.
    /// @param host The host the pane lives in.
    /// @param pane The pane the format is expanded against; null expands the variables to nothing.
    /// @param text The format text.
    /// @return The variables to expand it with.
    [[nodiscard]] std::vector<FormatVariable> paneVariables(SessionHost const& host,
                                                            Pane const* pane,
                                                            std::string_view text)
    {
        constexpr auto CurrentPath = std::string_view { "#{pane_current_path}" };
        auto variables = std::vector<FormatVariable> {};
        if (!text.contains(CurrentPath))
            return variables;
        auto const directory = pane != nullptr ? host.workingDirectoryOf(pane->session())
                                               : std::optional<std::filesystem::path> {};
        variables.push_back(
            FormatVariable { CurrentPath, directory ? core::pathToUtf8(*directory) : std::string {} });
        return variables;
    }

    /// The positional arguments of a command line: everything after the command that is neither an option
    /// of @p valueOptions with its value, nor one of @p flags, nor the end-of-options `--`.
    /// @param arguments The split command line.
    /// @param valueOptions The options that take a following value (e.g. `-t`).
    /// @param flags The boolean flags to skip (e.g. `-l`).
    /// @return The positional arguments, in order.
    [[nodiscard]] std::vector<std::string_view> positionalsOf(
        std::vector<std::string> const& arguments,
        std::initializer_list<std::string_view> valueOptions,
        std::initializer_list<std::string_view> flags = {})
    {
        auto positionals = std::vector<std::string_view> {};
        auto skipNext = false;
        for (auto const& argument: arguments | std::views::drop(1))
        {
            if (std::exchange(skipNext, false))
                continue;
            if (std::ranges::contains(valueOptions, argument))
                skipNext = true;
            else if (!std::ranges::contains(flags, argument) && argument != "--")
                positionals.emplace_back(argument);
        }
        return positionals;
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

ControlSession::ControlSession(core::net::EventLoop& loop,
                               SessionHost& host,
                               ConnectionId id,
                               std::unique_ptr<core::net::ISocket> connection,
                               std::function<std::int64_t()> wallClock,
                               Options options):
    _loop(loop),
    _host(host),
    _id(std::move(id)),
    _connection(std::move(connection)),
    _wallClock(std::move(wallClock)),
    _options(options),
    _writer(loop, _connection.get(), _options.writeQueueMaxBytes),
    _output([this](std::string line) {
        if (_peerLost)
            return; // the client is already gone; further lines drop silently
        // Every outbound control line — %begin/%end guards, %output, every notification —
        // passes through this one lambda, so tracing here covers the whole outbound stream
        // including the dual-queue ordering.
        if (tmuxTraceLog)
            tmuxTraceLog()("{} < {}", _id, line);
        if (!_writer.enqueue(std::move(line)))
        {
            // Through the same helper as the native paths, so a dead transport is no longer
            // misreported here as an overflow.
            errorLog()("{}: {}; disconnecting", _id, _writer.describeRefusal());
            handlePeerLost(); // queue overflow/failure: disconnect, never drop-and-continue
        }
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

core::async::Task<void> ControlSession::run()
{
    // The implicit initial command's empty guard pair, then the session state —
    // the preamble iTerm2-style clients gate their notification handling on.
    // The imsg path stamps it flag 0 (the MSG_COMMAND-originated attach).
    emitGuarded(HandlerResult { std::vector<std::string> {} }, _options.initialGuardFlag);
    _output.enqueueNotification("%session-changed $0 0");

    auto reader = core::net::AsyncBufferedReader { _connection.get() };
    while (true)
    {
        auto line = co_await reader.readLine();
        if (!line.has_value())
        {
            // The error's OWN text, not a fixed "end of stream": readLine fails with any of
            // NetErrorCode's categories, and reporting them all as a clean FIN made a broken
            // transport indistinguishable from a peer that hung up. NetError::toString already
            // spells Eof "end of stream", so the clean case reads as it always did.
            tmuxLog()("{}: control client detached ({}; {} commands)",
                      _id,
                      line.error().toString(),
                      _commandsDispatched);
            break; // disconnect or poisoned stream
        }
        if (line->empty())
        {
            tmuxLog()("{}: control client detached (empty line; {} commands)", _id, _commandsDispatched);
            break; // an empty line detaches (control.c:547)
        }
        if (tmuxTraceLog)
            tmuxTraceLog()("{} > {}", _id, *line);
        dispatch(*line);
    }

    // The tmux client binary prints its own %exit; the imsg path suppresses ours.
    if (_options.emitExitLine)
        _output.enqueueNotification("%exit");
    // Drain the ordering queue first — a burst still being paced out (and the
    // notifications gated behind it) must fully reach the writer; a lost peer
    // skips that wait. Then let the write queue flush %exit and any trailing
    // replies before the connection dies.
    co_await core::net::pollUntil(&_loop, [this] { return _peerLost || !_output.hasPending(); });
    co_await _writer.flushThenClose();
    _connection->close();
}

void ControlSession::dispatch(std::string_view line)
{
    // Counted here rather than per successful handler: an empty or unknown command is still the
    // peer speaking, which is the question the detach line's count answers.
    ++_commandsDispatched;

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
        // Answered to the peer AND recorded: the peer's %error goes to a client that may well
        // discard it, leaving "tmux -CC does not work here" with nothing behind it.
        errorLog()("{}: unknown control command '{}'", _id, arguments.front());
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
    // Map the session to its hosting leaf pane (the %N the client knows), in
    // whichever window hosts it — or output for secondary-window panes is dropped.
    if (auto const [window, tab, leaf] = _host.model().findSessionLeaf(session); leaf != nullptr)
    {
        _output.enqueueOutput(leaf->id().value, bytes, std::chrono::steady_clock::now());
        pumpOutput();
    }
}

void ControlSession::sessionResized(SessionId session)
{
    if (_peerLost)
        return;
    // The grid has ALREADY moved by the time this arrives, and the paths that moved it raise no
    // ModelEvents at all: a native client's applyClientSize, or a detach re-resolving the
    // authoritative area, both end in reprojectLayouts, which announces itself only through this
    // callback. commandRefreshClient re-emits the same notification after its own -C proposal,
    // which covers the client that made it and no other — so without this a second client's resize
    // left this one rendering the old cell geometry: output wrapping at the wrong column and
    // full-screen apps painting into the wrong rectangle until the user resized their own window.
    if (auto const [window, tab, leaf] = _host.model().findSessionLeaf(session); tab != nullptr)
        notifyLayoutChanged(tab->id());
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
    // Cancelled, so it unwinds through the normal teardown epilogue.
    // The queue closes _connection too. A close resumes a parked read at once, which can end
    // the flow that owns this object, so it comes last and is not repeated.
    _writer.close();
}

// ---------------------------------------------------------------------------
// Notifications

void ControlSession::tabAdded(WorkspaceId /*window*/, TabId tab, int /*index*/)
{
    _output.enqueueNotification(std::format("%window-add @{}", tab.value));
    notifyLayoutChanged(tab);
}

void ControlSession::tabClosed(WorkspaceId /*window*/, TabId tab, int /*index*/)
{
    _output.enqueueNotification(std::format("%window-close @{}", tab.value));
}

void ControlSession::tabMoved(WorkspaceId /*window*/, TabId /*tab*/, int /*fromIndex*/, int /*toIndex*/)
{
    _output.enqueueNotification("%sessions-changed");
}

void ControlSession::tabMovedToWorkspace(
    WorkspaceId /*from*/, TabId tab, int /*fromIndex*/, WorkspaceId /*to*/, int /*toIndex*/)
{
    // The tab changed windows: the client's session list and the tab's layout are both stale.
    _output.enqueueNotification("%sessions-changed");
    notifyLayoutChanged(tab);
}

void ControlSession::paneSwapped(TabId tab, PaneId /*a*/, PaneId /*b*/)
{
    notifyLayoutChanged(tab);
}

void ControlSession::activeTabChanged(WorkspaceId /*window*/, TabId tab, int /*index*/)
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
    auto* window = _host.model().workspace(_host.workspaceId());
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
        // tmux %N ids are model-global leaf panes; the target may live in ANY
        // window, not just the primary one.
        if (auto* pane = _host.model().findLeafPane(PaneId { *id }); pane != nullptr)
            return pane;
        return std::unexpected(std::format("pane not found: {}", *target));
    }
    auto* active = _host.model().workspace(_host.workspaceId())->activeTab();
    if (active == nullptr || active->activePane() == nullptr)
        return std::unexpected("no current pane");
    return active->activePane();
}

ControlSession::HandlerResult ControlSession::commandListSessions(std::vector<std::string> const& /*args*/)
{
    auto const* window = _host.model().workspace(_host.workspaceId());
    return std::vector<std::string> { std::format("0: 0 [{} windows] (attached)", window->tabCount()) };
}

ControlSession::HandlerResult ControlSession::commandListWindows(std::vector<std::string> const& arguments)
{
    auto const formats = valuesOf(arguments, "-F");
    auto lines = std::vector<std::string> {};
    auto const* window = _host.model().workspace(_host.workspaceId());
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
        auto const variables = std::array {
            FormatVariable { "#{window_id}", std::format("@{}", tab->id().value) },
            FormatVariable { "#{window_index}", std::format("{}", tabIndex) },
            FormatVariable { "#{window_name}", tab->runtimeTitle().value_or("") },
            FormatVariable { "#{window_layout}", encodeLayout(*tab->rootPane(), pageSize()) },
        };
        auto line = expandFormat(std::string { formats.front() }, variables);
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

std::expected<ControlSession::SplitTarget, std::string> ControlSession::resolveSplitTarget(
    std::vector<std::string> const& arguments) const
{
    auto const target = targetOf(arguments);
    if (!target || !target->starts_with('%'))
    {
        auto const tab = resolveTab(arguments);
        if (!tab)
            return std::unexpected(tab.error());
        return SplitTarget { .tab = *tab };
    }
    auto const pane = resolvePane(arguments);
    if (!pane)
        return std::unexpected(pane.error());
    auto const [workspace, tab, leaf] = _host.model().findSessionLeaf((*pane)->session());
    if (tab == nullptr)
        return std::unexpected(std::format("pane not in a window: {}", *target));
    return SplitTarget { .tab = tab, .pane = *pane };
}

std::expected<Pane*, std::string> ControlSession::resolveFormatPane(
    std::vector<std::string> const& arguments) const
{
    auto const target = targetOf(arguments);
    if (!target || !target->starts_with('@'))
        return resolvePane(arguments);
    auto const tab = resolveTab(arguments);
    if (!tab)
        return std::unexpected(tab.error());
    return (*tab)->activePane();
}

std::optional<std::filesystem::path> ControlSession::startDirectoryOf(
    std::vector<std::string> const& arguments, Pane const* source) const
{
    auto const directories = valuesOf(arguments, "-c");
    if (directories.empty())
        return std::nullopt;
    auto const directory =
        expandFormat(std::string { directories.back() }, paneVariables(_host, source, directories.back()));
    if (directory.empty())
        return std::nullopt;
    auto path = core::pathFromUtf8(directory);
    if (!path)
        return std::nullopt;
    return *std::move(path);
}

ControlSession::HandlerResult ControlSession::commandNewWindow(std::vector<std::string> const& arguments)
{
    // tmux expands -c against the target window's active pane. Here -t only places the new window (`$0`,
    // `main:`, `:2`) and names no window to inherit from, so such a target, like none, means the current
    // window; a host with no window yet has no pane to inherit from.
    auto const named = resolveTab(arguments);
    auto const* const source = named ? *named : _host.model().workspace(_host.workspaceId())->activeTab();
    auto request = SessionSpawnRequest {};
    if (auto const directory =
            startDirectoryOf(arguments, source != nullptr ? source->activePane() : nullptr))
        startIn(request, *directory);
    auto* created = _host.createTab(request);
    if (created == nullptr)
        return std::unexpected("create window failed");
    return std::vector<std::string> {};
}

ControlSession::HandlerResult ControlSession::commandSplitWindow(std::vector<std::string> const& arguments)
{
    auto const target = resolveSplitTarget(arguments);
    if (!target)
        return std::unexpected(target.error());
    // A pane target is the pane to split; a window target splits that window's active pane.
    auto const previous = target->tab->activePane();
    if (target->pane != nullptr)
        _host.model().setActivePane(target->tab->id(), target->pane->id());
    // tmux: -h splits left|right (our Vertical), -v (default) stacks (Horizontal).
    auto const orientation = hasFlag(arguments, "-h") ? SplitState::Vertical : SplitState::Horizontal;
    auto request = SessionSpawnRequest {};
    if (auto const directory = startDirectoryOf(arguments, target->tab->activePane()))
        startIn(request, *directory);
    auto const panesBefore = target->tab->paneCount();
    _host.splitActivePane(target->tab->id(), orientation, 0.5, request);
    if (target->tab->paneCount() == panesBefore)
    {
        // The split did not happen, so the pane the target made active must not stay so.
        if (previous != nullptr && previous != target->tab->activePane())
            _host.model().setActivePane(target->tab->id(), previous->id());
        return std::unexpected("split failed");
    }
    return std::vector<std::string> {};
}

ControlSession::HandlerResult ControlSession::commandKillPane(std::vector<std::string> const& arguments)
{
    auto pane = resolvePane(arguments);
    if (!pane)
        return std::unexpected(pane.error());
    _host.closeSession((*pane)->session());
    return std::vector<std::string> {};
}

ControlSession::HandlerResult ControlSession::commandSelectPane(std::vector<std::string> const& arguments)
{
    auto pane = resolvePane(arguments);
    if (!pane)
        return std::unexpected(pane.error());
    auto* window = _host.model().workspace(_host.workspaceId());
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
    _host.model().activateTab(_host.workspaceId(), (*tab)->id());
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
    for (auto const argument: positionalsOf(arguments, { "-t" }, { "-l", "-H" }))
    {
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
        if (literal)
            bytes += argument;
        else
            bytes += translateKey(argument);
    }

    if (!bytes.empty())
        std::ignore = terminal->device().write(bytes);
    return std::vector<std::string> {};
}

ControlSession::HandlerResult ControlSession::commandCapturePane(std::vector<std::string> const& arguments)
{
    constexpr auto ValueOptions = std::string_view { "tSEb" };       // capture-pane's value-taking options
    auto const quiet = hasBooleanFlag(arguments, 'q', ValueOptions); // swallow errors → empty capture
    auto pane = resolvePane(arguments);
    if (!pane)
        return quiet ? HandlerResult { std::vector<std::string> {} } : std::unexpected(pane.error());
    auto* terminal = _host.terminal((*pane)->session());
    if (terminal == nullptr)
        return quiet ? HandlerResult { std::vector<std::string> {} }
                     : std::unexpected("pane has no live session");

    // The session's pump thread mutates this grid (writeText/resize reallocate the
    // line storage) under the terminal's _stateMutex. Hold that lock across every
    // read below — historyLineCount/pageSize and the renderRange walk — so we never
    // read the line SoA vectors while they are being reallocated. Mirrors the locked
    // grid reads NativeSession::pushDelta performs on the same state, as a reader's hold.
    auto const guard = std::lock_guard { std::as_const(*terminal) };
    // The DISPLAYED page, not the primary one — capture-pane answers "what is in this pane", and a
    // pane running vim/less/htop is showing the alternate screen. Reading the primary grid there
    // returns the shell prompt the editor replaced, and `-S -` compounds it by counting history the
    // pane does not show at all. This is the same page NativeSession::pushDelta mirrors, for the
    // same reason.
    auto const& grid = terminal->displayedPage().grid();
    // tmux capture rows: 0 = top of the visible page, negative = into scrollback — the same coordinates
    // as the grid's LineOffset. `-S -` starts at the top of history; `-E -` ends at the visible bottom.
    // `-e` interleaves SGR. `-p` is accepted (the reply IS the printed text); `-J` is accepted for its
    // trailing-space half only — wrapped-line joining is not yet implemented.
    // renderRange clamps the range to the addressable rows.
    auto const historyTop = -boxed_cast<vtbackend::LineOffset>(grid.historyLineCount());
    auto const visibleBottom = unbox<vtbackend::LineOffset>(grid.pageSize().lines) - vtbackend::LineOffset(1);
    auto const rowOf =
        [&](std::string_view flag, vtbackend::LineOffset dash, vtbackend::LineOffset fallback) {
            auto const values = valuesOf(arguments, flag);
            if (values.empty())
                return fallback;
            if (values.front() == "-")
                return dash;
            auto row = std::int64_t {};
            auto const text = values.front();
            auto const [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), row);
            if (ec != std::errc {} || ptr != text.data() + text.size())
                return fallback;
            return vtbackend::LineOffset(static_cast<int>(row));
        };
    auto const start = rowOf("-S", historyTop, vtbackend::LineOffset(0));
    auto const end = rowOf("-E", visibleBottom, visibleBottom);
    // tmux trims each row at its last non-default cell unless -J or -N asks for the trailing spaces
    // (capture-pane's man page: "-J ... preserves trailing spaces at each line's end", likewise -N).
    auto const trailing =
        hasBooleanFlag(arguments, 'J', ValueOptions) || hasBooleanFlag(arguments, 'N', ValueOptions)
            ? vtbackend::CaptureTrailingSpaces::Keep
            : vtbackend::CaptureTrailingSpaces::Trim;
    return grid.renderRange(start,
                            end,
                            hasBooleanFlag(arguments, 'e', ValueOptions)
                                ? vtbackend::CaptureRendition::WithSgr
                                : vtbackend::CaptureRendition::PlainText,
                            trailing);
}

ControlSession::HandlerResult ControlSession::commandRenameWindow(std::vector<std::string> const& arguments)
{
    auto tab = resolveTab(arguments);
    if (!tab)
        return std::unexpected(tab.error());
    auto const names = positionalsOf(arguments, { "-t" });
    auto const name = names.empty() ? std::string {} : std::string { names.back() };
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
    // -t names the pane the format is expanded against; without it the current pane, if there is one.
    auto source = static_cast<Pane const*>(nullptr);
    if (auto const pane = resolveFormatPane(arguments))
        source = *pane;
    else if (targetOf(arguments))
        return std::unexpected(pane.error());

    auto message = std::string {};
    for (auto const argument: positionalsOf(arguments, { "-t" }, { "-p" }))
    {
        if (!message.empty())
            message += ' ';
        message += argument;
    }
    return std::vector<std::string> { expandFormat(message, paneVariables(_host, source, message)) };
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
        std::ignore =
            _host.applyClientSize(this,
                                  vtpty::PageSize { .lines = vtpty::LineCount(static_cast<int>(*height)),
                                                    .columns = vtpty::ColumnCount(static_cast<int>(*width)) },
                                  std::nullopt); // control mode states no cell size
        // The server decides: answer the proposal with authoritative layouts.
        auto const* window = _host.model().workspace(_host.workspaceId());
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
    core::async::Task<void> serveControlClient(core::net::EventLoop* loop,
                                               SessionHost* host,
                                               ConnectionId id,
                                               std::unique_ptr<core::net::ISocket> connection)
    {
        auto session =
            std::make_unique<ControlSession>(*loop, *host, std::move(id), std::move(connection), [] {
                return std::chrono::duration_cast<std::chrono::seconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                    .count();
            });

        // Subscribe the byte tap for the lifetime of this connection; any
        // number of clients may be attached concurrently.
        auto const subscription = makeScopedStreamSubscription(*host, *session);
        co_await session->run();
    }
} // namespace

ConnectionHandler makeControlModeHandler(core::net::EventLoop& loop, SessionHost& host)
{
    // NOT a coroutine itself: it merely constructs the free coroutine's task,
    // so the captures never outlive an activation frame.
    return [&loop, &host](ConnectionId id, std::unique_ptr<core::net::ISocket> connection) {
        return serveControlClient(&loop, &host, std::move(id), std::move(connection));
    };
}

} // namespace vthost::tmux
