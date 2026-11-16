// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/tui/GenericSyntaxHighlighter.hpp>
#include <core/tui/MarkdownHtml.hpp>
#include <core/tui/TerminalOutput.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace core::tui
{

class ImageProvider;

/// @brief Visual style for table rendering.
enum class TableRenderStyle : std::uint8_t
{
    Bordered, ///< Box-drawing borders (default).
    Compact,  ///< Bold header + underline separator, space-separated columns, no vertical borders.
    Plain,    ///< No borders or emphasis — plain text only.
};

/// @brief Optional callback for per-cell styling in table rendering.
///
/// Returns a custom Style for the cell at (row, col), or nullopt for the default.
/// @p row is 0-based data row index (headers are not passed through the callback).
using CellStyleFn = std::function<std::optional<Style>(size_t row, size_t col, std::string_view text)>;

/// @brief Theme for markdown rendering with terminal styles.
struct MarkdownTheme
{
    Style heading1;        ///< H1: bold + double-height.
    Style heading2;        ///< H2: bold + underline.
    Style heading3;        ///< H3: bold.
    Style headingEmphasis; ///< **bold** text within headings (distinct color to raise attention).
    Style codeBlock;       ///< Fenced code block.
    Style codeInline;      ///< Inline `code`.
    Style bold;            ///< **bold** text.
    Style italic;          ///< *italic* text.
    Style link;            ///< [link](url).
    Style listMarker;      ///< List bullet/number.
    Style blockquote;      ///< > quoted text.
    Style thinkBlock;      ///< <think>...</think> content.
    Style tableBorder;     ///< Box-drawing characters for table borders.
    Style tableHeader;     ///< Table header cell text.
    Style tableCell;       ///< Table data cell text.
};

/// @brief Renders markdown to a terminal output with styling.
///
/// Supports both batch rendering of complete markdown strings and streaming
/// rendering for incremental token output (e.g. from an LLM). Handles headings,
/// code blocks, inline code, bold, italic, links, lists, blockquotes, and
/// <think>...</think> blocks.
class MarkdownRenderer
{
  public:
    /// @brief Constructs a renderer targeting the given terminal output.
    /// @param output The terminal output to render to.
    /// @param theme The styling theme (defaults to defaultTheme()).
    /// @param highlighters The languages a fenced code block may be tagged with beyond the
    ///        built-in ones, or nullptr (the default) for the built-in ones alone. Non-owning:
    ///        the registry must outlive this renderer.
    explicit MarkdownRenderer(TerminalOutput& output,
                              MarkdownTheme theme = defaultTheme(),
                              SyntaxHighlighterRegistry const* highlighters = nullptr);

    /// @brief Renders a complete markdown string.
    /// @param markdown The markdown text to render.
    void render(std::string_view markdown);

    /// @brief Begins a streaming rendering session.
    void beginStream();

    /// @brief Feeds a token (partial text) for incremental rendering.
    /// @param token The next chunk of text from the LLM.
    void feedToken(std::string_view token);

    /// @brief Ends the streaming session and flushes remaining buffered content.
    void endStream();

    /// @brief Enables full-width mode for DEC line attribute headings.
    ///
    /// When enabled, H1 headings use double-height/double-width (ESC#3/ESC#4)
    /// and H2 headings use double-width (ESC#6). Only works correctly when
    /// rendering to the full terminal width.
    void setFullWidthMode(bool enabled) noexcept;

    /// @brief Sets the maximum width for table rendering with word wrapping.
    ///
    /// When set to a positive value, table columns are constrained to fit within
    /// this width, and cell text is word-wrapped as needed. A value of 0 disables
    /// width constraining (default).
    /// @param width Maximum table width in columns (0 = unconstrained).
    void setMaxWidth(int width) noexcept;

    /// @brief Sets the visual style for table rendering.
    /// @param style The desired table render style.
    void setTableRenderStyle(TableRenderStyle style) noexcept;

    /// @brief Sets a left margin, in cells, applied to every rendered line.
    ///
    /// A small indent separates the document from the terminal's left edge,
    /// which makes the first word of each line easier to read. The indent is
    /// subtracted from the width available to content, so centering and table
    /// widths account for it.
    ///
    /// @param columns Indent width in cells; negative values are treated as zero.
    void setIndent(int columns) noexcept;

    /// @brief Sets a per-cell style callback for custom table cell coloring.
    /// @param fn The callback, or nullptr to clear.
    void setCellStyleCallback(CellStyleFn fn);

    /// @brief Injects the source of inline images, enabling Sixel embedding.
    ///
    /// When set and the provider reports Sixel support, a line consisting only of
    /// an image (`![alt](path)` or `<img src="path">`) renders as a Sixel image.
    /// Otherwise — and on any load, decode or encode failure — the alt text is
    /// rendered instead. Images that share a line with other text always render
    /// as alt text.
    ///
    /// @param provider Non-owning pointer that must outlive this renderer,
    ///        or nullptr (the default) to disable image embedding.
    void setImageProvider(ImageProvider* provider) noexcept;

    /// @brief Returns the default theme with sensible terminal colors.
    [[nodiscard]] static auto defaultTheme() -> MarkdownTheme;

  private:
    TerminalOutput& _output;
    MarkdownTheme _theme;

    // Streaming state
    std::string _streamBuffer;
    bool _streaming = false;
    bool _fullWidthMode = false;
    int _maxWidth = 0; ///< Maximum width for table rendering (0 = unconstrained).
    int _indent = 0;   ///< Left margin in cells applied to every rendered line.
    TableRenderStyle _tableRenderStyle = TableRenderStyle::Bordered; ///< Visual table style.
    CellStyleFn _cellStyleFn;                                        ///< Optional per-cell style callback.
    bool _inCodeBlock = false;
    bool _inThinkBlock = false;
    std::string _codeFence; ///< The fence string (e.g. "```") that opened the current code block.
    LanguageId _codeLanguage = LanguageId::None;                 ///< Detected language of current code block.
    HighlightState _codeHighlightState = HighlightState::Normal; ///< Multi-line highlight state.

    /// @brief Languages registered beside the built-in ones; nullptr means built-ins only.
    SyntaxHighlighterRegistry const* _highlighters = nullptr;

    // Table buffering state
    bool _inTable = false;                ///< Currently buffering table rows.
    std::vector<std::string> _tableLines; ///< Buffered table lines.
    bool _tableSeparatorSeen = false;     ///< Separator row detected in buffered lines.

    ImageProvider* _imageProvider = nullptr; ///< Optional inline image source (nullptr disables).

    /// @brief One open GitHub-style HTML alignment container (`<div>`, `<p>`, `<center>`).
    struct AlignFrame
    {
        std::string tag; ///< Lowercased tag name, used to match the closing tag.
        HtmlAlign align; ///< Alignment applied to content inside this container.
    };

    std::vector<AlignFrame> _alignStack; ///< Innermost container last; empty means left-aligned.

    /// @brief Returns the alignment currently in effect (innermost container wins).
    [[nodiscard]] auto currentAlign() const noexcept -> HtmlAlign;

    /// @brief Returns the width available to content, i.e. the render width less the indent.
    [[nodiscard]] auto effectiveWidth() const noexcept -> int;

    /// @brief Writes the left margin. Call once at the start of every rendered line.
    void writeIndent();

    /// @brief Columns of leading space that align @p contentWidth within @p fieldWidth.
    ///
    /// Includes the indent, so a left-aligned line still starts at the margin.
    ///
    /// @param contentWidth Display width of the content.
    /// @param fieldWidth Width of the field to align within.
    /// @return The offset in cells, never less than the indent.
    [[nodiscard]] auto alignOffset(int contentWidth, int fieldWidth) const noexcept -> int;

    /// @brief Writes the leading spaces that align @p contentWidth within @p fieldWidth.
    /// @param contentWidth Display width of the content about to be written.
    /// @param fieldWidth Width of the field to align within.
    void writeAlignPadding(int contentWidth, int fieldWidth);

    /// @brief Handles a line that opens, closes, or wholly contains an HTML block tag.
    /// @param line The trimmed source line.
    /// @return true when the line was consumed as HTML.
    auto handleHtmlBlockLine(std::string_view line) -> bool;

    /// @brief Renders the inner content of an HTML block, splitting on `<br>`.
    /// @param content The inner text (may contain inline HTML and markdown).
    /// @param headingLevel 1-6 to render as a heading, 0 for a paragraph.
    void renderHtmlContent(std::string_view content, int headingLevel);

    /// @brief Renders a standalone image as Sixel, or its alt text on any failure.
    /// @param alt The image's alt text.
    /// @param src The image source as written in the document.
    /// @param widthPx Explicit pixel width from an HTML `width=` attribute, if any.
    void renderBlockImage(std::string_view alt, std::string_view src, std::optional<int> widthPx);

    /// @brief Renders an image's alt text as an aligned paragraph.
    ///
    /// The single fallback sink for every image failure, so the emitted bytes do
    /// not depend on which failure occurred.
    /// @param alt The alt text (may be empty).
    void renderImageAltText(std::string_view alt);

    /// @brief Renders a single line of markdown (not inside a code block).
    void renderLine(std::string_view line);

    /// @brief Renders inline markdown elements (bold, italic, code, links).
    /// @param text The inline text to render.
    /// @param baseStyle Optional style for plain text (nullptr = unstyled raw output).
    /// @param boldStyle Optional style override for **bold** text (nullptr = theme default).
    void renderInline(std::string_view text,
                      Style const* baseStyle = nullptr,
                      Style const* boldStyle = nullptr);

    /// @brief Processes buffered streaming content, rendering complete lines.
    void processStreamBuffer();

    /// @brief Handles a potential table line (buffers or flushes).
    /// @param line The line to check.
    /// @return true if the line was consumed as part of a table.
    auto handleTableLine(std::string_view line) -> bool;

    /// @brief Flushes buffered table lines, rendering or falling back to paragraphs.
    void flushTable();

    /// @brief Renders a fully parsed table with box-drawing borders.
    /// @param table The parsed table to render.
    void renderTable(struct ParsedTable const& table);

    /// @brief Renders a table in compact style (bold header + underline, no vertical borders).
    /// @param table The parsed table to render.
    /// @param showHeader Whether to bold the header and render an underline separator.
    void renderTableCompact(struct ParsedTable const& table, bool showHeader);

    /// @brief Renders a heading line.
    /// @param level The heading level (1-6).
    /// @param text The heading text.
    void renderHeading(int level, std::string_view text);
};

} // namespace core::tui
