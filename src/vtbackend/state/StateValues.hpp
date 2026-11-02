// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/state/StateCodec.hpp>

#include <cstdint>
#include <expected>
#include <limits>
#include <utility>

/// The value-level helpers the state codecs of the individual aggregates share.
namespace vtbackend::detail
{

/// A decode failure, as the unexpected a decoder returns.
using StateFailure = std::unexpected<StateDecodeError>;

/// Narrows @p value to T.
/// @param value A decoded varint.
/// @return @p value as T, or InvalidValue when T cannot hold it.
template <typename T>
[[nodiscard]] std::expected<T, StateDecodeError> narrow(uint64_t value)
{
    if (value > static_cast<uint64_t>(std::numeric_limits<T>::max()))
        return StateFailure { StateDecodeError::InvalidValue };
    return static_cast<T>(value);
}

/// Narrows a signed @p value to T.
/// @param value A decoded signed varint.
/// @return @p value as T, or InvalidValue when T cannot hold it.
template <typename T>
[[nodiscard]] std::expected<T, StateDecodeError> narrowSigned(int64_t value)
{
    if (value < static_cast<int64_t>(std::numeric_limits<T>::min())
        || value > static_cast<int64_t>(std::numeric_limits<T>::max()))
        return StateFailure { StateDecodeError::InvalidValue };
    return static_cast<T>(value);
}

} // namespace vtbackend::detail
