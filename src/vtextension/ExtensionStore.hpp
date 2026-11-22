// SPDX-License-Identifier: Apache-2.0
//
// The durable key/value store of a daemon-world extension, as its host-function table reaches it
// (`store-get`, `store-set`, `store-remove`, `store-keys`): an interface, so that the table is tested with a
// fake and the store behind it without a guest.
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <vtextension/DaemonWorld.hpp>

namespace vtextension
{

/// Why the store refused a call.
enum class StoreError : uint8_t
{
    BadKey,      ///< The key is not of the store's key grammar, or is longer than its limit.
    TooLarge,    ///< The value is larger than the store's limit.
    TooManyKeys, ///< A new key would exceed the number of keys the store holds.
    Io,          ///< The storage failed; a write that reports it is not known to be durable.
    Busy,        ///< Another store holds the directory: open() only.
};

/// How much one extension's store may hold.
struct StoreLimits
{
    std::size_t maxValueBytes = 64 << 10; ///< Bytes of one value.
    std::size_t maxKeys = 1024;           ///< Keys held at once.
    std::size_t maxKeyBytes = 128;        ///< Bytes of one key.
};

/// One extension's durable key/value store.
///
/// A key matches `[a-z0-9/._-]{1,maxKeyBytes}`, does not start with `/`, and has no empty component; no
/// component ends in `.` (so none is `.` or `..`), contains `.tmp-`, or is a name Windows reserves for a
/// device
/// (`con`, `prn`, `aux`, `nul`, `com1`–`com9`, `lpt1`–`lpt9`), alone or before a `.`. Any other key is
/// refused with `BadKey`. The grammar is the same on every platform, so a guest's keys are portable.
class IExtensionStore
{
  public:
    virtual ~IExtensionStore() = default;

    /// Reads a value.
    /// @param key The key.
    /// @return The value, or nothing if the key holds none; `BadKey` or `Io` otherwise.
    [[nodiscard]] virtual std::expected<std::optional<std::string>, StoreError> get(
        std::string_view key) const = 0;

    /// Writes a value, replacing the key's previous one. It is durable when the call returns: an error means
    /// the write is not known to have reached the disk, never that it did.
    /// @param key The key.
    /// @param value The value.
    /// @return Nothing; or `BadKey`, `TooLarge`, `TooManyKeys` or `Io`.
    [[nodiscard]] virtual std::expected<void, StoreError> set(std::string_view key,
                                                              std::string_view value) = 0;

    /// Removes a value durably. A key that holds none is removed already.
    /// @param key The key.
    /// @return Nothing; or `BadKey` or `Io`.
    [[nodiscard]] virtual std::expected<void, StoreError> remove(std::string_view key) = 0;

    /// Lists keys.
    /// @param prefix What the keys listed start with; empty for all of them.
    /// @return The keys that start with @p prefix, sorted; or `Io`.
    [[nodiscard]] virtual std::expected<std::vector<std::string>, StoreError> keys(
        std::string_view prefix) const = 0;

  protected:
    IExtensionStore() = default;
    IExtensionStore(IExtensionStore const&) = default;
    IExtensionStore(IExtensionStore&&) = default;
    IExtensionStore& operator=(IExtensionStore const&) = default;
    IExtensionStore& operator=(IExtensionStore&&) = default;
};

/// One row of the table that turns a StoreError into what the guest sees.
struct StoreErrorRow
{
    StoreError error;    ///< The store's reason.
    HostError hostError; ///< The WIT host-error it reaches the guest as.
};

/// One row per error, indexed by value: the WIT host-error it reaches the guest as. A failing disk is `gone`:
/// the guest learns the write did not happen, and carries on. `Busy` comes from opening a store, before any
/// guest call; its row exists for the table's completeness.
inline constexpr auto StoreErrorToHostError = std::array {
    StoreErrorRow { .error = StoreError::BadKey, .hostError = HostError::BadArgument },
    StoreErrorRow { .error = StoreError::TooLarge, .hostError = HostError::Limit },
    StoreErrorRow { .error = StoreError::TooManyKeys, .hostError = HostError::Limit },
    StoreErrorRow { .error = StoreError::Io, .hostError = HostError::Gone },
    StoreErrorRow { .error = StoreError::Busy, .hostError = HostError::Gone },
};

static_assert(StoreErrorToHostError.size() == std::to_underlying(StoreError::Busy) + 1U,
              "every StoreError has a row");
static_assert(std::ranges::all_of(std::views::iota(std::size_t { 0 }, StoreErrorToHostError.size()),
                                  [](std::size_t index) {
                                      return std::to_underlying(StoreErrorToHostError.at(index).error)
                                             == index;
                                  }),
              "StoreErrorToHostError is indexed by the enumerator's value");

/// @param error Why the store refused.
/// @return The host-error the guest is answered with, by StoreErrorToHostError.
[[nodiscard]] constexpr HostError hostErrorOf(StoreError error) noexcept
{
    return StoreErrorToHostError.at(std::to_underlying(error)).hostError;
}

} // namespace vtextension
