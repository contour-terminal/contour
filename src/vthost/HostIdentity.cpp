// SPDX-License-Identifier: Apache-2.0
#include <vthost/HostIdentity.hpp>

#include <core/Sha256.hpp>
#include <core/Utils.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <ranges>
#include <span>

namespace vthost
{

namespace
{
    /// Bytes in a 128-bit id.
    constexpr auto Id128Size = std::size_t { 16 };

    /// A 128-bit id's bytes, most significant first, as sd_id128_t holds them.
    using Id128 = std::array<std::byte, Id128Size>;

    /// @param hex 32 hex digits (vtbackend::isMachineId).
    /// @return The 16 bytes they spell.
    [[nodiscard]] Id128 id128Of(std::string_view hex) noexcept
    {
        auto const digit = [hex](std::size_t index) {
            return core::digitValue<16>(hex[index]).value_or(0);
        };
        auto bytes = Id128 {};
        for (auto const index: std::views::iota(std::size_t { 0 }, bytes.size()))
            bytes[index] = static_cast<std::byte>((digit(2 * index) << 4U) | digit((2 * index) + 1));
        return bytes;
    }
} // namespace

std::string appSpecificMachineId(std::string_view machineId)
{
    if (!vtbackend::isMachineId(machineId))
        return {};

    auto const digest = core::hmacSha256(id128Of(machineId), id128Of(ContourApplicationId));
    auto id = Id128 {};
    std::ranges::copy(std::span { digest }.first<Id128Size>(), id.begin());
    // Made a version-4 UUID (RFC 9562, variant 10) as systemd's id128_make_v4_uuid() makes it, so
    // systemd-id128 and this derivation agree to the bit.
    id[6] = (id[6] & std::byte { 0x0F }) | std::byte { 0x40 };
    id[8] = (id[8] & std::byte { 0x3F }) | std::byte { 0x80 };
    return core::toLowerHexString(id);
}

vtbackend::HostIdentity appSpecificIdentity(vtbackend::LocalIdentity self)
{
    return vtbackend::HostIdentity { .machineId = appSpecificMachineId(self.machineId),
                                     .hostname = std::string { self.hostname } };
}

DaemonMachine daemonMachineOf(vtbackend::HostIdentity const& daemon, vtbackend::LocalIdentity self)
{
    // A daemon states every id its machine has, so one it does not state is one its machine has not got.
    return vtbackend::sameMachine(
               daemon.view(), vtbackend::MissingMachineId::Absent, appSpecificIdentity(self).view())
               ? DaemonMachine::Same
               : DaemonMachine::Other;
}

} // namespace vthost
