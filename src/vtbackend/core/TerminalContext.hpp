// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/ContextId.hpp>

#include <crispy/Flags.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace vtbackend
{

// {{{ vocabulary

/// Whether a context type is entered by crossing onto a different host, kernel or filesystem.
///
/// Not decoration: it is what lets a frontend decide that a `cwd=` inherited from above a boundary
/// does not name a path on THIS machine. @see ContextStack::localityOf.
enum class HostBoundary : uint8_t
{
    Same = 0,
    Crossed = 1,
};

/// The single source for every `type=` the protocol names: one row each, giving the enumerator, the
/// wire spelling, and whether entering it crosses a host boundary.
///
/// The enumeration, the two name lookups and @ref contextTypeCrossesHost are all generated from this
/// table, so **adding a type is adding one row here**. Mirrors VTBACKEND_LINE_FLAGS in LineFlags.hpp,
/// which exists for the same reason.
#define VTBACKEND_CONTEXT_TYPES(_)      \
    _(Service, "service", Same)         \
    _(Session, "session", Same)         \
    _(Shell, "shell", Same)             \
    _(Command, "command", Same)         \
    _(Vm, "vm", Crossed)                \
    _(Container, "container", Crossed)  \
    _(Elevate, "elevate", Same)         \
    _(ChangePrivileges, "chpriv", Same) \
    _(Subcontext, "subcontext", Same)   \
    _(Remote, "remote", Crossed)        \
    _(Boot, "boot", Crossed)            \
    _(App, "app", Same)

/// What kind of thing took control of the terminal.
/// @see VTBACKEND_CONTEXT_TYPES for the table this is generated from.
enum class ContextType : uint8_t
{
    None = 0, ///< No `type=` was given, or one this terminal does not know.

#define VTBACKEND_CONTEXT_TYPE_ENUMERATOR(Name, Spelling, Boundary) Name,
    VTBACKEND_CONTEXT_TYPES(VTBACKEND_CONTEXT_TYPE_ENUMERATOR)
#undef VTBACKEND_CONTEXT_TYPE_ENUMERATOR
};

/// Every ContextType, in declaration order, excluding None.
///
/// Anything that must visit every type reads this rather than writing the list out again -- the
/// colour-scheme tint map and the breadcrumb's segment table both do.
inline constexpr auto ContextTypeList = std::array {
#define VTBACKEND_CONTEXT_TYPE_ROW(Name, Spelling, Boundary) ContextType::Name,
    VTBACKEND_CONTEXT_TYPES(VTBACKEND_CONTEXT_TYPE_ROW)
#undef VTBACKEND_CONTEXT_TYPE_ROW
};

/// One past the largest ContextType value, so an array indexed by type is sized from the table.
inline constexpr size_t ContextTypeCount = ContextTypeList.size() + 1;

/// The wire spelling of @p type, or an empty view for @ref ContextType::None.
[[nodiscard]] constexpr std::string_view contextTypeName(ContextType type) noexcept
{
    switch (type)
    {
#define VTBACKEND_CONTEXT_TYPE_NAME(Name, Spelling, Boundary) \
    case ContextType::Name: return Spelling;
        VTBACKEND_CONTEXT_TYPES(VTBACKEND_CONTEXT_TYPE_NAME)
#undef VTBACKEND_CONTEXT_TYPE_NAME
        case ContextType::None: break;
    }
    return {};
}

/// The type @p name spells, or @ref ContextType::None when it spells none.
[[nodiscard]] constexpr ContextType contextTypeFrom(std::string_view name) noexcept
{
#define VTBACKEND_CONTEXT_TYPE_FROM(Name, Spelling, Boundary) \
    if (name == (Spelling))                                   \
        return ContextType::Name;
    VTBACKEND_CONTEXT_TYPES(VTBACKEND_CONTEXT_TYPE_FROM)
#undef VTBACKEND_CONTEXT_TYPE_FROM
    return ContextType::None;
}

/// Whether entering @p type means leaving this machine's process tree and filesystem.
[[nodiscard]] constexpr HostBoundary contextTypeCrossesHost(ContextType type) noexcept
{
    switch (type)
    {
#define VTBACKEND_CONTEXT_TYPE_BOUNDARY(Name, Spelling, Boundary) \
    case ContextType::Name: return HostBoundary::Boundary;
        VTBACKEND_CONTEXT_TYPES(VTBACKEND_CONTEXT_TYPE_BOUNDARY)
#undef VTBACKEND_CONTEXT_TYPE_BOUNDARY
        case ContextType::None: break;
    }
    return HostBoundary::Same;
}

/// Whether @p type names a PRIVILEGE or MACHINE boundary -- a change in who you are, or in which kernel
/// you are talking to.
///
/// These five are what a `boundaries`-scoped tint honours and what the breadcrumb shows at its default
/// verbosity: their absence from the screen is what is genuinely costly to a reader. `shell` and
/// `command` are not news; `session`, `boot`, `service`, `app` and `subcontext` are structure, not
/// identity.
[[nodiscard]] constexpr bool isBoundaryContext(ContextType type) noexcept
{
    switch (type)
    {
        case ContextType::Elevate:
        case ContextType::ChangePrivileges:
        case ContextType::Container:
        case ContextType::Vm:
        case ContextType::Remote: return true;
        default: return false;
    }
}

/// How a context ended, as `end=`'s `exit=` reports it.
enum class ContextExit : uint8_t
{
    Unknown = 0,  ///< No `exit=` was given, or the context was terminated implicitly.
    Success = 1,  ///< `exit=success`.
    Failure = 2,  ///< `exit=failure`; `status=` names the code.
    Crash = 3,    ///< `exit=crash`; `signal=` names the signal.
    Interrupt = 4 ///< `exit=interrupt`.
};

/// The wire spelling of @p exit, or an empty view for @ref ContextExit::Unknown.
[[nodiscard]] constexpr std::string_view contextExitName(ContextExit exit) noexcept
{
    switch (exit)
    {
        case ContextExit::Success: return "success";
        case ContextExit::Failure: return "failure";
        case ContextExit::Crash: return "crash";
        case ContextExit::Interrupt: return "interrupt";
        case ContextExit::Unknown: break;
    }
    return {};
}

/// The exit kind @p name spells, or @ref ContextExit::Unknown when it spells none.
[[nodiscard]] constexpr ContextExit contextExitFrom(std::string_view name) noexcept
{
    if (name == "success")
        return ContextExit::Success;
    if (name == "failure")
        return ContextExit::Failure;
    if (name == "crash")
        return ContextExit::Crash;
    if (name == "interrupt")
        return ContextExit::Interrupt;
    return ContextExit::Unknown;
}

/// Every signal `signal=` may name, with the number Linux gives it.
///
/// The numbers are Linux's, hard-coded rather than taken from <signal.h>, and that is deliberate: a
/// context may describe a process on ANOTHER machine (`type=remote`, `type=vm`), so `SIGSEGV` has to
/// mean 11 regardless of the platform Contour itself was built for. Including the host's header would
/// make the same sequence decode differently on Windows.
#define VTBACKEND_CONTEXT_SIGNALS(_) \
    _(Hup, "SIGHUP", 1)              \
    _(Int, "SIGINT", 2)              \
    _(Quit, "SIGQUIT", 3)            \
    _(Ill, "SIGILL", 4)              \
    _(Trap, "SIGTRAP", 5)            \
    _(Abrt, "SIGABRT", 6)            \
    _(Bus, "SIGBUS", 7)              \
    _(Fpe, "SIGFPE", 8)              \
    _(Kill, "SIGKILL", 9)            \
    _(Usr1, "SIGUSR1", 10)           \
    _(Segv, "SIGSEGV", 11)           \
    _(Usr2, "SIGUSR2", 12)           \
    _(Pipe, "SIGPIPE", 13)           \
    _(Alrm, "SIGALRM", 14)           \
    _(Term, "SIGTERM", 15)           \
    _(Xcpu, "SIGXCPU", 24)           \
    _(Xfsz, "SIGXFSZ", 25)           \
    _(Sys, "SIGSYS", 31)

/// The signal a crashed context died of. @see VTBACKEND_CONTEXT_SIGNALS.
enum class ContextSignal : uint8_t
{
    None = 0, ///< No `signal=` was given, or its symbolic name is not one we know.

#define VTBACKEND_CONTEXT_SIGNAL_ENUMERATOR(Name, Spelling, Number) Name = (Number),
    VTBACKEND_CONTEXT_SIGNALS(VTBACKEND_CONTEXT_SIGNAL_ENUMERATOR)
#undef VTBACKEND_CONTEXT_SIGNAL_ENUMERATOR
};

/// The symbolic name of @p signal, or an empty view for @ref ContextSignal::None.
[[nodiscard]] constexpr std::string_view contextSignalName(ContextSignal signal) noexcept
{
    switch (signal)
    {
#define VTBACKEND_CONTEXT_SIGNAL_NAME(Name, Spelling, Number) \
    case ContextSignal::Name: return Spelling;
        VTBACKEND_CONTEXT_SIGNALS(VTBACKEND_CONTEXT_SIGNAL_NAME)
#undef VTBACKEND_CONTEXT_SIGNAL_NAME
        case ContextSignal::None: break;
    }
    return {};
}

/// The signal @p name spells, or @ref ContextSignal::None when it spells none.
[[nodiscard]] constexpr ContextSignal contextSignalFrom(std::string_view name) noexcept
{
#define VTBACKEND_CONTEXT_SIGNAL_FROM(Name, Spelling, Number) \
    if (name == (Spelling))                                   \
        return ContextSignal::Name;
    VTBACKEND_CONTEXT_SIGNALS(VTBACKEND_CONTEXT_SIGNAL_FROM)
#undef VTBACKEND_CONTEXT_SIGNAL_FROM
    return ContextSignal::None;
}

/// Whether @p value is a signal number this terminal knows, so a byte off the wire can be validated
/// rather than cast.
[[nodiscard]] constexpr bool isKnownContextSignal(uint8_t value) noexcept
{
    if (value == 0)
        return true;
#define VTBACKEND_CONTEXT_SIGNAL_KNOWN(Name, Spelling, Number) \
    if (value == (Number))                                     \
        return true;
    VTBACKEND_CONTEXT_SIGNALS(VTBACKEND_CONTEXT_SIGNAL_KNOWN)
#undef VTBACKEND_CONTEXT_SIGNAL_KNOWN
    return false;
}

/// The single source for every STARTFIELD: one row each, giving the enumerator, its bit, the wire
/// spelling, the member of @ref TerminalContext it fills, the validator it needs and the decoded length
/// the ABNF allows.
///
/// The bit enumeration, the parser's key dispatch, the flush-to-defaults pass a re-`start=` performs
/// and the length validation are all generated from this table, so **adding a field is adding one row
/// here**. Without it, a sixteenth field would be four edits in three files.
///
/// `Kind` selects the validator: Text is `1*255SAFE`, OptionalText is `*255SAFE` (may be empty), Id128
/// is `32*36(HEX / "-")`, Uint64 is `1*20DECIMAL`, Enum is a fixed vocabulary.
#define VTBACKEND_CONTEXT_FIELDS(_)                                                  \
    /*   Name             bit  spelling       member            kind          max */ \
    _(Type, 0, "type", type, Enum, 0)                                                \
    _(User, 1, "user", user, Text, 255)                                              \
    _(Hostname, 2, "hostname", hostname, Text, 255)                                  \
    _(MachineId, 3, "machineid", machineId, Id128, 36)                               \
    _(BootId, 4, "bootid", bootId, Id128, 36)                                        \
    _(Pid, 5, "pid", pid, Uint64, 20)                                                \
    _(PidFdId, 6, "pidfdid", pidFdId, Uint64, 20)                                    \
    _(Comm, 7, "comm", comm, Text, 255)                                              \
    _(WorkingDirectory, 8, "cwd", workingDirectory, Text, 255)                       \
    _(CommandLine, 9, "cmdline", commandLine, OptionalText, 255)                     \
    _(Vm, 10, "vm", vm, Text, 255)                                                   \
    _(Container, 11, "container", container, Text, 255)                              \
    _(TargetUser, 12, "targetuser", targetUser, Text, 255)                           \
    _(TargetHost, 13, "targethost", targetHost, Text, 255)                           \
    _(SessionId, 14, "sessionid", sessionId, Text, 255)

/// Which STARTFIELDs a `start=` payload carried. @see VTBACKEND_CONTEXT_FIELDS.
enum class ContextField : uint16_t
{
    None = 0,

#define VTBACKEND_CONTEXT_FIELD_ENUMERATOR(Name, Bit, Spelling, Member, Kind, Max) Name = (1U << (Bit)),
    VTBACKEND_CONTEXT_FIELDS(VTBACKEND_CONTEXT_FIELD_ENUMERATOR)
#undef VTBACKEND_CONTEXT_FIELD_ENUMERATOR
};

using ContextFields = crispy::Flags<ContextField>;

/// Every ContextField, in declaration order, excluding None.
inline constexpr auto ContextFieldList = std::array {
#define VTBACKEND_CONTEXT_FIELD_ROW(Name, Bit, Spelling, Member, Kind, Max) ContextField::Name,
    VTBACKEND_CONTEXT_FIELDS(VTBACKEND_CONTEXT_FIELD_ROW)
#undef VTBACKEND_CONTEXT_FIELD_ROW
};

/// The bits VTBACKEND_CONTEXT_FIELDS actually assigns, so a peer-supplied mask can be validated rather
/// than trusted.
inline constexpr uint16_t ContextFieldMask = [] {
    auto bits = uint16_t {};
#define VTBACKEND_CONTEXT_FIELD_BIT(Name, Bit, Spelling, Member, Kind, Max) \
    bits = static_cast<uint16_t>(bits | (1U << (Bit)));
    VTBACKEND_CONTEXT_FIELDS(VTBACKEND_CONTEXT_FIELD_BIT)
#undef VTBACKEND_CONTEXT_FIELD_BIT
    return bits;
}();

/// How a context ended.
struct ContextOutcome
{
    ContextExit exit = ContextExit::Unknown;
    ContextSignal signal = ContextSignal::None;

    /// The `status=` code. Meaningful for @ref ContextExit::Failure; zero otherwise.
    uint64_t status = 0;

    bool operator==(ContextOutcome const&) const = default;

    /// The exit code a shell would have reported for this outcome, in the encoding OSC 133;D uses --
    /// so a session driven by OSC 3008 alone still feeds the command-block machinery.
    ///
    /// success is 0, failure is `status`, a signal is 128+n as every POSIX shell spells it, and a bare
    /// interrupt is 130 (128+SIGINT) because that is the code the shell itself would have had.
    [[nodiscard]] constexpr int asShellExitCode() const noexcept
    {
        if (signal != ContextSignal::None)
            return 128 + static_cast<int>(signal);
        switch (exit)
        {
            case ContextExit::Success: return 0;
            case ContextExit::Failure: return static_cast<int>(status & 0xFF);
            case ContextExit::Crash: return 128 + static_cast<int>(ContextSignal::Segv);
            case ContextExit::Interrupt: return 128 + static_cast<int>(ContextSignal::Int);
            case ContextExit::Unknown: break;
        }
        return 0;
    }
};

/// One hierarchical context: what the application told us about a level of its own nesting.
///
/// Every text field holds the bytes the application sent, unescaped but NOT transcoded and NOT
/// validated as UTF-8 -- a `cwd=` on Linux is an arbitrary byte string, and rejecting one that is not
/// valid UTF-8 would discard a legitimate path. A frontend that puts one on screen transcodes with
/// replacement; nothing here may assume the bytes are printable.
///
/// METADATA DRIFT, which a reader of these fields must know about: a `start=` naming a context that is
/// already active reinitialises it IN PLACE, keeping its ContextId (@see ContextStack::apply). systemd
/// re-announces the shell context on every prompt, so a scrollback line stamped with a shell context
/// reports TODAY's cwd, not the one in effect when it was written. Per-command metadata is not affected
/// -- `type=command` contexts are never updated -- so present this as current state, never as history.
struct TerminalContext
{
    ContextId id {};     ///< This terminal's handle. Never zero for a stored record.
    ContextId parent {}; ///< The context this one was pushed under; zero for the outermost.

    /// The application's own CTXID, unescaped. What `start=` and `end=` name a context by.
    std::string identifier;

    ContextType type = ContextType::None;

    std::string user;
    std::string hostname;
    std::string machineId;
    std::string bootId;
    std::string comm;
    std::string workingDirectory;
    std::string commandLine;
    std::string vm;
    std::string container;
    std::string targetUser;
    std::string targetHost;
    std::string sessionId;

    uint64_t pid = 0;
    uint64_t pidFdId = 0;

    /// Which fields the most recent `start=` actually carried.
    ///
    /// Not the same question as "is the string empty": an application may legitimately send `cmdline=`,
    /// and telling that apart from "no cmdline was mentioned" is what lets the nearest-`cwd=` walk in
    /// @ref ContextStack::effectiveWorkingDirectory skip a context rather than stop at it with an empty
    /// answer.
    ContextFields present {};

    /// How it ended, once it has. All-default while it is still live.
    ContextOutcome outcome {};

    bool operator==(TerminalContext const&) const = default;
};

// }}}
// {{{ the decoded command

/// Which of the protocol's two verbs a payload carries.
enum class ContextVerb : uint8_t
{
    Start = 0, ///< `start=`: initiates, updates, or returns to a context.
    End = 1,   ///< `end=`: terminates a context.
};

/// One decoded OSC 3008 payload.
///
/// Every string is a VIEW -- into the sequence buffer, or into the scratch buffer the decoder was
/// handed -- and both die when the sequence does. Nothing here may outlive @ref ContextStack::apply.
///
/// Views rather than strings because of what the traffic looks like: systemd re-announces the shell
/// context on EVERY prompt, with six fields byte-identical to what is already stored. Decoding into
/// owning strings would allocate on every prompt to store what we already have; @ref ContextStack::apply
/// instead assigns view-to-string field by field, which reuses the destination's capacity and so
/// allocates nothing after the first prompt.
struct ContextCommand
{
    ContextVerb verb = ContextVerb::Start;

    /// The CTXID, unescaped.
    std::string_view identifier;

    /// Which STARTFIELDs were present AND valid. A field the payload carried but that failed validation
    /// is absent here -- the protocol is lenient, so an invalid field is dropped and the rest stands.
    ContextFields present {};

    ContextType type = ContextType::None;

    std::string_view user;
    std::string_view hostname;
    std::string_view machineId;
    std::string_view bootId;
    std::string_view comm;
    std::string_view workingDirectory;
    std::string_view commandLine;
    std::string_view vm;
    std::string_view container;
    std::string_view targetUser;
    std::string_view targetHost;
    std::string_view sessionId;

    uint64_t pid = 0;
    uint64_t pidFdId = 0;

    /// Filled from the ENDFIELDs; all-default for a `start=`.
    ContextOutcome outcome {};
};

// }}}
// {{{ the stack

/// What @ref ContextStack::apply did with a command.
enum class ContextTransitionKind : uint8_t
{
    Ignored = 0,   ///< Nothing happened. An `end=` naming a context that is not in the ancestry.
    Pushed,        ///< A context the ancestry did not hold was created beneath the active one.
    Updated,       ///< The ACTIVE context was re-announced and reinitialised from the payload.
    ReturnedTo,    ///< An ANCESTOR was re-announced; everything below it was implicitly terminated.
    Ended,         ///< A context was terminated by `end=`.
    DepthExceeded, ///< A push was refused because the ancestry is already at its limit.
};

/// Whether a command changed anything a frontend can see.
enum class ContextChange : uint8_t
{
    None = 0, ///< The record is byte-for-byte what it already was; nothing needs redrawing.
    Yes = 1,  ///< Metadata, the ancestry, or both moved.
};

/// The outcome of one command, and everything a caller needs to react to it.
struct ContextTransition
{
    ContextTransitionKind kind = ContextTransitionKind::Ignored;

    /// The context the command named. Zero when @ref kind is Ignored or DepthExceeded.
    ContextId subject {};

    /// @ref subject's type, so a caller can consult its mark table without a second lookup.
    ContextType subjectType = ContextType::None;

    /// Whether anything observable moved.
    ///
    /// Separate from @ref kind because the systemd hot path -- a shell context re-announced on every
    /// prompt -- is an Updated transition that usually changes NOTHING, and re-notifying a frontend of
    /// an unchanged value is work for no change.
    ContextChange change = ContextChange::None;

    /// How many descendants this command terminated implicitly (a return-to, or an `end=` of an
    /// ancestor). Zero for every other transition.
    ///
    /// Wide enough for @ref ContextStackLimits::maxDepth, which the configuration lets a user raise to
    /// 256: a uint8_t counter silently wrapped to zero at exactly that limit, and the `end=` arm's
    /// `retired - 1` then reported 255 descendants for a stack that had just been emptied.
    uint16_t implicitlyEnded = 0;
};

/// How deep the ancestry may go and how much history is kept.
///
/// A struct rather than two parameters because a third limit would then be a field rather than a
/// signature change, and because @ref ContextStack takes its limits at construction and never after.
struct ContextStackLimits
{
    /// The deepest ancestry the terminal will track. A push beyond it is REFUSED -- the spec's "keep
    /// the earlier contexts, discard the newer".
    ///
    /// Note this is the OPPOSITE of MaxSavedTitles, where a push onto a full stack discards the oldest
    /// entry. Same shape, inverted rule; confusing the two would be an easy and invisible bug.
    size_t maxDepth = 16;

    /// How many context records are kept for lines that reference them. Must be >= maxDepth, so a
    /// context that is still an ancestor can never be evicted.
    size_t maxRetained = 256;

    bool operator==(ContextStackLimits const&) const = default;
};

/// This machine's identity, for deciding whether a context describes it.
///
/// Injected rather than read here, exactly as @ref vtbackend::isLocalHost takes the local host name
/// rather than calling gethostname(2): reading /etc/machine-id is an ambient resource, and a terminal
/// engine that reaches for one stops being testable. The session supplies it.
struct LocalIdentity
{
    std::string_view machineId;
    std::string_view hostname;
};

/// Whether a path named by a context can be opened on this machine.
enum class ContextLocality : uint8_t
{
    Unknown = 0, ///< Nothing on the ancestry says either way.
    Local = 1,   ///< This machine, this boot: the path is openable.
    Foreign = 2, ///< Behind a container, VM, remote or different-machine boundary.
};

/// A working directory and where it came from.
struct EffectiveWorkingDirectory
{
    /// The `cwd=` value. A view into the owning record; valid while the stack is not mutated.
    std::string_view path;

    /// Which context supplied it -- not necessarily the active one.
    ContextId owner;

    /// Whether @ref path names a file on the machine Contour runs on.
    ContextLocality locality = ContextLocality::Unknown;
};

/// The terminal's model of the application's own nesting: OSC 3008's ancestry, and the records that
/// scrolled-back lines still point at.
///
/// Deliberately free of Screen, Terminal and the grid: given a decoded command it produces the next
/// state and says what changed, so every one of the protocol's transitions is exercised without a
/// terminal. core/WindowSizeStack.hpp is the model.
///
/// Exactly one context is active -- the innermost of @ref chain() -- and its ancestors stay valid. The
/// ancestry may be EMPTY: before the first `start=`, and after the last context is ended. There is no
/// terminal-owned bottom entry, because unlike a pointer shape there is nothing the terminal could
/// truthfully say about the application's process tree on its own.
class ContextStack
{
  public:
    /// One level of the ancestry.
    struct Entry
    {
        /// A strong reference, so a context that is still an ancestor outlives eviction from the
        /// retained store. The same reason HyperlinkStorage hands out shared_ptr.
        std::shared_ptr<TerminalContext> record;
    };

    /// @param limits How deep the ancestry may go and how much history is kept. Fixed for the object's
    ///        lifetime: a session configured differently is a different session. Requires
    ///        `limits.maxRetained >= limits.maxDepth`.
    explicit ContextStack(ContextStackLimits limits = {}) noexcept;

    // -- mutation ---------------------------------------------------------------------------------

    /// Applies one decoded command.
    ///
    /// Total, not fallible: the protocol is lenient, so "the command named a context I do not have" is
    /// a defined OUTCOME (@ref ContextTransitionKind::Ignored), not an error. Malformed PAYLOADS never
    /// reach here -- the decoder rejects those with std::expected.
    ///
    /// @param command The decoded payload. Its views are read, never retained.
    /// @return What happened, and whether anything observable changed.
    ContextTransition apply(ContextCommand const& command);

    /// Adopts a whole record, for a mirror being brought in line with a host that already has one.
    ///
    /// The daemon replicates records rather than the sequences that produced them, so a client needs a
    /// way in that is not `apply`. Not reachable from any escape sequence.
    ///
    /// @param record The record to store, whose @ref TerminalContext::id must be non-zero.
    void adopt(TerminalContext record);

    /// Replaces the ancestry with the given ids, for the same mirroring reason as @ref adopt.
    /// Ids naming records this stack does not hold are skipped.
    void setChain(std::span<ContextId const> ids);

    /// Drops the entire ancestry and every retained record.
    ///
    /// NOT reachable from any escape sequence, deliberately. RIS and DECSTR must not clear the stack: a
    /// program down the ancestry must not be able to erase context established above it. This exists
    /// for session teardown only. @see Terminal::hardReset.
    void clear() noexcept;

    // -- the ancestry -----------------------------------------------------------------------------

    /// The ancestry, outermost first; empty when no context has been announced.
    [[nodiscard]] std::span<Entry const> chain() const noexcept { return _chain; }

    /// The innermost context's id, or zero when the ancestry is empty.
    [[nodiscard]] ContextId activeId() const noexcept
    {
        return _chain.empty() ? ContextId {} : _chain.back().record->id;
    }

    /// The innermost context, or nullptr when the ancestry is empty.
    [[nodiscard]] TerminalContext const* active() const noexcept
    {
        return _chain.empty() ? nullptr : _chain.back().record.get();
    }

    /// @return The record @p id names, or nullptr if it was never created or has been evicted.
    ///         A line whose context has aged out of the store resolves to nullptr; that is the defined
    ///         answer, not an error. @see HyperlinkStorage::hyperlinkById.
    [[nodiscard]] TerminalContext const* find(ContextId id) const noexcept;

    /// As @ref find, but keeps the record alive for as long as the caller holds the result.
    [[nodiscard]] std::shared_ptr<TerminalContext const> retain(ContextId id) const noexcept;

    /// Every retained record, oldest first -- what a daemon host replicates.
    ///
    /// A VISITOR rather than a returned container, because the caller that matters runs once per
    /// daemon flush (every 20ms, per session) and reads two fields per record before dropping the lot:
    /// handing back a std::vector<TerminalContext> deep-copied the whole pool -- up to maxRetained
    /// records of a dozen strings each -- to answer a question that copies nothing.
    template <typename Fn>
    void forEachRecord(Fn const& fn) const
    {
        // Cast rather than as_const: shared_ptr::operator* yields T& however const the shared_ptr is,
        // so without this a visitor could mutate a record the stack believes it owns.
        for (auto const id: _creationOrder)
            if (auto const it = _byId.find(id); it != _byId.end())
                fn(static_cast<TerminalContext const&>(*it->second));
    }

    /// Every retained record, oldest first, as owned copies. Prefer @ref forEachRecord where the
    /// records are only read; this exists for a caller that needs them to outlive the stack.
    [[nodiscard]] std::vector<TerminalContext> records() const;

    // -- derived questions ------------------------------------------------------------------------

    /// The working directory in effect: the nearest `cwd=` walking UP from the active context.
    ///
    /// Walking up rather than reading the active context alone is what makes a `type=command` context
    /// -- which systemd sends without a `cwd=` when nothing changed -- inherit the shell's. Returns
    /// nullopt when no context on the ancestry carried one; the caller then falls back to OSC 7.
    ///
    /// @param self This machine's identity, for the locality verdict.
    [[nodiscard]] std::optional<EffectiveWorkingDirectory> effectiveWorkingDirectory(
        LocalIdentity const& self) const noexcept;

    /// Whether @p id sits behind a host boundary, i.e. whether a path it names exists here.
    ///
    /// Three questions, in order of strength:
    ///   1. Is any context from the outermost down to @p id a VM, container, remote or boot context?
    ///      Then the path names another filesystem, whatever it looks like. A boundary is never
    ///      escaped by a deeper context.
    ///   2. Does `machineid=` match this machine's? This is the strong one and it is free: systemd's
    ///      shim emits it. It catches ssh-to-a-similar-host -- where NOTHING emits a `remote` context
    ///      and the remote shell's own sequences look entirely local -- plus containers sharing a
    ///      hostname and VMs, all with no heuristics.
    ///   3. Does `hostname=` name this machine?
    ///
    /// An ANONYMOUS context -- no boundary, no machineid, no hostname -- is Unknown, NOT Local. That is
    /// the conservative answer and it degrades to exactly the behaviour before OSC 3008 existed.
    [[nodiscard]] ContextLocality localityOf(ContextId id, LocalIdentity const& self) const noexcept;

    // -- diagnostics ------------------------------------------------------------------------------

    /// Bumped by every transition that changed something. A frontend caching a derived view keys on it,
    /// exactly as the fold caches key on Terminal::_semanticMarkRevision.
    [[nodiscard]] uint64_t revision() const noexcept { return _revision; }

    [[nodiscard]] size_t depth() const noexcept { return _chain.size(); }
    [[nodiscard]] size_t retainedCount() const noexcept { return _byId.size(); }

    /// How many pushes the depth limit refused. Non-zero means the ancestry on screen is a prefix of
    /// the truth; worth surfacing in the inspector, and what a depth-overflow test asserts.
    [[nodiscard]] size_t droppedPushes() const noexcept { return _droppedPushes; }

    [[nodiscard]] ContextStackLimits const& limits() const noexcept { return _limits; }

  private:
    /// Finds @p identifier in the ANCESTRY only, never in the retained store.
    ///
    /// That restriction is the protocol's stated safety property in code: an `end=` that could reach a
    /// context outside the ancestry would let a program guess an identifier and terminate a context
    /// established above it, which is exactly what "RIS must not clear the stack" exists to prevent.
    [[nodiscard]] std::optional<size_t> indexOf(std::string_view identifier) const noexcept;

    /// Retires the ancestry entries from @p firstIndex to the innermost, innermost first.
    /// @return How many were retired. Counted in the chain's own index type, so it cannot wrap at a
    ///         depth the configuration allows.
    uint16_t popFrom(size_t firstIndex);

    /// Whether @p id is currently an ancestor, and so must not be evicted.
    [[nodiscard]] bool isOnChain(ContextId id) const noexcept;

    /// Stores @p record, evicting the oldest retained record if that would exceed the bound.
    void store(std::shared_ptr<TerminalContext> record);

    ContextStackLimits _limits;
    std::vector<Entry> _chain;
    std::unordered_map<ContextId, std::shared_ptr<TerminalContext>> _byId;
    std::deque<ContextId> _creationOrder; ///< Eviction order; oldest at the front.
    ContextId _nextId { 1 };              ///< Monotonic; zero is reserved for "no context".
    uint64_t _revision = 0;
    size_t _droppedPushes = 0;
};

// }}}

} // namespace vtbackend
