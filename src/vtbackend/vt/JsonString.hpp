// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>
#include <string_view>

namespace vtbackend
{

/// Spells @p text as a JSON string (RFC 8259 §7), quotes included, for a reply written to the PTY.
///
/// Every control code is escaped -- as `\uXXXX`, or as the short form JSON has for it -- not only the C0 ones
/// JSON requires: DEL and C1 as well, since such a reply travels in a DCS, which a C1-aware reader ends at
/// U+009C (ST). What is not UTF-8 is decoded as the parser decodes text, an invalid sequence becoming U+FFFD.
/// @param text UTF-8 text.
/// @return The JSON string.
[[nodiscard]] std::string jsonString(std::string_view text);

} // namespace vtbackend
