// SPDX-License-Identifier: Apache-2.0
#include <vthost/keeperlink/Recovery.hpp>

#include <vtpty/PageSize.hpp>

#include <sys/ioctl.h>

#include <algorithm>
#include <csignal>
#include <ranges>
#include <utility>
#include <variant>

#include <termios.h>
#include <unistd.h>

#include <vthost/HistorySeed.hpp>
#include <vthost/Logging.hpp>
#include <vthost/SessionSnapshot.hpp>
#include <vthost/keeper/SessionTable.hpp>
#include <vthost/keeperlink/Structure.hpp>

namespace vthost::keeperlink
{

namespace
{
    /// @return @p fds sorted by what @p roles names each; a descriptor named twice, or not at all, is closed.
    [[nodiscard]] keeper::SessionTable::Descriptors keptDescriptorsOf(
        std::span<keeper::KeptDescriptor const> roles, std::vector<keeper::UniqueFd>& fds)
    {
        auto kept = keeper::SessionTable::Descriptors {};
        for (auto&& [role, fd]: std::views::zip(roles, fds))
            if (auto& slot = kept[keeper::slotOf(role)]; !slot.isOpen())
                slot = std::move(fd);
        return kept;
    }

    /// @return @p fd, as vtpty owns descriptors.
    [[nodiscard]] vtpty::OwnedFd owned(keeper::UniqueFd fd) noexcept
    {
        return vtpty::OwnedFd { fd.release() };
    }

    /// Reads the structure the keeper held, recording what went wrong.
    [[nodiscard]] handover::HandoverImage structureOf(keeper::UniqueFd object,
                                                      handover::HandoverMemory& memory,
                                                      std::vector<RecoveryProblem>& problems)
    {
        if (!object.isOpen())
        {
            problems.push_back(RecoveryProblem { .error = RecoveryError::NoStructure, .session = 0 });
            return {};
        }
        auto const handle = vtpty::CrossingHandle::of(object.release());
        auto structure = memory.read(handle).and_then([](handover::HandoverView const& view) {
            return decodeStructure(view.bytes()).transform_error([](StructureError error) {
                errorLog()("recovery: the keeper's structure cannot be read: {}", describe(error));
                return handover::HandoverError::Malformed;
            });
        });
        memory.discard(handle);
        if (!structure)
        {
            problems.push_back(RecoveryProblem { .error = RecoveryError::StructureUnreadable, .session = 0 });
            return {};
        }
        return *std::move(structure);
    }
} // namespace

std::string_view describe(RecoveryError error) noexcept
{
    switch (error)
    {
        case RecoveryError::NoStructure: return "the keeper held no structure";
        case RecoveryError::StructureUnreadable: return "the keeper's structure cannot be read";
        case RecoveryError::SessionWithoutPty: return "the keeper kept the session without its PTY";
    }
    return "it could not be recovered";
}

std::optional<vtpty::PageSize> pageSizeOf(int master)
{
    auto size = winsize {};
    if (::ioctl(master, TIOCGWINSZ, &size) != 0 || size.ws_row == 0 || size.ws_col == 0)
        return std::nullopt;
    return vtpty::PageSize { vtpty::LineCount(size.ws_row), vtpty::ColumnCount(size.ws_col) };
}

std::optional<vtpty::ImageSize> pixelSizeOf(int master)
{
    auto size = winsize {};
    if (::ioctl(master, TIOCGWINSZ, &size) != 0 || size.ws_xpixel == 0 || size.ws_ypixel == 0)
        return std::nullopt;
    return vtpty::ImageSize { vtpty::Width(size.ws_xpixel), vtpty::Height(size.ws_ypixel) };
}

void nudgeRedraw(vtpty::Pty const& pty)
{
    auto const terminal = pty.handover();
    if (!terminal || terminal->masterFd < 0)
        return;
    // The terminal knows its foreground group: no parenthood is needed to find it, or to signal it.
    auto const group = ::tcgetpgrp(terminal->masterFd);
    if (group > 0)
        ::kill(-group, SIGWINCH);
}

Recovery imageFromInventory(std::span<ReceivedMessage> inventory, handover::HandoverMemory& memory)
{
    auto recovery = Recovery {};
    auto structure = std::optional<handover::HandoverImage> {};
    // The structure first: the listeners and sessions below are matched to it.
    for (auto& received: inventory)
        if (received.message && std::holds_alternative<keeper::InventoryBegin>(*received.message))
            structure =
                structureOf(received.fds.empty() ? keeper::UniqueFd {} : std::move(received.fds.front()),
                            memory,
                            recovery.problems);
    recovery.image = structure ? *std::move(structure) : handover::HandoverImage {};
    auto& image = recovery.image;
    // Named by the structure only for the identities it carries: the keeper holds the listeners.
    auto const named = std::exchange(image.listeners, {});
    auto described = std::exchange(image.sessions, {});
    image.lease = {};

    for (auto& received: inventory)
    {
        if (!received.message)
            continue;
        if (auto const* listener = std::get_if<keeper::InventoryListener>(&*received.message))
        {
            if (received.fds.size() != 1)
                continue;
            auto const known = std::ranges::find(named, listener->name, &handover::InheritedListener::name);
            image.listeners.push_back(handover::InheritedListener {
                .name = listener->name,
                .handle = vtpty::CrossingHandle::of(received.fds.front().release()),
                .tlsIdentity = known != named.end() ? known->tlsIdentity : std::nullopt,
            });
            continue;
        }
        auto const* kept = std::get_if<keeper::InventorySession>(&*received.message);
        if (kept == nullptr)
            continue;
        auto descriptors = keptDescriptorsOf(kept->descriptors, received.fds);
        auto& ptyMaster = descriptors[keeper::slotOf(keeper::KeptDescriptor::Master)];
        auto& fastPipeReader = descriptors[keeper::slotOf(keeper::KeptDescriptor::FastPipeReader)];
        if (!ptyMaster.isOpen())
        {
            recovery.problems.push_back(
                RecoveryProblem { .error = RecoveryError::SessionWithoutPty, .session = kept->session });
            recovery.unbound.push_back(keeper::KeeperSessionId { kept->session });
            continue;
        }

        // The session as the structure names it; a session the dead daemon spawned but never named there is a
        // program all the same, and gets a tab and an id of its own.
        auto const found = std::ranges::find_if(described, [&](handover::HandoverSession const& session) {
            return session.device == handover::SessionDevice::KeeperChild
                   && session.keeperSession == kept->session;
        });
        auto session = found != described.end() ? std::move(*found) : handover::HandoverSession {};
        session.device = handover::SessionDevice::KeeperChild;
        session.keeperSession = kept->session;
        session.child.pid = kept->pid;
        session.terminalState = {};
        session.carried = {};
        auto const master = ptyMaster.get();
        session.pty = vtpty::InheritedPty {
            .masterFd = ptyMaster.release(),
            .stdoutFastPipe = fastPipeReader.isOpen()
                                  ? std::optional { std::pair { fastPipeReader.release(), -1 } }
                                  : std::nullopt,
            // The size the program runs at: the terminal is rebuilt at it, so that nothing resizes it.
            .pageSize = pageSizeOf(master).value_or(handover::FallbackPageSize),
            .pixels = pixelSizeOf(master),
            .conpty = std::nullopt,
        };
        recovery.children.insert_or_assign(
            kept->session,
            RecoveredChild {
                .kept = KeptChild { .session = keeper::KeeperSessionId { kept->session },
                                    .pid = kept->pid,
                                    .exited = kept->exited.transform(&keeper::WireExitStatus::toExitStatus),
                                    .settle = owned(std::move(
                                        descriptors[keeper::slotOf(keeper::KeptDescriptor::Settle)])) },
                .gate = kept->placed == keeper::Placement::Parked
                            ? owned(std::move(descriptors[keeper::slotOf(keeper::KeptDescriptor::Gate)]))
                            : vtpty::OwnedFd {},
                .processHandle =
                    owned(std::move(descriptors[keeper::slotOf(keeper::KeptDescriptor::ProcessHandle)])),
            });
        image.sessions.push_back(std::move(session));
    }
    // The structure's sessions the keeper did not keep ended with the dead daemon: their panes go
    // (SessionHost::restoreHandover() closes a leaf whose session is not adopted).
    return recovery;
}

RecoveredSeeder snapshotSeederOf(WorkspaceRestore restore)
{
    auto histories = std::map<ObjectUuid, proto::Delta> {};
    auto const collect = [&](this auto const& self, PaneNode& node) -> void {
        if (node.children.empty() && node.pane.identity && node.pane.history)
            histories.insert_or_assign(node.pane.identity->uuid, *std::move(node.pane.history));
        for (auto& child: node.children)
            self(child);
    };
    for (auto& workspace: restore.snapshot.workspaces)
        for (auto& tab: workspace.tabs)
            collect(tab.root);
    return [histories = std::move(histories), divider = restore.divider](
               handover::HandoverSession const& session, vtbackend::Terminal& terminal) {
        auto const found = histories.find(session.uuid.bytes);
        if (found == histories.end())
            return;
        seedRestoredHistory(terminal, found->second, divider);
        sessionLog()("recovery: session {} seeded from the snapshot", session.id.value);
    };
}

} // namespace vthost::keeperlink
