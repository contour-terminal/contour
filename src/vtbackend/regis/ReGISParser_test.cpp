// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/regis/ReGISParser.h>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace vtbackend;
using namespace vtbackend::regis;
using crispy::point;

namespace
{

struct RecordingEvents: ReGISEvents
{
    std::vector<std::string> replies;
    std::optional<std::pair<int, int>> locator;
    void reply(std::string_view data) override { replies.emplace_back(data); }
    [[nodiscard]] std::optional<std::pair<int, int>> locatorPosition() const override { return locator; }
};

/// A parser test fixture with a canvas whose addressing window maps user coordinates 1:1 to pixels,
/// so an assertion can name pixels in the same coordinates the ReGIS string uses.
struct Fixture
{
    ReGISContext context;
    ReGISRasterizer canvas;
    EmbeddedReGISTextRasterizer textRasterizer;
    RecordingEvents events;

    Fixture(int w, int h): canvas { ImageSize { Width(w), Height(h) } }
    {
        context.canvasSize = ImageSize { Width(w), Height(h) };
        context.window = AddressWindow { .x0 = 0, .y0 = 0, .x1 = double(w - 1), .y1 = double(h - 1) };
    }

    bool run(std::string_view regis)
    {
        return ReGISParser::parse(regis, context, canvas, textRasterizer, events);
    }
    [[nodiscard]] bool painted(int x, int y) const { return canvas.at(x, y).alpha() == 0xFF; }
    [[nodiscard]] bool inked(int x, int y) const { return canvas.at(x, y).alpha() != 0; }
};

} // namespace

TEST_CASE("ReGISParser.position.absolute", "[regis][parser]")
{
    auto fx = Fixture { 100, 100 };
    CHECK_FALSE(fx.run("P[30,40]")); // P moves; it draws nothing
    CHECK(fx.context.position == point { .x = 30, .y = 40 });
}

TEST_CASE("ReGISParser.position.relative", "[regis][parser]")
{
    auto fx = Fixture { 100, 100 };
    fx.run("P[30,40]P[+5,-10]");
    CHECK(fx.context.position == point { .x = 35, .y = 30 });
}

TEST_CASE("ReGISParser.position.partialAxis", "[regis][parser]")
{
    auto fx = Fixture { 100, 100 };
    fx.run("P[30,40]P[50]"); // x only; y unchanged
    CHECK(fx.context.position == point { .x = 50, .y = 40 });
    fx.run("P[,70]"); // y only; x unchanged
    CHECK(fx.context.position == point { .x = 50, .y = 70 });
}

TEST_CASE("ReGISParser.vector.line", "[regis][parser]")
{
    auto fx = Fixture { 20, 8 };
    CHECK(fx.run("P[2,3]V[10,3]"));
    for (auto x = 2; x <= 10; ++x)
        CHECK(fx.painted(x, 3));
    CHECK(fx.context.position == point { .x = 10, .y = 3 });
}

TEST_CASE("ReGISParser.vector.dot", "[regis][parser]")
{
    auto fx = Fixture { 20, 8 };
    CHECK(fx.run("P[5,5]V[]"));
    CHECK(fx.painted(5, 5));
    CHECK_FALSE(fx.painted(6, 5));
}

TEST_CASE("ReGISParser.vector.pixelVector", "[regis][parser]")
{
    auto fx = Fixture { 20, 8 };
    // Three steps to the right (direction 0), one pixel each: draws (2,2)..(5,2).
    CHECK(fx.run("P[2,2]V000"));
    for (auto x = 2; x <= 5; ++x)
        CHECK(fx.painted(x, 2));
    CHECK(fx.context.position == point { .x = 5, .y = 2 });
}

TEST_CASE("ReGISParser.write.pixelVectorMultiplier", "[regis][parser]")
{
    auto fx = Fixture { 40, 8 };
    // W(M5) makes each pixel-vector step 5 pixels.
    fx.run("W(M5)P[2,2]V0");
    CHECK(fx.context.position == point { .x = 7, .y = 2 });
}

TEST_CASE("ReGISParser.write.foregroundRegister", "[regis][parser]")
{
    auto fx = Fixture { 20, 4 };
    // Register 2 is red (204,33,33) on the VT340.
    fx.run("W(I2)P[0,0]V[5,0]");
    auto const c = fx.canvas.at(3, 0);
    CHECK(c.red() == 204);
    CHECK(c.green() == 33);
    CHECK(c.blue() == 33);
}

TEST_CASE("ReGISParser.write.namedColor", "[regis][parser]")
{
    auto fx = Fixture { 20, 4 };
    // W(I(G)) selects the register closest to pure green.
    fx.run("W(I(G))P[0,0]V[5,0]");
    auto const c = fx.canvas.at(3, 0);
    CHECK(c.green() > c.red());
    CHECK(c.green() > c.blue());
}

TEST_CASE("ReGISParser.screen.erase", "[regis][parser]")
{
    auto fx = Fixture { 6, 6 };
    // Set an opaque blue background (register 1) then erase to it.
    fx.run("S(I1)S(E)");
    auto const c = fx.canvas.at(3, 3);
    CHECK(c.alpha() == 0xFF);
    CHECK(c.blue() > c.red()); // register 1 is blue-ish
}

TEST_CASE("ReGISParser.screen.addressWindow", "[regis][parser]")
{
    auto fx = Fixture { 100, 100 };
    // Shrink the addressing window to 0..9; user coordinate 9 now maps to the far pixel edge.
    fx.run("S(A[0,0][9,9])P[9,9]");
    CHECK(fx.context.position.x == 99);
    CHECK(fx.context.position.y == 99);
}

TEST_CASE("ReGISParser.fill.polygon", "[regis][parser]")
{
    auto fx = Fixture { 20, 20 };
    // Start at (2,2); the fill outline closes (2,2)->(15,2)->(15,15)->(2,15).
    CHECK(fx.run("P[2,2]F(V[15,2][15,15][2,15])"));
    CHECK(fx.painted(8, 8));         // interior
    CHECK_FALSE(fx.painted(18, 18)); // exterior
}

TEST_CASE("ReGISParser.report.position", "[regis][parser]")
{
    auto fx = Fixture { 800, 480 };
    fx.run("P[100,50]R(P)");
    REQUIRE(fx.events.replies.size() == 1);
    CHECK(fx.events.replies[0] == "[100,50]\r");
}

TEST_CASE("ReGISParser.text.drawsGlyphs", "[regis][parser]")
{
    auto fx = Fixture { 200, 40 };
    // Size 1 is a 9x20 cell; draw two characters and check some pixels were set within the first.
    CHECK(fx.run("P[0,0]T(S1)'HI'"));
    auto anyInk = false;
    for (auto y = 0; y < 20 && !anyInk; ++y)
        for (auto x = 0; x < 18 && !anyInk; ++x)
            if (fx.inked(x, y))
                anyInk = true;
    CHECK(anyInk);
    // The cursor advanced by two cell widths (2 * 9).
    CHECK(fx.context.position.x == 18);
}

TEST_CASE("ReGISParser.text.sizeSelectsCell", "[regis][parser]")
{
    auto fx = Fixture { 400, 300 };
    fx.run("T(S3)");
    CHECK(fx.context.textCellWidth == 27);
    CHECK(fx.context.textCellHeight == 45);
}

TEST_CASE("ReGISParser.text.directionAdvances", "[regis][parser]")
{
    auto fx = Fixture { 100, 200 };
    // Direction 90 degrees writes upward: the cursor y decreases by the cell height per character.
    fx.run("P[10,100]T(S0)(D90)'X'");
    CHECK(fx.context.position.x == 10);
    CHECK(fx.context.position.y == 90); // one 10px cell upward
}

TEST_CASE("ReGISParser.text.heightMultiplierAssignsNotCompounds", "[regis][parser]")
{
    auto fx = Fixture { 400, 300 };
    // T(H<n>) sets the cell height to n x the base character height (10), matching xterm; repeating it
    // must not compound (the original bug doubled the height on every use, e.g. 10->20->40).
    fx.run("T(H2)");
    CHECK(fx.context.textCellHeight == 20);
    fx.run("T(H2)");
    CHECK(fx.context.textCellHeight == 20); // still 20, not 40
    fx.run("T(H3)");
    CHECK(fx.context.textCellHeight == 30);
}

TEST_CASE("ReGISParser.text.cellSizeIsBounded", "[regis][parser]")
{
    auto fx = Fixture { 400, 300 };
    // A hostile T(S[w,h]) must be clamped so the text rasterizer cannot be asked for a giant glyph
    // mask (an unbounded value would request a multi-gigabyte allocation -- a denial of service).
    fx.run("T(S[100000,100000])");
    CHECK(fx.context.textCellWidth == MaxTextCellExtent);
    CHECK(fx.context.textCellHeight == MaxTextCellExtent);
}

TEST_CASE("ReGISParser.text.cellsScaleWithSupersample", "[regis][parser]")
{
    auto fx = Fixture { 400, 300 };
    fx.context.supersample = 2;
    // Text cells are specified in logical pixels but drawn on the supersampled canvas, so the pen
    // advances by the scaled cell width (standard-size-0 width 9 x 2 = 18) per character.
    fx.run("P[0,100]T(S0)(D0)'X'");
    CHECK(fx.context.position.x == 18);
}

TEST_CASE("ReGISParser.resetOnlyPayloadStillCommits", "[regis][parser]")
{
    auto fx = Fixture { 40, 40 };
    auto commits = 0;
    // Non-owning shared_ptr: the fixture owns the rasterizer's lifetime.
    auto const rasterizer =
        std::shared_ptr<ReGISTextRasterizer const> { std::shared_ptr<void> {}, &fx.textRasterizer };

    // A reset clears the canvas externally; the parser must still fire the commit callback even when
    // the payload draws nothing, so the cleared canvas is republished. Otherwise a reset-only DCS
    // string would leave the previously committed graphics visible on the grid.
    auto cleared = ReGISParser { fx.context, fx.canvas, rasterizer, fx.events, [&] { ++commits; } };
    cleared.notifyCanvasCleared();
    cleared.finalize(); // empty payload
    CHECK(commits == 1);

    // Control: with no clear and nothing drawn, there is nothing to publish.
    auto quiet = ReGISParser { fx.context, fx.canvas, rasterizer, fx.events, [&] { ++commits; } };
    quiet.finalize();
    CHECK(commits == 1); // unchanged
}

TEST_CASE("ReGISParser.reset.preservesDisplayProperties", "[regis][parser]")
{
    // reset() restores ReGIS state to power-up defaults but must preserve the physical display
    // properties (supersample factor and canvas buffer size), so the context stays consistent with the
    // actual canvas after a Pmode 1/3 reset.
    auto context = ReGISContext {};
    context.supersample = 3;
    context.canvasSize = ImageSize { Width(2400), Height(1440) };
    context.foregroundRegister = 2;                // a piece of ReGIS state that must be reset
    context.position = point { .x = 50, .y = 60 }; // ditto

    context.reset();

    CHECK(context.supersample == 3);
    CHECK(context.canvasSize == ImageSize { Width(2400), Height(1440) });
    CHECK(context.foregroundRegister == 7);              // default restored
    CHECK(context.position == point { .x = 0, .y = 0 }); // default restored
}

TEST_CASE("ReGISParser.text.multiplierNeverCollapsesCell", "[regis][parser]")
{
    auto fx = Fixture { 400, 300 };
    // T(M[0,0]) must not truncate the cell to 0 (which would make all later text vanish); a zero or
    // sub-unit multiplier is clamped up to the minimum cell extent of 1.
    fx.run("T(S1)");
    fx.run("T(M[0,0])");
    CHECK(fx.context.textCellWidth >= 1);
    CHECK(fx.context.textCellHeight >= 1);
}

TEST_CASE("ReGISParser.text.negativeCellSizeIsClamped", "[regis][parser]")
{
    auto fx = Fixture { 400, 300 };
    // A negative multiplier must be clamped (not cast out-of-range to unsigned, which is UB).
    fx.run("T(M[-1,-1])");
    CHECK(fx.context.textCellWidth >= 1);
    CHECK(fx.context.textCellWidth <= MaxTextCellExtent);
    CHECK(fx.context.textCellHeight >= 1);
    CHECK(fx.context.textCellHeight <= MaxTextCellExtent);
}

TEST_CASE("ReGISParser.position.nestedStackIsLIFO", "[regis][parser]")
{
    auto fx = Fixture { 100, 100 };
    // Position stacks are LIFO: the inner (E) restores the inner-most saved point, the outer (E) the
    // outer one -- not front()+clear(), which would jump to the outermost and discard both frames.
    fx.run("P[10,10]P(S)P[40,40]P(S)P[70,70]P(E)");
    CHECK(fx.context.position == point { .x = 40, .y = 40 }); // inner (E) -> inner-most save
    fx.run("P(E)");
    CHECK(fx.context.position == point { .x = 10, .y = 10 }); // outer (E) -> outer save
}

TEST_CASE("ReGISParser.curve.pixelVectorControlPoints", "[regis][parser]")
{
    auto fx = Fixture { 60, 60 };
    // A curve whose control points are given as pixel vectors must draw: the points have to be
    // appended to the collected curve, not dropped (which would collapse it to < 2 points).
    CHECK(fx.run("W(M10)P[10,30]C(S)0000(E)"));
}

TEST_CASE("ReGISParser.write.lineWidthIsBounded", "[regis][parser]")
{
    auto fx = Fixture { 40, 40 };
    // A hostile W(L<huge>) must be clamped so stamp()'s width*width brush cannot spin the terminal.
    fx.run("W(L100000)");
    CHECK(fx.context.lineWidth <= MaxLineWidth);
}

TEST_CASE("ReGISParser.write.pixelVectorMultiplierIsBounded", "[regis][parser]")
{
    auto fx = Fixture { 40, 40 };
    // A huge W(M<n>) must be clamped so the persistent cursor cannot overflow when stepped.
    fx.run("W(M2000000000)");
    CHECK(fx.context.pixelVectorMultiplier <= MaxPixelVectorMultiplier);
}

TEST_CASE("ReGISParser.deeplyNestedOptionsDoNotOverflow", "[regis][parser]")
{
    auto fx = Fixture { 40, 40 };
    // A pathological run of nested '(' must be bounded rather than recursing until the stack overflows.
    // This test simply has to return (no crash).
    auto payload = std::string { "W" } + std::string(5000, '(');
    fx.run(payload);
    SUCCEED("bounded recursion returned");
}

TEST_CASE("ReGISParser.hugeCoordinateDoesNotHang", "[regis][parser]")
{
    auto fx = Fixture { 40, 40 };
    // A vector to a far-off-canvas point must be bounded (plotLine is capped and coordinates clamped),
    // so this returns promptly rather than spinning ~10^8 iterations.
    CHECK(fx.run("P[0,0]V[80000000,0]"));
}

TEST_CASE("ReGISParser.curve.circleThroughPoint", "[regis][parser]")
{
    auto fx = Fixture { 40, 40 };
    // Centre at (20,20); C[30,20] draws a circle of radius 10 through (30,20).
    CHECK(fx.run("P[20,20]C[30,20]"));
    CHECK(fx.painted(30, 20));       // rightmost point
    CHECK(fx.painted(10, 20));       // leftmost point
    CHECK(fx.painted(20, 30));       // bottom
    CHECK_FALSE(fx.painted(20, 20)); // centre is empty
}

TEST_CASE("ReGISParser.curve.circleCentered", "[regis][parser]")
{
    auto fx = Fixture { 40, 40 };
    // C(C)[10,20]: the bracket is the centre, the current position (20,20) is on the circumference.
    CHECK(fx.run("P[20,20]C(C)[10,20]"));
    CHECK(fx.painted(20, 20)); // on the circle (radius 10 from centre 10,20)
    CHECK(fx.painted(0, 20));
}

TEST_CASE("ReGISParser.curve.interpolated", "[regis][parser]")
{
    auto fx = Fixture { 40, 40 };
    // An open interpolated curve through three points passes through each control point.
    CHECK(fx.run("P[2,20]C(S)[20,4][38,20](E)"));
    CHECK(fx.painted(2, 20));
    CHECK(fx.painted(20, 4));
    CHECK(fx.painted(38, 20));
}

TEST_CASE("ReGISParser.shading.enablesAndFills", "[regis][parser]")
{
    auto fx = Fixture { 20, 20 };
    // Enable shading to a horizontal reference at y=15, then draw a line at y=3.
    CHECK(fx.run("W(S[,15])P[2,3]V[17,3]"));
    for (auto y = 3; y <= 15; ++y)
        CHECK(fx.painted(9, y));
    CHECK(fx.context.shadingEnabled);
}

TEST_CASE("ReGISParser.report.error", "[regis][parser]")
{
    auto fx = Fixture { 40, 40 };
    fx.run("R(E)");
    REQUIRE(fx.events.replies.size() == 1);
    CHECK(fx.events.replies[0] == "0\r"); // no error pending
}

TEST_CASE("ReGISParser.report.alphabet", "[regis][parser]")
{
    auto fx = Fixture { 40, 40 };
    fx.run("R(L)");
    REQUIRE(fx.events.replies.size() == 1);
    CHECK(fx.events.replies[0] == "A0\r"); // the built-in alphabet
}

TEST_CASE("ReGISParser.report.interactiveLocator", "[regis][parser]")
{
    auto fx = Fixture { 800, 480 };
    fx.events.locator = std::pair { 123, 45 };
    fx.run("R(P(I))");
    REQUIRE(fx.events.replies.size() == 1);
    CHECK(fx.events.replies[0] == "0[123,45]\r"); // button 0 + locator position
}

TEST_CASE("ReGISParser.report.interactiveLocatorFallback", "[regis][parser]")
{
    // With no locator available, R(P(I)) falls back to the graphics cursor position.
    auto fx = Fixture { 800, 480 };
    fx.run("P[10,20]R(P(I))");
    REQUIRE(fx.events.replies.size() == 1);
    CHECK(fx.events.replies[0] == "0[10,20]\r");
}

TEST_CASE("ReGISParser.macrograph.defineAndInvoke", "[regis][parser]")
{
    auto fx = Fixture { 20, 8 };
    // Define macro A as a line-drawing sequence, then invoke it.
    CHECK(fx.run("@:AP[2,3]V[10,3]@;@A"));
    for (auto x = 2; x <= 10; ++x)
        CHECK(fx.painted(x, 3));
}

TEST_CASE("ReGISParser.macrograph.persistsAcrossStrings", "[regis][parser]")
{
    auto fx = Fixture { 20, 8 };
    fx.run("@:BP[1,1]V[9,1]@;"); // define only
    CHECK(fx.run("@B"));         // invoke in a later DCS string
    CHECK(fx.painted(5, 1));
}

TEST_CASE("ReGISParser.macrograph.clear", "[regis][parser]")
{
    auto fx = Fixture { 20, 8 };
    fx.run("@:CP[1,1]V[9,1]@;@.");
    CHECK_FALSE(fx.run("@C")); // cleared: invoking draws nothing
    CHECK_FALSE(fx.painted(5, 1));
}

TEST_CASE("ReGISParser.macrograph.recursionGuard", "[regis][parser]")
{
    // A self-referential macro must not hang: the depth cap bounds expansion.
    auto fx = Fixture { 20, 8 };
    fx.run("@:AP[1,1]@A@;@A"); // A invokes itself
    // Reaching here without hanging is the assertion; the cursor moved to the defined point.
    CHECK(fx.context.position == point { .x = 1, .y = 1 });
}

TEST_CASE("ReGISParser.persistence.acrossCalls", "[regis][parser]")
{
    // The context carries across DCS strings: a second parse continues from the first's position.
    auto fx = Fixture { 40, 8 };
    fx.run("P[5,3]");
    fx.run("V[10,3]");
    CHECK(fx.painted(7, 3));
    CHECK(fx.context.position == point { .x = 10, .y = 3 });
}
