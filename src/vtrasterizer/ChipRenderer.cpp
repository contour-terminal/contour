// SPDX-License-Identifier: Apache-2.0
#include <vtrasterizer/ChipRenderer.hpp>

#include <vtbackend/shell/BlockPill.hpp>

#include <vtrasterizer/RailGeometry.hpp>
#include <vtrasterizer/RenderTarget.hpp>
#include <vtrasterizer/TextRenderer.hpp>

#include <text_shaper/Shaper.hpp>

#include <algorithm>
#include <cmath>
#include <ranges>

namespace vtrasterizer
{

namespace
{
    /// How far a rounded box's edge stands in from its side on pixel row @p row.
    /// @param row The row, from the box's top.
    /// @param height The box's height.
    /// @param radius The corners' radius.
    /// @return The inset, in fractional pixels.
    [[nodiscard]] double insetAt(int row, int height, int radius) noexcept
    {
        auto const centre = static_cast<double>(row) + 0.5;
        // How far into a curved end the row's centre lies: from the top's centre down, or the bottom's up.
        auto const fromCurve = std::max({ 0.0, radius - centre, centre - (height - radius) });
        if (fromCurve <= 0.0)
            return 0.0;
        auto const r = static_cast<double>(radius);
        return r - std::sqrt(std::max(0.0, (r * r) - (fromCurve * fromCurve)));
    }

    /// @p color with its alpha scaled by @p coverage.
    [[nodiscard]] vtbackend::RGBAColor covered(vtbackend::RGBAColor color, double coverage) noexcept
    {
        return { color.rgb(),
                 static_cast<uint8_t>(std::lround(color.alpha() * std::clamp(coverage, 0.0, 1.0))) };
    }

    /// Appends the fill of @p box: the middle in one rectangle, each curved row as a span with covered edges.
    void appendFill(std::vector<PillQuad>& quads, RailRect box, int radius, vtbackend::RGBAColor color)
    {
        auto middleTop = std::optional<int> {};
        auto const flushMiddle = [&](int row) {
            if (middleTop)
                quads.push_back(PillQuad { .x = box.x,
                                           .y = box.y + *middleTop,
                                           .width = box.width,
                                           .height = row - *middleTop,
                                           .color = color });
            middleTop.reset();
        };
        for (auto const row: std::views::iota(0, box.height))
        {
            auto const inset = insetAt(row, box.height, radius);
            if (inset <= 0.0)
            {
                if (!middleTop)
                    middleTop = row;
                continue;
            }
            flushMiddle(row);
            auto const whole = static_cast<int>(std::floor(inset));
            auto const edge = 1.0 - (inset - whole);
            auto const y = box.y + row;
            auto const inner = box.width - (2 * (whole + 1));
            if (inner > 0)
                quads.push_back(
                    PillQuad { .x = box.x + whole + 1, .y = y, .width = inner, .height = 1, .color = color });
            if (box.width - (2 * whole) > 0)
            {
                quads.push_back(PillQuad {
                    .x = box.x + whole, .y = y, .width = 1, .height = 1, .color = covered(color, edge) });
                quads.push_back(PillQuad { .x = box.x + box.width - whole - 1,
                                           .y = y,
                                           .width = 1,
                                           .height = 1,
                                           .color = covered(color, edge) });
            }
        }
        flushMiddle(box.height);
    }

    /// The span a rounded box covers on @p row, in whole pixels: [left, right).
    [[nodiscard]] std::pair<int, int> spanAt(RailRect box, int radius, int row) noexcept
    {
        auto const inset = static_cast<int>(std::lround(insetAt(row, box.height, radius)));
        return { box.x + inset, box.x + box.width - inset };
    }

    /// Appends the ring of @p box: each row's span less the span of the box @p width inside it.
    void appendRing(
        std::vector<PillQuad>& quads, RailRect box, int radius, int width, vtbackend::RGBAColor color)
    {
        auto const inner = RailRect { .x = box.x + width,
                                      .y = box.y + width,
                                      .width = box.width - (2 * width),
                                      .height = box.height - (2 * width) };
        auto const innerRadius = std::max(0, radius - width);
        for (auto const row: std::views::iota(0, box.height))
        {
            auto const [left, right] = spanAt(box, radius, row);
            auto const y = box.y + row;
            auto const innerRow = y - inner.y;
            if (inner.width <= 0 || innerRow < 0 || innerRow >= inner.height)
            {
                if (right > left)
                    quads.push_back(
                        PillQuad { .x = left, .y = y, .width = right - left, .height = 1, .color = color });
                continue;
            }
            auto const [innerLeft, innerRight] = spanAt(inner, innerRadius, innerRow);
            if (innerLeft > left)
                quads.push_back(
                    PillQuad { .x = left, .y = y, .width = innerLeft - left, .height = 1, .color = color });
            if (right > innerRight)
                quads.push_back(PillQuad {
                    .x = innerRight, .y = y, .width = right - innerRight, .height = 1, .color = color });
        }
    }

    /// The CSS weights and the font weight each stands for, ascending.
    constexpr auto CssWeights = std::array {
        std::pair { 100, text::FontWeight::Thin },   std::pair { 200, text::FontWeight::ExtraLight },
        std::pair { 300, text::FontWeight::Light },  std::pair { 400, text::FontWeight::Normal },
        std::pair { 500, text::FontWeight::Medium }, std::pair { 600, text::FontWeight::DemiBold },
        std::pair { 700, text::FontWeight::Bold },   std::pair { 800, text::FontWeight::ExtraBold },
        std::pair { 900, text::FontWeight::Black },
    };

    void drawQuads(RenderTarget& target, std::vector<PillQuad> const& quads)
    {
        for (auto const& quad: quads)
            target.renderRectangle(quad.x,
                                   quad.y,
                                   vtbackend::Width::cast_from(quad.width),
                                   vtbackend::Height::cast_from(quad.height),
                                   quad.color);
    }
} // namespace

std::vector<PillQuad> pillQuads(RailRect box,
                                int radius,
                                std::optional<vtbackend::RGBAColor> fill,
                                std::optional<vtbackend::RGBAColor> ring,
                                int ringWidth)
{
    auto quads = std::vector<PillQuad> {};
    if (box.width <= 0 || box.height <= 0)
        return quads;
    auto const r = std::clamp(radius, 0, std::min(box.width, box.height) / 2);
    if (fill)
        appendFill(quads, box, r, *fill);
    if (ring && ringWidth > 0)
        appendRing(quads, box, r, ringWidth, *ring);
    return quads;
}

RailRect pillBoxOf(vtbackend::RenderChip const& chip,
                   PillMetrics const& pill,
                   int labelWidth,
                   int labelLineHeight,
                   GridMetrics const& metrics,
                   int smoothScrollOffset) noexcept
{
    auto const left = metrics.mapTopLeft(chip.lineOffset, chip.columns.fromColumn, smoothScrollOffset);
    auto const right = metrics
                           .mapTopLeft(chip.lineOffset,
                                       chip.columns.toColumn + vtbackend::ColumnOffset(1),
                                       smoothScrollOffset)
                           .x;
    auto const rowHeight = metrics.cellSize.height.as<int>();
    auto const width = std::min(labelWidth + (2 * pill.padX), right - left.x);
    auto const wanted = pill.height > 0 ? pill.height : labelLineHeight + (2 * pill.padY);
    auto const height = std::clamp(wanted, 0, rowHeight);
    auto const anchor = vtbackend::pillStyleRow(chip.style).anchor;
    return RailRect { .x = anchor == vtbackend::PillAnchor::Right ? right - width : left.x,
                      .y = left.y + ((rowHeight - height) / 2),
                      .width = width,
                      .height = height };
}

text::FontWeight fontWeightOf(int weight) noexcept
{
    auto const nearest = std::ranges::min_element(
        CssWeights, {}, [weight](auto const& row) { return std::abs(row.first - weight); });
    return nearest->second;
}

ChipRenderer::ChipRenderer(GridMetrics const& gridMetrics,
                           TextRenderer& textRenderer,
                           FontDescriptions const& fontDescriptions,
                           FontKeys const& fontKeys):
    _gridMetrics { gridMetrics },
    _textRenderer { textRenderer },
    _fontDescriptions { fontDescriptions },
    _fontKeys { fontKeys }
{
}

void ChipRenderer::clearCache()
{
    _uiFonts.clear();
    _measuredFor.reset();
}

text::FontKey ChipRenderer::uiFont(int weight, double sizePt)
{
    // Never larger than the grid's own text: a label's glyph is a tile of the grid's atlas, one cell tall.
    auto const size = sizePt > 0.0 ? std::min(sizePt, _fontDescriptions.size.pt) : _fontDescriptions.size.pt;
    auto const key = std::pair { weight, size };
    if (auto const loaded = _uiFonts.find(key); loaded != _uiFonts.end())
        return loaded->second;

    auto description = text::FontDescription {};
    description.familyName = std::string(text::SystemUiFontFamily);
    description.weight = fontWeightOf(weight);
    description.spacing = text::FontSpacing::Proportional;
    // The platform's UI font, through the locator the grid's shaper was given; where none loads, the grid's
    // regular face -- a bundled one, at worst -- says the same words.
    auto const font =
        _textRenderer.textShaper().loadFont(description, text::FontSize { size }).value_or(_fontKeys.regular);
    _uiFonts.emplace(key, font);
    return font;
}

vtbackend::PillLabelMetrics const& ChipRenderer::labelMetrics()
{
    auto const& chips = _gridMetrics.rail.chips;
    auto const cellWidth = _gridMetrics.cellSize.width.as<int>();
    auto const measuredFor = std::pair { chips, cellWidth };
    if (_measuredFor == measuredFor)
        return _labelMetrics;
    _measuredFor = measuredFor;
    _labelMetrics = {};

    // A row that draws no pills -- the Terminal row's metrics are all zero -- is not measured.
    if (chips == BlockChipMetrics {} || cellWidth <= 0)
        return _labelMetrics;

    _labelMetrics.cellWidth = cellWidth;
    for (auto const style: std::views::iota(std::size_t { 0 }, vtbackend::PillStyleCount))
    {
        auto const& pill = chips.pills[style];
        auto const font = uiFont(pill.weight, pill.fontSize);
        auto& face = _labelMetrics.styles[style];
        face.padding = 2 * pill.padX;
        for (auto const index: std::views::iota(std::size_t { 0 }, vtbackend::PillMeasuredCount))
        {
            auto const glyph =
                _textRenderer.textShaper().shape(font, vtbackend::pillMeasuredCodepoint(index));
            face.advances[index] = glyph ? glyph->advance.x : 0;
        }
        face.widest = std::ranges::max(face.advances);
        // A codepoint the face lacks counts as its widest one: an over-estimate covers a column too many,
        // never too few.
        for (auto& advance: face.advances)
            if (advance == 0)
                advance = face.widest;
    }
    return _labelMetrics;
}

void ChipRenderer::renderChip(RenderTarget& target, vtbackend::RenderChip const& chip, int smoothScrollOffset)
{
    auto const& pill = _gridMetrics.rail.chips.pills[static_cast<std::size_t>(chip.style)];
    auto const font = uiFont(pill.weight, pill.fontSize);
    auto const fontMetrics = _textRenderer.textShaper().metrics(font);
    auto const labelWidth = _textRenderer.labelWidth(font, chip.text);
    auto const box =
        pillBoxOf(chip, pill, labelWidth, fontMetrics.lineHeight, _gridMetrics, smoothScrollOffset);

    // Cut to what the box holds inside its padding: the box was clamped to the placed columns, the text must
    // not outrun it.
    auto const label = fittedLabel(chip.text, box.width - (2 * pill.padX), [&](std::u32string_view text) {
        return _textRenderer.labelWidth(font, text);
    });
    auto const shownWidth =
        label.size() == chip.text.size() ? labelWidth : _textRenderer.labelWidth(font, label);

    drawQuads(target, pillQuads(box, pill.radius, chip.fill, chip.ring, pill.ring));

    // Centred in the pill, the label's line centred on the row.
    auto const rowTop =
        _gridMetrics.mapTopLeft(chip.lineOffset, chip.columns.fromColumn, smoothScrollOffset).y;
    auto const rowHeight = _gridMetrics.cellSize.height.as<int>();
    auto const baseline = rowTop + ((rowHeight - fontMetrics.lineHeight) / 2) + fontMetrics.ascender;
    _textRenderer.renderLabel(crispy::Point { .x = box.x + ((box.width - shownWidth) / 2), .y = baseline },
                              font,
                              label,
                              vtbackend::RGBAColor(chip.ink));
}

void ChipRenderer::renderMark(RenderTarget& target,
                              vtbackend::RenderGutterCell const& cell,
                              vtbackend::RGBColor page,
                              int smoothScrollOffset)
{
    auto const& face = railMarkFace(cell.mark);
    auto const rowTop =
        _gridMetrics.mapTopLeft(cell.lineOffset, vtbackend::ColumnOffset(0), smoothScrollOffset).y;
    auto const box = railMarkDiscBox(_gridMetrics, rowTop);
    auto const markColor = cell.attributes.foregroundColor;
    if (face.shape == MarkShape::Disc)
        drawQuads(target, pillQuads(box, box.width / 2, vtbackend::RGBAColor(markColor), std::nullopt, 0));

    auto const& chips = _gridMetrics.rail.chips;
    auto const font = uiFont(chips.markWeight, chips.markFontSize);
    auto const fontMetrics = _textRenderer.textShaper().metrics(font);
    auto const glyph = std::u32string(1, face.glyph);
    auto const width = _textRenderer.labelWidth(font, glyph);
    auto const centreY = box.y + (box.height / 2);
    _textRenderer.renderLabel(
        crispy::Point { .x = box.x + ((box.width - width) / 2),
                        .y = centreY - (fontMetrics.lineHeight / 2) + fontMetrics.ascender },
        font,
        glyph,
        vtbackend::RGBAColor(face.shape == MarkShape::Disc ? page : markColor));
}

} // namespace vtrasterizer
