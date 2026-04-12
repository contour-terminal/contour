// SPDX-License-Identifier: Apache-2.0

/// @file drcs-demo.cpp
/// Demonstrates DECDLD (Down-Line-Load Character Set) — defining custom glyphs
/// using the DEC DRCS mechanism and displaying them on screen.
///
/// This example defines a small set of custom characters (a smiley face,
/// a heart, and a diamond) as 10x20 bitmap glyphs, loads them into a DRCS
/// font via DECDLD, designates the font into G1 via SCS, and then prints
/// the custom characters using a locking shift to G1.
///
/// The sixel-like encoding in DECDLD works as follows:
/// - Each character in the data (0x3F-0x7E) encodes a column of 6 pixel rows.
/// - The value is offset by 0x3F, so '?' = 0 (all off), '@' = 1 (bottom bit on),
///   'A' = 2 (second bit on), '~' = 0x3F = all 6 bits on.
/// - '/' separates sixel rows (each advancing by 6 pixel rows).
/// - ';' separates glyphs.

#include <algorithm>
#include <cstdint>
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

/// Encodes a 10-wide, 20-tall monochrome bitmap as DECDLD sixel data.
/// @param bitmap  Row-major array of 200 bytes (10 columns x 20 rows), 1=pixel set, 0=clear.
/// @return  The sixel-encoded string for one glyph.
std::string encodeDRCSGlyph(const uint8_t bitmap[20][10])
{
    std::string result;

    // Process in bands of 6 rows (sixel rows)
    for (auto band = 0; band < 20; band += 6)
    {
        if (band > 0)
            result += '/'; // sixel row separator

        auto const rowsInBand = std::min(6, 20 - band);
        for (auto col = 0; col < 10; ++col)
        {
            auto sixel = uint8_t { 0 };
            for (auto bit = 0; bit < rowsInBand; ++bit)
            {
                if (bitmap[band + bit][col])
                    sixel |= static_cast<uint8_t>(1 << bit);
            }
            result += static_cast<char>(sixel + 0x3F); // offset by '?'
        }
    }

    return result;
}

// clang-format off

// A 10x20 smiley face bitmap
// NOLINTBEGIN(modernize-avoid-c-arrays)
constexpr uint8_t SmileyBitmap[20][10] = {
    {0,0,0,1,1,1,1,0,0,0},
    {0,0,1,1,1,1,1,1,0,0},
    {0,1,1,0,0,0,0,1,1,0},
    {0,1,0,0,0,0,0,0,1,0},
    {1,1,0,0,0,0,0,0,1,1},
    {1,0,0,1,0,0,1,0,0,1},
    {1,0,0,1,0,0,1,0,0,1},
    {1,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,1},
    {1,0,1,0,0,0,0,1,0,1},
    {1,0,0,1,0,0,1,0,0,1},
    {1,1,0,0,1,1,0,0,1,1},
    {0,1,0,0,0,0,0,0,1,0},
    {0,1,1,0,0,0,0,1,1,0},
    {0,0,1,1,1,1,1,1,0,0},
    {0,0,0,1,1,1,1,0,0,0},
    {0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0},
};

// A 10x20 heart bitmap
constexpr uint8_t HeartBitmap[20][10] = {
    {0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0},
    {0,1,1,0,0,0,1,1,0,0},
    {1,1,1,1,0,1,1,1,1,0},
    {1,1,1,1,1,1,1,1,1,0},
    {1,1,1,1,1,1,1,1,1,0},
    {1,1,1,1,1,1,1,1,1,0},
    {0,1,1,1,1,1,1,1,0,0},
    {0,1,1,1,1,1,1,1,0,0},
    {0,0,1,1,1,1,1,0,0,0},
    {0,0,1,1,1,1,1,0,0,0},
    {0,0,0,1,1,1,0,0,0,0},
    {0,0,0,1,1,1,0,0,0,0},
    {0,0,0,0,1,0,0,0,0,0},
    {0,0,0,0,1,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0},
};

// A 10x20 diamond bitmap
constexpr uint8_t DiamondBitmap[20][10] = {
    {0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,1,0,0,0,0,0},
    {0,0,0,1,1,1,0,0,0,0},
    {0,0,1,1,1,1,1,0,0,0},
    {0,1,1,1,1,1,1,1,0,0},
    {1,1,1,1,1,1,1,1,1,0},
    {1,1,1,1,1,1,1,1,1,0},
    {1,1,1,1,1,1,1,1,1,0},
    {1,1,1,1,1,1,1,1,1,0},
    {0,1,1,1,1,1,1,1,0,0},
    {0,0,1,1,1,1,1,0,0,0},
    {0,0,0,1,1,1,0,0,0,0},
    {0,0,0,0,1,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0},
};
// NOLINTEND(modernize-avoid-c-arrays)
// clang-format on

} // namespace

int main()
{
    // Encode the three glyphs as DECDLD sixel data
    auto const smiley = encodeDRCSGlyph(SmileyBitmap);
    auto const heart = encodeDRCSGlyph(HeartBitmap);
    auto const diamond = encodeDRCSGlyph(DiamondBitmap);

    // DECDLD: DCS Pfn;Pcn;Pe;Pcmw;Pw;Pt;Pcmh;Pcss { Dscs data ST
    //   Pfn=1 (font number 1)
    //   Pcn=0 (starting character = 0x21 = '!')
    //   Pe=0  (erase all characters in the set first)
    //   Pcmw=10 (character matrix width = 10 pixels)
    //   Pw=10   (font width = 10 pixels)
    //   Pt=0    (text cell)
    //   Pcmh=20 (character matrix height = 20 pixels)
    //   Pcss=0  (94-character set)
    //   Dscs = designator character (space + 'A' for a user-defined 94-set)
    auto decdld = std::string("\033P1;0;0;10;10;0;20;0{ A");
    decdld += smiley;  // Position 0x21 ('!')
    decdld += ';';
    decdld += heart;   // Position 0x22 ('"')
    decdld += ';';
    decdld += diamond; // Position 0x23 ('#')
    decdld += "\033\\";

    // Send the DECDLD sequence to define the glyphs
    writeToTTY(decdld);

    // Print the demo
    writeToTTY("\n  DRCS (Soft Character Set) Demo\n");
    writeToTTY("  ==============================\n\n");
    writeToTTY("  Standard characters:  A B C ! \" #\n\n");

    // Designate our DRCS font (designator ' A') into G1: ESC ) <space> A
    // Note: The DRCS designator is a two-byte sequence (intermediate + final).
    // For a single-byte designator 'A' with intermediate space:
    writeToTTY("\033) A");

    // Locking shift to G1 (SO = 0x0E) to activate the DRCS charset
    writeToTTY("  DRCS characters:      ");
    writeToTTY("\x0E");       // SO — switch to G1
    writeToTTY("!");          // Smiley (position 0x21)
    writeToTTY(" ");
    writeToTTY("\"");         // Heart (position 0x22)
    writeToTTY(" ");
    writeToTTY("#");          // Diamond (position 0x23)
    writeToTTY("\x0F");       // SI — switch back to G0

    writeToTTY("\n\n");
    writeToTTY("  (If your terminal supports DRCS rendering,\n");
    writeToTTY("   you should see custom glyphs above.)\n\n");

    return 0;
}
