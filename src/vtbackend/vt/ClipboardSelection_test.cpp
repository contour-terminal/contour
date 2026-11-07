// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for OSC 52's selection parameter (Pc): which places a copy lands in.

#include <vtbackend/vt/ClipboardSelection.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <optional>
#include <string_view>

using namespace std::string_view_literals;
using vtbackend::ClipboardTarget;
using vtbackend::ClipboardTargets;
namespace clipboard_selection = vtbackend::clipboard_selection;

namespace
{
constexpr auto Clipboard = ClipboardTargets { ClipboardTarget::Clipboard };
constexpr auto Primary = ClipboardTargets { ClipboardTarget::PrimarySelection };
constexpr auto Both = ClipboardTargets { ClipboardTarget::Clipboard, ClipboardTarget::PrimarySelection };

struct ParseCase
{
    std::string_view pc {};
    std::optional<ClipboardTargets> expected {};
    std::string_view why {};
};
} // namespace

TEST_CASE("ClipboardSelection.parse", "[osc52][clipboard]")
{
    auto const cases = std::array {
        ParseCase { .pc = ""sv, .expected = Clipboard, .why = "xterm's s0; the Clipboard Extension's must" },
        ParseCase { .pc = "c"sv, .expected = Clipboard, .why = "the clipboard" },
        ParseCase {
            .pc = "s"sv, .expected = Clipboard, .why = "the configurable selection is the clipboard" },
        ParseCase { .pc = "p"sv, .expected = Primary, .why = "the primary selection alone" },
        ParseCase { .pc = "cp"sv, .expected = Both, .why = "both, as xterm sets both" },
        ParseCase { .pc = "pc"sv, .expected = Both, .why = "order does not matter" },
        ParseCase { .pc = "cc"sv, .expected = Clipboard, .why = "a repeated letter is one target" },
        ParseCase { .pc = "s0"sv, .expected = Clipboard, .why = "the cut buffer is skipped, not refused" },
        ParseCase { .pc = "cq"sv, .expected = Clipboard, .why = "the secondary selection is skipped" },
        ParseCase {
            .pc = "xc"sv, .expected = Clipboard, .why = "an undefined letter is skipped, as xterm does" },
        ParseCase { .pc = "q"sv, .expected = std::nullopt, .why = "only a place Contour has not got" },
        ParseCase { .pc = "01234567"sv, .expected = std::nullopt, .why = "only cut buffers" },
        ParseCase { .pc = "x"sv, .expected = std::nullopt, .why = "nothing defined at all" },
    };

    for (auto const& testCase: cases)
    {
        INFO("Pc=\"" << testCase.pc << "\": " << testCase.why);
        CHECK(clipboard_selection::parse(testCase.pc) == testCase.expected);
    }
}

TEST_CASE("ClipboardSelection.format", "[osc52][clipboard]")
{
    // The canonical spelling: what the daemon puts on the wire for the client to parse back.
    CHECK(clipboard_selection::format(Clipboard) == "c");
    CHECK(clipboard_selection::format(Primary) == "p");
    CHECK(clipboard_selection::format(Both) == "cp");
}
