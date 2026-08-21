// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/vt/Screenshot.hpp>

#include <crispy/Base64.hpp>
#include <crispy/Utils.hpp>

#include <algorithm>
#include <charconv>
#include <format>
#include <limits>
#include <memory>
#include <optional>
#include <system_error>
#include <vector>

using std::string_view;

namespace vtbackend::screenshot
{

namespace
{
    /// A parameter that was omitted or given empty. Distinct from a parameter that was given as zero,
    /// which the coordinates fold onto their default but which is a legitimate id and a legitimate
    /// format.
    constexpr auto Absent = std::optional<uint32_t> { std::nullopt };

    /// Reads one `;`-separated parameter as a non-negative integer.
    ///
    /// A value too large for uint32_t saturates rather than failing: an application writing a large
    /// number for the bottom-right corner is saying "the edge", and the region is clamped to the page
    /// anyway. A value that is not a run of digits is an error -- there is nothing to guess at.
    ///
    /// @param text The parameter, which may be empty.
    /// @return The value, Absent if @p text was empty, or Status::Malformed.
    [[nodiscard]] std::expected<std::optional<uint32_t>, Status> readParameter(string_view text) noexcept
    {
        if (text.empty())
            return Absent;

        auto value = uint32_t {};
        auto const* const first = std::to_address(text.begin());
        auto const* const last = std::to_address(text.end());
        auto const [ptr, ec] = std::from_chars(first, last, value);

        if (ec == std::errc::result_out_of_range && ptr == last)
            return std::optional { std::numeric_limits<uint32_t>::max() };
        if (ec != std::errc {} || ptr != last)
            return std::unexpected { Status::Malformed };

        return std::optional { value };
    }

    /// @param fields The parameters read from the payload.
    /// @param index  Which parameter to read.
    /// @return The parameter's value, or Absent when it was not given at all.
    [[nodiscard]] std::expected<std::optional<uint32_t>, Status> parameterAt(
        std::vector<string_view> const& fields, size_t index) noexcept
    {
        if (index >= fields.size())
            return Absent;
        return readParameter(fields[index]);
    }
} // namespace

std::expected<Request, Rejection> parseRequest(string_view payload, PageSize page)
{
    // Pid, Pt, Pl, Pb, Pr, Pf -- and nothing after them.
    constexpr auto MaxFields = size_t { 6 };

    // A trailing empty parameter is dropped rather than kept, which parameterAt() cannot tell apart
    // from one that was never given -- and neither can the grammar, so the two mean the same thing.
    auto const fields = crispy::split(payload, ';');

    // The id is read FIRST, before the arity is judged: a request with one parameter too many still
    // names itself perfectly well, and every reply -- refusals included -- promises to echo Pid back.
    // Only an unreadable id leaves nothing to correlate against.
    auto const id = parameterAt(fields, 0);
    if (!id)
        return std::unexpected { Rejection { .id = 0, .status = id.error() } };

    // Every rejection from here on can name the request it rejects.
    auto const requestId = id->value_or(0);
    auto const reject = [requestId](Status status) {
        return std::unexpected { Rejection { .id = requestId, .status = status } };
    };

    if (fields.size() > MaxFields)
        return reject(Status::Malformed);

    auto const lines = static_cast<uint32_t>(unbox(page.lines));
    auto const columns = static_cast<uint32_t>(unbox(page.columns));

    // One-based, defaulting when omitted, empty or zero, and never past the page's edge -- the same
    // reading impl::readRectangularArea() gives the four coordinates of a DEC rectangular-area
    // sequence. That function cannot be called here because it reads CSI parameters, not a
    // `;`-separated body, but the semantics it documents are the ones implemented here.
    auto const coordinate = [&](size_t index, uint32_t fallback, uint32_t limit) {
        return parameterAt(fields, index).transform([&](std::optional<uint32_t> value) {
            return std::min(value.value_or(0) == 0 ? fallback : *value, limit);
        });
    };

    auto const top = coordinate(1, 1, lines);
    if (!top)
        return reject(top.error());
    auto const left = coordinate(2, 1, columns);
    if (!left)
        return reject(left.error());
    auto const bottom = coordinate(3, lines, lines);
    if (!bottom)
        return reject(bottom.error());
    auto const right = coordinate(4, columns, columns);
    if (!right)
        return reject(right.error());

    auto const format = parameterAt(fields, 5);
    if (!format)
        return reject(format.error());
    auto const formatValue = format->value_or(static_cast<uint32_t>(Format::PlainText));
    if (formatValue > std::numeric_limits<uint8_t>::max())
        return reject(Status::UnsupportedFormat);
    auto const requestedFormat = static_cast<Format>(formatValue);
    if (!isSupported(requestedFormat))
        return reject(Status::UnsupportedFormat);

    // An inverted region names no cell. Reporting that beats replying with an empty screenshot, which
    // an application could not tell apart from a screenshot of blank cells.
    if (*top > *bottom || *left > *right)
        return reject(Status::EmptyRegion);

    return Request {
        .id = requestId,
        // Zero-based, as every consumer of Rect expects.
        .area = Rect { .top = Top::cast_from(*top - 1),
                       .left = Left::cast_from(*left - 1),
                       .bottom = Bottom::cast_from(*bottom - 1),
                       .right = Right::cast_from(*right - 1) },
        .format = requestedFormat,
    };
}

void writeReply(Request const& request, string_view content, Sink const& sink)
{
    // One-based on the wire, in the units the request used, so a reply can be read without knowing
    // what the terminal defaulted the request to.
    auto const header = std::format("\033^{};{};{};{};{};{};{};{};",
                                    Code,
                                    request.id,
                                    static_cast<unsigned>(Status::Data),
                                    unbox(request.area.top) + 1,
                                    unbox(request.area.left) + 1,
                                    unbox(request.area.bottom) + 1,
                                    unbox(request.area.right) + 1,
                                    static_cast<unsigned>(request.format));

    auto remaining = content;
    while (!remaining.empty())
    {
        auto const chunk = remaining.substr(0, std::min(MaxChunkSize, remaining.size()));
        remaining.remove_prefix(chunk.size());
        sink(std::format("{}{}\033\\", header, crispy::base64::encode(chunk)));
    }

    // The terminator has exactly the shape of a status-only message, which is what writeError writes.
    writeError(request.id, Status::EndOfData, sink);
}

void writeError(uint32_t requestId, Status status, Sink const& sink)
{
    sink(std::format("\033^{};{};{}\033\\", Code, requestId, static_cast<unsigned>(status)));
}

} // namespace vtbackend::screenshot
