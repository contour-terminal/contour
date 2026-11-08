// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
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

/// What the spawned program is attached to.
enum class SpawnMode : std::uint8_t
{
    Terminal,
    Detached,
};

/// How the image is located.
enum class ExecLookup : std::uint8_t
{
    ExactPath,  ///< execve
    SearchPath, ///< execvp
};

/// Whether the child takes the PTY slave as its controlling terminal.
enum class ControllingTerminal : std::uint8_t
{
    Skip,
    Acquire,
};

/// Whether the child's terminal attributes are configured (util::constructTerminalSettings).
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
inline constexpr std::uint8_t TrampolineSlotCount = 6;

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

/// Everything the trampoline needs to start one program.
struct SpawnSpec
{
    SpawnMode mode = SpawnMode::Terminal;
    std::string image {};                                   ///< What to exec; empty: argv[0].
    std::vector<std::string> argv {};                       ///< argv[0] is the name the program runs under.
    ExecLookup lookup = ExecLookup::ExactPath;              ///< How the image is located.
    std::vector<std::string> fallbackArgv {};               ///< Login-shell fallback; empty: none.
    std::string fallbackText {};                            ///< The fallback as shown to the user.
    std::string workingDirectory {};                        ///< Empty: inherit.
    std::optional<std::vector<std::string>> environment {}; ///< "NAME=value"; nullopt: the trampoline's own.
    std::optional<int> oomScoreAdjust {};                   ///< Linux OOM score adjustment, if any.
    ControllingTerminal controllingTerminal = ControllingTerminal::Skip;
    TerminalSettings terminalSettings = TerminalSettings::Keep;
    FailureReport failureReport = FailureReport::Silent;
    /// The descriptors passed along. Not named `slots`: Qt defines that as a macro.
    TrampolineSlots inheritedSlots {};

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
