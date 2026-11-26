// SPDX-License-Identifier: Apache-2.0
//
// Internal to vtextension: how an instance's guest memory is read and reached through its runtime, shared by
// Instance's own IGuestMemory and the tests that decode a memory another process writes.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <limits>
#include <span>

#include <vtextension/CanonicalAbi.hpp>
#include <vtextension/ExtensionError.hpp>
#include <vtextension/wasm/WasmRuntime.hpp>

namespace vtextension::detail
{

/// @return A range of guest memory a runtime answered, as the codec sees it: a range the guest named
///         outside its memory traps; a host that died meanwhile is the host's fault, and counts as such.
/// @param range The runtime's answer.
[[nodiscard]] inline abi::Result<std::span<std::byte>> guestRange(
    std::expected<std::span<std::byte>, wasm::MemoryFailure> range) noexcept
{
    return range.transform_error([](wasm::MemoryFailure failure) {
        switch (failure)
        {
            case wasm::MemoryFailure::OutOfBounds: return ExtensionError::Trapped;
            case wasm::MemoryFailure::HostCrashed: return ExtensionError::HostUnavailable;
        }
        return ExtensionError::Trapped;
    });
}

/// Reads guest memory as IGuestMemory::read() must: one copy, of a range @p memory's writable() bounds by the
/// size the memory has when it is taken (a shared memory's acknowledged size), however the guest's side
/// writes or grows it meanwhile.
/// @param memory The memory; its writable() proves the range.
/// @param address Where the copy starts.
/// @param into Where it goes; its size is how many bytes are copied.
/// @return Nothing once copied; `Trapped` if any byte lies outside the memory, or what writable() failed
/// with.
template <typename Memory>
[[nodiscard]] abi::Result<void> readOnce(Memory& memory, abi::Address address, std::span<std::byte> into)
{
    if (into.size() > std::numeric_limits<uint32_t>::max())
        return std::unexpected(ExtensionError::Trapped);
    return memory.writable(address, static_cast<uint32_t>(into.size()))
        .transform([&](std::span<std::byte> bytes) {
            if (!bytes.empty())
                std::memcpy(into.data(), bytes.data(), bytes.size());
        });
}

} // namespace vtextension::detail
