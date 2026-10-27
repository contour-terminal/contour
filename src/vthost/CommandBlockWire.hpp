// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// A command block's record as it travels on the native protocol — single-sourced so the host's
/// capture (NativeSession) and the mirror's adoption (ScreenMirror) cannot disagree, exactly as
/// @ref vthost/ContextWire.hpp does for OSC 3008 contexts.
///
/// Decoding is TOTAL: the enums and a presence mask arrive as bytes a peer chose, and none of them is
/// cast into its enum unchecked. What only the host can use never crosses the wire, so a mirror keeps
/// its own.
/// @see proto::WireCommandBlock

#include <vtbackend/core/TerminalContext.hpp>
#include <vtbackend/grid/Grid.hpp>
#include <vtbackend/shell/CommandBlock.hpp>

#include <cstdint>
#include <optional>
#include <utility>

#include <vthost/ContextWire.hpp>
#include <vthost/TimeWire.hpp>
#include <vthost/WireEnum.hpp>
#include <vthost/proto/Pdu.hpp>

namespace vthost
{

// The record's enums travel as raw bytes, so their numbering is frozen here. Renumbering one in
// CommandBlock.hpp then fails to compile at the boundary, where the question "does the wire change?"
// has to be answered, instead of a peer silently reading Running as Finished.
static_assert(std::to_underlying(vtbackend::CommandBlockState::Prompting) == 0);
static_assert(std::to_underlying(vtbackend::CommandBlockState::Running) == 1);
static_assert(std::to_underlying(vtbackend::CommandBlockState::Finished) == 2);
static_assert(std::to_underlying(vtbackend::CommandBlockEnd::Reported) == 0);
static_assert(std::to_underlying(vtbackend::CommandBlockEnd::Implicit) == 1);
static_assert(std::to_underlying(vtbackend::CommandBlockEnd::Abandoned) == 2);
static_assert(std::to_underlying(vtbackend::CommandLineSource::None) == 0);
static_assert(std::to_underlying(vtbackend::CommandLineSource::Reported) == 1);
static_assert(std::to_underlying(vtbackend::CommandLineSource::Recovered) == 2);
static_assert(std::to_underlying(vtbackend::ContextLocality::Unknown) == 0);
static_assert(std::to_underlying(vtbackend::ContextLocality::Local) == 1);
static_assert(std::to_underlying(vtbackend::ContextLocality::Foreign) == 2);
static_assert(std::to_underlying(vtbackend::ForeignDirectory::Unreported) == 0);
static_assert(std::to_underlying(vtbackend::ForeignDirectory::Reported) == 1);

/// @param value A CommandBlockState as the wire spells it.
/// @return The state it names, or nullopt.
[[nodiscard]] constexpr std::optional<vtbackend::CommandBlockState> commandBlockStateOf(
    uint8_t value) noexcept
{
    return enumUpTo(value, vtbackend::CommandBlockState::Finished);
}

/// @param value A CommandBlockEnd as the wire spells it.
/// @return The end it names, or nullopt.
[[nodiscard]] constexpr std::optional<vtbackend::CommandBlockEnd> commandBlockEndOf(uint8_t value) noexcept
{
    return enumUpTo(value, vtbackend::CommandBlockEnd::Abandoned);
}

/// @param value A CommandLineSource as the wire spells it.
/// @return The command-line source it names, or nullopt.
[[nodiscard]] constexpr std::optional<vtbackend::CommandLineSource> commandLineSourceOf(
    uint8_t value) noexcept
{
    return enumUpTo(value, vtbackend::CommandLineSource::Recovered);
}

/// @param value A ContextLocality as the wire spells it.
/// @return The locality it names, or nullopt.
[[nodiscard]] constexpr std::optional<vtbackend::ContextLocality> contextLocalityOf(uint8_t value) noexcept
{
    return enumUpTo(value, vtbackend::ContextLocality::Foreign);
}

/// @param value A ForeignDirectory as the wire spells it.
/// @return The verdict it names, or nullopt.
[[nodiscard]] constexpr std::optional<vtbackend::ForeignDirectory> foreignDirectoryOf(uint8_t value) noexcept
{
    return enumUpTo(value, vtbackend::ForeignDirectory::Reported);
}

/// Whether the host still holds a block's head row, as the record carries it on the wire.
enum class HostHead : uint8_t
{
    Held = 0,    ///< Among the host's rows, or not known to have left them.
    Evicted = 1, ///< Scrolled out of the host's history or erased: no row on any side can head it now.
};

/// @param grid The host's primary grid.
/// @param record The host's record.
/// @return Evicted when @p record's head row has left @p grid (vtbackend::isHeadEvicted), else Held.
[[nodiscard]] inline HostHead hostHeadOf(vtbackend::Grid const& grid,
                                         vtbackend::CommandBlockRecord const& record) noexcept
{
    return vtbackend::isHeadEvicted(grid, record) ? HostHead::Evicted : HostHead::Held;
}

/// @param wire A record off the wire.
/// @return What it says of the host's head row.
[[nodiscard]] constexpr HostHead hostHeadOf(proto::WireCommandBlock const& wire) noexcept
{
    return (wire.present & proto::CommandBlockHeadEvicted) != 0 ? HostHead::Evicted : HostHead::Held;
}

/// @param record A command block's record.
/// @return The CommandBlockHas* bits naming which of @p record's optionals are engaged.
[[nodiscard]] constexpr uint8_t presenceOf(vtbackend::CommandBlockRecord const& record) noexcept
{
    auto present = uint8_t { 0 };
    if (record.promptStartedAt.has_value())
        present = static_cast<uint8_t>(present | proto::CommandBlockHasPromptStart);
    if (record.exitCode.has_value())
        present = static_cast<uint8_t>(present | proto::CommandBlockHasExitCode);
    if (record.commandStartedAt.has_value())
        present = static_cast<uint8_t>(present | proto::CommandBlockHasCommandStart);
    if (record.duration.has_value())
        present = static_cast<uint8_t>(present | proto::CommandBlockHasDuration);
    return present;
}

/// The wire form of @p record, for a host replicating it.
/// @param record The host's record.
/// @param head Whether the host still holds @p record's head row (@see hostHeadOf), judged as it is sent.
/// @return Every field the wire carries; an optional that is absent travels as zero.
[[nodiscard]] inline proto::WireCommandBlock toWireCommandBlock(vtbackend::CommandBlockRecord const& record,
                                                                HostHead head)
{
    auto wire = proto::WireCommandBlock {};
    wire.id = record.id.value;
    wire.state = std::to_underlying(record.state);
    wire.end = std::to_underlying(record.end);
    wire.commandLineSource = std::to_underlying(record.commandLineSource);
    wire.locality = std::to_underlying(record.workingDirectory.locality);
    wire.present = presenceOf(record);
    if (head == HostHead::Evicted)
        wire.present = static_cast<uint8_t>(wire.present | proto::CommandBlockHeadEvicted);
    wire.outcomeExit = std::to_underlying(record.outcome.exit);
    wire.outcomeSignal = std::to_underlying(record.outcome.signal);
    wire.outcomeStatus = record.outcome.status;
    wire.promptStartedAt = record.promptStartedAt ? toWireTime(*record.promptStartedAt) : 0;
    wire.exitCode = record.exitCode.value_or(0);
    wire.commandStartedAt = record.commandStartedAt ? toWireTime(*record.commandStartedAt) : 0;
    wire.duration = record.duration ? toWireDuration(*record.duration) : 0;
    wire.commandLine = record.commandLine.raw();
    wire.workingDirectory = record.workingDirectory.path;
    wire.endLocality = std::to_underlying(record.endWorkingDirectory.locality);
    wire.endWorkingDirectory = record.endWorkingDirectory.path;
    wire.foreignDirectory = std::to_underlying(record.foreignDirectory);
    return wire;
}

/// Builds the record a mirror adopts from @p wire.
///
/// Total: every enum-ish byte goes through a validator and falls to its zero value when it names
/// nothing; an exit code no `int` holds, or a negative duration, is treated as absent. Instants and
/// spans convert as sent (vthost/TimeWire.hpp).
///
/// What the wire does not carry comes from @p held, so a resend of a record the mirror holds rebuilds
/// that record exactly, which is how CommandBlockStore::adopt() recognizes it as one. Each working
/// directory's locality is the host's verdict, as the host judged it: whether it holds on the mirror's
/// machine is the mirror terminal's to decide (vtbackend::Terminal::adoptCommandBlock).
/// @param wire The record off the wire.
/// @param held The mirror's current copy of this id, or nullptr; without one the head is
///        vtbackend::UnknownHeadGeneration and the command's context zero. Whether the host's head row
///        is gone (hostHeadOf()) is not the record's: the mirror applies it to its own grid.
/// @return The record to adopt.
[[nodiscard]] inline vtbackend::CommandBlockRecord fromWireCommandBlock(
    proto::WireCommandBlock const& wire, vtbackend::CommandBlockRecord const* held)
{
    auto record = vtbackend::CommandBlockRecord {};
    record.id = vtbackend::CommandBlockId { wire.id };
    record.state = commandBlockStateOf(wire.state).value_or(vtbackend::CommandBlockState::Prompting);
    record.end = commandBlockEndOf(wire.end).value_or(vtbackend::CommandBlockEnd::Reported);
    record.commandLine = vtbackend::CommandLineText { wire.commandLine };
    record.commandLineSource =
        commandLineSourceOf(wire.commandLineSource).value_or(vtbackend::CommandLineSource::None);
    if ((wire.present & proto::CommandBlockHasExitCode) != 0 && std::in_range<int>(wire.exitCode))
        record.exitCode = static_cast<int>(wire.exitCode);
    record.outcome.exit = contextExitOf(wire.outcomeExit).value_or(vtbackend::ContextExit::Unknown);
    record.outcome.signal = contextSignalOf(wire.outcomeSignal).value_or(vtbackend::ContextSignal::None);
    record.outcome.status = wire.outcomeStatus;
    if ((wire.present & proto::CommandBlockHasPromptStart) != 0)
        record.promptStartedAt = fromWireTime(wire.promptStartedAt);
    if ((wire.present & proto::CommandBlockHasCommandStart) != 0)
        record.commandStartedAt = fromWireTime(wire.commandStartedAt);
    if ((wire.present & proto::CommandBlockHasDuration) != 0 && wire.duration >= 0)
        record.duration = fromWireDuration(wire.duration);
    record.workingDirectory.path = wire.workingDirectory;
    record.workingDirectory.locality =
        contextLocalityOf(wire.locality).value_or(vtbackend::ContextLocality::Unknown);
    record.endWorkingDirectory.path = wire.endWorkingDirectory;
    record.endWorkingDirectory.locality =
        contextLocalityOf(wire.endLocality).value_or(vtbackend::ContextLocality::Unknown);
    record.foreignDirectory =
        foreignDirectoryOf(wire.foreignDirectory).value_or(vtbackend::ForeignDirectory::Unreported);
    if (held != nullptr)
    {
        record.commandContext = held->commandContext;
        record.headStableId = held->headStableId;
        record.headIdGeneration = held->headIdGeneration;
    }
    else
        record.headIdGeneration = vtbackend::UnknownHeadGeneration;
    return record;
}

} // namespace vthost
