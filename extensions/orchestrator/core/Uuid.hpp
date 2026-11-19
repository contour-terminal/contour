// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace orchestrator
{

/// Where fresh random bytes for a UUID come from: the guest's WASI entropy, a counting fake in tests. The
/// core reads no randomness itself.
class IUuidSource
{
  public:
    IUuidSource() = default;
    IUuidSource(IUuidSource const&) = delete;
    IUuidSource(IUuidSource&&) = delete;
    IUuidSource& operator=(IUuidSource const&) = delete;
    IUuidSource& operator=(IUuidSource&&) = delete;
    virtual ~IUuidSource() = default;

    /// @return 16 bytes no earlier call returned.
    [[nodiscard]] virtual std::array<uint8_t, 16> next() = 0;
};

/// A version 4 (random) UUID in its canonical form.
/// @param bytes The UUID's 16 bytes; its version nibble is set to 4 and its variant bits to `10`.
/// @return Such as `123e4567-e89b-42d3-a456-426614174000`, in lower case.
[[nodiscard]] std::string formatUuidV4(std::array<uint8_t, 16> bytes);

} // namespace orchestrator
