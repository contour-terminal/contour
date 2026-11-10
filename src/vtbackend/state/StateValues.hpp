// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/state/StateCodec.hpp>

#include <core/codec/Fields.hpp>

#include <cstdint>
#include <expected>
#include <utility>

/// The value-level helpers the state codecs of the individual aggregates share.
namespace vtbackend::detail
{

/// A decode failure, as the unexpected a decoder returns.
using StateFailure = std::unexpected<StateDecodeError>;

using core::codec::narrow;
using core::codec::narrowSigned;

} // namespace vtbackend::detail
