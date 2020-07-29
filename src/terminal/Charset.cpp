/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019-2020 Christian Parpart <christian@parpart.family>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <terminal/Charset.h>

namespace terminal {

constexpr CharsetMap usasciiCharset()
{
    CharsetMap result{};

    for (char ch = 0; ch < 127; ++ch)
        result[static_cast<size_t>(ch)] = static_cast<char32_t>(ch);

    return result;
}

/// British:
///     ESC (A
///     Reference: http://vt100.net/docs/vt220-rm/table2-5.html
constexpr CharsetMap createBritishCharset()
{
    auto result = usasciiCharset();
    result['#'] = U'£';
    return result;
}

/// German:
///     ESC ( K
constexpr CharsetMap createGermanCharset()
{
    auto result = usasciiCharset();

    result[static_cast<size_t>('@')] = U'§';
    result[static_cast<size_t>('[')] = U'Ä';
    result[static_cast<size_t>('\\')] = U'Ö';
    result[static_cast<size_t>(']')] = U'Ü';
    result[static_cast<size_t>('{')] = U'ä';
    result[static_cast<size_t>('|')] = U'ö';
    result[static_cast<size_t>('}')] = U'ü';
    result[static_cast<size_t>('~')] = U'ß';

    return result;
}

/// DEC Special Character and Line Drawing Set.
///
/// Reference: http://vt100.net/docs/vt102-ug/table5-13.html
constexpr CharsetMap createSpecialCharset()
{
    auto result = usasciiCharset();

    result[static_cast<size_t>('`')] = U'\u25c6'; // '◆'
    result[static_cast<size_t>('a')] = U'\u2592'; // '▒'
    result[static_cast<size_t>('b')] = U'\u2409'; // '␉'
    result[static_cast<size_t>('c')] = U'\u240c'; // '␌'
    result[static_cast<size_t>('d')] = U'\u240d'; // '␍'
    result[static_cast<size_t>('e')] = U'\u240a'; // '␊'
    result[static_cast<size_t>('f')] = U'\u00b0'; // '°'
    result[static_cast<size_t>('g')] = U'\u00b1'; // '±'
    result[static_cast<size_t>('h')] = U'\u2424'; // '␤'
    result[static_cast<size_t>('i')] = U'\u240b'; // '␋'
    result[static_cast<size_t>('j')] = U'\u2518'; // '┘'
    result[static_cast<size_t>('k')] = U'\u2510'; // '┐'
    result[static_cast<size_t>('l')] = U'\u250c'; // '┌'
    result[static_cast<size_t>('m')] = U'\u2514'; // '└'
    result[static_cast<size_t>('n')] = U'\u253c'; // '┼'
    result[static_cast<size_t>('o')] = U'\u23ba'; // '⎺'
    result[static_cast<size_t>('p')] = U'\u23bb'; // '⎻'
    result[static_cast<size_t>('q')] = U'\u2500'; // '─'
    result[static_cast<size_t>('r')] = U'\u23bc'; // '⎼'
    result[static_cast<size_t>('s')] = U'\u23bd'; // '⎽'
    result[static_cast<size_t>('t')] = U'\u251c'; // '├'
    result[static_cast<size_t>('u')] = U'\u2524'; // '┤'
    result[static_cast<size_t>('v')] = U'\u2534'; // '┴'
    result[static_cast<size_t>('w')] = U'\u252c'; // '┬'
    result[static_cast<size_t>('x')] = U'\u2502'; // '│'
    result[static_cast<size_t>('y')] = U'\u2264'; // '≤'
    result[static_cast<size_t>('z')] = U'\u2265'; // '≥'
    result[static_cast<size_t>('{')] = U'\u03c0'; // 'π'
    result[static_cast<size_t>('|')] = U'\u2260'; // '≠'
    result[static_cast<size_t>('}')] = U'\u00a3'; // '£'
    result[static_cast<size_t>('~')] = U'\u00b7'; // '·'

    return result;
}

/// Dutch:
///     ESC ( 4
///
constexpr CharsetMap createDutchCharset()
{
    auto result = usasciiCharset();

    result[static_cast<size_t>('#')] = U'£';
    result[static_cast<size_t>('@')] = U'¾';
    //TODO: result[static_cast<size_t>('[')] = U'ij';
    result[static_cast<size_t>('\\')] = U'½';
    result[static_cast<size_t>(']')] = U'|';
    result[static_cast<size_t>('{')] = U'¨';
    result[static_cast<size_t>('|')] = U'f';
    result[static_cast<size_t>('}')] = U'¼';
    result[static_cast<size_t>('~')] = U'´';

    return result;
}

/// Finish:
///     ESC ( C
///     ESC ( 5
constexpr CharsetMap createFinishCharset()
{
    auto result = usasciiCharset();

    result[static_cast<size_t>('[')] = U'Ä';
    result[static_cast<size_t>('\\')] = U'Ö';
    result[static_cast<size_t>(']')] = U'Å';
    result[static_cast<size_t>('^')] = U'Ü';
    result[static_cast<size_t>('`')] = U'é';
    result[static_cast<size_t>('{')] = U'ä';
    result[static_cast<size_t>('|')] = U'ö';
    result[static_cast<size_t>('}')] = U'å';
    result[static_cast<size_t>('~')] = U'ü';

    return result;
}

/// French:
///     ESC ( R
constexpr CharsetMap createFrenchCharset()
{
    auto result = usasciiCharset();

    result[static_cast<size_t>('#')] = U'£';
    result[static_cast<size_t>('@')] = U'à';
    result[static_cast<size_t>('[')] = U'°';
    result[static_cast<size_t>('\\')] = U'ç';
    result[static_cast<size_t>(']')] = U'§';
    result[static_cast<size_t>('{')] = U'é';
    result[static_cast<size_t>('|')] = U'ù';
    result[static_cast<size_t>('}')] = U'è';
    result[static_cast<size_t>('~')] = U'¨';

    return result;
}

/// French Canadian:
///     ESC ( Q
constexpr CharsetMap createFrenchCanadianCharset()
{
    auto result = usasciiCharset();

    result[static_cast<size_t>('@')] = U'à';
    result[static_cast<size_t>('[')] = U'â';
    result[static_cast<size_t>('\\')] = U'ç';
    result[static_cast<size_t>(']')] = U'ê';
    result[static_cast<size_t>('^')] = U'î';
    result[static_cast<size_t>('`')] = U'ô';
    result[static_cast<size_t>('{')] = U'é';
    result[static_cast<size_t>('|')] = U'ù';
    result[static_cast<size_t>('}')] = U'è';
    result[static_cast<size_t>('~')] = U'û';

    return result;
}

/// Norwegian/Danich:
///     ESC ( E
///     ESC ( 6
constexpr CharsetMap createNorwegianDanishCharset()
{
    auto result = usasciiCharset();

    result[static_cast<size_t>('@')] = U'Ä';
    result[static_cast<size_t>('[')] = U'Æ';
    result[static_cast<size_t>('\\')] = U'Ø';
    result[static_cast<size_t>(']')] = U'Å';
    result[static_cast<size_t>('^')] = U'Ü';
    result[static_cast<size_t>('`')] = U'ä';
    result[static_cast<size_t>('{')] = U'æ';
    result[static_cast<size_t>('|')] = U'ø';
    result[static_cast<size_t>('}')] = U'å';
    result[static_cast<size_t>('~')] = U'ü';

    return result;
}

/// Spanish:
///     ESC ( Z
constexpr CharsetMap createSpanishCharset()
{
    auto result = usasciiCharset();

    result[static_cast<size_t>('#')] = U'£';
    result[static_cast<size_t>('@')] = U'§';
    result[static_cast<size_t>('[')] = U'¡';
    result[static_cast<size_t>('\\')] = U'Ñ';
    result[static_cast<size_t>(']')] = U'¿';
    result[static_cast<size_t>('{')] = U'°';
    result[static_cast<size_t>('|')] = U'ñ';
    result[static_cast<size_t>('}')] = U'ç';

    return result;
}

/// Swedish:
///     ESC ( H
///     ESC ( 7
constexpr CharsetMap createSwedishCharset()
{
    auto result = usasciiCharset();

    result[static_cast<size_t>('@')] = U'É';
    result[static_cast<size_t>('[')] = U'Ä';
    result[static_cast<size_t>('\\')] = U'Ö';
    result[static_cast<size_t>(']')] = U'Å';
    result[static_cast<size_t>('^')] = U'Ü';
    result[static_cast<size_t>('`')] = U'é';
    result[static_cast<size_t>('{')] = U'ä';
    result[static_cast<size_t>('|')] = U'ö';
    result[static_cast<size_t>('}')] = U'å';
    result[static_cast<size_t>('~')] = U'ü';

    return result;
}

/// Swiss:
///     ESC ( =
constexpr CharsetMap createSwissCharset()
{
    auto result = usasciiCharset();

    result[static_cast<size_t>('#')] = U'ù';
    result[static_cast<size_t>('@')] = U'à';
    result[static_cast<size_t>('[')] = U'é';
    result[static_cast<size_t>('\\')] = U'ç';
    result[static_cast<size_t>(']')] = U'ê';
    result[static_cast<size_t>('^')] = U'î';
    result[static_cast<size_t>('_')] = U'è';
    result[static_cast<size_t>('`')] = U'ô';
    result[static_cast<size_t>('{')] = U'ä';
    result[static_cast<size_t>('|')] = U'ö';
    result[static_cast<size_t>('}')] = U'ü';
    result[static_cast<size_t>('~')] = U'û';

    return result;
}

CharsetMap const* charsetMap(CharsetId _id) noexcept
{
    static auto const british = createBritishCharset();
    static auto const dutch = createDutchCharset();
    static auto const finish = createFinishCharset();
    static auto const french = createFrenchCharset();
    static auto const frenchCanadian = createFrenchCanadianCharset();
    static auto const german = createGermanCharset();
    static auto const norweigianDanish = createNorwegianDanishCharset();
    static auto const spanish = createSpanishCharset();
    static auto const special = createSpecialCharset();
    static auto const swedish = createSwedishCharset();
    static auto const swiss = createSwissCharset();
    static auto const usascii = usasciiCharset();

    switch (_id)
    {
        case CharsetId::British: return &british;
        case CharsetId::Dutch: return &dutch;
        case CharsetId::Finish: return &finish;
        case CharsetId::French: return &french;
        case CharsetId::FrenchCanadian: return &frenchCanadian;
        case CharsetId::German: return &german;
        case CharsetId::NorwegianDanish: return &norweigianDanish;
        case CharsetId::Spanish: return &spanish;
        case CharsetId::Special: return &special;
        case CharsetId::Swedish: return &swedish;
        case CharsetId::Swiss: return &swiss;
        case CharsetId::USASCII: return &usascii;
    }

    return nullptr;
}

} // end terminal
