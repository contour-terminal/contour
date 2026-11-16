// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/tui/TerminalOutput.hpp>

#include <cstddef>
#include <optional>

namespace core::tui
{

/// @brief Position of a grapheme cluster in the InputField buffer.
///
/// Passed to TextDecorator methods to identify which grapheme cluster is being styled.
/// The graphemeIndex is the primary key; byteOffset is provided as a convenience for
/// implementations that already have byte-indexed data (e.g., syntax highlight maps).
/// The byteOffset always points to the first byte of a valid UTF-8 grapheme cluster.
struct TextPosition
{
    std::size_t graphemeIndex; ///< 0-based grapheme cluster index in the buffer.
    std::size_t byteOffset;    ///< Byte offset of the cluster's first byte.
};

/// @brief Interface for customizing per-grapheme and per-column text rendering in InputField.
///
/// Callers implement this to provide syntax highlighting, error underlines, and
/// custom backgrounds. InputField queries the decorator during render().
class TextDecorator
{
  public:
    virtual ~TextDecorator() = default;

    /// @brief Returns the foreground color for a grapheme cluster.
    /// @param pos Position of the grapheme cluster in the buffer.
    /// @return The foreground color, or nullopt to use the default text style.
    [[nodiscard]] virtual auto foreground(TextPosition pos) const -> std::optional<RgbColor>
    {
        (void) pos;
        return {};
    }

    /// @brief Underline decoration for a grapheme cluster.
    struct UnderlineDecoration
    {
        UnderlineStyle style = UnderlineStyle::Curly;
        RgbColor color;
    };

    /// @brief Returns underline decoration for a grapheme cluster.
    /// @param pos Position of the grapheme cluster in the buffer.
    /// @return The underline decoration, or nullopt for no underline.
    [[nodiscard]] virtual auto underline(TextPosition pos) const -> std::optional<UnderlineDecoration>
    {
        (void) pos;
        return {};
    }

    /// @brief Returns the background color for a display column in the text content area.
    /// @param displayCol Display column (0-based, relative to the InputField canvas).
    /// @return The background color, or nullopt to use the default background.
    [[nodiscard]] virtual auto background(int displayCol) const -> std::optional<RgbColor>
    {
        (void) displayCol;
        return {};
    }
};

} // namespace core::tui
