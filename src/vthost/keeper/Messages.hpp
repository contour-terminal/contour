// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtpty/ChildProcess.hpp>

#include <core/codec/Fields.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

/// The messages of the channel between the PTY keeper and the daemon it keeps.
///
/// Version 1 of the channel. A message is a struct that lists its fields once, in `visitFields`, under
/// stable ids (@see core::codec): an id is never reused or renumbered, a field a newer writer added is
/// skipped by an older reader, and a field an older writer lacks stays at its default. The messages are the
/// alternatives of #Message, in the table's order, appended to and never reordered: a message crosses as its
/// index + 1.
///
/// A receiver answers every message it cannot read -- an unknown tag, an enum value beyond its wire table,
/// more descriptors than the message may carry -- with Unsupported{tag}, closes what came with it, and keeps
/// the link; only a frame cut short (CodecError::Truncated) breaks it. A v1 keeper serves every newer daemon
/// a live upgrade or a crash restart runs, so a sender must not count on that answer to be understood: it
/// sends a message, an enum value (a wire table's appended entry) or a field that carries descriptors that a
/// later version added only once the version both ends negotiated in their Hellos is that version or newer
/// (KeeperLink::channelVersion() on the daemon's side, the version the keeper recorded from the daemon's
/// Hello on the keeper's).
namespace vthost::keeper
{

/// The channel version this build speaks (Hello::version).
inline constexpr uint32_t ChannelVersion = 1;

/// The keeper's name for one session; minted by the keeper and never reused.
enum class KeeperSessionId : uint64_t
{
};

/// The most descriptors one frame carries; well under Linux's 253 per `sendmsg`.
inline constexpr std::size_t MaxFdsPerFrame = 16;

/// What a daemon process starts out from.
enum class LaunchContinuity : uint8_t
{
    Fresh = 0,  ///< A first start: nothing to restore.
    Recovered,  ///< A restart after a crash: restore from the checkpoints.
    HandedOver, ///< The successor of a live upgrade.
};

/// Whether the keeper restarts a daemon that stops answering heartbeats.
enum class HungRestart : uint8_t
{
    Off = 0,
    On,
};

/// Whether a session's pane is placed in the workspace.
enum class Placement : uint8_t
{
    Placed = 0,
    Parked,
};

/// Why the keeper could not spawn a child.
enum class SpawnError : uint8_t
{
    Unknown = 0,
    ForkFailed,
    ExecutableUnusable,
    TooManyDescriptors,
    ShuttingDown, ///< The daemon is ending (announced Shutdown, exited, killed); the keeper takes on nothing
                  ///< new.
};

/// What a descriptor the keeper holds for a session is. A session holds each at most once.
enum class KeptDescriptor : uint8_t
{
    Master = 0,     ///< The PTY master.
    FastPipeReader, ///< The read end of the stdout fast pipe, so its writer never sees EPIPE.
    Gate,           ///< The placement gate, until the session is Placed.
    Settle,         ///< The settle descriptor.
    ProcessHandle,  ///< The child's pidfd; minted by the keeper, never sent to it.
};

/// How a child ended, as the channel carries it.
enum class ExitKind : uint8_t
{
    Normal = 0, ///< It exited; the value is its exit code.
    Signal,     ///< A signal ended it; the value is the signal number.
};

/// How a Spawn names the executable the keeper runs.
enum class ExecutableSource : uint8_t
{
    Path = 0,   ///< By Spawn::executablePath, with execve(): what a platform without execveat() runs (macOS).
    Descriptor, ///< By a descriptor the Spawn carries last, with execveat(AT_EMPTY_PATH) (Linux): the image
                ///< the daemon runs, even after a package upgrade replaced the file its path names.
};

/// A signal the daemon asks the keeper to send a session's child.
enum class ChildSignal : uint8_t
{
    Terminate = 0, ///< SIGTERM, which the child may catch or ignore.
    Kill,          ///< SIGKILL, which no child can refuse.
};

} // namespace vthost::keeper

namespace core::codec
{

// {{{ wire tables -- appended to, never reordered; an appended value goes only to a peer of its version
template <>
struct EnumWire<vthost::keeper::LaunchContinuity>
{
    static constexpr auto Values = std::array {
        vthost::keeper::LaunchContinuity::Fresh,
        vthost::keeper::LaunchContinuity::Recovered,
        vthost::keeper::LaunchContinuity::HandedOver,
    };
};

template <>
struct EnumWire<vthost::keeper::HungRestart>
{
    static constexpr auto Values = std::array {
        vthost::keeper::HungRestart::Off,
        vthost::keeper::HungRestart::On,
    };
};

template <>
struct EnumWire<vthost::keeper::Placement>
{
    static constexpr auto Values = std::array {
        vthost::keeper::Placement::Placed,
        vthost::keeper::Placement::Parked,
    };
};

template <>
struct EnumWire<vthost::keeper::SpawnError>
{
    static constexpr auto Values = std::array {
        vthost::keeper::SpawnError::Unknown,
        vthost::keeper::SpawnError::ForkFailed,
        vthost::keeper::SpawnError::ExecutableUnusable,
        vthost::keeper::SpawnError::TooManyDescriptors,
        vthost::keeper::SpawnError::ShuttingDown,
    };
};

template <>
struct EnumWire<vthost::keeper::KeptDescriptor>
{
    static constexpr auto Values = std::array {
        vthost::keeper::KeptDescriptor::Master,        vthost::keeper::KeptDescriptor::FastPipeReader,
        vthost::keeper::KeptDescriptor::Gate,          vthost::keeper::KeptDescriptor::Settle,
        vthost::keeper::KeptDescriptor::ProcessHandle,
    };
};

template <>
struct EnumWire<vthost::keeper::ExitKind>
{
    static constexpr auto Values = std::array {
        vthost::keeper::ExitKind::Normal,
        vthost::keeper::ExitKind::Signal,
    };
};

template <>
struct EnumWire<vthost::keeper::ExecutableSource>
{
    static constexpr auto Values = std::array {
        vthost::keeper::ExecutableSource::Path,
        vthost::keeper::ExecutableSource::Descriptor,
    };
};

template <>
struct EnumWire<vthost::keeper::ChildSignal>
{
    static constexpr auto Values = std::array {
        vthost::keeper::ChildSignal::Terminate,
        vthost::keeper::ChildSignal::Kill,
    };
};
// }}}

} // namespace core::codec

namespace vthost::keeper
{

/// How a child ended, as it crosses the channel. Converts to and from vtpty::ExitStatus.
struct WireExitStatus
{
    ExitKind kind = ExitKind::Normal;
    int32_t value = 0; ///< The exit code, or the signal number.

    /// Lists the fields.
    void visitFields(this auto& self, auto& visitor)
    {
        visitor(1, self.kind);
        visitor(2, self.value);
    }

    [[nodiscard]] bool operator==(WireExitStatus const&) const = default;

    /// @param status How a child ended.
    /// @return The channel's form of @p status.
    [[nodiscard]] static WireExitStatus from(vtpty::ExitStatus const& status) noexcept
    {
        if (auto const* normal = std::get_if<vtpty::NormalExit>(&status))
            return { .kind = ExitKind::Normal, .value = static_cast<int32_t>(normal->exitCode) };
        return { .kind = ExitKind::Signal,
                 .value = static_cast<int32_t>(std::get<vtpty::SignalExit>(status).signum) };
    }

    /// @return The status this form stands for.
    [[nodiscard]] vtpty::ExitStatus toExitStatus() const noexcept
    {
        if (kind == ExitKind::Signal)
            return vtpty::SignalExit { .signum = static_cast<int>(value) };
        return vtpty::NormalExit { .exitCode = static_cast<int>(value) };
    }
};

// clang-format off
/// Both sides' opening words. Sent first by each; both speak min(own, peer). Never refused. A daemon process
/// says it once per image it runs: a second Hello on the same channel is the new image of a live upgrade
/// (an exec in place), which the keeper answers with a Hello and a Launch{HandedOver} of its own.
struct Hello
{
    /// How many descriptors this message carries at most.
    static constexpr std::size_t MaxDescriptors = 0;
    uint32_t version = ChannelVersion;
    uint64_t capabilities = 0;
    void visitFields(this auto& self, auto& visitor) { visitor(1, self.version); visitor(2, self.capabilities); }
    [[nodiscard]] bool operator==(Hello const&) const = default;
};

/// The answer to a message the receiver cannot read: its tag is unknown, or an enum value or the descriptors
/// it carries are beyond what the receiver's version knows. The link stays up.
struct Unsupported
{
    /// How many descriptors this message carries at most.
    static constexpr std::size_t MaxDescriptors = 0;
    uint64_t tag = 0;
    void visitFields(this auto& self, auto& visitor) { visitor(1, self.tag); }
    [[nodiscard]] bool operator==(Unsupported const&) const = default;
};

/// Keeper to daemon, once: how this daemon process starts. The options record is opaque to the keeper.
struct Launch
{
    /// How many descriptors this message carries at most.
    static constexpr std::size_t MaxDescriptors = 0;
    LaunchContinuity continuity = LaunchContinuity::Fresh;
    std::vector<uint8_t> options;
    void visitFields(this auto& self, auto& visitor) { visitor(1, self.continuity); visitor(2, self.options); }
    [[nodiscard]] bool operator==(Launch const&) const = default;
};

/// Daemon to keeper: a listening socket the keeper keeps a duplicate of. Carries one descriptor. A daemon
/// deposits each of its listeners once; a listener deposited again under the same name (by the new image of
/// a live upgrade) replaces the one held.
struct Listener
{
    /// How many descriptors this message carries at most.
    static constexpr std::size_t MaxDescriptors = 1;
    std::string name;
    std::optional<std::vector<uint8_t>> tlsIdentity;
    std::string path; ///< The socket file of a Unix listener, which a keeper that gives up unlinks; empty for TCP.
    void visitFields(this auto& self, auto& visitor) { visitor(1, self.name); visitor(2, self.tlsIdentity); visitor(3, self.path); }
    [[nodiscard]] bool operator==(Listener const&) const = default;
};

/// Daemon to keeper: the daemon serves, and under which hung-daemon policy.
struct Serving
{
    /// How many descriptors this message carries at most.
    static constexpr std::size_t MaxDescriptors = 0;
    HungRestart hungRestart = HungRestart::On;
    uint32_t heartbeatTimeoutMs = 30'000;
    void visitFields(this auto& self, auto& visitor) { visitor(1, self.hungRestart); visitor(2, self.heartbeatTimeoutMs); }
    [[nodiscard]] bool operator==(Serving const&) const = default;
};

/// Daemon to keeper: fork a child. The n-th received descriptor becomes child descriptor childFds[n]; the
/// kept.size() descriptors after those are not the child's but the keeper's to hold, kept[n] naming each. For
/// ExecutableSource::Descriptor one more follows them, last: the executable, which the keeper runs by it.
struct Spawn
{
    /// How many descriptors this message carries at most.
    static constexpr std::size_t MaxDescriptors = 12;
    uint64_t request = 0;
    std::vector<std::string> argv;
    std::string executablePath;
    std::vector<uint32_t> childFds;
    std::vector<KeptDescriptor> kept;
    ExecutableSource executable = ExecutableSource::Path;
    void visitFields(this auto& self, auto& visitor)
    {
        visitor(1, self.request);
        visitor(2, self.argv);
        visitor(3, self.executablePath);
        visitor(4, self.childFds);
        visitor(5, self.kept);
        visitor(6, self.executable);
    }
    [[nodiscard]] bool operator==(Spawn const&) const = default;
};

/// Keeper to daemon: the child runs. Carries its pidfd on Linux.
struct Spawned
{
    /// How many descriptors this message carries at most.
    static constexpr std::size_t MaxDescriptors = 1;
    uint64_t request = 0;
    uint64_t session = 0; ///< A KeeperSessionId's value.
    int64_t pid = 0;
    void visitFields(this auto& self, auto& visitor) { visitor(1, self.request); visitor(2, self.session); visitor(3, self.pid); }
    [[nodiscard]] bool operator==(Spawned const&) const = default;
};

/// Keeper to daemon: the child could not be started.
struct SpawnFailed
{
    /// How many descriptors this message carries at most.
    static constexpr std::size_t MaxDescriptors = 0;
    uint64_t request = 0;
    SpawnError error = SpawnError::Unknown;
    void visitFields(this auto& self, auto& visitor) { visitor(1, self.request); visitor(2, self.error); }
    [[nodiscard]] bool operator==(SpawnFailed const&) const = default;
};

/// Daemon to keeper: the session's pane is placed.
struct Placed
{
    /// How many descriptors this message carries at most.
    static constexpr std::size_t MaxDescriptors = 0;
    uint64_t session = 0; ///< A KeeperSessionId's value.
    void visitFields(this auto& self, auto& visitor) { visitor(1, self.session); }
    [[nodiscard]] bool operator==(Placed const&) const = default;
};

/// Daemon to keeper: let the session go, after a grace period.
struct Release
{
    /// How many descriptors this message carries at most.
    static constexpr std::size_t MaxDescriptors = 0;
    uint64_t session = 0; ///< A KeeperSessionId's value.
    uint32_t graceMs = 0;
    void visitFields(this auto& self, auto& visitor) { visitor(1, self.session); visitor(2, self.graceMs); }
    [[nodiscard]] bool operator==(Release const&) const = default;
};

/// Keeper to daemon: a session's child ended.
struct Exited
{
    /// How many descriptors this message carries at most.
    static constexpr std::size_t MaxDescriptors = 0;
    uint64_t session = 0; ///< A KeeperSessionId's value.
    WireExitStatus status;
    void visitFields(this auto& self, auto& visitor) { visitor(1, self.session); visitor(2, self.status); }
    [[nodiscard]] bool operator==(Exited const&) const = default;
};

/// Daemon to keeper: the structure blob, as the one descriptor it carries.
struct Structure
{
    /// How many descriptors this message carries at most.
    static constexpr std::size_t MaxDescriptors = 1;
    void visitFields(this auto& /*self*/, auto& /*visitor*/) {}
    [[nodiscard]] bool operator==(Structure const&) const = default;
};

/// Daemon to keeper: still alive.
struct Heartbeat
{
    /// How many descriptors this message carries at most.
    static constexpr std::size_t MaxDescriptors = 0;
    void visitFields(this auto& /*self*/, auto& /*visitor*/) {}
    [[nodiscard]] bool operator==(Heartbeat const&) const = default;
};

/// Daemon to keeper: the daemon has adopted the inventory.
struct Adopt
{
    /// How many descriptors this message carries at most.
    static constexpr std::size_t MaxDescriptors = 0;
    void visitFields(this auto& /*self*/, auto& /*visitor*/) {}
    [[nodiscard]] bool operator==(Adopt const&) const = default;
};

/// Daemon to keeper: the daemon shuts down, for a cause of its own table.
struct Shutdown
{
    /// How many descriptors this message carries at most.
    static constexpr std::size_t MaxDescriptors = 0;
    uint8_t cause = 0;
    void visitFields(this auto& self, auto& visitor) { visitor(1, self.cause); }
    [[nodiscard]] bool operator==(Shutdown const&) const = default;
};

/// Daemon to keeper: the daemon is about to exec this image.
struct Exec
{
    /// How many descriptors this message carries at most.
    static constexpr std::size_t MaxDescriptors = 0;
    std::string executablePath;
    void visitFields(this auto& self, auto& visitor) { visitor(1, self.executablePath); }
    [[nodiscard]] bool operator==(Exec const&) const = default;
};

/// Daemon to keeper: send a session's child a signal. Ignored for a child that was reaped already, whose pid
/// may name another process by now.
struct Signal
{
    /// How many descriptors this message carries at most.
    static constexpr std::size_t MaxDescriptors = 0;
    uint64_t session = 0; ///< A KeeperSessionId's value.
    ChildSignal signal = ChildSignal::Terminate;
    void visitFields(this auto& self, auto& visitor) { visitor(1, self.session); visitor(2, self.signal); }
    [[nodiscard]] bool operator==(Signal const&) const = default;
};

/// Keeper to daemon: opens the inventory. Carries the structure descriptor, if there is one.
struct InventoryBegin
{
    /// How many descriptors this message carries at most.
    static constexpr std::size_t MaxDescriptors = 1;
    std::string daemonImagePath;
    uint32_t sessionCount = 0;
    uint32_t listenerCount = 0;
    void visitFields(this auto& self, auto& visitor)
    {
        visitor(1, self.daemonImagePath);
        visitor(2, self.sessionCount);
        visitor(3, self.listenerCount);
    }
    [[nodiscard]] bool operator==(InventoryBegin const&) const = default;
};

/// Keeper to daemon: one kept listener, as Listener. Carries one descriptor.
struct InventoryListener
{
    /// How many descriptors this message carries at most.
    static constexpr std::size_t MaxDescriptors = 1;
    std::string name;
    std::optional<std::vector<uint8_t>> tlsIdentity;
    std::string path; ///< As Listener::path.
    void visitFields(this auto& self, auto& visitor) { visitor(1, self.name); visitor(2, self.tlsIdentity); visitor(3, self.path); }
    [[nodiscard]] bool operator==(InventoryListener const&) const = default;
};

/// Keeper to daemon: one kept session. Carries master, fast-pipe reader, gate, settle and pidfd, as present;
/// descriptors[n] names the n-th.
struct InventorySession
{
    /// How many descriptors this message carries at most.
    static constexpr std::size_t MaxDescriptors = 5;
    uint64_t session = 0; ///< A KeeperSessionId's value.
    int64_t pid = 0;
    std::optional<WireExitStatus> exited;
    Placement placed = Placement::Placed;
    std::vector<KeptDescriptor> descriptors;
    void visitFields(this auto& self, auto& visitor)
    {
        visitor(1, self.session);
        visitor(2, self.pid);
        visitor(3, self.exited);
        visitor(4, self.placed);
        visitor(5, self.descriptors);
    }
    [[nodiscard]] bool operator==(InventorySession const&) const = default;
};

/// Keeper to daemon: closes the inventory.
struct InventoryEnd
{
    /// How many descriptors this message carries at most.
    static constexpr std::size_t MaxDescriptors = 0;
    void visitFields(this auto& /*self*/, auto& /*visitor*/) {}
    [[nodiscard]] bool operator==(InventoryEnd const&) const = default;
};
// clang-format on

/// Every message of the channel, in the order of the wire table: a message's tag is its index + 1.
/// Appended to, never reordered.
using Message = std::variant<Hello,
                             Unsupported,
                             Launch,
                             Listener,
                             Serving,
                             Spawn,
                             Spawned,
                             SpawnFailed,
                             Placed,
                             Release,
                             Exited,
                             Structure,
                             Heartbeat,
                             Adopt,
                             Shutdown,
                             Exec,
                             InventoryBegin,
                             InventoryListener,
                             InventorySession,
                             InventoryEnd,
                             Signal>;

namespace detail
{
    template <typename... Alternatives>
    [[nodiscard]] consteval auto descriptorBudgets(std::variant<Alternatives...> const*)
    {
        return std::array<std::size_t, sizeof...(Alternatives)> { Alternatives::MaxDescriptors... };
    }
} // namespace detail

/// How many descriptors each message carries at most, by the message's index in #Message; derived from
/// the messages' own `MaxDescriptors`, so a message without one does not compile.
inline constexpr auto MaxDescriptorsOfMessage =
    detail::descriptorBudgets(static_cast<Message const*>(nullptr));

} // namespace vthost::keeper
