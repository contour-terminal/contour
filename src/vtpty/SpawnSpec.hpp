// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtpty/ControllingTerminal.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace vtpty
{

/// The argv[1] prefix that makes the executable run as a spawn trampoline; the suffix is the protocol
/// version.
inline constexpr std::string_view SpawnTrampolineMarker = "--vtpty-spawn-trampoline=";

/// The wire-format version the marker's suffix must carry.
inline constexpr std::uint32_t SpawnTrampolineProtocolVersion = 1;

/// The file descriptor the spec socket has in the trampoline.
inline constexpr int SpawnSpecFd = 3;

/// The file descriptor a session's program finds the writer of its stdout fast pipe on. The same number as
/// SpawnSpecFd, in another process: the trampoline has closed the spec socket by the time it puts the pipe
/// there.
inline constexpr int StdoutFastPipeFd = 3;

/// The size of the length, a little-endian u32, that precedes the encoded spec on the spec socket.
inline constexpr std::size_t SpawnSpecLengthSize = 4;

/// How the image is located.
enum class ExecLookup : std::uint8_t
{
    ExactPath,  ///< execve
    SearchPath, ///< execvp
};

/// Whether the child's terminal attributes are configured (util::configureTerminal()).
enum class TerminalSettings : std::uint8_t
{
    Keep,
    Configure,
};

/// Where an exec failure is reported.
enum class FailureReport : std::uint8_t
{
    Silent,
    OnTerminal,
};

/// The status the trampoline exits with where the program cannot run.
enum class FailureExit : std::uint8_t
{
    Failure,  ///< EXIT_FAILURE.
    NotFound, ///< 127, the status a shell gives a command it could not run.
};

/// The descriptors a trampoline can be handed beyond the spec socket, at consecutive numbers.
enum class TrampolineSlot : std::uint8_t
{
    Gate,
    Settle,
    Terminal,
    FastPipe,
    Input,
    Output,
};

/// The number of TrampolineSlot enumerators.
inline constexpr std::uint8_t TrampolineSlotCount = std::to_underlying(TrampolineSlot::Output) + 1;

/// The file descriptor number a slot has in the trampoline.
/// @param slot The slot.
/// @return 4 + the slot's index.
[[nodiscard]] constexpr int trampolineFd(TrampolineSlot slot) noexcept
{
    return SpawnSpecFd + 1 + static_cast<int>(slot);
}

/// A set of TrampolineSlot values, stored as a bitmask.
class TrampolineSlots
{
  public:
    constexpr TrampolineSlots() noexcept = default;

    /// Reconstructs a set from its raw bits.
    /// @param bits One bit per slot, bit n for the slot with index n.
    [[nodiscard]] static constexpr TrampolineSlots fromRaw(std::uint8_t bits) noexcept
    {
        return TrampolineSlots { bits };
    }

    /// @return a copy of this set that also contains @p slot.
    [[nodiscard]] constexpr TrampolineSlots with(TrampolineSlot slot) const noexcept
    {
        return TrampolineSlots { static_cast<std::uint8_t>(_bits | bitOf(slot)) };
    }

    /// @return whether @p slot is in the set.
    [[nodiscard]] constexpr bool has(TrampolineSlot slot) const noexcept
    {
        return (_bits & bitOf(slot)) != 0;
    }

    /// @return the raw bits.
    [[nodiscard]] constexpr std::uint8_t raw() const noexcept { return _bits; }

    constexpr bool operator==(TrampolineSlots const&) const noexcept = default;

  private:
    explicit constexpr TrampolineSlots(std::uint8_t bits) noexcept: _bits { bits } {}

    [[nodiscard]] static constexpr std::uint8_t bitOf(TrampolineSlot slot) noexcept
    {
        return static_cast<std::uint8_t>(1U << static_cast<std::uint8_t>(slot));
    }

    std::uint8_t _bits = 0;
};

/// How a program on a terminal is started: a session's shell, the tmux control client.
struct TerminalLaunch
{
    std::vector<std::string> fallbackArgv {}; ///< Login-shell fallback; empty: none.
    std::string workingDirectory {};          ///< Empty: inherit.
    ControllingTerminal controllingTerminal = ControllingTerminal::Skip;
    TerminalSettings terminalSettings = TerminalSettings::Keep;
    FailureReport failureReport = FailureReport::Silent;
    FailureExit failureExit = FailureExit::Failure;

    bool operator==(TerminalLaunch const&) const = default;
};

/// How a program on its own is started, in a session of its own: nothing beyond the shared fields.
struct DetachedLaunch
{
    bool operator==(DetachedLaunch const&) const = default;
};

/// What the spawned program is attached to, with the fields only that mode has; its index is the wire's
/// mode byte.
using SpawnLaunch = std::variant<TerminalLaunch, DetachedLaunch>;

/// Everything the trampoline needs to start one program.
struct SpawnSpec
{
    std::string image {};                                   ///< What to exec; empty: argv[0].
    std::vector<std::string> argv {};                       ///< argv[0] is the name the program runs under.
    ExecLookup lookup = ExecLookup::ExactPath;              ///< How the image is located.
    std::optional<std::vector<std::string>> environment {}; ///< "NAME=value"; nullopt: the trampoline's own.
    std::optional<int> oomScoreAdjust {};                   ///< Linux OOM score adjustment, if any.
    /// The descriptors passed along. Not named `slots`: Qt defines that as a macro.
    TrampolineSlots inheritedSlots {};
    SpawnLaunch launch {}; ///< The mode, and what only it needs.

    bool operator==(SpawnSpec const&) const = default;
};

/// Why a byte string is not a SpawnSpec.
enum class SpawnSpecError : std::uint8_t
{
    Truncated,
    UnknownEnumerator,
    TrailingBytes,
    EmptyArgv,
};

/// Serializes a spec: little-endian u32 counts and lengths, enumerators as one byte, optionals with a
/// presence byte.
/// @param spec The spec.
/// @return the encoded bytes.
[[nodiscard]] std::string encodeSpawnSpec(SpawnSpec const& spec);

/// Serializes a spec as it travels on the spec socket: its encoded length, then encodeSpawnSpec()'s bytes.
/// @param spec The spec.
/// @return the framed bytes, built in one allocation.
[[nodiscard]] std::string encodeFramedSpawnSpec(SpawnSpec const& spec);

/// Reads the length that precedes a framed spec.
/// @param prefix The first SpawnSpecLengthSize bytes of the frame.
/// @return the length of the spec that follows; Truncated where @p prefix is short.
[[nodiscard]] std::expected<std::uint32_t, SpawnSpecError> decodeSpawnSpecLength(std::string_view prefix);

/// Parses what encodeSpawnSpec produced.
/// @param bytes The encoded spec.
/// @return the spec, or why the bytes are not one.
[[nodiscard]] std::expected<SpawnSpec, SpawnSpecError> decodeSpawnSpec(std::string_view bytes);

/// What the command line asks of this process.
enum class TrampolineInvocation : std::uint8_t
{
    NotRequested,
    Supported,
    UnsupportedVersion,
};

/// Decides from argv[1] whether the process is to run as a spawn trampoline.
/// @param argv The process arguments.
/// @return the verdict; arguments after argv[1] are ignored.
[[nodiscard]] TrampolineInvocation classifyInvocation(std::span<char const* const> argv) noexcept;

} // namespace vtpty
