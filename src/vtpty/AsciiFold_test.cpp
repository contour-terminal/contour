// SPDX-License-Identifier: Apache-2.0
#include <vtpty/AsciiFold.hpp>

#include <catch2/catch_test_macros.hpp>

using vtpty::asciiLower;
using vtpty::asciiUpper;
using vtpty::equalsIgnoringAsciiCase;
using vtpty::isAsciiLetter;

static_assert(isAsciiLetter('a') && isAsciiLetter('z') && isAsciiLetter('A') && isAsciiLetter('Z'));
static_assert(!isAsciiLetter('@') && !isAsciiLetter('[') && !isAsciiLetter('`') && !isAsciiLetter('{'));
static_assert(!isAsciiLetter('0') && !isAsciiLetter('\xc3'));
static_assert(asciiUpper('a') == 'A' && asciiUpper('z') == 'Z' && asciiUpper('A') == 'A');
static_assert(asciiUpper('{') == '{' && asciiUpper('\xa4') == '\xa4');
static_assert(asciiLower('A') == 'a' && asciiLower('Z') == 'z' && asciiLower('a') == 'a');
static_assert(asciiLower('@') == '@' && asciiLower('\x84') == '\x84');

TEST_CASE("AsciiFold: only ASCII letters fold", "[vtpty]")
{
    CHECK(equalsIgnoringAsciiCase("Git_Dir", "GIT_DIR"));
    CHECK(equalsIgnoringAsciiCase("", ""));
    CHECK_FALSE(equalsIgnoringAsciiCase("PATH", "PATHS"));
    CHECK_FALSE(equalsIgnoringAsciiCase("a@", "a`"));
    CHECK_FALSE(equalsIgnoringAsciiCase("\xc3\x84", "\xc3\xa4")); // Ä and ä
}
