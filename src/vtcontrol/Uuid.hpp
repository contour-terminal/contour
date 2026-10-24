// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/platform/Clock.hpp>

#include <array>
#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace vtcontrol
{

/// A 128-bit universally unique identifier.
struct Uuid
{
    std::array<uint8_t, 16> bytes {}; ///< The identifier in network byte order.

    auto operator<=>(Uuid const&) const = default;
};

/// @param uuid The identifier.
/// @return Its canonical text: lower-case hex in the 8-4-4-4-12 grouping.
[[nodiscard]] std::string toString(Uuid const& uuid);

/// @param text Hex digits in either case, with the four dashes.
/// @return The identifier, or nothing when `text` is not one.
[[nodiscard]] std::optional<Uuid> parseUuid(std::string_view text);

/// Lays out a UUIDv7 (RFC 9562 section 5.7).
/// @param now The Unix time in milliseconds, which fills the first 48 bits.
/// @param random The bits that follow: the first two bytes carry `rand_a` (low 12 bits used), the
///        remaining eight carry `rand_b` (low 62 bits used).
/// @return The identifier, with version 7 and variant 0b10 set.
[[nodiscard]] Uuid makeUuidV7(std::chrono::sys_time<std::chrono::milliseconds> now,
                              std::span<uint8_t const, 10> random) noexcept;

/// Where identifiers come from. Every id minted by the daemon goes through this seam.
class UuidSource
{
  public:
    UuidSource() = default;
    UuidSource(UuidSource const&) = delete;
    UuidSource(UuidSource&&) = delete;
    UuidSource& operator=(UuidSource const&) = delete;
    UuidSource& operator=(UuidSource&&) = delete;
    virtual ~UuidSource() = default;

    /// @return A fresh identifier, never one returned before.
    [[nodiscard]] virtual Uuid next() = 0;
};

/// A UUIDv7 source that is monotonic within one process (RFC 9562 section 6.2, method 1).
///
/// Within one millisecond a 12-bit counter in `rand_a` counts up; when it would overflow, the
/// timestamp is advanced by one millisecond instead. Thread-safe.
class UuidV7Source final: public UuidSource
{
  public:
    /// Supplies 64 random bits per call.
    using RandomBits = std::function<uint64_t()>;

    /// @param clock The wall clock the timestamps are read from; it must outlive the source.
    /// @param bits The random bits.
    UuidV7Source(core::platform::WallClockRef clock, RandomBits bits);

    /// @return The next identifier, greater than every one before it.
    [[nodiscard]] Uuid next() override;

  private:
    core::platform::WallClockRef _clock;
    RandomBits _bits;
    std::mutex _mutex;
    std::chrono::sys_time<std::chrono::milliseconds> _lastMillis {};
    uint32_t _counter = 0;
};

/// @return A source over the system clock and `std::random_device`.
[[nodiscard]] std::shared_ptr<UuidSource> systemUuidSource();

} // namespace vtcontrol

template <>
struct std::hash<vtcontrol::Uuid> // NOLINT(readability-identifier-naming)
{
    /// @param uuid The identifier.
    /// @return A hash of its 16 bytes.
    [[nodiscard]] std::size_t operator()(
        vtcontrol::Uuid const& uuid) const noexcept; // NOLINT(readability-identifier-naming)
};
