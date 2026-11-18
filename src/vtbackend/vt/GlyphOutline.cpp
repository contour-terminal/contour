// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/vt/GlyphOutline.hpp>

#include <algorithm>
#include <array>
#include <optional>
#include <ranges>
#include <utility>

namespace vtbackend
{

namespace
{
    // The simple-glyph flag bits, as the OpenType `glyf` table defines them.
    constexpr auto OnCurvePoint = uint8_t { 0x01 };
    constexpr auto XShortVector = uint8_t { 0x02 };
    constexpr auto YShortVector = uint8_t { 0x04 };
    constexpr auto RepeatFlag = uint8_t { 0x08 };
    constexpr auto XIsSameOrPositive = uint8_t { 0x10 };
    constexpr auto YIsSameOrPositive = uint8_t { 0x20 };

    /// The record's fixed header: numberOfContours and the four bounds, two bytes each.
    constexpr auto HeaderSize = size_t { 10 };

    /// Reads big-endian integers off the front of a record.
    class Reader
    {
      public:
        explicit Reader(std::span<uint8_t const> data) noexcept: _data { data } {}

        [[nodiscard]] std::optional<uint8_t> u8() noexcept
        {
            if (_data.empty())
                return std::nullopt;
            auto const value = _data.front();
            _data = _data.subspan(1);
            return value;
        }

        [[nodiscard]] std::optional<uint16_t> u16() noexcept
        {
            if (_data.size() < 2)
                return std::nullopt;
            auto const value = static_cast<uint16_t>((_data[0] << 8) | _data[1]);
            _data = _data.subspan(2);
            return value;
        }

        [[nodiscard]] std::optional<int16_t> i16() noexcept
        {
            return u16().transform([](uint16_t value) { return static_cast<int16_t>(value); });
        }

      private:
        std::span<uint8_t const> _data;
    };

    /// How one axis's deltas are encoded in a point's flag byte.
    struct AxisEncoding
    {
        uint8_t shortVector;                     ///< Set: the delta is one unsigned byte.
        uint8_t sameOrPositive;                  ///< With shortVector, the byte's sign; without, "no delta".
        int32_t GlyphOutlinePoint::* coordinate; ///< Where the running position is stored.
    };

    constexpr auto Axes = std::array {
        AxisEncoding { .shortVector = XShortVector,
                       .sameOrPositive = XIsSameOrPositive,
                       .coordinate = &GlyphOutlinePoint::x },
        AxisEncoding { .shortVector = YShortVector,
                       .sameOrPositive = YIsSameOrPositive,
                       .coordinate = &GlyphOutlinePoint::y },
    };

    /// Reads one axis's deltas for every point and stores the running positions.
    /// @return Whether the record held a delta for every point that needs one.
    [[nodiscard]] std::expected<void, GlyphRegisterError> readAxis(Reader& reader,
                                                                   AxisEncoding const& axis,
                                                                   std::span<uint8_t const> flags,
                                                                   std::span<GlyphOutlinePoint> points)
    {
        auto position = int32_t { 0 };
        for (auto const index: std::views::iota(size_t { 0 }, points.size()))
        {
            auto const flag = flags[index];
            if ((flag & axis.shortVector) != 0)
            {
                auto const delta = reader.u8();
                if (!delta)
                    return std::unexpected { GlyphRegisterError::MalformedPayload };
                position += (flag & axis.sameOrPositive) != 0 ? int32_t { *delta } : -int32_t { *delta };
            }
            else if ((flag & axis.sameOrPositive) == 0)
            {
                auto const delta = reader.i16();
                if (!delta)
                    return std::unexpected { GlyphRegisterError::MalformedPayload };
                position += *delta;
            }
            points[index].*axis.coordinate = position;
        }
        return {};
    }
} // namespace

GlyphOutlineBounds boundsOf(std::span<GlyphOutlinePoint const> points) noexcept
{
    if (points.empty())
        return {};
    auto const [xMin, xMax] = std::ranges::minmax(points, {}, &GlyphOutlinePoint::x);
    auto const [yMin, yMax] = std::ranges::minmax(points, {}, &GlyphOutlinePoint::y);
    return { .xMin = xMin.x, .yMin = yMin.y, .xMax = xMax.x, .yMax = yMax.y };
}

std::string_view toString(GlyphRegisterError error) noexcept
{
    switch (error)
    {
        case GlyphRegisterError::OutOfNamespace: return "out_of_namespace";
        case GlyphRegisterError::CompositeUnsupported: return "composite_unsupported";
        case GlyphRegisterError::HintingUnsupported: return "hinting_unsupported";
        case GlyphRegisterError::MalformedPayload: return "malformed_payload";
        case GlyphRegisterError::PayloadTooLarge: return "payload_too_large";
        case GlyphRegisterError::OutlineTooLarge: return "outline_too_large";
    }
    std::unreachable();
}

std::expected<GlyphOutline, GlyphRegisterError> decodeGlyf(std::span<uint8_t const> record)
{
    auto const malformed = std::unexpected { GlyphRegisterError::MalformedPayload };
    if (record.size() < HeaderSize)
        return malformed;

    auto reader = Reader { record };
    auto const contourCount = reader.i16().value_or(0);
    if (contourCount < 0)
        return std::unexpected { GlyphRegisterError::CompositeUnsupported };
    reader = Reader { record.subspan(HeaderSize) };

    auto outline = GlyphOutline {};
    if (contourCount == 0)
        return outline;

    outline.contourEnds.reserve(static_cast<size_t>(contourCount));
    for ([[maybe_unused]] auto const _: std::views::iota(0, int { contourCount }))
    {
        auto const end = reader.u16();
        // The ends are strictly increasing: each contour holds at least one point.
        if (!end || (!outline.contourEnds.empty() && *end <= outline.contourEnds.back()))
            return malformed;
        outline.contourEnds.push_back(*end);
    }

    // Judged before a point is stored: the wire form is compressed, and the count is all it takes to bound
    // what decoding it would allocate.
    auto const pointCount = size_t { outline.contourEnds.back() } + 1;
    if (pointCount > MaxGlyphOutlinePoints)
        return std::unexpected { GlyphRegisterError::OutlineTooLarge };

    auto const instructionLength = reader.u16();
    if (!instructionLength)
        return malformed;
    if (*instructionLength != 0)
        return std::unexpected { GlyphRegisterError::HintingUnsupported };

    auto flags = std::vector<uint8_t> {};
    flags.reserve(pointCount);
    while (flags.size() < pointCount)
    {
        auto const flag = reader.u8();
        if (!flag)
            return malformed;
        auto repeats = size_t { 1 };
        if ((*flag & RepeatFlag) != 0)
        {
            auto const extra = reader.u8();
            if (!extra)
                return malformed;
            repeats += *extra;
        }
        if (repeats > pointCount - flags.size())
            return malformed;
        flags.insert(flags.end(), repeats, *flag);
    }

    outline.points.resize(pointCount);
    for (auto const& axis: Axes)
        if (auto const read = readAxis(reader, axis, flags, outline.points); !read)
            return std::unexpected { read.error() };

    for (auto const index: std::views::iota(size_t { 0 }, pointCount))
        outline.points[index].kind =
            (flags[index] & OnCurvePoint) != 0 ? GlyphPointKind::OnCurve : GlyphPointKind::OffCurve;

    outline.bounds = boundsOf(outline.points);
    return outline;
}

} // namespace vtbackend
