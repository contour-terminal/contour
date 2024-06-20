// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/CellFlags.h>
#include <vtbackend/Color.h>
#include <vtbackend/ColorPalette.h>
#include <vtbackend/GraphicsAttributes.h>
#include <vtbackend/Hyperlink.h>
#include <vtbackend/Image.h>
#include <vtbackend/primitives.h>

#include <concepts>

namespace vtbackend
{

/**
 * Terminal Cell Concept!
 *
 * We're doing this in order to to eventually support two different - yet most efficient -
 * Cell implementations. One for the primary screen and one for the alternate screen.
 *
 * The primary screen's grid cell will have different use patterns than the one for the alternate screen,
 * which makes it a perfect contender to optimize the Cell's implementation based on their use.
 *
 * The Cell for the primary screen must be lightweight and fast for the standard text-scrolling case,
 * whereas the alternate-screen's Cell will most likely use all the Cell's features a intensively
 * but won't be needed for scrollback.
 */
template <typename T>
concept CellConcept = requires(T t, T const& u) {
    T(GraphicsAttributes {});
    T(GraphicsAttributes {}, HyperlinkId {});

    t.reset();
    t.reset(GraphicsAttributes {});
    t.reset(GraphicsAttributes {}, HyperlinkId {});

    { u.empty() } noexcept -> std::same_as<bool>;

    t.write(GraphicsAttributes {}, char32_t {}, uint8_t {});
    t.write(GraphicsAttributes {}, char32_t {}, uint8_t {}, HyperlinkId {});
    t.writeTextOnly(char32_t {}, uint8_t {});

    { u.codepoints() } -> std::convertible_to<std::u32string>;
    { u.codepoint(size_t {}) } noexcept -> std::same_as<char32_t>;
    { u.codepointCount() } noexcept -> std::same_as<size_t>;

    t.setCharacter(char32_t {});
    { t.appendCharacter(char32_t {}) } -> std::same_as<int>;

    { u.toUtf8() } -> std::convertible_to<std::string>;

    { u.width() } noexcept -> std::convertible_to<uint8_t>;
    { t.setWidth(uint8_t {}) } noexcept;

    { u.flags() } noexcept -> std::same_as<CellFlags>;
    { u.isFlagEnabled(CellFlags {}) } noexcept -> std::same_as<bool>;
    t.resetFlags();
    t.resetFlags(CellFlags {});

    t.setGraphicsRendition(GraphicsRendition {});

    t.setForegroundColor(Color {});
    { u.foregroundColor() } noexcept -> std::same_as<Color>;

    t.setBackgroundColor(Color {});
    { u.backgroundColor() } noexcept -> std::same_as<Color>;

    t.setUnderlineColor(Color {});
    { u.underlineColor() } noexcept -> std::same_as<Color>;

    { u.imageFragment() } -> std::same_as<std::shared_ptr<ImageFragment>>;
    t.setImageFragment(std::shared_ptr<RasterizedImage> {}, CellLocation {} /*offset*/);

    { u.hyperlink() } -> std::same_as<HyperlinkId>;
    t.setHyperlink(HyperlinkId {});
};

} // namespace vtbackend
