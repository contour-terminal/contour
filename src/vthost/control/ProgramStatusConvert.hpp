// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/screen/ProgramStatusSet.hpp>
#include <vtbackend/vt/ProgramStatus.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <vtcontrol/Api.hpp>
#include <vthost/proto/Pdu.hpp>

namespace vthost::control
{

/// One enumerator of vtbackend's and its counterpart in vtcontrol's vocabulary.
///
/// The two modules spell the same enums apart on purpose (vtcontrol must not see vtbackend), so these
/// rows are the one bridge between them. Rows are in vtbackend's enumerator order; the test walks both
/// enums' tables so that the two cannot drift.
/// @tparam Backend The vtbackend enum.
/// @tparam Api The vtcontrol enum.
template <typename Backend, typename Api>
struct EnumBridge
{
    Backend backend; ///< The vtbackend enumerator.
    Api api;         ///< The vtcontrol enumerator it is.
};

/// ProgramState, vtbackend to vtcontrol.
inline constexpr auto ProgramStateBridge = std::array {
    EnumBridge { vtbackend::ProgramState::Idle, vtcontrol::ProgramState::Idle },
    EnumBridge { vtbackend::ProgramState::Working, vtcontrol::ProgramState::Working },
    EnumBridge { vtbackend::ProgramState::Done, vtcontrol::ProgramState::Done },
    EnumBridge { vtbackend::ProgramState::Blocked, vtcontrol::ProgramState::Blocked },
    EnumBridge { vtbackend::ProgramState::Error, vtcontrol::ProgramState::Error },
};

/// BlockedKind, vtbackend to vtcontrol.
inline constexpr auto BlockedKindBridge = std::array {
    EnumBridge { vtbackend::BlockedKind::None, vtcontrol::BlockedKind::None },
    EnumBridge { vtbackend::BlockedKind::Permission, vtcontrol::BlockedKind::Permission },
    EnumBridge { vtbackend::BlockedKind::Question, vtcontrol::BlockedKind::Question },
    EnumBridge { vtbackend::BlockedKind::Auth, vtcontrol::BlockedKind::Auth },
};

/// ProgramStatusReason, vtbackend to vtcontrol.
inline constexpr auto ProgramStatusReasonBridge = std::array {
    EnumBridge { vtbackend::ProgramStatusReason::Report, vtcontrol::ProgramStatusReason::Report },
    EnumBridge { vtbackend::ProgramStatusReason::Prompt, vtcontrol::ProgramStatusReason::Prompt },
    EnumBridge { vtbackend::ProgramStatusReason::ProcessExit, vtcontrol::ProgramStatusReason::ProcessExit },
    EnumBridge { vtbackend::ProgramStatusReason::Reset, vtcontrol::ProgramStatusReason::Reset },
    EnumBridge { vtbackend::ProgramStatusReason::Mirror, vtcontrol::ProgramStatusReason::Mirror },
};

/// @return The vtcontrol enumerator @p value is, by @p bridge.
template <typename Backend, typename Api, std::size_t N>
[[nodiscard]] constexpr Api toApi(std::array<EnumBridge<Backend, Api>, N> const& bridge,
                                  Backend value) noexcept
{
    auto const row = std::ranges::find(bridge, value, &EnumBridge<Backend, Api>::backend);
    return row != bridge.end() ? row->api : bridge.front().api;
}

/// @return The vtbackend enumerator whose wire byte is @p wire, or nothing when this build knows none.
template <typename Backend, typename Api, std::size_t N>
[[nodiscard]] constexpr std::optional<Backend> fromWireByte(
    std::array<EnumBridge<Backend, Api>, N> const& bridge, uint8_t wire) noexcept
{
    auto const row = std::ranges::find_if(
        bridge, [wire](auto const& entry) { return std::to_underlying(entry.backend) == wire; });
    if (row == bridge.end())
        return std::nullopt;
    return row->backend;
}

/// A record as the control API lists it.
/// @param record The terminal's record.
/// @param inheritedApp The app the record speaks for: ProgramStatusSet::appOf(), its own or an ancestor's.
/// @return The API record.
[[nodiscard]] inline vtcontrol::ProgramStatusInfo toApi(vtbackend::ProgramStatusRecord const& record,
                                                        std::string_view inheritedApp)
{
    return vtcontrol::ProgramStatusInfo {
        .id = record.id,
        .state = toApi(ProgramStateBridge, record.state),
        .kind = toApi(BlockedKindBridge, record.kind),
        .progress = record.progress,
        .app = std::string { inheritedApp },
        .title = record.title,
        .message = record.message,
    };
}

/// @return Every record of @p set as the control API lists it, least recently updated first.
[[nodiscard]] inline std::vector<vtcontrol::ProgramStatusInfo> toApi(vtbackend::ProgramStatusSet const& set)
{
    auto records = std::vector<vtcontrol::ProgramStatusInfo> {};
    records.reserve(set.records().size());
    for (auto const& record: set.records())
        records.push_back(toApi(record, set.appOf(record)));
    return records;
}

/// A record as it travels.
/// @param record The terminal's record; its own app, never an inherited one, so that the mirror inherits
///        exactly as the host does.
[[nodiscard]] inline proto::WireProgramStatusRecord toWire(vtbackend::ProgramStatusRecord const& record)
{
    return proto::WireProgramStatusRecord {
        .id = record.id,
        .state = std::to_underlying(record.state),
        .kind = std::to_underlying(record.kind),
        .progress = record.progress.value_or(proto::WireProgramStatusRecord::AbsentProgress),
        .app = record.app,
        .title = record.title,
        .message = record.message,
    };
}

/// A record as it arrived. Total: whatever the bytes, the result holds to the rules the decoder holds a
/// report to.
/// @param wire The record off the wire.
/// @return The record, or nothing when its state is one this build does not know: a newer host's state
///         has no meaning here, and showing it as another would be a lie. An unknown kind reads as none,
///         and a progress out of range, or on a state that takes none, as absent.
[[nodiscard]] inline std::optional<vtbackend::ProgramStatusRecord> fromWire(
    proto::WireProgramStatusRecord const& wire)
{
    auto const state = fromWireByte(ProgramStateBridge, wire.state);
    if (!state)
        return std::nullopt;
    auto record = vtbackend::ProgramStatusRecord {
        .id = wire.id,
        .state = *state,
        .kind = vtbackend::BlockedKind::None,
        .progress = std::nullopt,
        .app = wire.app,
        .title = wire.title,
        .message = wire.message,
    };
    if (*state == vtbackend::ProgramState::Blocked)
        record.kind = fromWireByte(BlockedKindBridge, wire.kind).value_or(vtbackend::BlockedKind::None);
    if (vtbackend::takesProgress(*state) && wire.progress <= 100)
        record.progress = wire.progress;
    return record;
}

} // namespace vthost::control
