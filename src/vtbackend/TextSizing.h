// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <expected>
#include <string_view>

namespace vtbackend::text_sizing
{

/// The largest `s` the protocol allows -- and therefore the most lines a block can ever span.
inline constexpr uint8_t MaxScale = 7;

/// The largest `w` the protocol allows.
inline constexpr uint8_t MaxWidth = 7;

/// One decoded `OSC 66` (kitty text sizing protocol) request.
///
/// `OSC 66 ; <key>=<value>:<key>=<value>... ; <text> ST`
///
/// Field names follow the protocol's vocabulary rather than the single letters on the wire.
struct Request
{
    /// `s` -- the scale, 1..7. The text is rendered in a block `scale` cells tall.
    uint8_t scale = 1;

    /// `w` -- the width in cells, 0..7.
    ///
    /// When non-zero the text occupies exactly `scale * width` cells. When zero the terminal splits
    /// the text as it normally would, and each resulting cell becomes a `scale` x `scale` block.
    uint8_t width = 0;

    /// `n` / `d` -- fractional scale as a numerator over a denominator, each 0..15.
    ///
    /// Fractional scaling changes how large the glyph is DRAWN but never how many cells it occupies,
    /// which is why it does not enter any of the layout arithmetic below.
    uint8_t numerator = 0;
    uint8_t denominator = 0;

    /// `v` / `h` -- alignment of a fractionally-scaled glyph within its cell block.
    /// 0 = top/left, 1 = bottom/right, 2 = centered. Meaningless without a fractional scale.
    uint8_t verticalAlignment = 0;
    uint8_t horizontalAlignment = 0;

    /// The text to lay out, exactly as it arrived.
    std::string_view text;

    /// @return How many columns @p textWidth columns' worth of text occupies under this request.
    ///
    /// With an explicit `w`, the answer is `scale * width` regardless of what the text actually is --
    /// the application is stating the size it wants, not asking. Without one, each cell the text
    /// would normally occupy becomes a `scale`-wide block.
    [[nodiscard]] constexpr unsigned columnsFor(unsigned textWidth) const noexcept
    {
        return width != 0 ? static_cast<unsigned>(scale) * width : static_cast<unsigned>(scale) * textWidth;
    }
};

/// Why an `OSC 66` request could not be decoded.
enum class Error : uint8_t
{
    MalformedKey,    ///< A key=value pair was not shaped like one.
    ValueOutOfRange, ///< A key carried a value outside the range the protocol allows.
    BadFraction,     ///< `d` was non-zero and not greater than `n`.
};

/// Decodes the metadata and text of one OSC 66 request.
///
/// @param payload the OSC body WITHOUT its leading "66;", i.e. `<metadata>;<text>`.
/// @return the decoded request, or the reason it could not be decoded.
[[nodiscard]] std::expected<Request, Error> parseRequest(std::string_view payload);

} // namespace vtbackend::text_sizing
