// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/codec/DecodeError.hpp>

namespace vtbackend
{

/// Why a captured terminal state could not be decoded or restored: core::codec's decode failure.
/// @see vtbackend/state/StateCodec.hpp
using StateDecodeError = core::codec::DecodeError;

} // namespace vtbackend
