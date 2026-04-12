// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/Charset.h>

namespace vtbackend
{

constexpr CharsetMap usasciiCharset() noexcept
{
    CharsetMap result {};

    for (char ch = 0; ch < 127; ++ch)
        result[static_cast<std::size_t>(ch)] = static_cast<char32_t>(ch);

    return result;
}

/// British:
///     ESC (A
///     Reference: https://vt100.net/docs/vt220-rm/chapter2.html#T2-5
constexpr CharsetMap createBritishCharset() noexcept
{
    auto result = usasciiCharset();
    result['#'] = 0x00A3; // U'£';
    return result;
}

/// German:
///     ESC ( K
constexpr CharsetMap createGermanCharset() noexcept
{
    auto result = usasciiCharset();

    result[static_cast<std::size_t>('@')] = 0x00A7;  // U'§';
    result[static_cast<std::size_t>('[')] = 0x00C4;  // U'Ä';
    result[static_cast<std::size_t>('\\')] = 0x00D6; // U'Ö';
    result[static_cast<std::size_t>(']')] = 0x00DC;  // U'Ü';
    result[static_cast<std::size_t>('{')] = 0x00E4;  // U'ä';
    result[static_cast<std::size_t>('|')] = 0x00F6;  // U'ö';
    result[static_cast<std::size_t>('}')] = 0x00FC;  // U'ü';
    result[static_cast<std::size_t>('~')] = 0x00DF;  // U'ß';

    return result;
}

/// DEC Special Character and Line Drawing Set.
///
/// Reference: https://vt100.net/docs/vt102-ug/chapter5.html#T5-13
constexpr CharsetMap createSpecialCharset() noexcept
{
    auto result = usasciiCharset();

    result[static_cast<std::size_t>('`')] = 0x25c6; // '◆'
    result[static_cast<std::size_t>('a')] = 0x2592; // '▒'
    result[static_cast<std::size_t>('b')] = 0x2409; // '␉'
    result[static_cast<std::size_t>('c')] = 0x240c; // '␌'
    result[static_cast<std::size_t>('d')] = 0x240d; // '␍'
    result[static_cast<std::size_t>('e')] = 0x240a; // '␊'
    result[static_cast<std::size_t>('f')] = 0x00b0; // '°'
    result[static_cast<std::size_t>('g')] = 0x00b1; // '±'
    result[static_cast<std::size_t>('h')] = 0x2424; // '␤'
    result[static_cast<std::size_t>('i')] = 0x240b; // '␋'
    result[static_cast<std::size_t>('j')] = 0x2518; // '┘'
    result[static_cast<std::size_t>('k')] = 0x2510; // '┐'
    result[static_cast<std::size_t>('l')] = 0x250c; // '┌'
    result[static_cast<std::size_t>('m')] = 0x2514; // '└'
    result[static_cast<std::size_t>('n')] = 0x253c; // '┼'
    result[static_cast<std::size_t>('o')] = 0x23ba; // '⎺'
    result[static_cast<std::size_t>('p')] = 0x23bb; // '⎻'
    result[static_cast<std::size_t>('q')] = 0x2500; // '─'
    result[static_cast<std::size_t>('r')] = 0x23bc; // '⎼'
    result[static_cast<std::size_t>('s')] = 0x23bd; // '⎽'
    result[static_cast<std::size_t>('t')] = 0x251c; // '├'
    result[static_cast<std::size_t>('u')] = 0x2524; // '┤'
    result[static_cast<std::size_t>('v')] = 0x2534; // '┴'
    result[static_cast<std::size_t>('w')] = 0x252c; // '┬'
    result[static_cast<std::size_t>('x')] = 0x2502; // '│'
    result[static_cast<std::size_t>('y')] = 0x2264; // '≤'
    result[static_cast<std::size_t>('z')] = 0x2265; // '≥'
    result[static_cast<std::size_t>('{')] = 0x03c0; // 'π'
    result[static_cast<std::size_t>('|')] = 0x2260; // '≠'
    result[static_cast<std::size_t>('}')] = 0x00a3; // '£'
    result[static_cast<std::size_t>('~')] = 0x00b7; // '·'

    return result;
}

/// Dutch:
///     ESC ( 4
///
constexpr CharsetMap createDutchCharset() noexcept
{
    auto result = usasciiCharset();

    result[static_cast<std::size_t>('#')] = 0x00A3; // U'£';
    result[static_cast<std::size_t>('@')] = 0x00BE; // U'¾';
    // TODO: result[static_cast<std::size_t>('[')] = U'ij';
    result[static_cast<std::size_t>('\\')] = 0x00BD; // U'½';
    result[static_cast<std::size_t>(']')] = 0x007C;  // U'|';
    result[static_cast<std::size_t>('{')] = 0x00A8;  // U'¨';
    result[static_cast<std::size_t>('|')] = 0x0066;  // U'f';
    result[static_cast<std::size_t>('}')] = 0x00BC;  // U'¼';
    result[static_cast<std::size_t>('~')] = 0x00B4;  // U'´';

    return result;
}

/// Finnish:
///     ESC ( C
///     ESC ( 5
constexpr CharsetMap createFinnishCharset() noexcept
{
    auto result = usasciiCharset();

    result[static_cast<std::size_t>('[')] = 0x00C4;  // U'Ä';
    result[static_cast<std::size_t>('\\')] = 0x00D6; // U'Ö';
    result[static_cast<std::size_t>(']')] = 0x00C5;  // U'Å';
    result[static_cast<std::size_t>('^')] = 0x00DC;  // U'Ü';
    result[static_cast<std::size_t>('`')] = 0x00E9;  // U'é';
    result[static_cast<std::size_t>('{')] = 0x00E4;  // U'ä';
    result[static_cast<std::size_t>('|')] = 0x00F6;  // U'ö';
    result[static_cast<std::size_t>('}')] = 0x00E5;  // U'å';
    result[static_cast<std::size_t>('~')] = 0x00FC;  // U'ü';

    return result;
}

/// French:
///     ESC ( R
constexpr CharsetMap createFrenchCharset() noexcept
{
    auto result = usasciiCharset();

    result[static_cast<std::size_t>('#')] = 0x00A3;  // U'£';
    result[static_cast<std::size_t>('@')] = 0x00E0;  // U'à';
    result[static_cast<std::size_t>('[')] = 0x00B0;  // U'°';
    result[static_cast<std::size_t>('\\')] = 0x00E7; // U'ç';
    result[static_cast<std::size_t>(']')] = 0x00A7;  // U'§';
    result[static_cast<std::size_t>('{')] = 0x00E9;  // U'é';
    result[static_cast<std::size_t>('|')] = 0x00F9;  // U'ù';
    result[static_cast<std::size_t>('}')] = 0x00E8;  // U'è';
    result[static_cast<std::size_t>('~')] = 0x00A8;  // U'¨';

    return result;
}

/// French Canadian:
///     ESC ( Q
constexpr CharsetMap createFrenchCanadianCharset() noexcept
{
    auto result = usasciiCharset();

    result[static_cast<std::size_t>('@')] = 0x00E0;  // U'à';
    result[static_cast<std::size_t>('[')] = 0x00E2;  // U'â';
    result[static_cast<std::size_t>('\\')] = 0x00E7; // U'ç';
    result[static_cast<std::size_t>(']')] = 0x00EA;  // U'ê';
    result[static_cast<std::size_t>('^')] = 0x00EE;  // U'î';
    result[static_cast<std::size_t>('`')] = 0x00F4;  // U'ô';
    result[static_cast<std::size_t>('{')] = 0x00E9;  // U'é';
    result[static_cast<std::size_t>('|')] = 0x00F9;  // U'ù';
    result[static_cast<std::size_t>('}')] = 0x00E8;  // U'è';
    result[static_cast<std::size_t>('~')] = 0x00FB;  // U'û';

    return result;
}

/// Norwegian/Danich:
///     ESC ( E
///     ESC ( 6
constexpr CharsetMap createNorwegianDanishCharset() noexcept
{
    auto result = usasciiCharset();

    result[static_cast<std::size_t>('@')] = 0x00C4;  // U'Ä';
    result[static_cast<std::size_t>('[')] = 0x00C6;  // U'Æ';
    result[static_cast<std::size_t>('\\')] = 0x00D8; // U'Ø';
    result[static_cast<std::size_t>(']')] = 0x00C5;  // U'Å';
    result[static_cast<std::size_t>('^')] = 0x00DC;  // U'Ü';
    result[static_cast<std::size_t>('`')] = 0x00E4;  // U'ä';
    result[static_cast<std::size_t>('{')] = 0x00E6;  // U'æ';
    result[static_cast<std::size_t>('|')] = 0x00F8;  // U'ø';
    result[static_cast<std::size_t>('}')] = 0x00E5;  // U'å';
    result[static_cast<std::size_t>('~')] = 0x00FC;  // U'ü';

    return result;
}

/// Spanish:
///     ESC ( Z
constexpr CharsetMap createSpanishCharset() noexcept
{
    auto result = usasciiCharset();

    result[static_cast<std::size_t>('#')] = 0x00A3;  // U'£';
    result[static_cast<std::size_t>('@')] = 0x00A7;  // U'§';
    result[static_cast<std::size_t>('[')] = 0x00A1;  // U'¡';
    result[static_cast<std::size_t>('\\')] = 0x00D1; // U'Ñ';
    result[static_cast<std::size_t>(']')] = 0x00BF;  // U'¿';
    result[static_cast<std::size_t>('{')] = 0x00B0;  // U'°';
    result[static_cast<std::size_t>('|')] = 0x00F1;  // U'ñ';
    result[static_cast<std::size_t>('}')] = 0x00E7;  // U'ç';

    return result;
}

/// Swedish:
///     ESC ( H
///     ESC ( 7
constexpr CharsetMap createSwedishCharset() noexcept
{
    auto result = usasciiCharset();

    result[static_cast<std::size_t>('@')] = 0x00C9;  // U'É';
    result[static_cast<std::size_t>('[')] = 0x00C4;  // U'Ä';
    result[static_cast<std::size_t>('\\')] = 0x00D6; // U'Ö';
    result[static_cast<std::size_t>(']')] = 0x00C5;  // U'Å';
    result[static_cast<std::size_t>('^')] = 0x00DC;  // U'Ü';
    result[static_cast<std::size_t>('`')] = 0x00E9;  // U'é';
    result[static_cast<std::size_t>('{')] = 0x00E4;  // U'ä';
    result[static_cast<std::size_t>('|')] = 0x00F6;  // U'ö';
    result[static_cast<std::size_t>('}')] = 0x00E5;  // U'å';
    result[static_cast<std::size_t>('~')] = 0x00FC;  // U'ü';

    return result;
}

/// Swiss:
///     ESC ( =
constexpr CharsetMap createSwissCharset() noexcept
{
    auto result = usasciiCharset();

    result[static_cast<std::size_t>('#')] = 0x00F9;  // U'ù';
    result[static_cast<std::size_t>('@')] = 0x00E0;  // U'à';
    result[static_cast<std::size_t>('[')] = 0x00E9;  // U'é';
    result[static_cast<std::size_t>('\\')] = 0x00E7; // U'ç';
    result[static_cast<std::size_t>(']')] = 0x00EA;  // U'ê';
    result[static_cast<std::size_t>('^')] = 0x00EE;  // U'î';
    result[static_cast<std::size_t>('_')] = 0x00E8;  // U'è';
    result[static_cast<std::size_t>('`')] = 0x00F4;  // U'ô';
    result[static_cast<std::size_t>('{')] = 0x00E4;  // U'ä';
    result[static_cast<std::size_t>('|')] = 0x00F6;  // U'ö';
    result[static_cast<std::size_t>('}')] = 0x00FC;  // U'ü';
    result[static_cast<std::size_t>('~')] = 0x00FB;  // U'û';

    return result;
}

/// DEC Technical Character Set
/// Reference: VT320/VT510 programmer reference
constexpr CharsetMap createTechnicalCharset() noexcept
{
    auto result = usasciiCharset();
    // Row 2 (0x21-0x2F): Mathematical symbols
    result[0x21] = 0x23B7; // ⎷ RADICAL SYMBOL BOTTOM
    result[0x22] = 0x250C; // ┌ BOX DRAWINGS LIGHT DOWN AND RIGHT (used as radical top)
    result[0x23] = 0x2500; // ─ BOX DRAWINGS LIGHT HORIZONTAL
    result[0x24] = 0x2320; // ⌠ TOP HALF INTEGRAL
    result[0x25] = 0x2321; // ⌡ BOTTOM HALF INTEGRAL
    result[0x26] = 0x2502; // │ BOX DRAWINGS LIGHT VERTICAL (integral extension)
    result[0x27] = 0x23A1; // ⎡ LEFT SQUARE BRACKET UPPER CORNER
    result[0x28] = 0x23A3; // ⎣ LEFT SQUARE BRACKET LOWER CORNER
    result[0x29] = 0x23A4; // ⎤ RIGHT SQUARE BRACKET UPPER CORNER
    result[0x2A] = 0x23A6; // ⎦ RIGHT SQUARE BRACKET LOWER CORNER
    result[0x2B] = 0x23A7; // ⎧ LEFT CURLY BRACKET UPPER HOOK
    result[0x2C] = 0x23A9; // ⎩ LEFT CURLY BRACKET LOWER HOOK
    result[0x2D] = 0x23AB; // ⎫ RIGHT CURLY BRACKET UPPER HOOK
    result[0x2E] = 0x23AD; // ⎭ RIGHT CURLY BRACKET LOWER HOOK
    result[0x2F] = 0x23A8; // ⎨ LEFT CURLY BRACKET MIDDLE PIECE
    // Row 3 (0x30-0x3F)
    result[0x30] = 0x23AC; // ⎬ RIGHT CURLY BRACKET MIDDLE PIECE
    result[0x31] = 0x2264; // ≤ LESS-THAN OR EQUAL TO (used for top of summation)
    result[0x32] = 0x2502; // │ (bracket/paren extension)
    result[0x33] = 0x23A2; // ⎢ LEFT SQUARE BRACKET EXTENSION
    result[0x34] = 0x23A5; // ⎥ RIGHT SQUARE BRACKET EXTENSION
    result[0x35] = 0x23A7; // ⎧ (left curly bracket extension, duplicate used contextually)
    result[0x36] = 0x23A9; // ⎩ (right curly bracket extension)
    result[0x37] = 0x23AA; // ⎪ CURLY BRACKET EXTENSION
    result[0x38] = 0x23AA; // ⎪ CURLY BRACKET EXTENSION (duplicate)
    result[0x39] = 0x2264; // ≤ LESS-THAN OR EQUAL TO
    result[0x3A] = 0x2265; // ≥ GREATER-THAN OR EQUAL TO
    result[0x3B] = 0x03C0; // π GREEK SMALL LETTER PI
    result[0x3C] = 0x2260; // ≠ NOT EQUAL TO
    result[0x3D] = 0x00A3; // £ POUND SIGN
    result[0x3E] = 0x00B7; // · MIDDLE DOT
    result[0x3F] = 0x00F7; // ÷ DIVISION SIGN
    // Row 4 (0x40-0x4F): Greek uppercase
    result[0x40] = 0x0020; // (space — undefined in some references)
    result[0x41] = 0x0391; // Α GREEK CAPITAL LETTER ALPHA
    result[0x42] = 0x0392; // Β GREEK CAPITAL LETTER BETA
    result[0x43] = 0x03A7; // Χ GREEK CAPITAL LETTER CHI
    result[0x44] = 0x0394; // Δ GREEK CAPITAL LETTER DELTA
    result[0x45] = 0x0395; // Ε GREEK CAPITAL LETTER EPSILON
    result[0x46] = 0x03A6; // Φ GREEK CAPITAL LETTER PHI
    result[0x47] = 0x0393; // Γ GREEK CAPITAL LETTER GAMMA
    result[0x48] = 0x0397; // Η GREEK CAPITAL LETTER ETA
    result[0x49] = 0x0399; // Ι GREEK CAPITAL LETTER IOTA
    result[0x4A] = 0x03B8; // θ GREEK SMALL LETTER THETA
    result[0x4B] = 0x039A; // Κ GREEK CAPITAL LETTER KAPPA
    result[0x4C] = 0x039B; // Λ GREEK CAPITAL LETTER LAMDA
    result[0x4D] = 0x039C; // Μ — but often mapped to space
    result[0x4E] = 0x039D; // Ν GREEK CAPITAL LETTER NU
    result[0x4F] = 0x039F; // Ο GREEK CAPITAL LETTER OMICRON
    // Row 5 (0x50-0x5F): Greek uppercase (cont.) and misc
    result[0x50] = 0x03A0; // Π GREEK CAPITAL LETTER PI
    result[0x51] = 0x0398; // Θ GREEK CAPITAL LETTER THETA
    result[0x52] = 0x03A1; // Ρ GREEK CAPITAL LETTER RHO
    result[0x53] = 0x03A3; // Σ GREEK CAPITAL LETTER SIGMA
    result[0x54] = 0x03A4; // Τ GREEK CAPITAL LETTER TAU
    result[0x55] = 0x0020; // (undefined)
    result[0x56] = 0x03A8; // Ψ GREEK CAPITAL LETTER PSI (was mapped to ΩF6 in some refs)
    result[0x57] = 0x03A9; // Ω GREEK CAPITAL LETTER OMEGA
    result[0x58] = 0x039E; // Ξ GREEK CAPITAL LETTER XI
    result[0x59] = 0x03A5; // Υ GREEK CAPITAL LETTER UPSILON
    result[0x5A] = 0x0396; // Ζ GREEK CAPITAL LETTER ZETA
    result[0x5B] = 0x2190; // ← LEFTWARDS ARROW
    result[0x5C] = 0x2191; // ↑ UPWARDS ARROW
    result[0x5D] = 0x2192; // → RIGHTWARDS ARROW
    result[0x5E] = 0x2193; // ↓ DOWNWARDS ARROW
    // Row 6 (0x60-0x6F): Greek lowercase
    result[0x60] = 0x0020; // (undefined)
    result[0x61] = 0x03B1; // α GREEK SMALL LETTER ALPHA
    result[0x62] = 0x03B2; // β GREEK SMALL LETTER BETA
    result[0x63] = 0x03C7; // χ GREEK SMALL LETTER CHI
    result[0x64] = 0x03B4; // δ GREEK SMALL LETTER DELTA
    result[0x65] = 0x03B5; // ε GREEK SMALL LETTER EPSILON
    result[0x66] = 0x03C6; // φ GREEK SMALL LETTER PHI
    result[0x67] = 0x03B3; // γ GREEK SMALL LETTER GAMMA
    result[0x68] = 0x03B7; // η GREEK SMALL LETTER ETA
    result[0x69] = 0x03B9; // ι GREEK SMALL LETTER IOTA
    result[0x6A] = 0x03B8; // θ GREEK SMALL LETTER THETA (duplicate for convenience)
    result[0x6B] = 0x03BA; // κ GREEK SMALL LETTER KAPPA
    result[0x6C] = 0x03BB; // λ GREEK SMALL LETTER LAMDA
    result[0x6D] = 0x03BC; // μ GREEK SMALL LETTER MU (also MICRO SIGN)
    result[0x6E] = 0x03BD; // ν GREEK SMALL LETTER NU
    result[0x6F] = 0x2202; // ∂ PARTIAL DIFFERENTIAL (used as lowercase omicron in context)
    // Row 7 (0x70-0x7E): Greek lowercase (cont.) and symbols
    result[0x70] = 0x03C0; // π GREEK SMALL LETTER PI
    result[0x71] = 0x03C8; // ψ GREEK SMALL LETTER PSI
    result[0x72] = 0x03C1; // ρ GREEK SMALL LETTER RHO
    result[0x73] = 0x03C3; // σ GREEK SMALL LETTER SIGMA
    result[0x74] = 0x03C4; // τ GREEK SMALL LETTER TAU
    result[0x75] = 0x0020; // (undefined)
    result[0x76] = 0x0192; // ƒ LATIN SMALL LETTER F WITH HOOK
    result[0x77] = 0x03C9; // ω GREEK SMALL LETTER OMEGA
    result[0x78] = 0x03BE; // ξ GREEK SMALL LETTER XI
    result[0x79] = 0x03C5; // υ GREEK SMALL LETTER UPSILON
    result[0x7A] = 0x03B6; // ζ GREEK SMALL LETTER ZETA
    result[0x7B] = 0x2039; // ‹ SINGLE LEFT-POINTING ANGLE QUOTATION MARK
    result[0x7C] = 0x007C; // | (unchanged)
    result[0x7D] = 0x203A; // › SINGLE RIGHT-POINTING ANGLE QUOTATION MARK
    result[0x7E] = 0x223C; // ∼ TILDE OPERATOR
    return result;
}

CharsetMap const* charsetMap(CharsetId id) noexcept
{
    static auto const british = createBritishCharset();
    static auto const dutch = createDutchCharset();
    static auto const finish = createFinnishCharset();
    static auto const french = createFrenchCharset();
    static auto const frenchCanadian = createFrenchCanadianCharset();
    static auto const german = createGermanCharset();
    static auto const norweigianDanish = createNorwegianDanishCharset();
    static auto const spanish = createSpanishCharset();
    static auto const special = createSpecialCharset();
    static auto const swedish = createSwedishCharset();
    static auto const swiss = createSwissCharset();
    static auto const technical = createTechnicalCharset();
    static auto const usascii = usasciiCharset();

    switch (id)
    {
        case CharsetId::British: return &british;
        case CharsetId::Dutch: return &dutch;
        case CharsetId::Finnish: return &finish;
        case CharsetId::French: return &french;
        case CharsetId::FrenchCanadian: return &frenchCanadian;
        case CharsetId::German: return &german;
        case CharsetId::NorwegianDanish: return &norweigianDanish;
        case CharsetId::Spanish: return &spanish;
        case CharsetId::Special: return &special;
        case CharsetId::Swedish: return &swedish;
        case CharsetId::Swiss: return &swiss;
        case CharsetId::Technical: return &technical;
        case CharsetId::USASCII: return &usascii;
    }

    return nullptr;
}

} // namespace vtbackend
