// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/state/GridState.hpp>

#include <vtbackend/grid/Grid.hpp>
#include <vtbackend/state/StateFields.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <type_traits>
#include <utility>

namespace vtbackend
{

// {{{ RenditionTableBuilder
size_t RenditionTableBuilder::Hash::operator()(GraphicsAttributes const& rendition) const noexcept
{
    auto const mix = [](uint64_t seed, uint64_t value) {
        constexpr auto Golden = uint64_t { 0x9E37'79B9'7F4A'7C15 };
        return seed ^ (value + Golden + (seed << 6U) + (seed >> 2U));
    };
    auto seed = uint64_t { rendition.foregroundColor.content };
    seed = mix(seed, rendition.backgroundColor.content);
    seed = mix(seed, rendition.underlineColor.content);
    seed = mix(seed, rendition.flags.value());
    return static_cast<size_t>(seed);
}

uint64_t RenditionTableBuilder::indexOf(GraphicsAttributes const& rendition)
{
    auto const [it, inserted] = _indexOf.try_emplace(rendition, _table.size());
    if (inserted)
        _table.push_back(rendition);
    return it->second;
}
// }}}

namespace
{
    using detail::fieldKey;
    using detail::StateFailure;
    using detail::WireKind;

    /// The highest Unicode scalar value a cell may hold.
    constexpr uint32_t MaxCodepoint = 0x10FFFF;

    // {{{ UTF-8
    // A cell holds any value up to U+10FFFF, surrogates included, so this is UTF-8 as its bit layout defines
    // it, without the scalar-value restriction: every value has exactly one encoding, and the decoder refuses
    // an overlong one.

    constexpr uint8_t ContinuationMark = 0b1000'0000;
    constexpr uint8_t ContinuationMask = 0b1100'0000;
    constexpr uint8_t ContinuationPayload = 0b0011'1111;
    constexpr unsigned ContinuationBits = 6;

    /// One row per encoded length: the lead byte's mark and payload mask, and the smallest value it encodes.
    struct Utf8Form
    {
        uint8_t leadMark;
        uint8_t leadMask;
        uint32_t minimum;
    };
    constexpr auto Utf8Forms = std::array {
        Utf8Form { .leadMark = 0b0000'0000, .leadMask = 0b1000'0000, .minimum = 0 },
        Utf8Form { .leadMark = 0b1100'0000, .leadMask = 0b1110'0000, .minimum = 0x80 },
        Utf8Form { .leadMark = 0b1110'0000, .leadMask = 0b1111'0000, .minimum = 0x800 },
        Utf8Form { .leadMark = 0b1111'0000, .leadMask = 0b1111'1000, .minimum = 0x1'0000 },
    };

    void putUtf8(StateWriter& out, char32_t codepoint)
    {
        auto const value = static_cast<uint32_t>(codepoint);
        auto length = size_t { 1 };
        while (length < Utf8Forms.size() && value >= Utf8Forms.at(length).minimum)
            ++length;
        auto const& form = Utf8Forms.at(length - 1);
        auto const trailing = static_cast<unsigned>(length - 1);
        out.u8(static_cast<uint8_t>(form.leadMark | (value >> (trailing * ContinuationBits))));
        for (auto const index: std::views::iota(0U, trailing) | std::views::reverse)
            out.u8(static_cast<uint8_t>(ContinuationMark
                                        | ((value >> (index * ContinuationBits)) & ContinuationPayload)));
    }

    [[nodiscard]] std::expected<char32_t, StateDecodeError> getUtf8(StateReader& in)
    {
        auto const lead = in.u8();
        if (!lead)
            return StateFailure { lead.error() };
        auto const form = std::ranges::find_if(Utf8Forms, [&](Utf8Form const& candidate) {
            return (*lead & candidate.leadMask) == candidate.leadMark;
        });
        if (form == Utf8Forms.end())
            return StateFailure { StateDecodeError::InvalidValue };
        auto value = static_cast<uint32_t>(*lead & ~form->leadMask);
        for ([[maybe_unused]] auto const index: std::views::iota(Utf8Forms.begin(), form))
        {
            auto const next = in.u8();
            if (!next)
                return StateFailure { next.error() };
            if ((*next & ContinuationMask) != ContinuationMark)
                return StateFailure { StateDecodeError::InvalidValue };
            value = (value << ContinuationBits) | (*next & ContinuationPayload);
        }
        if (value < form->minimum || value > MaxCodepoint)
            return StateFailure { StateDecodeError::InvalidValue };
        return static_cast<char32_t>(value);
    }
    // }}}

    // {{{ the fields of a line

    /// The wire ids of a line's fields. Appended to, never renumbered. A field holding its default -- zero,
    /// empty, every cell as a fresh cell has it -- is not written.
    enum class LineField : uint8_t
    {
        Columns = 1,
        Flags = 2,
        ContextId = 3,
        CommandEndOffset = 4,
        PromptEndOffset = 5,
        Revision = 6,
        Dirty = 7,
        BornAt = 8,
        BlockId = 9,
        StorageFlags = 10,
        UsedColumns = 11,
        Shape = 12,
        FillRendition = 13,
        Codepoints = 20, ///< UTF-8, up to the last cell holding one; present exactly on a materialized line.
        Widths = 21,
        Scales = 22,
        Renditions = 23,
        Hyperlinks = 24,
        TextScaleExtras = 25,
        ClusterSizes = 26,
        ClusterPoolIndices = 27,
        ClusterPool = 28,
        ImageFragments = 29,
    };

    [[nodiscard]] constexpr uint64_t keyOf(LineField field, WireKind kind) noexcept
    {
        return fieldKey(static_cast<uint32_t>(field), kind);
    }

    /// What a cell of a run-length encoded array is on the wire, and back.
    template <typename T>
    struct CellCodec
    {
        [[nodiscard]] static uint64_t toWire(T value, LineEncodeContext& /*context*/) noexcept
        {
            return static_cast<uint64_t>(value);
        }
        [[nodiscard]] static std::expected<T, StateDecodeError> fromWire(uint64_t value,
                                                                         LineDecodeContext const& /*context*/)
        {
            return detail::narrow<T>(value);
        }
    };

    template <>
    struct CellCodec<HyperlinkId>
    {
        [[nodiscard]] static uint64_t toWire(HyperlinkId value, LineEncodeContext& /*context*/) noexcept
        {
            return value.value;
        }
        [[nodiscard]] static std::expected<HyperlinkId, StateDecodeError> fromWire(
            uint64_t value, LineDecodeContext const& /*context*/)
        {
            return detail::narrow<uint16_t>(value).transform([](uint16_t id) { return HyperlinkId { id }; });
        }
    };

    template <>
    struct CellCodec<GraphicsAttributes>
    {
        [[nodiscard]] static uint64_t toWire(GraphicsAttributes const& value, LineEncodeContext& context)
        {
            return context.renditions.indexOf(value);
        }
        [[nodiscard]] static std::expected<GraphicsAttributes, StateDecodeError> fromWire(
            uint64_t value, LineDecodeContext const& context)
        {
            if (value >= context.renditions.size())
                return StateFailure { StateDecodeError::InvalidValue };
            return context.renditions[static_cast<size_t>(value)];
        }
    };

    /// What a cell nobody wrote holds in an array: the value a line materializes it to.
    template <typename T>
    using CellDefault = T (*)(LineSoA const& line, size_t column);

    /// Calls @p visit once per run-length encoded array of a line, with its field, its member and the value a
    /// cell holds there unless written: the one list of those arrays, so the encoder, the decoder and the
    /// fresh-line defaults cannot disagree. The codepoints and the cluster pool are not runs, and are coded
    /// on their own.
    template <typename Visitor>
    void forEachRunArray(Visitor const& visit)
    {
        visit(LineField::Widths, &LineSoA::widths, CellDefault<uint8_t> { [](LineSoA const&, size_t) {
                  return uint8_t { 1 };
              } });
        visit(LineField::Scales, &LineSoA::scales, CellDefault<uint8_t> { [](LineSoA const&, size_t) {
                  return uint8_t { 1 };
              } });
        visit(LineField::Renditions,
              &LineSoA::sgr,
              CellDefault<GraphicsAttributes> { [](LineSoA const& line, size_t) { return line.fillAttrs; } });
        visit(LineField::Hyperlinks,
              &LineSoA::hyperlinks,
              CellDefault<HyperlinkId> { [](LineSoA const&, size_t) { return HyperlinkId {}; } });
        visit(LineField::TextScaleExtras,
              &LineSoA::textScaleExtras,
              CellDefault<uint16_t> { [](LineSoA const&, size_t) { return uint16_t { 0 }; } });
        // A cell holds one codepoint when it holds any, so only a cluster or an erased cell is written.
        visit(LineField::ClusterSizes,
              &LineSoA::clusterSize,
              CellDefault<uint8_t> { [](LineSoA const& line, size_t column) {
                  return line.codepoints[column] != 0 ? uint8_t { 1 } : uint8_t { 0 };
              } });
        visit(LineField::ClusterPoolIndices,
              &LineSoA::clusterPoolIndex,
              CellDefault<uint16_t> { [](LineSoA const&, size_t) { return uint16_t { 0 }; } });
    }

    /// Writes @p cells as runs of equal values, unless every cell holds its default.
    template <typename T>
    void putRuns(StateWriter& out,
                 LineField field,
                 LineSoA const& line,
                 AlignedVector<T> const& cells,
                 CellDefault<T> defaultAt,
                 LineEncodeContext& context)
    {
        auto const columns = cells.size();
        auto const isDefault = [&](size_t column) {
            return cells[column] == defaultAt(line, column);
        };
        if (std::ranges::all_of(std::views::iota(size_t { 0 }, columns), isDefault))
            return;
        out.varint(keyOf(field, WireKind::Framed));
        auto const frame = out.beginFrame();
        auto runStart = size_t { 0 };
        for (auto const column: std::views::iota(size_t { 1 }, columns + 1))
        {
            if (column < columns && cells[column] == cells[runStart])
                continue;
            out.varint(column - runStart);
            out.varint(CellCodec<T>::toWire(cells[runStart], context));
            runStart = column;
        }
        out.endFrame(frame);
    }

    /// Reads runs of equal values into @p cells, which holds the line's columns.
    template <typename T>
    [[nodiscard]] std::expected<void, StateDecodeError> getRuns(std::span<uint8_t const> payload,
                                                                AlignedVector<T>& cells,
                                                                LineDecodeContext const& context)
    {
        auto in = StateReader::ofPayload(payload);
        auto column = size_t { 0 };
        while (!in.atEnd())
        {
            auto const length = in.varint();
            if (!length)
                return StateFailure { length.error() };
            auto const wire = in.varint();
            if (!wire)
                return StateFailure { wire.error() };
            if (*length == 0 || *length > cells.size() - column)
                return StateFailure { StateDecodeError::InvalidValue };
            auto const value = CellCodec<T>::fromWire(*wire, context);
            if (!value)
                return StateFailure { value.error() };
            std::ranges::fill_n(std::next(cells.begin(), static_cast<ptrdiff_t>(column)),
                                static_cast<ptrdiff_t>(*length),
                                *value);
            column += static_cast<size_t>(*length);
        }
        if (column != cells.size())
            return StateFailure { StateDecodeError::InvalidValue };
        return {};
    }

    /// Writes a scalar field unless it holds zero.
    void putScalar(StateWriter& out, LineField field, uint64_t value)
    {
        if (value == 0)
            return;
        out.varint(keyOf(field, WireKind::Varint));
        out.varint(value);
    }

    [[nodiscard]] constexpr uint64_t zigzag(int64_t value) noexcept
    {
        return (static_cast<uint64_t>(value) << 1U)
               ^ static_cast<uint64_t>(value >> std::numeric_limits<int64_t>::digits);
    }

    [[nodiscard]] constexpr int64_t unzigzag(uint64_t value) noexcept
    {
        return static_cast<int64_t>(value >> 1U) ^ -static_cast<int64_t>(value & 1U);
    }

    /// A line's fields as the frame holds them, before any is interpreted.
    struct LineFields
    {
        struct Field
        {
            uint32_t id = 0;
            WireKind kind = WireKind::Varint;
            uint64_t scalar = 0;
            std::span<uint8_t const> payload {};
        };
        std::vector<Field> fields;

        [[nodiscard]] Field const* find(LineField id) const noexcept
        {
            auto const it = std::ranges::find(fields, static_cast<uint32_t>(id), &Field::id);
            return it != fields.end() ? &*it : nullptr;
        }
    };

    [[nodiscard]] std::expected<LineFields, StateDecodeError> readLineFields(std::span<uint8_t const> frame)
    {
        auto in = StateReader::ofPayload(frame);
        auto result = LineFields {};
        while (!in.atEnd())
        {
            auto const key = in.varint();
            if (!key)
                return StateFailure { key.error() };
            if ((*key >> 1U) > std::numeric_limits<uint32_t>::max())
                return StateFailure { StateDecodeError::MalformedRecord };
            auto field = LineFields::Field { .id = static_cast<uint32_t>(*key >> 1U),
                                             .kind = (*key & 1U) != 0 ? WireKind::Framed : WireKind::Varint };
            if (std::ranges::contains(result.fields, field.id, &LineFields::Field::id))
                return StateFailure { StateDecodeError::MalformedRecord };
            if (field.kind == WireKind::Varint)
            {
                auto const scalar = in.varint();
                if (!scalar)
                    return StateFailure { scalar.error() };
                field.scalar = *scalar;
            }
            else
            {
                auto const payload = in.bytes();
                if (!payload)
                    return StateFailure { payload.error() };
                field.payload = *payload;
            }
            result.fields.push_back(field);
        }
        return result;
    }

    /// Reads the scalar field @p id into @p target, narrowed to what it holds; leaves it at zero when absent.
    template <typename T>
    [[nodiscard]] std::expected<void, StateDecodeError> getScalar(LineFields const& fields,
                                                                  LineField id,
                                                                  T& target)
    {
        auto const* field = fields.find(id);
        if (!field)
            return {};
        if (field->kind != WireKind::Varint)
            return StateFailure { StateDecodeError::MalformedRecord };
        if constexpr (std::is_signed_v<T>)
            return detail::narrowSigned<T>(unzigzag(field->scalar)).transform([&](T value) {
                target = value;
            });
        else
            return detail::narrow<T>(field->scalar).transform([&](T value) { target = value; });
    }

    /// @return The payload of the framed field @p id; empty when absent, nullopt-like failure on a wrong
    /// kind.
    [[nodiscard]] std::expected<std::optional<std::span<uint8_t const>>, StateDecodeError> getFramed(
        LineFields const& fields, LineField id)
    {
        auto const* field = fields.find(id);
        if (!field)
            return std::optional<std::span<uint8_t const>> {};
        if (field->kind != WireKind::Framed)
            return StateFailure { StateDecodeError::MalformedRecord };
        return std::optional { field->payload };
    }

    /// Checks what no decoder of a single field can: that the cells' clusters lie inside the pool.
    [[nodiscard]] std::expected<void, StateDecodeError> validateClusters(LineSoA const& line)
    {
        for (auto const column: std::views::iota(size_t { 0 }, line.clusterSize.size()))
        {
            auto const extras = line.clusterSize[column] > 1 ? size_t { line.clusterSize[column] } - 1 : 0;
            if (line.clusterSize[column] > MaxGraphemeClusterSize
                || (extras > 0
                    && size_t { line.clusterPoolIndex[column] } + extras > line.clusterPool.size()))
                return StateFailure { StateDecodeError::InvalidValue };
        }
        return {};
    }
    // }}}
} // namespace

// {{{ Line
void Line::encodeState(StateWriter& out, LineEncodeContext& context) const
{
    auto const frame = out.beginFrame();
    out.varint(keyOf(LineField::Columns, WireKind::Varint));
    out.varint(unbox<uint64_t>(_columns));
    putScalar(out, LineField::Flags, detail::wireBitsOf(_flags));
    putScalar(out, LineField::ContextId, unbox<uint64_t>(_contextId));
    putScalar(out, LineField::CommandEndOffset, zigzag(unbox<int64_t>(_commandEndOffset)));
    putScalar(out, LineField::PromptEndOffset, zigzag(unbox<int64_t>(_promptEndOffset)));
    putScalar(out, LineField::Revision, _revision);
    putScalar(out, LineField::Dirty, _dirty ? 1 : 0);
    putScalar(out, LineField::BornAt, bornAt());
    putScalar(out, LineField::BlockId, unbox<uint64_t>(_blockId));
    putScalar(out, LineField::StorageFlags, detail::wireBitsOf(_storage.lineFlags));
    putScalar(out, LineField::UsedColumns, zigzag(unbox<int64_t>(_storage.usedColumns)));
    putScalar(out, LineField::Shape, detail::wireIndexOf(_storage.shape));
    putScalar(out, LineField::FillRendition, context.renditions.indexOf(_storage.fillAttrs));

    if (!isBlankLineSoA(_storage))
    {
        auto const& codepoints = _storage.codepoints;
        // The cells past the last that holds a codepoint hold none: the column count says how many.
        auto const lastUsed =
            std::ranges::find_last_if(codepoints, [](char32_t codepoint) { return codepoint != 0; });
        auto const written =
            lastUsed.empty() ? size_t { 0 }
                             : static_cast<size_t>(std::distance(codepoints.begin(), lastUsed.begin())) + 1;
        out.varint(keyOf(LineField::Codepoints, WireKind::Framed));
        auto const text = out.beginFrame();
        for (auto const codepoint: std::span { codepoints }.first(written))
            putUtf8(out, codepoint);
        out.endFrame(text);

        forEachRunArray([&](LineField field, auto member, auto defaultAt) {
            putRuns(out, field, _storage, _storage.*member, defaultAt, context);
        });

        if (!_storage.clusterPool.empty())
        {
            out.varint(keyOf(LineField::ClusterPool, WireKind::Framed));
            auto const pool = out.beginFrame();
            for (auto const codepoint: _storage.clusterPool)
                out.varint(static_cast<uint64_t>(codepoint));
            out.endFrame(pool);
        }
    }

    if (_storage.imageFragments && !_storage.imageFragments->empty())
    {
        auto columns = std::vector<uint16_t> {};
        columns.reserve(_storage.imageFragments->size());
        for (auto const column: *_storage.imageFragments | std::views::keys)
            columns.push_back(column);
        std::ranges::sort(columns);
        out.varint(keyOf(LineField::ImageFragments, WireKind::Framed));
        auto const fragments = out.beginFrame();
        for (auto const column: columns)
        {
            out.varint(column);
            out.varint(context.images.fragmentId(_storage.imageFragments->at(column)));
        }
        out.endFrame(fragments);
    }
    out.endFrame(frame);
}

namespace
{
    /// Reads the cells of a materialized line into @p storage, which holds the line's columns.
    [[nodiscard]] std::expected<void, StateDecodeError> getCells(LineFields const& fields,
                                                                 std::span<uint8_t const> codepoints,
                                                                 LineSoA& storage,
                                                                 LineDecodeContext const& context)
    {
        auto text = StateReader::ofPayload(codepoints);
        auto column = size_t { 0 };
        while (!text.atEnd())
        {
            auto const codepoint = getUtf8(text);
            if (!codepoint)
                return StateFailure { codepoint.error() };
            if (column == storage.codepoints.size())
                return StateFailure { StateDecodeError::InvalidValue };
            storage.codepoints[column++] = *codepoint;
        }

        auto runs = std::expected<void, StateDecodeError> {};
        forEachRunArray([&](LineField field, auto member, auto defaultAt) {
            if (!runs)
                return;
            auto& cells = storage.*member;
            auto const payload = getFramed(fields, field);
            if (!payload)
                runs = StateFailure { payload.error() };
            else if (*payload)
                runs = getRuns(**payload, cells, context);
            else
                for (auto const index: std::views::iota(size_t { 0 }, cells.size()))
                    cells[index] = defaultAt(storage, index);
        });
        if (!runs)
            return runs;

        auto const pool = getFramed(fields, LineField::ClusterPool);
        if (!pool)
            return StateFailure { pool.error() };
        if (*pool)
        {
            auto poolIn = StateReader::ofPayload(**pool);
            while (!poolIn.atEnd())
            {
                auto const codepoint = poolIn.varint();
                if (!codepoint)
                    return StateFailure { codepoint.error() };
                if (*codepoint > MaxCodepoint)
                    return StateFailure { StateDecodeError::InvalidValue };
                storage.clusterPool.push_back(static_cast<char32_t>(*codepoint));
            }
        }
        return validateClusters(storage);
    }

    /// Reads the image fragments of a line of @p columns into @p storage.
    [[nodiscard]] std::expected<void, StateDecodeError> getFragments(std::span<uint8_t const> payload,
                                                                     uint32_t columns,
                                                                     LineSoA& storage,
                                                                     LineDecodeContext const& context)
    {
        auto& map = storage.imageFragments.emplace();
        auto in = StateReader::ofPayload(payload);
        auto previous = std::optional<uint64_t> {};
        while (!in.atEnd())
        {
            auto const column = in.varint();
            if (!column)
                return StateFailure { column.error() };
            auto const id = in.varint();
            if (!id)
                return StateFailure { id.error() };
            if (*column >= uint64_t { columns } || (previous && *previous >= *column))
                return StateFailure { StateDecodeError::InvalidValue };
            previous = *column;
            auto fragment = context.images.fragment(*id);
            if (!fragment)
                return StateFailure { StateDecodeError::InvalidValue };
            map.emplace(static_cast<uint16_t>(*column), std::move(fragment));
        }
        return {};
    }

    /// The scalar fields of a line, as the wire holds them.
    struct LineScalars
    {
        uint32_t columns = 0;
        uint64_t flags = 0;
        uint16_t contextId = 0;
        int commandEnd = 0;
        int promptEnd = 0;
        uint64_t revision = 0;
        uint8_t dirty = 0;
        uint32_t bornAt = 0;
        uint32_t blockId = 0;
        uint64_t storageFlags = 0;
        uint32_t usedColumns = 0;
        uint64_t shape = 0;
        uint64_t fillRendition = 0;
    };

    [[nodiscard]] std::expected<LineScalars, StateDecodeError> getScalars(LineFields const& fields)
    {
        auto scalars = LineScalars {};
        auto usedColumns = 0;
        auto const read =
            getScalar(fields, LineField::Columns, scalars.columns)
                .and_then([&] { return getScalar(fields, LineField::Flags, scalars.flags); })
                .and_then([&] { return getScalar(fields, LineField::ContextId, scalars.contextId); })
                .and_then([&] { return getScalar(fields, LineField::CommandEndOffset, scalars.commandEnd); })
                .and_then([&] { return getScalar(fields, LineField::PromptEndOffset, scalars.promptEnd); })
                .and_then([&] { return getScalar(fields, LineField::Revision, scalars.revision); })
                .and_then([&] { return getScalar(fields, LineField::Dirty, scalars.dirty); })
                .and_then([&] { return getScalar(fields, LineField::BornAt, scalars.bornAt); })
                .and_then([&] { return getScalar(fields, LineField::BlockId, scalars.blockId); })
                .and_then([&] { return getScalar(fields, LineField::StorageFlags, scalars.storageFlags); })
                .and_then([&] { return getScalar(fields, LineField::UsedColumns, usedColumns); })
                .and_then([&] { return getScalar(fields, LineField::Shape, scalars.shape); })
                .and_then([&] { return getScalar(fields, LineField::FillRendition, scalars.fillRendition); });
        if (!read)
            return StateFailure { read.error() };
        constexpr auto MaxBornAtValue = uint32_t { 0xFF'FFFF };
        if (usedColumns < 0 || std::cmp_greater(usedColumns, scalars.columns)
            || scalars.columns > static_cast<uint32_t>(MaxStateLineColumns) || scalars.dirty > 1
            || scalars.bornAt > MaxBornAtValue)
            return StateFailure { StateDecodeError::InvalidValue };
        scalars.usedColumns = static_cast<uint32_t>(usedColumns);
        return scalars;
    }
} // namespace

std::expected<Line, StateDecodeError> Line::decodeState(StateReader& in, LineDecodeContext const& context)
{
    auto const frame = in.bytes();
    if (!frame)
        return StateFailure { frame.error() };
    auto const fields = readLineFields(*frame);
    if (!fields)
        return StateFailure { fields.error() };
    auto const scalars = getScalars(*fields);
    if (!scalars)
        return StateFailure { scalars.error() };

    constexpr auto const& Shapes = detail::StateEnumWire<LineShape>::Values;
    auto const flags = detail::flagsFromWire<LineFlags>(scalars->flags);
    auto const storageFlags = detail::flagsFromWire<LineFlags>(scalars->storageFlags);
    if (scalars->shape >= Shapes.size() || scalars->fillRendition >= context.renditions.size() || !flags
        || !storageFlags)
        return StateFailure { StateDecodeError::InvalidValue };

    auto line = Line {};
    line._columns = ColumnCount::cast_from(scalars->columns);
    line._flags = *flags;
    line._contextId = ContextId { scalars->contextId };
    line._commandEndOffset = ColumnOffset(scalars->commandEnd);
    line._promptEndOffset = ColumnOffset(scalars->promptEnd);
    line._revision = scalars->revision;
    line._dirty = scalars->dirty != 0;
    line._bornAt = { static_cast<uint8_t>(scalars->bornAt),
                     static_cast<uint8_t>(scalars->bornAt >> 8U),
                     static_cast<uint8_t>(scalars->bornAt >> 16U) };
    line._blockId = CommandBlockId { scalars->blockId };
    auto& storage = line._storage;
    storage.fillAttrs = context.renditions[static_cast<size_t>(scalars->fillRendition)];

    auto const codepoints = getFramed(*fields, LineField::Codepoints);
    if (!codepoints)
        return StateFailure { codepoints.error() };
    if (*codepoints)
    {
        if (scalars->columns > context.cellBudget)
            return StateFailure { StateDecodeError::TooLarge };
        context.cellBudget -= scalars->columns;
        initializeLineSoA(storage, line._columns, storage.fillAttrs);
        if (auto const cells = getCells(*fields, **codepoints, storage, context); !cells)
            return StateFailure { cells.error() };
    }
    else if (std::ranges::any_of(fields->fields, [](LineFields::Field const& field) {
                 return field.id >= static_cast<uint32_t>(LineField::Widths)
                        && field.id <= static_cast<uint32_t>(LineField::ClusterPool);
             }))
        // A blank line carries no cells: a field of one is a lie.
        return StateFailure { StateDecodeError::InvalidValue };

    // After initializeLineSoA(), which resets them.
    storage.lineFlags = *storageFlags;
    storage.usedColumns = ColumnCount::cast_from(scalars->usedColumns);
    storage.shape = Shapes[static_cast<size_t>(scalars->shape)];

    auto const fragments = getFramed(*fields, LineField::ImageFragments);
    if (!fragments)
        return StateFailure { fragments.error() };
    if (*fragments)
        if (auto const done = getFragments(**fragments, scalars->columns, storage, context); !done)
            return StateFailure { done.error() };
    return line;
}
// }}}

// {{{ Grid
GridState Grid::captureState(ImageFragmentNamer& images) const
{
    auto state = GridState {};
    state.pageSize = _pageSize;
    state.reflow = _reflowOnResize ? ReflowOnResize::Yes : ReflowOnResize::No;
    state.limits = _historyLimits;
    state.linesUsed = _linesUsed;
    state.generation = _generation;
    state.stableIdGeneration = _stableIdGeneration;
    state.stableBase = _stableBase;
    state.stableFloor = _stableFloor;
    state.evictedRowCount = _evictedRowCount;
    state.dirtyHistoryFloor = _dirtyHistoryFloor;
    state.changedHistoryFloor = _changedHistoryFloor;
    state.changedHistorySeqno = _changedHistorySeqno;
    state.seqno = _seqno;
    state.stableBaseAtLastFinalize = _stableBaseAtLastFinalize;

    auto renditions = RenditionTableBuilder {};
    auto context = LineEncodeContext { .renditions = renditions, .images = images };
    auto lines = StateWriter::payload();
    for (auto const offset: std::views::iota(-unbox<int>(historyLineCount()), unbox<int>(_pageSize.lines)))
        lineAt(LineOffset(offset)).encodeState(lines, context);
    state.renditions = std::move(renditions).finish();
    state.lines = std::move(lines).take();
    return state;
}

std::expected<StagedGrid, StateDecodeError> Grid::stageRestore(GridState const& state,
                                                               ImageFragmentResolver const& resolver) const
{
    if (state.limits != _historyLimits || state.pageSize.lines < LineCount(1)
        || state.pageSize.columns < ColumnCount(1)
        || state.pageSize.columns > ColumnCount(MaxStateLineColumns)
        || state.linesUsed < state.pageSize.lines
        // Every line takes at least a byte of the stream: more lines than bytes is a lie.
        || unbox<size_t>(state.linesUsed) > state.lines.size())
        return StateFailure { StateDecodeError::InvalidValue };

    auto const pageLines = unbox<size_t>(state.pageSize.lines);
    auto const linesUsed = unbox<size_t>(state.linesUsed);
    auto const historyLines = linesUsed - pageLines;
    auto slotCount = linesUsed;
    if (auto const* capacity = std::get_if<LineCount>(&state.limits.capacity))
    {
        if (*capacity < LineCount(0) || historyLines > unbox<size_t>(*capacity))
            return StateFailure { StateDecodeError::InvalidValue };
        slotCount = pageLines + unbox<size_t>(*capacity);
    }
    if (state.stableFloor != state.stableBase - static_cast<int64_t>(historyLines))
        return StateFailure { StateDecodeError::InvalidValue };

    auto restored = std::vector<Line> {};
    restored.reserve(linesUsed);
    // Every line page-wide and materialized, and as much again in lines wider than the page.
    auto cellBudget = 2 * linesUsed * unbox<size_t>(state.pageSize.columns);
    auto const context =
        LineDecodeContext { .renditions = state.renditions, .images = resolver, .cellBudget = cellBudget };
    auto in = StateReader::ofPayload(state.lines);
    while (!in.atEnd())
    {
        if (restored.size() == linesUsed)
            return StateFailure { StateDecodeError::InvalidValue };
        auto line = Line::decodeState(in, context);
        if (!line)
            return StateFailure { line.error() };
        if (line->size() < state.pageSize.columns)
            return StateFailure { StateDecodeError::InvalidValue };
        restored.push_back(*std::move(line));
    }
    if (restored.size() != linesUsed)
        return StateFailure { StateDecodeError::InvalidValue };

    // The layout setHistoryLimits() leaves: [page][free][history], history newest last, so that the ring's
    // zero is the first page line and offset -1 is the slot before it.
    auto const defaultFlags = state.reflow == ReflowOnResize::Yes ? LineFlag::Wrappable : LineFlag::None;
    auto staged = StagedGrid { .state = &state, .ringLines = {} };
    staged.ringLines.reserve(slotCount);
    auto const history = std::span { restored }.first(historyLines);
    auto const page = std::span { restored }.last(pageLines);
    for (auto& line: page)
        staged.ringLines.push_back(std::move(line));
    for ([[maybe_unused]] auto const free: std::views::iota(size_t { 0 }, slotCount - restored.size()))
        staged.ringLines.emplace_back(state.pageSize.columns, defaultFlags, GraphicsAttributes {});
    for (auto& line: history)
        staged.ringLines.push_back(std::move(line));
    return staged;
}

void Grid::commitRestore(StagedGrid staged)
{
    auto const& state = *staged.state;
    _pageSize = state.pageSize;
    _reflowOnResize = state.reflow == ReflowOnResize::Yes;
    _historyLimits = state.limits;
    _lines.storage() = std::move(staged.ringLines);
    _lines.unrotate();
    _linesUsed = state.linesUsed;
    _evictionScan.reset();
    _generation = state.generation;
    _stableIdGeneration = state.stableIdGeneration;
    _stableBase = state.stableBase;
    _stableFloor = state.stableFloor;
    _evictedRowCount = state.evictedRowCount;
    _dirtyHistoryFloor = state.dirtyHistoryFloor;
    _changedHistoryFloor = state.changedHistoryFloor;
    _changedHistorySeqno = state.changedHistorySeqno;
    _seqno = state.seqno;
    _stableBaseAtLastFinalize = state.stableBaseAtLastFinalize;
    verifyState();
}

std::expected<void, StateDecodeError> Grid::restoreState(GridState const& state,
                                                         ImageFragmentResolver const& resolver)
{
    return stageRestore(state, resolver).transform([this](StagedGrid staged) {
        commitRestore(std::move(staged));
    });
}
// }}}

} // namespace vtbackend
