// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The structure blob: what a daemon restarted after a crash needs to serve as the same instance -- the
/// workspaces with their ids, the object registry, the sessions' identities and settings, the listeners' TLS
/// identities -- and how the daemon keeps its PTY keeper's copy of it current.
///
/// The structure is a handover image without terminal state, carried input or anything that names a
/// descriptor: one encoder (encodeHandover()), shared with the live upgrade, so the two cannot drift. Its own
/// envelope -- magic and version -- refuses a structure a newer release wrote: the keeper holds the blob
/// opaquely, and a daemon that would misread it must not try.

#include <core/async/Task.hpp>
#include <core/net/DeadlineTimer.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <vthost/SessionHost.hpp>
#include <vthost/handover/HandoverImage.hpp>
#include <vthost/handover/HandoverMemory.hpp>
#include <vthost/keeperlink/StructurePushPolicy.hpp>

namespace core::net
{
class EventLoop;
}

namespace vthost::keeperlink
{

class KeeperLink;

/// The structure format this build writes, and the newest it reads. Every older one is read too.
inline constexpr uint32_t StructureVersion = 1;

/// The four bytes a structure starts with.
inline constexpr std::array<uint8_t, 4> StructureMagic { 'C', 'S', 'T', 'R' };

/// Why a structure could not be read.
enum class StructureError : uint8_t
{
    BadMagic = 0, ///< It is no structure.
    NewerVersion, ///< A newer release wrote it.
    Malformed,    ///< Its body does not decode.
};

/// @param error Why a structure could not be read.
/// @return The reason, as a sentence fragment.
[[nodiscard]] std::string_view describe(StructureError error) noexcept;

/// What a structure carries that the host does not know: which run of the daemon it is, and its listeners'
/// identities.
struct StructureExtras
{
    uint64_t daemonInstance = 0;    ///< The run, as every ServerHello states it.
    std::string release;            ///< The release that serves it.
    uint64_t upgradeGeneration = 0; ///< The live upgrades the run completed.
    /// The listeners, by name, each with the TLS identity the daemon made up for it; no handle: the keeper
    /// holds the listeners themselves.
    std::vector<handover::InheritedListener> listeners;
};

/// @param host The host. Loop thread.
/// @param extras What the host does not know.
/// @return The host's structure: a handover image without terminal state, carried input, PTY, child, or
///         lease. Locks no terminal.
[[nodiscard]] handover::HandoverImage captureStructure(SessionHost const& host,
                                                       StructureExtras const& extras);

/// @param structure A structure (captureStructure()).
/// @return It, encoded: the magic, StructureVersion, then the handover encoding of it.
[[nodiscard]] std::vector<uint8_t> encodeStructure(handover::HandoverImage const& structure);

/// @param blob What encodeStructure() wrote, of this version or an older one.
/// @return The structure; or why it cannot be read.
[[nodiscard]] std::expected<handover::HandoverImage, StructureError> decodeStructure(
    std::span<uint8_t const> blob);

/// Where a pushed structure goes. An interface, so that the pusher is tested without a keeper.
class StructureSink
{
  public:
    StructureSink() = default;
    StructureSink(StructureSink const&) = delete;
    StructureSink& operator=(StructureSink const&) = delete;
    StructureSink(StructureSink&&) = delete;
    StructureSink& operator=(StructureSink&&) = delete;
    virtual ~StructureSink() = default;

    /// Takes an encoded structure, which replaces the one before.
    /// @param blob encodeStructure()'s output.
    virtual void push(std::span<uint8_t const> blob) = 0;
};

/// Hands each structure to the keeper: written into a fresh, sealed, anonymous memory object (a memfd sealed
/// against writes and size changes on Linux; an unlinked shared memory object on macOS), whose descriptor
/// rides a Structure message.
class KeeperStructureSink final: public StructureSink
{
  public:
    /// @param link The keeper's link (not owned; outlives this).
    /// @param memory Makes the memory objects (not owned; outlives this).
    KeeperStructureSink(KeeperLink& link, handover::HandoverMemory& memory);

    void push(std::span<uint8_t const> blob) override;

  private:
    KeeperLink& _link;
    handover::HandoverMemory& _memory;
};

/// Pushes the host's structure to a sink as it changes (StructurePushPolicy): a change that mints or retires
/// an id, or changes settings, before any client hears of it; a cosmetic one within StructureDebounce.
/// Attaches itself to the host as its structure observer as it is constructed, pushes the structure as it
/// stands, and detaches as it goes. Loop thread.
///
/// The changes of one turn of the loop are pushed once: a tab with its session and its activation, a tab
/// closed with its panes. A client hears of a change only from its connection's write queue, whose drain
/// the loop resumes (core::net::WriteQueue: an enqueue never writes), in the order it was queued; a flow
/// spawned by the first change, ahead of every drain an announcement queues, pushes them all before any
/// of them is written. Where something is queued on the loop already, which may be a drain, the change is
/// pushed at once instead.
///
/// "Pushed" means queued on the KeeperLink, in order with every other frame for the keeper, and sent as far
/// as the socket takes it: a keeper whose socket buffer is full may receive the Structure frame after a
/// client heard of the change. A crash in that window loses the change to the restarted daemon, as any crash
/// before a push would.
class StructurePusher final: public StructureEvents
{
  public:
    /// @param loop The host's loop, whose clock times the debounce (not owned; outlives this).
    /// @param host The host to push the structure of (not owned; outlives this).
    /// @param sink Where each structure goes (not owned; outlives this).
    /// @param extras What each structure carries that the host does not know.
    /// @param debounce How long a cosmetic change waits.
    StructurePusher(core::net::EventLoop& loop,
                    SessionHost& host,
                    StructureSink& sink,
                    StructureExtras extras,
                    std::chrono::milliseconds debounce = StructureDebounce);
    StructurePusher(StructurePusher const&) = delete;
    StructurePusher& operator=(StructurePusher const&) = delete;
    StructurePusher(StructurePusher&&) = delete;
    StructurePusher& operator=(StructurePusher&&) = delete;
    /// Detaches from the host; a push not yet made, debounced or coalesced, is dropped.
    ~StructurePusher() override;

    /// Notes @p event: pushes now, or within the debounce, as its timing says.
    /// @param event The change.
    void note(StructureEvent event);

    // vtworkspace::ModelEvents
    void tabAdded(vtworkspace::WorkspaceId, vtworkspace::TabId, int) override
    {
        note(StructureEvent::TabAdded);
    }
    void tabClosed(vtworkspace::WorkspaceId, vtworkspace::TabId, int) override
    {
        note(StructureEvent::TabClosed);
    }
    void tabMoved(vtworkspace::WorkspaceId, vtworkspace::TabId, int, int) override
    {
        note(StructureEvent::TabMoved);
    }
    void tabMovedToWorkspace(
        vtworkspace::WorkspaceId, vtworkspace::TabId, int, vtworkspace::WorkspaceId, int) override
    {
        note(StructureEvent::TabMovedToWorkspace);
    }
    void activeTabChanged(vtworkspace::WorkspaceId, vtworkspace::TabId, int) override
    {
        note(StructureEvent::ActiveTabChanged);
    }
    void paneSplit(vtworkspace::TabId, vtworkspace::PaneId, vtworkspace::PaneId) override
    {
        note(StructureEvent::PaneSplit);
    }
    void paneClosed(vtworkspace::TabId, vtworkspace::PaneId, vtworkspace::PaneId) override
    {
        note(StructureEvent::PaneClosed);
    }
    void activePaneChanged(vtworkspace::TabId, vtworkspace::PaneId) override
    {
        note(StructureEvent::ActivePaneChanged);
    }
    void paneRatioChanged(vtworkspace::TabId, vtworkspace::PaneId, double) override
    {
        note(StructureEvent::PaneRatioChanged);
    }
    void paneOrientationChanged(vtworkspace::TabId, vtworkspace::PaneId, vtworkspace::SplitState) override
    {
        note(StructureEvent::PaneOrientationChanged);
    }
    void paneSwapped(vtworkspace::TabId, vtworkspace::PaneId, vtworkspace::PaneId) override
    {
        note(StructureEvent::PaneSwapped);
    }
    void paneSessionReplaced(vtworkspace::TabId, vtworkspace::PaneId) override
    {
        note(StructureEvent::PaneSessionReplaced);
    }
    void paneZoomChanged(vtworkspace::TabId, std::optional<vtworkspace::PaneId>) override
    {
        note(StructureEvent::PaneZoomChanged);
    }
    void paneTreeRestructured(vtworkspace::TabId) override { note(StructureEvent::PaneTreeRestructured); }
    void tabTitleChanged(vtworkspace::TabId) override { note(StructureEvent::TabTitleChanged); }
    void tabColorChanged(vtworkspace::TabId) override { note(StructureEvent::TabColorChanged); }

    // StructureEvents
    void sessionCreated(vtworkspace::SessionId) override { note(StructureEvent::SessionCreated); }
    void sessionClosed(vtworkspace::SessionId) override { note(StructureEvent::SessionClosed); }
    void settingsChanged() override { note(StructureEvent::SettingsChanged); }
    void colorPreferenceChanged(vtworkspace::SessionId) override
    {
        note(StructureEvent::ColorPreferenceChanged);
    }
    void interruptionChanged(vtworkspace::SessionId) override { note(StructureEvent::InterruptionChanged); }

  private:
    /// Whether a coalesced push is owed.
    enum class Coalesced : uint8_t
    {
        None = 0, ///< Nothing is owed: the sink holds every change pushed before announcement.
        Owed,     ///< A flow the loop runs before any announcement pushes the changes of this turn.
    };

    /// Pushes a change before any client hears of it: within this turn, once, or at once (see the class).
    void pushBeforeAnnouncement();
    /// The flow pushBeforeAnnouncement() spawns: pushes what is owed, unless the pusher is gone.
    [[nodiscard]] static core::async::Task<void> pushOwed(std::weak_ptr<StructurePusher*> pusher);

    /// Captures, encodes and hands the structure to the sink; settles every push pending.
    void push();
    static void pushDebounced(void* pusher);

    core::net::EventLoop& _loop;
    SessionHost& _host;
    StructureSink& _sink;
    StructureExtras _extras;
    std::chrono::milliseconds _debounce;
    std::optional<core::net::DeadlineTimer> _pending; ///< A debounced push, armed.
    Coalesced _coalesced = Coalesced::None;
    /// What a flow spawned for a coalesced push finds this pusher by; expires with it.
    std::shared_ptr<StructurePusher*> _self = std::make_shared<StructurePusher*>(this);
};

} // namespace vthost::keeperlink
