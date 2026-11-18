// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/vt/ControlCode.hpp>
#include <vtbackend/vt/Functions.hpp>
#include <vtbackend/vt/ServedTabSequences.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <string_view>
#include <vector>

using namespace vtbackend;

namespace
{

/// A function a served session must not execute, and why the test names it.
struct Dropped
{
    Function function;
    std::string_view what;
};

/// A sample of what reaches past the grid: titles, clipboard, working directory, links, notifications, user
/// variables, images, window operations, replies and the conformance level.
constexpr auto DroppedSample = std::to_array<Dropped>({
    { SETTITLE, "OSC 0 title" },
    { SETWINTITLE, "OSC 2 title" },
    { CLIPBOARD, "OSC 52 clipboard" },
    { SETCWD, "OSC 7 working directory" },
    { HYPERLINK, "OSC 8 hyperlink" },
    { CONEMU, "OSC 9 notification" },
    { NOTIFY, "OSC 777 notification" },
    { ITERM2, "OSC 1337 SetUserVar" },
    { DECSIXEL, "sixel image" },
    { WINMANIP, "XTWINOPS" },
    { DA1, "primary device attributes" },
    { DSR, "device status report" },
    { DECRQSS, "status string request" },
    { DECSCL, "conformance level" },
});

} // namespace

TEST_CASE("a served session executes exactly the functions that draw", "[vtbackend][served]")
{
    // Spelled out here rather than read off the table, so that a function the table gains or loses shows: the
    // rendition, eleven movements, seven edits, the margins and scrolling, save and restore (DECRC is spelled
    // DECRS), DECSET and DECRST, and the cursor's shape.
    constexpr auto Expected = std::to_array<std::string_view>({
        "SGR",     "CUU", "CUD", "CUF",   "CUB",   "CNL",   "CPL",   "CHA",      "CUP", "HVP",
        "VPA",     "HPA", "ED",  "EL",    "ECH",   "DCH",   "ICH",   "IL",       "DL",  "DECSTBM",
        "DECSLRM", "SU",  "SD",  "DECSC", "DECRS", "DECSM", "DECRM", "DECSCUSR",
    });
    auto executed = std::vector<std::string_view> {};
    for (auto const& function: allFunctionsArray())
        if (servedTabExecutes(function))
            executed.push_back(function.documentation.mnemonic);
    std::ranges::sort(executed);
    auto expected = std::vector<std::string_view>(Expected.begin(), Expected.end());
    std::ranges::sort(expected);
    CHECK(executed == expected);
}

TEST_CASE("a function off the allowlist is dropped in a served session", "[vtbackend][served]")
{
    for (auto const& [function, what]: DroppedSample)
    {
        INFO(what);
        CHECK_FALSE(servedTabExecutes(function));
    }
    // The ANSI modes (IRM, LNM) are not the DEC private ones, and a character set designation draws in
    // another alphabet than the one the guest wrote.
    CHECK_FALSE(servedTabExecutes(SM));
    CHECK_FALSE(servedTabExecutes(RM));
    CHECK_FALSE(servedTabExecutes(SCS_G0_SPECIAL));
}

TEST_CASE("a served session sets only the DEC private modes that draw", "[vtbackend][served]")
{
    for (auto const mode:
         { 7U, 25U, 47U, 1047U, 1049U, 1000U, 1002U, 1003U, 1006U, 1015U, 1016U, 1004U, 2026U })
    {
        INFO(mode);
        CHECK(servedTabSetsMode(mode));
    }
    // Bracketed paste, origin mode, column mode, sixel scrolling, the reporting modes of the host's own:
    // none draws.
    for (auto const mode: { 1U, 3U, 6U, 69U, 80U, 2004U, 2027U, 2031U, 8452U })
    {
        INFO(mode);
        CHECK_FALSE(servedTabSetsMode(mode));
    }
}

TEST_CASE("a served session executes only the C0 controls that move the cursor", "[vtbackend][served]")
{
    for (auto const code: { '\x08', '\x09', '\x0A', '\x0B', '\x0C', '\x0D', '\x1B' })
    {
        INFO(static_cast<int>(code));
        CHECK(servedTabExecutesControl(code));
    }
    for (auto const code: { '\x00', '\x05', '\x07', '\x0E', '\x0F', '\x11', '\x13', '\x18', '\x1A' })
    {
        INFO(static_cast<int>(code));
        CHECK_FALSE(servedTabExecutesControl(code));
    }
}
