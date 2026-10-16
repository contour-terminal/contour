// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/screen/Gutter.hpp>

#include <vtbackend/screen/GutterSettings.hpp>

#include <libunicode/convert.h>
#include <libunicode/width.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vtbackend
{

namespace
{
    /// The instant a timestamp format is measured at: the longest-spelled weekday (Wednesday) and month
    /// (September) there are, at the widest time of day.
    constexpr auto TimestampSample =
        std::chrono::local_days { std::chrono::year { 2000 } / std::chrono::September
                                  / std::chrono::day { 27 } }
        + std::chrono::hours { 23 } + std::chrono::minutes { 59 } + std::chrono::seconds { 59 };

    /// The last C0 control and DEL: what `%n` and `%t` spell, and what a gutter cell must never hold.
    constexpr auto LastC0Control = 0x1FU;
    constexpr auto DeleteControl = 0x7FU;

    /// Whether @p text holds a C0 control or DEL.
    [[nodiscard]] bool containsControl(std::string_view text) noexcept
    {
        return std::ranges::any_of(text, [](char ch) {
            auto const code = static_cast<unsigned char>(ch);
            return code <= LastC0Control || code == DeleteControl;
        });
    }

    /// Whether @p format begins with a `%` conversion, as a chrono specification must. A `%` followed by
    /// an alignment is not one: std::format reads it as a fill character.
    [[nodiscard]] constexpr bool beginsWithConversion(std::string_view format) noexcept
    {
        constexpr auto Alignments = std::string_view { "<>^" };
        return format.starts_with('%') && (format.size() == 1 || !Alignments.contains(format[1]));
    }
} // namespace

std::string_view timestampFormatErrorText(TimestampFormatError error) noexcept
{
    constexpr auto Texts = std::array<std::string_view, 5> {
        "it is empty",
        "it does not begin with a % conversion",
        "it is not a std::format chrono specification, or it spells a control character",
        "it spells a double-width or a zero-width character",
        "it spells more than the gutter has room for",
    };
    static_assert(Texts.size() == static_cast<std::size_t>(TimestampFormatError::TooWide) + 1);
    return Texts[static_cast<std::size_t>(error)];
}

std::expected<int, TimestampFormatError> measureTimestampFormat(std::string_view format)
{
    if (format.empty())
        return std::unexpected { TimestampFormatError::Empty };
    if (!beginsWithConversion(format))
        return std::unexpected { TimestampFormatError::NoLeadingConversion };

    auto const sample = formatGutterTimestamp(format, TimestampSample);
    if (sample.empty() || containsControl(sample))
        return std::unexpected { TimestampFormatError::Malformed };

    // The painter gives every character of a timestamp one column (appendGutterCells), so a format is usable
    // only where that is what each one takes -- and its width is then its character count.
    auto const characters = unicode::convert_to<char32_t>(std::string_view { sample });
    if (!std::ranges::all_of(characters, [](char32_t ch) { return unicode::width(ch) == 1; }))
        return std::unexpected { TimestampFormatError::WideOrZeroWidthCharacter };

    auto const columns = static_cast<int>(characters.size());
    if (columns > MaxTimestampColumns)
        return std::unexpected { TimestampFormatError::TooWide };
    return columns;
}

ResolvedTimestampFormat resolveTimestampFormat(std::string_view requested)
{
    if (auto const columns = measureTimestampFormat(requested))
        return ResolvedTimestampFormat { .format = requested, .columns = *columns };
    return ResolvedTimestampFormat { .format = DefaultTimestampFormat,
                                     .columns = measureTimestampFormat(DefaultTimestampFormat).value_or(0) };
}

std::string formatGutterTimestamp(std::string_view format, std::chrono::local_seconds when)
{
    // The specification is spliced into a replacement field, so a brace in it -- which no chrono
    // specification holds -- would end that field early: `%H}}` would spell the hour and a literal `}`. The
    // formatter cannot tell, so it is refused here.
    if (format.find_first_of("{}") != std::string_view::npos)
        return {};

    // std::vformat reports a bad specification by throwing, and the specification is user input -- so
    // this is the boundary where that becomes "nothing to show".
    try
    {
        return std::vformat(std::format("{{:{}}}", format), std::make_format_args(when));
    }
    catch (std::format_error const&)
    {
        return {};
    }
}

TimestampRun::TimestampRun(std::string_view format, LocalTimeConverter localTime):
    _field { std::format("{{:{}}}", format) }, _localTime { localTime }
{
}

std::string TimestampRun::next(std::optional<std::chrono::system_clock::time_point> when)
{
    // The rows of one output batch share their instant, so most rows end here: an equal instant spells
    // equal text, and is neither converted to local time nor formatted.
    if (when == _aboveInstant)
        return {};
    _aboveInstant = when;

    auto spelled = when ? spell(_localTime(*when)) : std::string {};
    if (spelled == _above)
        return {};
    _above = std::move(spelled);
    return _above;
}

std::string TimestampRun::spell(std::chrono::local_seconds when) const
{
    // The format was validated before it got here, but formatting still reports a failure by throwing,
    // and the render pass is no place for that to land.
    try
    {
        return std::vformat(_field, std::make_format_args(when));
    }
    catch (std::format_error const&)
    {
        return {};
    }
}

namespace
{
    /// One blank column after the timestamps, so a right-aligned line number that fills its own width
    /// never runs into the time beside it.
    constexpr auto TimestampGap = 1;

    /// What the gutter's table reads: the settings, and the timestamp format already resolved from them,
    /// so the format is validated once per layout.
    struct SegmentInputs
    {
        GutterSettings const& settings; ///< What the gutter shows.
        std::optional<ResolvedTimestampFormat>
            timestamp; ///< The resolved format; nullopt with timestamps off.
    };

    [[nodiscard]] int timestampColumns(SegmentInputs const& inputs)
    {
        return inputs.timestamp ? inputs.timestamp->columns + TimestampGap : 0;
    }

    [[nodiscard]] int lineNumberColumns(SegmentInputs const& inputs)
    {
        if (inputs.settings.lineNumbers == LineNumberMode::Off)
            return 0;
        return std::clamp(inputs.settings.lineNumberWidth, MinLineNumberWidth, MaxLineNumberWidth);
    }

    [[nodiscard]] int blockColumnColumns(SegmentInputs const& inputs)
    {
        // Its configured width whenever any of its three jobs is on. Fold markers alone already reserved
        // it, so turning exit status or user marks on beside them costs no width.
        auto const& settings = inputs.settings;
        if (!settings.foldMarkers && !settings.exitStatus && !settings.userMarks)
            return 0;
        return std::clamp(settings.blockColumnWidth, MinBlockColumnWidth, MaxBlockColumnWidth);
    }

    /// One row of the gutter's table: a segment, and how wide the settings make it.
    struct SegmentRule
    {
        using ColumnsFunction = int (*)(SegmentInputs const&);

        GutterSegmentKind kind {};         ///< The segment.
        ColumnsFunction columns = nullptr; ///< Its width under given settings; 0 leaves it out.
    };

    /// The gutter, left to right. A fourth segment is a row here.
    constexpr auto SegmentRules = std::array {
        SegmentRule { .kind = GutterSegmentKind::Timestamp, .columns = &timestampColumns },
        SegmentRule { .kind = GutterSegmentKind::LineNumber, .columns = &lineNumberColumns },
        SegmentRule { .kind = GutterSegmentKind::BlockColumn, .columns = &blockColumnColumns },
    };
    static_assert(SegmentRules.size() == static_cast<std::size_t>(GutterSegmentKind::Count));

    // Every rule sits at its kind's index: the table runs in the order GutterSegmentKind documents as left to
    // right, and names each kind exactly once.
    static_assert(std::ranges::all_of(std::views::iota(std::size_t { 0 }, SegmentRules.size()),
                                      [](std::size_t index) {
                                          return static_cast<std::size_t>(SegmentRules[index].kind) == index;
                                      }));

    /// Where a row's line number comes from.
    enum class LineNumberSource : uint8_t
    {
        Absolute = 0, ///< The row's own number.
        Distance,     ///< Its distance from the cursor row.
    };

    /// One row of vim's numbering table: what the cursor row and every other row show.
    struct LineNumberRule
    {
        LineNumberSource cursorRow {}; ///< What the cursor row shows.
        LineNumberSource otherRows {}; ///< What every other row shows.
    };

    /// Indexed by LineNumberMode, one row per mode in its declared order.
    constexpr auto LineNumberRules = std::array {
        // Off: never read, as Off has no segment to fill; present so every mode has its row.
        LineNumberRule { .cursorRow = LineNumberSource::Absolute, .otherRows = LineNumberSource::Absolute },
        // Absolute
        LineNumberRule { .cursorRow = LineNumberSource::Absolute, .otherRows = LineNumberSource::Absolute },
        // Relative
        LineNumberRule { .cursorRow = LineNumberSource::Distance, .otherRows = LineNumberSource::Distance },
        // Hybrid
        LineNumberRule { .cursorRow = LineNumberSource::Absolute, .otherRows = LineNumberSource::Distance },
    };
    static_assert(LineNumberRules.size() == static_cast<std::size_t>(LineNumberMode::Hybrid) + 1);

    /// What a line number too wide for its segment is spelled behind, in place of its leading digits.
    constexpr auto TruncationMark = U'…';
} // namespace

std::optional<GutterSegment> GutterLayout::segment(GutterSegmentKind kind) const noexcept
{
    auto const present = segments();
    auto const found = std::ranges::find(present, kind, &GutterSegment::kind);
    if (found == present.end())
        return std::nullopt;
    return *found;
}

void GutterLayout::append(GutterSegmentKind kind, int columns) noexcept
{
    _segments.at(_segmentCount) =
        GutterSegment { .kind = kind, .firstColumn = totalColumns, .columns = columns };
    ++_segmentCount;
    totalColumns += columns;
}

GutterLayout gutterLayoutFor(GutterSettings const& settings)
{
    auto const inputs =
        SegmentInputs { .settings = settings,
                        .timestamp = settings.timestamps
                                         ? std::optional { resolveTimestampFormat(settings.timestampFormat) }
                                         : std::nullopt };
    auto layout = GutterLayout {};
    for (auto const& rule: SegmentRules)
        if (auto const columns = rule.columns(inputs); columns > 0)
            layout.append(rule.kind, columns);
    if (inputs.timestamp)
        layout.timestampFormat = std::string { inputs.timestamp->format };
    return layout;
}

std::optional<GutterSegmentKind> gutterSegmentAt(GutterLayout const& layout, int column) noexcept
{
    auto const present = layout.segments();
    auto const found = std::ranges::find_if(present, [column](GutterSegment const& segment) {
        return column >= segment.firstColumn && column < segment.firstColumn + segment.columns;
    });
    if (found == present.end())
        return std::nullopt;
    return found->kind;
}

uint64_t lineNumberFor(LineNumberMode mode, uint64_t absoluteNumber, int64_t cursorDistance) noexcept
{
    auto const& rule = LineNumberRules[std::to_underlying(mode)];
    auto const source = cursorDistance == 0 ? rule.cursorRow : rule.otherRows;
    if (source == LineNumberSource::Absolute)
        return absoluteNumber;

    // The magnitude in unsigned arithmetic: negating the most negative distance as an int64_t overflows.
    auto const distance = static_cast<uint64_t>(cursorDistance);
    return cursorDistance < 0 ? uint64_t { 0 } - distance : distance;
}

LineNumberText lineNumberText(uint64_t number, int width) noexcept
{
    auto const columns = static_cast<std::size_t>(std::clamp(width, 0, MaxLineNumberWidth));
    auto buffer = std::array<char, std::numeric_limits<uint64_t>::digits10 + 1> {};
    auto const spelled = std::to_chars(buffer.data(), buffer.data() + buffer.size(), number);
    auto const digits = std::string_view { buffer.data(), spelled.ptr };

    auto text = LineNumberText {};
    auto const appendDigits = [&](std::string_view part) {
        for (auto const digit: part)
            text.append(static_cast<char32_t>(digit));
    };
    if (digits.size() <= columns)
    {
        for ([[maybe_unused]] auto const _: std::views::iota(digits.size(), columns))
            text.append(U' ');
        appendDigits(digits);
        return text;
    }
    if (columns == 0)
        return text;

    text.append(TruncationMark);
    appendDigits(digits.substr(digits.size() - (columns - 1)));
    return text;
}

namespace
{
    /// The block-column glyph each fold marker is drawn as.
    constexpr auto FoldGlyphs = std::array {
        BlockColumnGlyph::None,          // FoldMarker::None
        BlockColumnGlyph::FoldExpanded,  // FoldMarker::Expanded
        BlockColumnGlyph::FoldCollapsed, // FoldMarker::Collapsed
        BlockColumnGlyph::FoldBody,      // FoldMarker::Body
        BlockColumnGlyph::FoldBodyEnd,   // FoldMarker::BodyEnd
    };
    static_assert(FoldGlyphs.size() == static_cast<std::size_t>(FoldMarker::Count));

    /// The same for a fold whose command failed: its body heavy, its head the same control as any other's.
    constexpr auto FailedFoldGlyphs = std::array {
        BlockColumnGlyph::None,              // FoldMarker::None
        BlockColumnGlyph::FoldExpanded,      // FoldMarker::Expanded
        BlockColumnGlyph::FoldCollapsed,     // FoldMarker::Collapsed
        BlockColumnGlyph::FoldBodyFailed,    // FoldMarker::Body
        BlockColumnGlyph::FoldBodyEndFailed, // FoldMarker::BodyEnd
    };
    static_assert(FailedFoldGlyphs.size() == static_cast<std::size_t>(FoldMarker::Count));

    /// Whether the row's block failed, as far as the column may say: only with exit status on.
    [[nodiscard]] constexpr bool showsFailure(BlockColumnFacts const& facts,
                                              GutterSettings const& settings) noexcept
    {
        return settings.exitStatus && facts.outcome == CommandBlockOutcome::Failure;
    }

    [[nodiscard]] constexpr bool isFoldHead(FoldMarker marker) noexcept
    {
        return marker == FoldMarker::Expanded || marker == FoldMarker::Collapsed;
    }

    /// One step of the block column's precedence: the glyph it claims the row for, or None to pass the row
    /// on to the next step.
    using BlockColumnRule = BlockColumnGlyph (*)(BlockColumnFacts const&, GutterSettings const&) noexcept;

    [[nodiscard]] BlockColumnGlyph foldHeadRule(BlockColumnFacts const& facts,
                                                GutterSettings const& settings) noexcept
    {
        if (!settings.foldMarkers || !isFoldHead(facts.fold))
            return BlockColumnGlyph::None;
        return FoldGlyphs[static_cast<std::size_t>(facts.fold)];
    }

    [[nodiscard]] BlockColumnGlyph userMarkRule(BlockColumnFacts const& facts,
                                                GutterSettings const& settings) noexcept
    {
        // Never on a head row, fold control drawn or not: a prompt is a navigation target already, and
        // its status is what its column is for.
        if (!settings.userMarks || facts.mark == UserMarkPresence::Absent
            || facts.head != BlockHeadState::None || isFoldHead(facts.fold))
            return BlockColumnGlyph::None;
        return BlockColumnGlyph::UserMark;
    }

    [[nodiscard]] BlockColumnGlyph foldBodyRule(BlockColumnFacts const& facts,
                                                GutterSettings const& settings) noexcept
    {
        if (!settings.foldMarkers || isFoldHead(facts.fold))
            return BlockColumnGlyph::None;
        auto const& glyphs = showsFailure(facts, settings) ? FailedFoldGlyphs : FoldGlyphs;
        return glyphs[static_cast<std::size_t>(facts.fold)];
    }

    [[nodiscard]] BlockColumnGlyph runningHeadRule(BlockColumnFacts const& facts,
                                                   GutterSettings const& settings) noexcept
    {
        return settings.exitStatus && facts.head == BlockHeadState::Running ? BlockColumnGlyph::Running
                                                                            : BlockColumnGlyph::None;
    }

    [[nodiscard]] BlockColumnGlyph finishedHeadRule(BlockColumnFacts const& facts,
                                                    GutterSettings const& settings) noexcept
    {
        if (!settings.exitStatus || facts.head != BlockHeadState::Finished)
            return BlockColumnGlyph::None;
        return showsFailure(facts, settings) ? BlockColumnGlyph::Failed : BlockColumnGlyph::Finished;
    }

    /// The block column, highest precedence first. Another glyph is a rule here and a row in
    /// blockColumnCodepoint()'s table.
    constexpr auto BlockColumnPrecedence = std::to_array<BlockColumnRule>({
        &foldHeadRule,
        &userMarkRule,
        &foldBodyRule,
        &runningHeadRule,
        &finishedHeadRule,
    });

    /// The blank column between a row's text and its fold label.
    constexpr auto FoldLabelGap = 1;

    /// Digits per thousands group in a fold label's count.
    constexpr auto DigitsPerGroup = std::size_t { 3 };
} // namespace

RenderAttributes gutterCellAttributes(RGBColorPair colors) noexcept
{
    return RenderAttributes { .foregroundColor = colors.foreground,
                              .backgroundColor = colors.background,
                              .decorationColor = colors.foreground };
}

BlockColumnGlyph blockColumnGlyph(BlockColumnFacts const& facts, GutterSettings const& settings) noexcept
{
    for (auto const rule: BlockColumnPrecedence)
        if (auto const glyph = rule(facts, settings); glyph != BlockColumnGlyph::None)
            return glyph;
    return BlockColumnGlyph::None;
}

char32_t blockColumnCodepoint(BlockColumnGlyph glyph) noexcept
{
    constexpr auto Codepoints = std::array {
        U'\0',                                  // None
        foldMarkerGlyph(FoldMarker::Expanded),  // FoldExpanded
        foldMarkerGlyph(FoldMarker::Collapsed), // FoldCollapsed
        foldMarkerGlyph(FoldMarker::Body),      // FoldBody
        foldMarkerGlyph(FoldMarker::BodyEnd),   // FoldBodyEnd
        U'\U0010F003',                          // UserMark: a diamond, as U+25C6 ◆
        U'\U0010F002',                          // Running:  a small right-pointing triangle, as U+25B8 ▸
        U'\U0010F004',                          // Finished: a disc, as U+25CF ●
        U'\U0010F005',                          // Failed:   a cross, as U+2717 ✗
        U'┃',                                   // FoldBodyFailed:    BOX DRAWINGS HEAVY VERTICAL
        U'┗',                                   // FoldBodyEndFailed: BOX DRAWINGS HEAVY UP AND RIGHT
    };
    static_assert(Codepoints.size() == static_cast<std::size_t>(BlockColumnGlyph::Count));
    return Codepoints[static_cast<std::size_t>(glyph)];
}

BlockColumnPress blockColumnPressFor(BlockColumnGlyph glyph, FoldToggle fold) noexcept
{
    if (fold == FoldToggle::Available)
        return BlockColumnPress::ToggleFold;

    // With no fold to toggle, only a status mark has something to offer: its block's menu.
    constexpr auto Presses = std::array {
        BlockColumnPress::Swallow,       // None
        BlockColumnPress::Swallow,       // FoldExpanded: drawn, but folding is off
        BlockColumnPress::Swallow,       // FoldCollapsed: likewise
        BlockColumnPress::Swallow,       // FoldBody
        BlockColumnPress::Swallow,       // FoldBodyEnd
        BlockColumnPress::Swallow,       // UserMark
        BlockColumnPress::OpenBlockMenu, // Running
        BlockColumnPress::OpenBlockMenu, // Finished
        BlockColumnPress::OpenBlockMenu, // Failed
        BlockColumnPress::Swallow,       // FoldBodyFailed
        BlockColumnPress::Swallow,       // FoldBodyEndFailed
    };
    static_assert(Presses.size() == static_cast<std::size_t>(BlockColumnGlyph::Count));
    return Presses[static_cast<std::size_t>(glyph)];
}

BlockHeadState blockHeadStateOf(CommandBlockRecord const* record,
                                int64_t stableId,
                                uint64_t stableIdGeneration) noexcept
{
    if (record == nullptr || record->headStableId != stableId
        || record->headIdGeneration != stableIdGeneration)
        return BlockHeadState::None;

    switch (record->state)
    {
        case CommandBlockState::Prompting: return BlockHeadState::None;
        case CommandBlockState::Running: return BlockHeadState::Running;
        case CommandBlockState::Finished: return BlockHeadState::Finished;
    }
    return BlockHeadState::None;
}

RGBColorPair blockColumnColors(ColorPalette const& palette,
                               BlockColumnGlyph glyph,
                               std::optional<CommandBlockOutcome> outcome,
                               GutterHover hover) noexcept
{
    auto const fold =
        hover == GutterHover::Hovered ? palette.foldMarkerHoverColors() : palette.foldMarkerColors();
    auto const fullStrength = palette.foldMarkerHoverColors().foreground;

    // The user's own mark is about the row, not about how its command ended: it stands out the same in
    // every block and in both states.
    if (glyph == BlockColumnGlyph::UserMark)
        return RGBColorPair { .foreground = fullStrength, .background = fold.background };

    // No status to show, or a success the scheme leaves to the fold column: exactly today's column.
    if (!outcome
        || (*outcome == CommandBlockOutcome::Success
            && !palette.blockStatus[std::to_underlying(CommandBlockOutcome::Success)]))
        return fold;

    auto const status = palette.blockStatusColor(*outcome);
    return RGBColorPair { .foreground = hover == GutterHover::Hovered
                                            ? mixColor(status, fullStrength, BlockStatusHoverLift)
                                            : status,
                          .background = fold.background };
}

RGBColor blockBandColor(ColorPalette const& palette, std::optional<CommandBlockOutcome> outcome) noexcept
{
    // A disc's hovered colour is its block's: the outcome's, lifted, or the text colour for an uncoloured
    // success.
    return blockColumnColors(palette, BlockColumnGlyph::Finished, outcome, GutterHover::Hovered).foreground;
}

void appendGutterCells(GutterLayout const& layout,
                       GutterRow const& row,
                       RGBColorPair textColors,
                       std::vector<RenderGutterCell>& output)
{
    auto const columnOf = [&](int gutterColumn) {
        return ColumnOffset(gutterColumn - layout.totalColumns);
    };

    auto const appendText = [&](GutterSegmentKind kind, std::u32string_view text) {
        auto const segment = layout.segment(kind);
        if (!segment)
            return;
        auto const fitting = std::min(text.size(), static_cast<std::size_t>(segment->columns));
        for (auto const index: std::views::iota(std::size_t { 0 }, fitting))
        {
            // A blank is the page's own background, which is what the strip shows without a cell.
            if (text[index] == U' ')
                continue;
            output.push_back(
                RenderGutterCell { .lineOffset = row.screenLine,
                                   .codepoint = text[index],
                                   .attributes = gutterCellAttributes(textColors),
                                   .column = columnOf(segment->firstColumn + static_cast<int>(index)) });
        }
    };
    appendText(GutterSegmentKind::Timestamp,
               unicode::convert_to<char32_t>(std::string_view { row.timestamp }));
    appendText(GutterSegmentKind::LineNumber, row.lineNumber.view());

    auto const block = layout.segment(GutterSegmentKind::BlockColumn);
    if (!block)
        return;

    // Ahead of the glyph, so the glyph draws over it where the two meet.
    if (row.band)
        output.push_back(RenderGutterCell {
            .lineOffset = row.screenLine,
            .codepoint = BlockBandGlyph,
            .attributes = blockColumnCellAttributes(
                *block, RGBColorPair { .foreground = *row.band, .background = row.glyphColors.background }),
            .column = columnOf(block->firstColumn) });

    if (row.glyph == BlockColumnGlyph::None)
        return;
    output.push_back(RenderGutterCell { .lineOffset = row.screenLine,
                                        .codepoint = blockColumnCodepoint(row.glyph),
                                        .attributes = blockColumnCellAttributes(*block, row.glyphColors),
                                        .column = columnOf(block->firstColumn) });
}

RenderAttributes blockColumnCellAttributes(GutterSegment const& block, RGBColorPair colors) noexcept
{
    auto attributes = gutterCellAttributes(colors);
    // Drawn across the whole column: the renderer rasterises a cell flagged DoubleWidth into a tile two
    // cells wide, as it does a DECDWL line's, and draws its background two cells wide with it.
    if (block.columns >= 2)
        attributes.lineFlags = LineFlag::DoubleWidth;
    return attributes;
}

std::u32string collapsedFoldLabel(uint64_t hiddenLines)
{
    constexpr auto Lead = std::u32string_view { U"⋯ " };
    auto const unit = hiddenLines == 1 ? std::u32string_view { U" line" } : std::u32string_view { U" lines" };
    auto const digits = std::format("{}", hiddenLines);
    auto const separators = (digits.size() - 1) / DigitsPerGroup;

    // Sized up front: the label is built for every collapsed fold on screen, every frame.
    auto label = std::u32string {};
    label.reserve(Lead.size() + digits.size() + separators + unit.size());
    label += Lead;
    for (auto const index: std::views::iota(std::size_t { 0 }, digits.size()))
    {
        if (index != 0 && (digits.size() - index) % DigitsPerGroup == 0)
            label += U',';
        label += static_cast<char32_t>(digits[index]);
    }
    label += unit;
    return label;
}

void layoutFoldLabel(std::vector<RenderCell>& output,
                     LineOffset screenLine,
                     ColumnCount textColumns,
                     ColumnCount pageColumns,
                     std::u32string_view label,
                     RenderAttributes const& attributes)
{
    // The layout stops at the page's edge, glyph by glyph: a wide one that would cross it is dropped whole.
    layoutRenderText(
        output,
        label,
        attributes,
        CellLocation { .line = screenLine, .column = ColumnOffset(unbox<int>(textColumns) + FoldLabelGap) },
        boxed_cast<ColumnOffset>(pageColumns));
}

} // namespace vtbackend
