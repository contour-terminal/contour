// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

namespace vtbackend
{

/// Why a captured terminal state could not be decoded or restored. @see vtbackend/state/StateCodec.hpp
enum class StateDecodeError : uint8_t
{
    Truncated = 0,        ///< The stream ends inside a value or a frame.
    UnknownFormatVersion, ///< The stream's format is newer than this build, or older than it reads.
    MalformedRecord,      ///< A frame is not well-formed: a field twice, a kind that does not fit its type.
    InvalidValue,         ///< A value is outside the range of what it names.
    TooLarge,             ///< A value asks for more than this build gives a terminal.
};

} // namespace vtbackend
