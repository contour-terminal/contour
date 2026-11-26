// SPDX-License-Identifier: Apache-2.0
#include <vtextension/wasm/host/DescriptorRights.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <ranges>
#include <span>
#include <utility>

namespace vtextension::wasm::host
{

namespace
{
    using enum DescriptorRight;

    // Each role's rights. F_GETFL is every used stream's: it reads, and a stream's flags are what C's stdio
    // and WAMR ask of any descriptor they are handed.
    constexpr auto ControlRights =
        std::array { Read, Write, Receive, Send, Event, Shutdown, Fstat, GetStatusFlags };
    constexpr auto LaneRights = std::array { Read, Write, Receive, Send, Event, Shutdown, GetStatusFlags };
    constexpr auto PageRights = std::array { MapRead, Fstat, GetStatusFlags };
    constexpr auto WakeRights = std::array { Read, Event, GetStatusFlags };
    constexpr auto StandardErrorRights = std::array { Write, Fstat, GetStatusFlags };
    // WAMR's WASI stats the device and reads its flags as it instantiates a module; a guest writes to it,
    // seeks it (fd_seek) and reads its flags (fd_fdstat_get), each of which the backend contract asserts.
    constexpr auto EngineDeviceRights = std::array { Write, Fstat, Seek, GetStatusFlags };
    // Nothing reads or writes them; C's stdio may still ask what they are.
    constexpr auto IdleStreamRights = std::array { Fstat };
    constexpr auto StopReceiverRights = std::array { Read, Event };
    constexpr auto StopSenderRights = std::array { Write };
    // One handle in both roles: what each of them may. Windows' alone, where no sandbox limits a handle by
    // its role: the row names the handle's use, and nothing enforces it.
    constexpr auto StopEventRights = std::array { Read, Write, Event };

    /// Every role, at its enumerator's index.
    constexpr auto RoleRightsTable = std::array {
        DescriptorRoleRights { .role = DescriptorRole::Control, .rights = ControlRights },
        DescriptorRoleRights { .role = DescriptorRole::Lane, .rights = LaneRights },
        DescriptorRoleRights { .role = DescriptorRole::Page, .rights = PageRights },
        DescriptorRoleRights { .role = DescriptorRole::Wake, .rights = WakeRights },
        DescriptorRoleRights { .role = DescriptorRole::StandardError, .rights = StandardErrorRights },
        DescriptorRoleRights { .role = DescriptorRole::EngineDevice, .rights = EngineDeviceRights },
        DescriptorRoleRights { .role = DescriptorRole::IdleStream, .rights = IdleStreamRights },
        DescriptorRoleRights { .role = DescriptorRole::StopReceiver, .rights = StopReceiverRights },
        DescriptorRoleRights { .role = DescriptorRole::StopSender, .rights = StopSenderRights },
        DescriptorRoleRights { .role = DescriptorRole::StopEvent, .rights = StopEventRights },
    };

    static_assert(std::ranges::all_of(std::views::iota(std::size_t { 0 }, RoleRightsTable.size()),
                                      [](std::size_t index) {
                                          return std::size_t {
                                              std::to_underlying(RoleRightsTable[index].role)
                                          } == index;
                                      }),
                  "RoleRightsTable lists every DescriptorRole at its own index");
    static_assert(RoleRightsTable.back().role == DescriptorRole::StopEvent,
                  "RoleRightsTable ends at the last DescriptorRole");
} // namespace

std::span<DescriptorRoleRights const> descriptorRoleRights() noexcept
{
    return RoleRightsTable;
}

std::span<DescriptorRight const> rightsOf(DescriptorRole role) noexcept
{
    return RoleRightsTable[std::to_underlying(role)].rights;
}

} // namespace vtextension::wasm::host
