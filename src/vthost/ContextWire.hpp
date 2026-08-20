// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The OSC 3008 hierarchical context state as it travels on the native protocol — single-sourced so
/// the server's capture (NativeSession) and the client's apply (ScreenMirror) cannot disagree, exactly
/// as @ref vthost/StatusWire.hpp does for the status display and @ref vthost/MouseWire.hpp for the
/// mouse.
///
/// Decoding is TOTAL, which is the whole reason this file exists. Every one of these arrives as a raw
/// byte a peer chose, and casting one straight into its enum is how a value no enumerator has reaches
/// a switch that assumes otherwise. Three enums and a flag mask travel here; none of them is cast.

#include <vtbackend/core/TerminalContext.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <utility>

#include <vthost/proto/Pdu.hpp>

namespace vthost
{

/// @param value A ContextType as the wire spells it.
/// @return The type it names, or nullopt for a value no enumerator has.
[[nodiscard]] constexpr std::optional<vtbackend::ContextType> contextTypeOf(uint8_t value) noexcept
{
    // The enumerators are contiguous from None, so the bound is the last row of the type table.
    constexpr auto Last = static_cast<uint8_t>(vtbackend::ContextTypeList.back());
    return value <= Last ? std::optional { static_cast<vtbackend::ContextType>(value) } : std::nullopt;
}

/// @param value A ContextExit as the wire spells it.
/// @return The exit kind it names, or nullopt for a value no enumerator has.
[[nodiscard]] constexpr std::optional<vtbackend::ContextExit> contextExitOf(uint8_t value) noexcept
{
    return value <= std::to_underlying(vtbackend::ContextExit::Interrupt)
               ? std::optional { static_cast<vtbackend::ContextExit>(value) }
               : std::nullopt;
}

/// @param value A ContextSignal as the wire spells it -- a Linux signal NUMBER, not an index.
/// @return The signal it names, or nullopt for a number this terminal does not know.
[[nodiscard]] constexpr std::optional<vtbackend::ContextSignal> contextSignalOf(uint8_t value) noexcept
{
    // Not a range check: the signal numbers are sparse (there is no 16..23), so membership is the only
    // sound test. @see VTBACKEND_CONTEXT_SIGNALS.
    return vtbackend::isKnownContextSignal(value)
               ? std::optional { static_cast<vtbackend::ContextSignal>(value) }
               : std::nullopt;
}

/// @param value A ContextFields mask as the wire spells it.
/// @return The mask with any bit the field table does not assign cleared.
///
/// Masked rather than rejected: an unknown bit is an unknown FIELD, and the protocol's own rule for
/// one of those is to ignore it and keep the rest -- so a peer built from a newer commit that gained
/// a sixteenth field degrades to the fifteen this build understands.
[[nodiscard]] constexpr vtbackend::ContextFields contextFieldsOf(uint16_t value) noexcept
{
    return vtbackend::ContextFields { static_cast<vtbackend::ContextField>(
        static_cast<uint16_t>(value & vtbackend::ContextFieldMask)) };
}

/// Every ContextField, with the wire bit it is pinned to. @see ContextPresentMask.
constexpr inline auto PinnedContextFieldBits = std::array {
    std::pair { vtbackend::ContextField::Type, 0 },
    std::pair { vtbackend::ContextField::User, 1 },
    std::pair { vtbackend::ContextField::Hostname, 2 },
    std::pair { vtbackend::ContextField::MachineId, 3 },
    std::pair { vtbackend::ContextField::BootId, 4 },
    std::pair { vtbackend::ContextField::Pid, 5 },
    std::pair { vtbackend::ContextField::PidFdId, 6 },
    std::pair { vtbackend::ContextField::Comm, 7 },
    std::pair { vtbackend::ContextField::WorkingDirectory, 8 },
    std::pair { vtbackend::ContextField::CommandLine, 9 },
    std::pair { vtbackend::ContextField::Vm, 10 },
    std::pair { vtbackend::ContextField::Container, 11 },
    std::pair { vtbackend::ContextField::TargetUser, 12 },
    std::pair { vtbackend::ContextField::TargetHost, 13 },
    std::pair { vtbackend::ContextField::SessionId, 14 },
};

namespace detail
{
    [[nodiscard]] constexpr bool contextFieldBitsAreAsPinned() noexcept
    {
        for (auto const& [field, bit]: PinnedContextFieldBits)
            if (std::to_underlying(field) != (1U << static_cast<unsigned>(bit)))
                return false;
        return true;
    }
} // namespace detail

static_assert(detail::contextFieldBitsAreAsPinned(),
              "A ContextField's bit value changed. Those bits travel on the native wire verbatim "
              "inside a context record's `present`, so an older peer would read the wrong field: bump "
              "proto::CodecVersion (or restore the bit) and update the pin above.");
static_assert(PinnedContextFieldBits.size() == vtbackend::ContextFieldList.size(),
              "A ContextField was added or removed. Add (or drop) its row in PinnedContextFieldBits "
              "and in proto::ContextStringFields/ContextVarintFields, which together freeze the wire "
              "encoding, then widen proto::ContextPresentMask.");
static_assert(vtbackend::ContextFieldMask == proto::ContextPresentMask,
              "vtbackend's field table and the wire's presence mask disagree about which bits exist.");

/// Builds a TerminalContext from its wire form, with the two ids already translated.
///
/// Total, and THIS is where that is secured: the codec carries every enum-ish byte through raw -- it
/// knows nothing of vtbackend's enumerators, deliberately -- so each one is named here through the
/// validators above, and anything they cannot name falls to its zero value. Nothing below can produce
/// a value no enumerator has.
///
/// @param wire      The record off the wire.
/// @param localId   The id THIS terminal will hold it under.
/// @param localParent The translated parent id, or zero when the parent is not (yet) known here.
[[nodiscard]] inline vtbackend::TerminalContext fromWireContext(proto::WireContext const& wire,
                                                                vtbackend::ContextId localId,
                                                                vtbackend::ContextId localParent)
{
    auto record = vtbackend::TerminalContext {};
    record.id = localId;
    record.parent = localParent;
    record.identifier = wire.identifier;
    record.type = contextTypeOf(wire.type).value_or(vtbackend::ContextType::None);
    record.present = contextFieldsOf(wire.present);
    record.user = wire.user;
    record.hostname = wire.hostname;
    record.machineId = wire.machineId;
    record.bootId = wire.bootId;
    record.comm = wire.comm;
    record.workingDirectory = wire.workingDirectory;
    record.commandLine = wire.commandLine;
    record.vm = wire.vm;
    record.container = wire.container;
    record.targetUser = wire.targetUser;
    record.targetHost = wire.targetHost;
    record.sessionId = wire.sessionId;
    record.pid = wire.pid;
    record.pidFdId = wire.pidFdId;
    record.outcome.exit = contextExitOf(wire.exitKind).value_or(vtbackend::ContextExit::Unknown);
    record.outcome.signal = contextSignalOf(wire.signal).value_or(vtbackend::ContextSignal::None);
    record.outcome.status = wire.status;
    return record;
}

/// Whether @p record already holds exactly what @p wire describes under @p localParent.
///
/// Field by field rather than `fromWireContext(...) == record`, and the difference is not cosmetic:
/// this answers "did anything move?" once per retained record per delta -- every 20 ms on an attached
/// pane -- and building a TerminalContext to ask would allocate a dozen strings per record per flush
/// to usually learn that nothing did. Mirrors @ref fromWireContext line for line, deliberately: the
/// two are read together, and a field present in one but not the other is the bug this exists to
/// prevent.
[[nodiscard]] inline bool matchesWireContext(vtbackend::TerminalContext const& record,
                                             proto::WireContext const& wire,
                                             vtbackend::ContextId localParent) noexcept
{
    return record.parent == localParent && record.identifier == wire.identifier
           && record.type == contextTypeOf(wire.type).value_or(vtbackend::ContextType::None)
           && record.present == contextFieldsOf(wire.present) && record.user == wire.user
           && record.hostname == wire.hostname && record.machineId == wire.machineId
           && record.bootId == wire.bootId && record.comm == wire.comm
           && record.workingDirectory == wire.workingDirectory && record.commandLine == wire.commandLine
           && record.vm == wire.vm && record.container == wire.container
           && record.targetUser == wire.targetUser && record.targetHost == wire.targetHost
           && record.sessionId == wire.sessionId && record.pid == wire.pid && record.pidFdId == wire.pidFdId
           && record.outcome.exit == contextExitOf(wire.exitKind).value_or(vtbackend::ContextExit::Unknown)
           && record.outcome.signal == contextSignalOf(wire.signal).value_or(vtbackend::ContextSignal::None)
           && record.outcome.status == wire.status;
}

/// The wire form of @p record, for a host replicating it.
[[nodiscard]] inline proto::WireContext toWireContext(vtbackend::TerminalContext const& record)
{
    auto wire = proto::WireContext {};
    wire.id = record.id.value;
    wire.parent = record.parent.value;
    wire.identifier = record.identifier;
    wire.type = static_cast<uint8_t>(record.type);
    wire.present = record.present.value();
    wire.user = record.user;
    wire.hostname = record.hostname;
    wire.machineId = record.machineId;
    wire.bootId = record.bootId;
    wire.comm = record.comm;
    wire.workingDirectory = record.workingDirectory;
    wire.commandLine = record.commandLine;
    wire.vm = record.vm;
    wire.container = record.container;
    wire.targetUser = record.targetUser;
    wire.targetHost = record.targetHost;
    wire.sessionId = record.sessionId;
    wire.pid = record.pid;
    wire.pidFdId = record.pidFdId;
    wire.exitKind = static_cast<uint8_t>(record.outcome.exit);
    wire.signal = static_cast<uint8_t>(record.outcome.signal);
    wire.status = record.outcome.status;
    return wire;
}

} // namespace vthost
