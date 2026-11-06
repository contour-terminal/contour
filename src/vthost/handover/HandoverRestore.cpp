// SPDX-License-Identifier: Apache-2.0
#include <vthost/handover/HandoverRestore.hpp>

#include <vtbackend/state/TerminalState.hpp>
#include <vtbackend/state/TerminalStateCodec.hpp>

#include <vtpty/Process.hpp>
#include <vtpty/PtyAdoption.hpp>

#ifndef _WIN32
    #include <vtpty/UnixUtils.hpp>

    #include <sys/resource.h>
    #include <sys/socket.h>
    #include <sys/wait.h>

    #include <cerrno>

    #include <unistd.h>
#endif

#include <core/Assert.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iterator>
#include <mutex>
#include <ranges>
#include <span>
#include <utility>

#include <vthost/ListeningSocket.hpp>
#include <vthost/Logging.hpp>
#include <vthost/SessionHost.hpp>
#include <vthost/SessionSettings.hpp>

namespace vthost
{

using handover::HandoverError;
using handover::HandoverImage;
using handover::HandoverSession;
using vtworkspace::SessionId;

namespace
{
    /// @return The session ids @p tree's leaves carry, in tree order.
    [[nodiscard]] std::vector<SessionId> leafSessionsOf(vtworkspace::WorkspaceTreeState const& tree)
    {
        auto sessions = std::vector<SessionId> {};
        auto const visit = [&](this auto const& self, vtworkspace::PaneTreeState const& node) -> void {
            if (node.children.empty())
                sessions.push_back(node.session);
            for (auto const& child: node.children)
                self(child);
        };
        for (auto const& window: tree.windows)
            for (auto const& tab: window.tabs)
                visit(tab.root);
        return sessions;
    }
} // namespace

std::expected<void, HandoverError> SessionHost::restoreHandover(HandoverImage image)
{
    // The registry first: it names every object the model brings back, and every session adopted below.
    _objects.restore(image.objects);
    // Sessions spawned from now on are built under the preference the latest client stated, as they were.
    _defaultColorPreference = image.defaultColorPreference;
    // Before any session is adopted, so each is put on it.
    seedLearnedCellPixelSize(image.cellPixelSize, "handover");

    auto outcome = std::expected<void, HandoverError> {};
    if (auto const restored = _model.restore(image.tree, image.modelIds); !restored)
    {
        // The programs matter more than where they were shown: each still gets a tab
        // (adoptHandoverSession()).
        errorLog()("handover: the workspace tree was refused (error {}); every session gets a tab of its own",
                   std::to_underlying(restored.error()));
        outcome = std::unexpected(HandoverError::Malformed);
    }

    // No session minted from now on is named as one the previous image had: not one the tree names, not one
    // it hosted, not one it minted.
    _nextSessionId = std::ranges::fold_left(
        image.sessions,
        std::max(image.nextSessionId.value, vtworkspace::firstUnusedSessionId(image.tree).value),
        [](uint64_t next, HandoverSession const& session) { return std::max(next, session.id.value + 1); });

    if (_model.windowCount() > 0)
        _window = _model.windowAt(0)->id();
    else
    {
        _window = _model.createWindow()->id();
        if (!_objects.uuidOf(_window))
            _objects.bind(_window, _objects.mint());
    }

    for (auto& session: image.sessions)
        adoptHandoverSession(session);

    // A leaf whose session could not be adopted shows nothing, and nothing would ever close it.
    for (auto const session: leafSessionsOf(image.tree))
        if (!_sessions.contains(session.value))
            if (auto const [window, tab, leaf] = _model.findSessionLeaf(session); leaf != nullptr)
                _model.closePane(window->id(), tab->id(), leaf->id());

    // Each session crossed with the scrollback limit its terminal was built with, which its state restored:
    // the configuration this image loaded applies to it now, before any client sees it, as a reload would.
    replaceProfiles(_profiles);

    daemonLog()("took over {} of {} session(s) from release {}",
                _sessions.size(),
                image.sessions.size(),
                image.fromRelease.empty() ? std::string_view { "(unknown)" } : image.fromRelease);
    return outcome;
}

void SessionHost::adoptHandoverSession(HandoverSession& session)
{
    // A session that is not adopted leaves nothing behind: its child, which only this process can wait for,
    // is reaped once it exits (on Windows, its handle is let go), and its pane's UUID names nothing -- unless
    // another session of the image is adopted under the same id, whose it then is.
    auto const abandon = [&] {
        if (session.device == handover::SessionDevice::ChildOnPty)
        {
            vtpty::releaseUnadopted(session.child);
#ifndef _WIN32
            // Only POSIX leaves a child to reap, by its process id, and reaping it takes this host's loop: a
            // poll until it exits, which vtpty has no loop to run.
            reapWhenExited(session.child.pid);
#endif
        }
        if (!_sessions.contains(session.id.value))
            _objects.forget(session.id);
    };

    auto pty = vtpty::adoptPty(session.pty);
    if (!pty)
    {
        errorLog()(
            "handover: session {}: its PTY could not be adopted (error {}); closed, so that its program "
            "sees a hangup",
            session.id.value,
            std::to_underlying(pty.error()));
        vtpty::closeInherited(session.pty);
        abandon();
        return;
    }

    // An id another session of the image took already, or none at all, is a broken image; the program still
    // gets a session, under a fresh id and in a tab of its own.
    auto id = session.id;
    if (id == SessionId {} || _sessions.contains(id.value))
    {
        id = SessionId { _nextSessionId++ };
        errorLog()("handover: session {} is named twice in the image; adopted as session {}",
                   session.id.value,
                   id.value);
    }

    // A child that is gone already leaves its PTY to whatever still holds the other end: served until it
    // closes, as the pump ends a session at its PTY's end -- and crosses a later upgrade so again.
    auto device = [&]() -> std::unique_ptr<vtpty::Pty> {
        if (session.device == handover::SessionDevice::PtyOnly)
        {
            sessionLog()("handover: session {}: serving its PTY, on which no child of this one runs",
                         id.value);
            return std::move(*pty);
        }
        if (!vtpty::Process::canAdopt(session.child))
        {
            sessionLog()("handover: session {}: process {} is no child of this one any more; serving its PTY "
                         "until it closes",
                         id.value,
                         session.child.pid);
            return std::move(*pty);
        }
        auto process = vtpty::Process::adopt(session.child, std::move(*pty));
        if (!process)
        {
            errorLog()("handover: session {}: process {} could not be adopted", id.value, session.child.pid);
            return nullptr;
        }
        return std::move(*process);
    }();
    if (!device)
    {
        abandon();
        return;
    }

    // Built on settings this image's configuration makes up afresh, the terminal's construction settings
    // taken from the captured state, so that the state restores into a terminal of its own shape. A state
    // that cannot be read restores nothing: the terminal is fresh, at the PTY's size.
    auto base = handoverBaseSettings(session);
    auto const state = vtbackend::decodeTerminalState(session.terminalState);
    auto settings = state.and_then(
        [&](vtbackend::TerminalState const& captured) { return vtbackend::settingsFor(captured, base); });
    if (!settings)
        base.pageSize = session.pty.pageSize;
    auto hosted = makeHostedSession(id,
                                    std::move(device),
                                    settings ? *std::move(settings) : std::move(base),
                                    session.profileName,
                                    session.origin);

    // All or nothing (Terminal::restoreState()): one that is refused leaves the terminal exactly as it was
    // constructed -- the fresh terminal the program runs on from here, on the very same PTY.
    auto const restored = state.and_then([&](vtbackend::TerminalState const& captured) {
        auto& terminal = hosted->terminal();
        auto const _ = std::lock_guard { terminal };
        return terminal.restoreState(captured);
    });
    if (!restored)
    {
        errorLog()(
            "handover: session {}: its screen could not be restored (error {}); the program runs on with "
            "a fresh one",
            id.value,
            std::to_underlying(restored.error()));
        _unrestoredScreens.push_back(id);
    }

    // Not part of a terminal's state, and without it an image a program draws before a client attaches has no
    // cells to be laid out on: the host's cell size, which restoreHandover() took from the image, gives it
    // back.
    {
        auto& terminal = hosted->terminal();
        auto const _ = std::lock_guard { terminal };
        terminal.changeCellPixelSize(cellPixelSize());
    }

    // Delivered after the replies the restored terminal queued, before anything a client hands it from now.
    hosted->replayCarried(std::move(session.carried));
    installSession(std::move(hosted));
    // What the host knows of the session beside its terminal: the preference its palette was seeded under,
    // so that a client stating the same one again does not wipe what a program set (applyColorPreference()),
    // and the command a restart stopped in it.
    if (session.colorPreference)
        _colorPreferences.insert_or_assign(id.value, *session.colorPreference);
    if (session.interruption)
        _interruptions.insert_or_assign(id.value, *std::move(session.interruption));

    if (!_objects.uuidOf(id))
    {
        auto const known = session.uuid != vtcontrol::Uuid {}
                           && _objects.find(vtcontrol::ObjectRef { .kind = vtcontrol::ObjectKind::Pane,
                                                                   .uuid = session.uuid });
        auto const uuid = session.uuid == vtcontrol::Uuid {} || known ? _objects.mint() : session.uuid;
        _objects.bind(id, uuid);
        _objects.setLabel(vtcontrol::ObjectRef { .kind = vtcontrol::ObjectKind::Pane, .uuid = uuid },
                          session.label);
    }

    // A session the tree does not lay out still is a program: it gets a tab of its own.
    if (auto const [window, tab, leaf] = _model.findSessionLeaf(id); leaf == nullptr)
    {
        _pendingSessionId = id;
        if (_model.createTab(_window) == nullptr)
            errorLog()("handover: session {} could not be given a tab", id.value);
        _pendingSessionId.reset();
    }
}

vtbackend::Settings SessionHost::handoverBaseSettings(HandoverSession const& session) const
{
    // The profile, resolved in the configuration this image loaded, makes up a session's settings -- unless a
    // client stated them: then what it stated, over this host's settings, exactly as its connection built
    // them (NativeSession::adoptClientSessionSettings()).
    if (session.origin == SettingsOrigin::Client)
        return hostedSessionSettings(fromWireSessionSettings(session.statedSettings, _settings));
    if (!_profiles)
        return _settings;
    return hostedSessionSettings(_profiles->resolve(session.profileName).settings);
}

namespace
{
    /// How often the children of sessions a handover could not adopt are looked at, until each was reaped.
    constexpr auto OrphanReapInterval = std::chrono::milliseconds { 250 };
} // namespace

void SessionHost::reapWhenExited(int64_t pid)
{
    _orphans.push_back(pid);
    armOrphanReap();
}

void SessionHost::armOrphanReap()
{
    if (_orphans.empty() || (_orphanReap && !_orphanReap->settled()))
        return;
    _orphanReap.reset();
    _orphanReap.emplace(_loop, _loop.clock().now() + OrphanReapInterval, &reapOrphans, this);
}

void SessionHost::reapOrphans(void* host)
{
    auto& self = *static_cast<SessionHost*>(host);
#ifndef _WIN32
    std::erase_if(self._orphans, [](int64_t pid) {
        // Reaped now; or no child of this process (any more): either way, nothing is left to wait for.
        auto const reaped = ::waitpid(static_cast<pid_t>(pid), nullptr, WNOHANG);
        return reaped > 0 || (reaped < 0 && errno != EINTR);
    });
#else
    self._orphans.clear();
#endif
    // Re-armed from inside its own callback, which DeadlineTimer allows: it is settled before it is called.
    self._orphanReap.reset();
    self.armOrphanReap();
}

} // namespace vthost

namespace vthost::handover
{

namespace
{
    /// @return @p values without those that are @p none, sorted, each once.
    template <typename T>
    [[nodiscard]] std::vector<T> sortedUnique(std::vector<T> values, auto none)
    {
        std::erase_if(values, none);
        std::ranges::sort(values);
        auto const repeated = std::ranges::unique(values);
        values.erase(repeated.begin(), repeated.end());
        return values;
    }

#ifndef _WIN32
    /// Whether a descriptor is none: a negative one.
    constexpr auto NoDescriptor = [](int fd) {
        return fd < 0;
    };
#endif
} // namespace

std::vector<vtpty::CrossingHandle> handlesNamedBy(HandoverImage const& image)
{
    auto named = std::vector<vtpty::CrossingHandle> {};
    for (auto const& session: image.sessions)
    {
        std::ranges::copy(session.pty.handles(), std::back_inserter(named));
        named.push_back(session.child.processHandle);
    }
    for (auto const& listener: image.listeners)
        named.push_back(listener.handle);
    named.push_back(image.lease);
    return sortedUnique(std::move(named), [](vtpty::CrossingHandle handle) { return handle.isNone(); });
}

std::expected<HandoverImage, HandoverError> readHandover(HandoverMemory& memory,
                                                         vtpty::CrossingHandle memoryHandle)
{
    // Decoded where it was written, and unmapped as soon as it is.
    auto image = memory.read(memoryHandle).and_then([](HandoverView const& view) {
        return decodeHandover(view.bytes());
    });
    memory.discard(memoryHandle);
    return image;
}

#ifndef _WIN32
std::vector<int> descriptorsToKeep(HandoverImage const& image)
{
    auto kept = std::vector<int> { 0, 1, 2 };
    for (auto const handle: handlesNamedBy(image))
        kept.push_back(handle.native());
    return sortedUnique(std::move(kept), NoDescriptor);
}

namespace
{
    /// Closes descriptors @p first to @p last, both included; nothing when @p first is past @p last.
    void closeEach(int first, int last) noexcept
    {
        if (first > last)
            return;
        for (auto const fd: std::views::iota(first, last + 1))
            ::close(fd);
    }

    /// @return The highest descriptor this process can hold now (RLIMIT_NOFILE), bounded: a limit raised to
    ///         millions (or unlimited) must not make a loop over them the slowest part of a start.
    [[nodiscard]] int highestDescriptor() noexcept
    {
        constexpr auto MaxSweptDescriptor = 1 << 20;
        auto limit = rlimit {};
        if (::getrlimit(RLIMIT_NOFILE, &limit) != 0 || limit.rlim_cur == RLIM_INFINITY
            || limit.rlim_cur > static_cast<rlim_t>(MaxSweptDescriptor))
            return MaxSweptDescriptor;
        return static_cast<int>(limit.rlim_cur) - 1;
    }
} // namespace

std::size_t closeListeningSockets()
{
    auto closed = std::size_t { 0 };
    // Above the standard streams, up to the descriptor limit: the old image's listeners are among the first
    // descriptors it opened, but nothing says how far up the rest lie.
    for (auto const fd: std::views::iota(3, highestDescriptor() + 1))
    {
        if (!isListeningSocket(fd))
            continue;
        ::close(fd);
        ++closed;
    }
    return closed;
}

void closeDescriptorsExcept(std::span<int const> keep)
{
    Require(std::ranges::is_sorted(keep) && std::ranges::adjacent_find(keep) == keep.end()
            && (keep.empty() || keep.front() >= 0));
    // The gaps between kept descriptors are a few numbers each; everything above the last goes at once.
    auto next = 0;
    for (auto const fd: keep)
    {
        closeEach(next, fd - 1);
        next = fd + 1;
    }
    // Above the last: as a spawned child's are (close_range(), else /proc/self/fd, else every number).
    vtpty::util::closeDescriptorsAbove(next - 1, vtpty::util::openDescriptorLimit());
}
#endif

} // namespace vthost::handover
