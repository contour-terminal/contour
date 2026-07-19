// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/TextSizing.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <optional>

namespace vtbackend::text_sizing
{

namespace
{
    /// @return the parsed value, or nullopt when @p value is not a plain number.
    [[nodiscard]] std::optional<unsigned> parseNumber(std::string_view value) noexcept
    {
        if (value.empty())
            return std::nullopt;
        auto parsed = 0u;
        auto const* const last = value.data() + value.size();
        auto const [ptr, ec] = std::from_chars(value.data(), last, parsed);
        if (ec != std::errc {} || ptr != last)
            return std::nullopt;
        return parsed;
    }

    /// The largest value `n` and `d` may carry.
    constexpr unsigned MaxFractionTerm = 15;

    /// The largest value `v` and `h` may carry: 0 = top/left, 1 = bottom/right, 2 = centered.
    constexpr unsigned MaxAlignment = 2;

    /// One key the protocol defines, and the field of Request its value lands in.
    struct KeyDefinition
    {
        std::string_view key;     ///< The key as it appears on the wire. One letter, so far.
        uint8_t Request::* field; ///< Where the parsed value is stored.
        unsigned lo;              ///< Smallest value the protocol allows for this key.
        unsigned hi;              ///< Largest value the protocol allows for this key.
    };

    /// Every key `OSC 66` currently defines. A letter absent from this table is one a later revision
    /// of the protocol introduced; applyKey() ignores it. Supporting a new key is adding a row here.
    constexpr auto KeyDefinitions = std::array {
        KeyDefinition { .key = "s", .field = &Request::scale, .lo = 1, .hi = MaxScale },
        KeyDefinition { .key = "w", .field = &Request::width, .lo = 0, .hi = MaxWidth },
        KeyDefinition { .key = "n", .field = &Request::numerator, .lo = 0, .hi = MaxFractionTerm },
        KeyDefinition { .key = "d", .field = &Request::denominator, .lo = 0, .hi = MaxFractionTerm },
        KeyDefinition { .key = "v", .field = &Request::verticalAlignment, .lo = 0, .hi = MaxAlignment },
        KeyDefinition { .key = "h", .field = &Request::horizontalAlignment, .lo = 0, .hi = MaxAlignment },
    };

    /// Applies one `key=value` pair to @p request, rejecting values the protocol does not allow.
    ///
    /// @param key the key as it appeared on the wire, which need not be one of the letters this
    ///            terminal knows -- and need not be a single character at all.
    [[nodiscard]] std::expected<void, Error> applyKey(Request& request,
                                                      std::string_view key,
                                                      std::string_view value)
    {
        auto const definition = std::ranges::find(KeyDefinitions, key, &KeyDefinition::key);

        // An unknown key is ignored rather than rejected, so that a future addition to the protocol
        // does not cost an older terminal the parts it does understand. Its value is not even parsed:
        // a later revision is free to give a new key a non-numeric value, and refusing the whole
        // request over a value this terminal was never going to read defeats the allowance entirely.
        if (definition == KeyDefinitions.end())
            return {};

        auto const number = parseNumber(value);
        if (!number)
            return std::unexpected(Error::MalformedKey);
        if (*number < definition->lo || *number > definition->hi)
            return std::unexpected(Error::ValueOutOfRange);

        request.*(definition->field) = static_cast<uint8_t>(*number);
        return {};
    }
} // namespace

std::expected<Request, Error> parseRequest(std::string_view payload)
{
    auto request = Request {};

    // The text is separated from the metadata by the FIRST semicolon. Semicolons in the text are not
    // separators, which is why this is a find rather than a split.
    auto const separator = payload.find(';');
    auto const metadata = payload.substr(0, separator);
    if (separator != std::string_view::npos)
        request.text = payload.substr(separator + 1);

    // Metadata pairs are separated by colons, not semicolons -- unusual for an OSC, and easy to get
    // wrong by pattern-matching against the other sequences in this file.
    for (size_t offset = 0; offset < metadata.size();)
    {
        auto end = metadata.find(':', offset);
        if (end == std::string_view::npos)
            end = metadata.size();

        auto const pair = metadata.substr(offset, end - offset);
        offset = end + 1;

        if (pair.empty())
            continue;

        // Split at the FIRST `=` rather than assuming the key is one character: every key the
        // protocol defines today is a single letter, but insisting on that here rejects the whole
        // request the moment a later revision adds a longer one -- the same forward compatibility
        // applyKey() is careful to preserve, given away one layer above it.
        auto const assignment = pair.find('=');
        if (assignment == std::string_view::npos || assignment == 0)
            return std::unexpected(Error::MalformedKey);

        if (auto const result = applyKey(request, pair.substr(0, assignment), pair.substr(assignment + 1));
            !result)
            return std::unexpected(result.error());
    }

    // A fraction of n/d only means anything when it is a proper one.
    if (request.denominator != 0 && request.denominator <= request.numerator)
        return std::unexpected(Error::BadFraction);

    return request;
}

} // namespace vtbackend::text_sizing
