// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/TextSizing.h>

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

    /// Applies one `key=value` pair to @p request, rejecting values the protocol does not allow.
    [[nodiscard]] std::expected<void, Error> applyKey(Request& request, char key, std::string_view value)
    {
        auto const number = parseNumber(value);
        if (!number)
            return std::unexpected(Error::MalformedKey);

        auto const assign = [&](uint8_t& target, unsigned lo, unsigned hi) -> std::expected<void, Error> {
            if (*number < lo || *number > hi)
                return std::unexpected(Error::ValueOutOfRange);
            target = static_cast<uint8_t>(*number);
            return {};
        };

        switch (key)
        {
            case 's': return assign(request.scale, 1, MaxScale);
            case 'w': return assign(request.width, 0, MaxWidth);
            case 'n': return assign(request.numerator, 0, 15);
            case 'd': return assign(request.denominator, 0, 15);
            case 'v': return assign(request.verticalAlignment, 0, 2);
            case 'h': return assign(request.horizontalAlignment, 0, 2);
            default:
                // Unknown key: ignored rather than rejected, so that a future addition to the
                // protocol does not cost an older terminal the parts it does understand.
                return {};
        }
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
        if (pair.size() < 3 || pair[1] != '=')
            return std::unexpected(Error::MalformedKey);

        if (auto const result = applyKey(request, pair[0], pair.substr(2)); !result)
            return std::unexpected(result.error());
    }

    // A fraction of n/d only means anything when it is a proper one.
    if (request.denominator != 0 && request.denominator <= request.numerator)
        return std::unexpected(Error::BadFraction);

    return request;
}

} // namespace vtbackend::text_sizing
