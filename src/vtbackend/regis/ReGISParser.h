// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/regis/ReGISColor.h>
#include <vtbackend/regis/ReGISContext.h>
#include <vtbackend/regis/ReGISRasterizer.h>
#include <vtbackend/regis/ReGISTables.h>
#include <vtbackend/regis/ReGISTextRasterizer.h>

#include <vtparser/ParserExtension.h>

#include <crispy/point.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vtbackend::regis
{

/// The host-facing side effects a ReGIS interpreter needs, injected so the parser stays testable.
///
/// The @c R (report) command sends replies back to the application; a test double records them
/// instead of writing to a PTY.
class ReGISEvents
{
  public:
    virtual ~ReGISEvents() = default;

    /// Sends @p data back to the host application (the reply to an @c R report command).
    virtual void reply(std::string_view data) = 0;

    /// @return the interactive locator (mouse) position in ReGIS user coordinates, or nullopt if no
    /// locator is available. Used by @c R(P(I)); Contour reports the position immediately rather than
    /// blocking the parser waiting for a click.
    [[nodiscard]] virtual std::optional<std::pair<int, int>> locatorPosition() const { return std::nullopt; }
};

/// A @ref ReGISEvents that forwards to callbacks, bridging to the terminal reply and locator state.
class CallbackReGISEvents final: public ReGISEvents
{
  public:
    explicit CallbackReGISEvents(std::function<void(std::string_view)> onReply,
                                 std::function<std::optional<std::pair<int, int>>()> onLocator = {}):
        _onReply { std::move(onReply) }, _onLocator { std::move(onLocator) }
    {
    }

    void reply(std::string_view data) override
    {
        if (_onReply)
            _onReply(data);
    }

    [[nodiscard]] std::optional<std::pair<int, int>> locatorPosition() const override
    {
        return _onLocator ? _onLocator() : std::nullopt;
    }

  private:
    std::function<void(std::string_view)> _onReply;
    std::function<std::optional<std::pair<int, int>>()> _onLocator;
};

/// A ReGIS command-string interpreter, driven as a DCS sub-parser.
///
/// Because ReGIS is context-sensitive (a digit is a multi-digit number inside @c [...] but a single
/// pixel-vector step outside it), the payload is buffered as it arrives and parsed as a whole on
/// @ref finalize -- the approach mintty uses. The parser references (does not own) the persistent
/// @ref ReGISContext and the @ref ReGISRasterizer canvas, so ReGIS state and pixels survive across
/// DCS strings; only drawing that actually happened triggers the commit callback.
class ReGISParser: public ParserExtension
{
  public:
    /// @param context The persistent interpreter state (mutated in place).
    /// @param canvas The pixel canvas drawn into (mutated in place).
    /// @param events The reply sink for @c R commands.
    /// @param onCommit Invoked from @ref finalize when the payload drew anything, to publish the canvas.
    /// @param textRasterizer Shared ownership of the glyph rasterizer. Held as a @c shared_ptr (not a
    ///        raw reference) so that a session rebind reassigning the display's rasterizer mid-DCS
    ///        cannot free the referent while this parser -- which persists across PTY read chunks --
    ///        still uses it.
    ReGISParser(ReGISContext& context,
                ReGISRasterizer& canvas,
                std::shared_ptr<ReGISTextRasterizer const> textRasterizer,
                ReGISEvents& events,
                std::function<void()> onCommit);

    void pass(char ch) override;
    void pass(std::string_view bytes) override;
    void finalize() override;

    /// Records that the canvas was cleared externally (a Pmode 1/3 reset), so @ref finalize publishes
    /// it even when the payload draws nothing. Without this a reset-only DCS string would erase the
    /// canvas but never refresh the grid, leaving the previously committed graphics on screen.
    void notifyCanvasCleared() noexcept { _committedStateDirty = true; }

    /// Parses a complete ReGIS command string into @p context and @p canvas (test entry point).
    /// @return whether anything was drawn.
    static bool parse(std::string_view regis,
                      ReGISContext& context,
                      ReGISRasterizer& canvas,
                      ReGISTextRasterizer const& textRasterizer,
                      ReGISEvents& events);

  private:
    // --- scanning over the buffered payload ------------------------------------------------------
    [[nodiscard]] bool atEnd() const noexcept { return _pos >= _buffer.size(); }
    [[nodiscard]] char peek() const noexcept { return atEnd() ? '\0' : _buffer[_pos]; }
    char advance() noexcept { return _buffer[_pos++]; }
    void skipWhitespace() noexcept;
    [[nodiscard]] std::optional<double> scanNumber() noexcept; ///< signed fixed-point, or nullopt.
    [[nodiscard]] unsigned scanUnsigned() noexcept;
    [[nodiscard]] std::string scanQuotedString() noexcept;

    // --- grammar ---------------------------------------------------------------------------------
    void parseAll();
    /// Parses a parenthesized option set for @p command. @p depth tracks the current nesting level so
    /// a pathological run of nested @c ( cannot overflow the parser-thread stack (see MaxOptionSetDepth).
    void parseOptionSet(Command command, int depth = 0);
    void applyWriteOption(char option);
    void applyScreenOption(char option);
    void applyPositionOption(char option, Command command);
    void applyReportOption(char option);
    void applyTextOption(char option);
    /// Parses an optional @c [width,height] extent following a text size option and, if present, sets
    /// the text cell size clamped to @ref MaxTextCellExtent. Shared by @c T(S[...]) and @c T(U[...]).
    /// @return whether a bracketed extent was found and consumed.
    bool applyBracketedCellSize();
    void applyCurveOption(char option);
    void drawText(std::string_view text);
    void drawCurveItem(crispy::point p);
    void parseFill();
    void parseCoordinateItem(Command command);
    void parsePixelVectorRun(Command command);

    // --- coordinate helpers ----------------------------------------------------------------------
    struct Coord
    {
        std::optional<double> x;
        bool xRelative = false;
        std::optional<double> y;
        bool yRelative = false;
        [[nodiscard]] bool empty() const noexcept { return !x && !y; }
    };
    [[nodiscard]] Coord scanCoordinate() noexcept;
    [[nodiscard]] crispy::point resolveCoordinate(Coord const& coord) const noexcept;

    // --- colour ----------------------------------------------------------------------------------
    [[nodiscard]] unsigned parseColorReference() noexcept; ///< register index for W(I..)/S(I..).
    [[nodiscard]] std::optional<RGBColor> parseColorSpecParens() noexcept; ///< the "( ... )" body.

    void moveTo(crispy::point p) noexcept;
    void drawLineTo(crispy::point p) noexcept;

    ReGISContext& _context;
    ReGISRasterizer& _canvas;
    std::shared_ptr<ReGISTextRasterizer const> _textRasterizer;
    ReGISEvents& _events;
    std::function<void()> _onCommit;

    std::string _buffer;               ///< The accumulated DCS payload.
    std::size_t _pos = 0;              ///< The scan cursor into @ref _buffer.
    bool _drew = false;                ///< Whether any drawing happened this hook.
    bool _committedStateDirty = false; ///< Whether the canvas changed externally (reset) and must commit.

    // Collects vertices while parsing an F(V ...) polygon; empty otherwise.
    std::vector<crispy::point>* _fillSink = nullptr;

    // Curve (C command) state.
    bool _curveCenterMode = false;   ///< Whether the bracket names the centre (C(C)) vs. circumference.
    std::optional<double> _curveArc; ///< Arc sweep in degrees (C(A<deg>)), or nullopt for a full circle.
    bool _collectingCurve = false;   ///< Whether inside a C(B)/(S)...(E) interpolated-curve sequence.
    bool _curveClosed = false;       ///< Whether the collected curve is closed (C(B)) vs. open (C(S)).
    std::vector<crispy::point> _curvePoints; ///< Control points of the interpolated curve.
};

} // namespace vtbackend::regis
