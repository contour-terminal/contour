// SPDX-License-Identifier: Apache-2.0

/// @file regis-primitives.cpp
/// A static "kitchen-sink" demonstration of Contour's ReGIS (Remote Graphics Instruction Set)
/// support: lines, circles, arcs, interpolated curves, a filled polygon, line patterns, colours
/// (register, named, and HLS), and text at several sizes.
///
/// ReGIS is entered with the DCS sequence `ESC P p`, the command string, then `ESC \` (ST). The
/// VT340 addressing space is 800x480 with the origin at the top-left and Y increasing downward.

#include <string>
#include <string_view>

#include <unistd.h>

using namespace std::string_view_literals;

namespace
{

void writeToTTY(std::string_view s) noexcept
{
    ::write(STDOUT_FILENO, s.data(), s.size());
}

/// Wraps a ReGIS command string in the DCS introducer and ST terminator and writes it out.
void regis(std::string const& commands)
{
    writeToTTY("\033Pp");
    writeToTTY(commands);
    writeToTTY("\033\\");
}

} // namespace

int main()
{
    std::string r;

    // Clear the graphics canvas.
    r += "S(E)";

    // A row of coloured lines using the colour registers (W(I<n>)): 1=blue, 2=red, 3=green.
    r += "W(I1)P[40,40]V[240,40]";
    r += "W(I2)P[40,60]V[240,60]";
    r += "W(I3)P[40,80]V[240,80]";

    // Line patterns: solid (P1), dash (P2), dot (P4). W(P<n>) selects a standard pattern.
    r += "W(I7)";
    r += "W(P1)P[40,110]V[240,110]";
    r += "W(P2)P[40,125]V[240,125]";
    r += "W(P4)P[40,140]V[240,140]";
    r += "W(P1)"; // restore solid

    // A circle (centre at cursor, radius to the bracketed point) and an arc.
    r += "W(I5)P[360,90]C[420,90]";       // circle, radius 60
    r += "W(I6)P[360,90]C(A180)[300,90]"; // 180-degree arc

    // An interpolated open curve through three points.
    r += "W(I2)P[300,180]C(S)[380,140][460,180](E)";

    // A filled polygon (triangle) in HLS-specified colour (hue 120 = red on the VT340 wheel).
    r += "W(I(H120L50S100))";
    r += "P[520,60]F(V[620,60][570,150])";

    // A shaded region: enable shading to a horizontal reference, then draw a curve above it.
    r += "W(I3)W(S[,300])";
    r += "P[300,240]C(S)[360,210][420,260][480,220](E)";
    r += "W(S0)"; // shading off

    // Text at three standard sizes (T(S<n>)).
    r += "W(I7)";
    r += "P[40,340]T(S1)'ReGIS on Contour'";
    r += "P[40,380]T(S2)'Vector graphics'";
    r += "P[40,430]T(S0)'lines - curves - fills - text - colour'";

    regis(r);

    // Move the text cursor below the graphic so the shell prompt does not overwrite it.
    writeToTTY("\033[24H\n");
    return 0;
}
