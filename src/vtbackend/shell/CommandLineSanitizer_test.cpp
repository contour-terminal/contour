// SPDX-License-Identifier: Apache-2.0
//
// A command line arrives from whatever wrote to the PTY, so it is untrusted. These pin what survives
// being shown (Display), typed back into a shell (Insert, Type) and copied to the clipboard (Clipboard).

#include <vtbackend/shell/CommandLineSanitizer.hpp>

#include <libunicode/convert.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>

using vtbackend::sanitizeCommandLine;
using vtbackend::SanitizePurpose;

namespace
{

/// U+FFFD REPLACEMENT CHARACTER, UTF-8 encoded.
constexpr auto Fffd = std::string_view { "\xEF\xBF\xBD" };

/// Every purpose, for the checks that hold for all of them.
constexpr auto Purposes = std::array { SanitizePurpose::Display,
                                       SanitizePurpose::Insert,
                                       SanitizePurpose::Type,
                                       SanitizePurpose::Clipboard,
                                       SanitizePurpose::ClipboardMultiline };

/// @return @p codePoint, UTF-8 encoded.
[[nodiscard]] std::string utf8(char32_t codePoint)
{
    return unicode::convert_to<char>(codePoint);
}

/// @return @p pattern with every '#' standing for U+FFFD.
[[nodiscard]] std::string withReplacements(std::string_view pattern)
{
    auto result = std::string {};
    for (auto const ch: pattern)
    {
        if (ch == '#')
            result += Fffd;
        else
            result += ch;
    }
    return result;
}

} // namespace

TEST_CASE("sanitizeCommandLine keeps ordinary text as it is", "[sanitize]")
{
    auto const text = std::string_view {
        "git commit -m \"f\xc3\xbc\xc3\xbc\" && echo \xe2\x82\xac \xf0\x9f\x98\x80 ~/src"
    };
    for (auto const purpose: Purposes)
    {
        CHECK(sanitizeCommandLine(text, purpose) == text);
        CHECK(sanitizeCommandLine("", purpose).empty());
    }
}

TEST_CASE("sanitizeCommandLine replaces C0 controls for display and drops them for insertion", "[sanitize]")
{
    for (auto const code: std::views::iota(0x00, 0x20))
    {
        auto const control = static_cast<char>(code);
        if (control == '\t' || control == '\n')
            continue;
        INFO(std::format("C0 control 0x{:02X}", code));
        auto const text = std::string { "a" } + control + "b";
        CHECK(sanitizeCommandLine(text, SanitizePurpose::Display) == std::format("a{}b", Fffd));
        CHECK(sanitizeCommandLine(text, SanitizePurpose::Insert) == "ab");
        CHECK(sanitizeCommandLine(text, SanitizePurpose::Type) == "ab");
        // CR ends a line as LF does, so a copy joins at it rather than gluing the two halves together.
        CHECK(sanitizeCommandLine(text, SanitizePurpose::Clipboard) == (control == '\r' ? "a b" : "ab"));
    }
}

TEST_CASE("sanitizeCommandLine for the clipboard puts the command on one line", "[sanitize]")
{
    // The copy cannot know where it will be pasted: a shell without bracketed paste, cmd.exe, a REPL or a
    // serial console runs every line but the last as it arrives. Joined, the command is one line the user
    // sees before pressing Enter -- and without a trailing line end, not even that one runs by itself.
    CHECK(sanitizeCommandLine("make test\ncurl x|sh\n", SanitizePurpose::Clipboard) == "make test curl x|sh");
    CHECK(sanitizeCommandLine("for x in a b\n\tdo echo $x\ndone", SanitizePurpose::Clipboard)
          == "for x in a b  do echo $x done");
    CHECK(sanitizeCommandLine("make\r\n", SanitizePurpose::Clipboard) == "make");
}

TEST_CASE("sanitizeCommandLine for the clipboard trims trailing white space only", "[sanitize]")
{
    // Trailing, whatever it was before: spaces, TAB, LF and CR, and a space a dropped control leaves last.
    CHECK(sanitizeCommandLine("ls   ", SanitizePurpose::Clipboard) == "ls");
    CHECK(sanitizeCommandLine("ls \t\r\n", SanitizePurpose::Clipboard) == "ls");
    CHECK(sanitizeCommandLine("ls \x1b", SanitizePurpose::Clipboard) == "ls");
    CHECK(sanitizeCommandLine(" \n\t", SanitizePurpose::Clipboard).empty());
    // A leading space is the user's: bash's HISTCONTROL=ignorespace keeps such a command out of the history.
    CHECK(sanitizeCommandLine("  ls -l", SanitizePurpose::Clipboard) == "  ls -l");
    // The other purposes keep it.
    CHECK(sanitizeCommandLine("ls ", SanitizePurpose::Insert) == "ls ");
    CHECK(sanitizeCommandLine("ls ", SanitizePurpose::Display) == "ls ");
}

TEST_CASE("sanitizeCommandLine preserves multiline clipboard syntax across line ending conventions",
          "[sanitize]")
{
    auto const expected = "# comment\nWrite-Output @'\n\tfirst\n\nsecond\n'@";
    CHECK(sanitizeCommandLine(expected, SanitizePurpose::ClipboardMultiline) == expected);
    CHECK(sanitizeCommandLine("# comment\r\nWrite-Output @'\r\n\tfirst\r\n\r\nsecond\r\n'@\r\n",
                              SanitizePurpose::ClipboardMultiline)
          == expected);
    CHECK(sanitizeCommandLine("a\rb\r\r\nc\n \t", SanitizePurpose::ClipboardMultiline) == "a\nb\n\nc");
    CHECK(sanitizeCommandLine(" \t\r\n", SanitizePurpose::ClipboardMultiline).empty());
    CHECK(sanitizeCommandLine("  a\x1b[201~\n\tb\x07\n", SanitizePurpose::ClipboardMultiline)
          == "  a[201~\n\tb");
}

TEST_CASE("sanitizeCommandLine for the clipboard cannot end a bracketed paste", "[sanitize]")
{
    // What Insert drops, the clipboard drops too: the copy is pasted, and maybe into a bracketed paste.
    CHECK(sanitizeCommandLine("ls\x1b[201~rm -rf ~", SanitizePurpose::Clipboard) == "ls[201~rm -rf ~");
    CHECK(sanitizeCommandLine("ls\xC2\x9B"
                              "201~x",
                              SanitizePurpose::Clipboard)
          == "ls201~x");
    CHECK(sanitizeCommandLine("a\x7f"
                              "b",
                              SanitizePurpose::Clipboard)
          == "ab");
}

TEST_CASE("sanitizeCommandLine keeps TAB and LF for insertion only", "[sanitize]")
{
    auto const text = std::string_view { "for x in a b\n\tdo echo $x\ndone" };
    CHECK(sanitizeCommandLine(text, SanitizePurpose::Insert) == text);
    CHECK(sanitizeCommandLine(text, SanitizePurpose::Display)
          == std::format("for x in a b{0}{0}do echo $x{0}done", Fffd));
    // Typed raw, TAB completes and LF submits: neither survives.
    CHECK(sanitizeCommandLine(text, SanitizePurpose::Type) == "for x in a bdo echo $xdone");
}

TEST_CASE("sanitizeCommandLine treats DEL as a control", "[sanitize]")
{
    CHECK(sanitizeCommandLine("a\x7f"
                              "b",
                              SanitizePurpose::Display)
          == std::format("a{}b", Fffd));
    CHECK(sanitizeCommandLine("a\x7f"
                              "b",
                              SanitizePurpose::Insert)
          == "ab");
}

TEST_CASE("sanitizeCommandLine replaces C1 controls for display and drops them for insertion", "[sanitize]")
{
    for (auto const code: std::views::iota(0x80U, 0xA0U))
    {
        INFO(std::format("C1 control U+{:04X}", code));
        auto const text = "a" + utf8(static_cast<char32_t>(code)) + "b";
        CHECK(sanitizeCommandLine(text, SanitizePurpose::Display) == std::format("a{}b", Fffd));
        CHECK(sanitizeCommandLine(text, SanitizePurpose::Insert) == "ab");
    }

    // Their neighbours are ordinary text: '~' below, NO-BREAK SPACE above.
    for (auto const codePoint: std::array<char32_t, 2> { 0x7E, 0xA0 })
    {
        auto const text = "a" + utf8(codePoint) + "b";
        for (auto const purpose: Purposes)
            CHECK(sanitizeCommandLine(text, purpose) == text);
    }
}

TEST_CASE("sanitizeCommandLine replaces bidi embeddings, overrides and isolates for either purpose",
          "[sanitize]")
{
    constexpr auto BidiControls =
        std::array<char32_t, 9> { 0x202A, 0x202B, 0x202C, 0x202D, 0x202E, 0x2066, 0x2067, 0x2068, 0x2069 };
    for (auto const codePoint: BidiControls)
    {
        INFO(std::format("U+{:04X}", static_cast<uint32_t>(codePoint)));
        auto const text = "a" + utf8(codePoint) + "b";
        for (auto const purpose: Purposes)
            CHECK(sanitizeCommandLine(text, purpose) == std::format("a{}b", Fffd));
    }

    // The code points either side of both ranges are ordinary text.
    for (auto const codePoint: std::array<char32_t, 4> { 0x2027, 0x202F, 0x2065, 0x206A })
    {
        INFO(std::format("U+{:04X}", static_cast<uint32_t>(codePoint)));
        auto const text = "a" + utf8(codePoint) + "b";
        for (auto const purpose: Purposes)
            CHECK(sanitizeCommandLine(text, purpose) == text);
    }
}

TEST_CASE("sanitizeCommandLine replaces the line and paragraph separators for either purpose", "[sanitize]")
{
    // Qt's text layout breaks a line at either, so shown as they are they would put one command line on
    // two lines -- the spoof the LF rule exists to prevent. Replaced, the reader still sees one was there.
    for (auto const codePoint: std::array<char32_t, 2> { 0x2028, 0x2029 })
    {
        INFO(std::format("U+{:04X}", static_cast<uint32_t>(codePoint)));
        auto const text = "ls" + utf8(codePoint) + "rm -rf ~";
        for (auto const purpose: Purposes)
            CHECK(sanitizeCommandLine(text, purpose) == std::format("ls{}rm -rf ~", Fffd));
    }
}

TEST_CASE("sanitizeCommandLine replaces each maximal subpart of ill-formed UTF-8 once", "[sanitize]")
{
    struct Case
    {
        std::string_view input;
        std::string_view expected; ///< '#' stands for U+FFFD.
        std::string_view what;
    };
    constexpr auto Cases = std::array {
        // The Unicode Standard's own example (§3.9, Table 3-8, "U+FFFD for maximal subparts").
        Case { "\x61\xF1\x80\x80\xE1\x80\xC2\x62\x80\x63\x80\xBF\x64", "a###b#c##d", "Table 3-8" },
        Case { "\xC0\x80", "##", "overlong NUL" },
        Case { "\xE0\x80\x80", "###", "overlong three-byte form" },
        Case { "\xED\xA0\x80", "###", "a UTF-16 surrogate" },
        Case { "\xF4\x90\x80\x80", "####", "beyond U+10FFFF" },
        Case { "\xF5\x80", "##", "F5 never leads" },
        Case { "\xFF", "#", "FF never leads" },
        Case { "x\xE2\x82", "x#", "cut off by the end" },
        Case { "\xE2\x82x", "#x", "cut off by an ASCII byte" },
        Case { "\x80", "#", "a lone continuation byte" },
        Case { "\x9B"
               "31m",
               "#31m",
               "a raw 8-bit CSI" },
    };
    for (auto const& [input, expected, what]: Cases)
    {
        INFO(what);
        for (auto const purpose: Purposes)
            CHECK(sanitizeCommandLine(input, purpose) == withReplacements(expected));
    }
}

TEST_CASE("sanitizeCommandLine for insertion cannot end a bracketed paste", "[sanitize]")
{
    // A crafted cmdline_url carrying the bracketed-paste end marker would end the paste early and
    // have the shell run the remainder; without its ESC the marker is inert text.
    CHECK(sanitizeCommandLine("ls\x1b[201~rm -rf ~", SanitizePurpose::Insert) == "ls[201~rm -rf ~");

    // Nor do the 8-bit forms get through: U+009B (C1 CSI) is dropped, a raw 0x9B byte is ill-formed.
    CHECK(sanitizeCommandLine("ls\xC2\x9B"
                              "201~x",
                              SanitizePurpose::Insert)
          == "ls201~x");
    CHECK(sanitizeCommandLine("ls\x9B"
                              "201~x",
                              SanitizePurpose::Insert)
          == std::format("ls{}201~x", Fffd));

    CHECK(sanitizeCommandLine("\x1b]0;pwned\x07"
                              "ok",
                              SanitizePurpose::Insert)
          == "]0;pwnedok");
}

TEST_CASE("sanitizeCommandLine for display shows where each control was", "[sanitize]")
{
    CHECK(sanitizeCommandLine("ls\x1b[201~", SanitizePurpose::Display) == std::format("ls{}[201~", Fffd));
}

TEST_CASE("sanitizeCommandLine never truncates", "[sanitize]")
{
    // Bounding a command line is the store's business (MaxRecordedCommandLineBytes), not this
    // function's: 64 KiB in, every byte accounted for out.
    constexpr auto Length = size_t { 64 } * 1024;
    auto const plain = std::string(Length, 'x');
    auto const escapes = std::string(Length, '\x1b');
    for (auto const purpose: Purposes)
        CHECK(sanitizeCommandLine(plain, purpose) == plain);
    CHECK(sanitizeCommandLine(escapes, SanitizePurpose::Display).size() == Length * Fffd.size());
    CHECK(sanitizeCommandLine(escapes, SanitizePurpose::Insert).empty());
}

TEST_CASE("sanitizeCommandLine output is final", "[sanitize]")
{
    // What comes out is well-formed and free of everything the purpose forbids, so a second pass
    // changes nothing: a caller that sanitises twice cannot double-replace. (The RLO is spliced in
    // rather than written into the literal, where it would be a misleading bidi character itself.)
    auto const hostile = "a\x1b[31m\xC2\x9B\x7f\t\n" + utf8(0x202E)
                         + "b\xF0\x9F\x98"
                           "c\xFF";
    for (auto const purpose: Purposes)
    {
        auto const once = sanitizeCommandLine(hostile, purpose);
        CHECK(sanitizeCommandLine(once, purpose) == once);
    }
}

TEST_CASE("Text typed raw takes only what reaches a shell as written", "[sanitize]")
{
    auto const isSafeToType = [](std::string_view text) {
        return vtbackend::isUnchangedBySanitizing(text, SanitizePurpose::Type);
    };

    // Paths and command lines as a shell reads them.
    CHECK(isSafeToType(""));
    CHECK(isSafeToType("/home/u/notes.txt"));
    CHECK(isSafeToType("echo \"a  b;c\" 'd\"e' 50%"));
    CHECK(isSafeToType("\xc3\xbc\xe2\x82\xac\xf0\x9f\x98\x80"));

    // Keys, when typed raw: TAB completes, LF and CR submit.
    CHECK_FALSE(isSafeToType("a\tb"));
    CHECK_FALSE(isSafeToType("a\nb"));
    CHECK_FALSE(isSafeToType("a\rb"));
    // Controls a line editor or the terminal would act on: ESC and BEL, NUL, DEL, a C1 CSI.
    CHECK_FALSE(isSafeToType("/tmp/x\x1b]2;y\x07"));
    CHECK_FALSE(isSafeToType(std::string_view { "a\0b", 3 }));
    CHECK_FALSE(isSafeToType("a\x7f"));
    CHECK_FALSE(isSafeToType("a\xc2\x9b"));
    // A bidi override, and ill-formed UTF-8: the sanitiser would change either.
    CHECK_FALSE(isSafeToType("a" + utf8(0x202E) + "b"));
    CHECK_FALSE(isSafeToType("a\xff"));
}

TEST_CASE("isUnchangedBySanitizing answers what comparing with the sanitised copy would", "[sanitize]")
{
    using vtbackend::isUnchangedBySanitizing;

    // Plain text, a control at either end and in the middle, TAB and LF, C1, a separator, a bidi override,
    // well-formed and ill-formed multi-byte sequences, a sequence cut off by the end, and white space at
    // either end.
    auto const samples = std::array<std::string, 22> {
        "",
        "/home/u/notes.txt",
        "\x1b"
        "abc",
        "abc\x1b",
        "a\x07z",
        "a\tb\nc",
        "a\xc2\x9b"
        "b",
        "a" + utf8(0x2028) + "b",
        "a" + utf8(0x202E) + "b",
        "f\xc3\xbc\xe2\x82\xac\xf0\x9f\x98\x80",
        "a\xff",
        "a\xe2\x82",
        "\xed\xa0\x80",
        "x\x7f",
        "ls ",
        " ls",
        "f\xc3\xbc ",
        "a\r\nb",
        "a\rb",
        "a\n",
        "a\t",
        "a\n\tb",
    };
    for (auto const& sample: samples)
        for (auto const purpose: Purposes)
        {
            INFO(std::format("purpose {} on \"{}\"", std::to_underlying(purpose), sample));
            CHECK(isUnchangedBySanitizing(sample, purpose)
                  == (sanitizeCommandLine(sample, purpose) == sample));
        }
}

TEST_CASE("sanitizeCommandLine sanitises what follows a leading run of plain text", "[sanitize]")
{
    // The leading printable ASCII is copied whole; everything after it is still decoded.
    CHECK(sanitizeCommandLine("echo ok\x1b[2J", SanitizePurpose::Insert) == "echo ok[2J");
    CHECK(sanitizeCommandLine("echo ok\xff!", SanitizePurpose::Insert) == std::format("echo ok{}!", Fffd));
    CHECK(sanitizeCommandLine("echo " + utf8(0x202E) + "x", SanitizePurpose::Display)
          == std::format("echo {}x", Fffd));
}

TEST_CASE("trimTrailingWhiteSpace drops every White_Space code point the text ends with", "[sanitize]")
{
    using vtbackend::trimTrailingWhiteSpace;

    // Unicode's White_Space property (PropList.txt), not just ASCII's: a shell may pad what it draws below
    // its input with U+00A0, and that is read back when the command line is recovered from the grid.
    auto const whiteSpace = std::array<char32_t, 25> {
        0x0009, 0x000A, 0x000B, 0x000C, 0x000D, 0x0020, 0x0085, 0x00A0, 0x1680,
        0x2000, 0x2001, 0x2002, 0x2003, 0x2004, 0x2005, 0x2006, 0x2007, 0x2008,
        0x2009, 0x200A, 0x2028, 0x2029, 0x202F, 0x205F, 0x3000,
    };
    for (auto const codePoint: whiteSpace)
    {
        INFO(std::format("U+{:04X}", static_cast<uint32_t>(codePoint)));
        CHECK(trimTrailingWhiteSpace("ls" + utf8(codePoint)) == "ls");
        CHECK(trimTrailingWhiteSpace(utf8(codePoint)).empty());
        // Only what the text ends with: the same code point between two others stays.
        auto const inner = "a" + utf8(codePoint) + "b";
        CHECK(trimTrailingWhiteSpace(inner) == inner);
    }

    // A line break followed by a row of nothing but white space, as recovered from endo's spacer row.
    CHECK(trimTrailingWhiteSpace("echo hi\n\xC2\xA0  ") == "echo hi");
    CHECK(trimTrailingWhiteSpace("").empty());
}

TEST_CASE("trimTrailingWhiteSpace keeps what only looks blank", "[sanitize]")
{
    using vtbackend::trimTrailingWhiteSpace;

    // Not White_Space: the zero-width space and joiner, the word joiner, the BOM, the braille blank.
    for (auto const codePoint: std::array<char32_t, 5> { 0x200B, 0x200D, 0x2060, 0xFEFF, 0x2800 })
    {
        INFO(std::format("U+{:04X}", static_cast<uint32_t>(codePoint)));
        auto const text = "ls" + utf8(codePoint);
        CHECK(trimTrailingWhiteSpace(text) == text);
    }

    // Ill-formed UTF-8 is not white space, even where its bytes are a no-break space's: a lone continuation
    // byte 0xA0, and a lead byte 0xC2 cut off by the end.
    CHECK(trimTrailingWhiteSpace("ls\xA0") == "ls\xA0");
    CHECK(trimTrailingWhiteSpace("ls\xC2") == "ls\xC2");
    CHECK(trimTrailingWhiteSpace("ls\xC2 ") == "ls\xC2");
}
