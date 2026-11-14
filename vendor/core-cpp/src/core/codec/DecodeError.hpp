// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

namespace core::codec
{

/// Why a stream of the field codec could not be decoded.
enum class DecodeError : uint8_t
{
    Truncated = 0,        ///< The stream ends inside a value or a frame.
    UnknownFormatVersion, ///< The stream's format is one this reader does not speak. Raised by the caller's
                          ///< own header check; the codec itself carries no header.
    MalformedRecord,      ///< A frame is not well-formed: a field twice, a kind that does not fit its type.
    InvalidValue,         ///< A value is outside the range of what it names.
    TooLarge,             ///< A value asks for more memory than the stream's size allows.
};

} // namespace core::codec
