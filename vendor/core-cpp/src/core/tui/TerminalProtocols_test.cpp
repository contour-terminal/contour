// SPDX-License-Identifier: Apache-2.0
#include <core/tui/TerminalProtocols.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <string>
#include <string_view>

using namespace core::tui::protocols;

// ============================================================================
// OSC 8 hyperlink sequences
// ============================================================================

TEST_CASE("TerminalProtocols.buildHyperlinkOpen_byte_exact")
{
    CHECK(buildHyperlinkOpen("https://x.com") == "\033]8;;https://x.com\033\\");
}

TEST_CASE("TerminalProtocols.buildHyperlinkOpen_empty_url")
{
    CHECK(buildHyperlinkOpen("") == "\033]8;;\033\\");
}

TEST_CASE("TerminalProtocols.hyperlinkClose_byte_exact")
{
    CHECK(std::string(HyperlinkClose) == "\033]8;;\033\\");
}

TEST_CASE("TerminalProtocols.hyperlink_round_trip")
{
    auto const sequence = buildHyperlinkOpen("https://a.b/c?d=1#e") + "label" + std::string(HyperlinkClose);
    CHECK(sequence == "\033]8;;https://a.b/c?d=1#e\033\\label\033]8;;\033\\");
}

TEST_CASE("TerminalProtocols.buildHyperlinkOpen_with_id_byte_exact")
{
    CHECK(buildHyperlinkOpen("file:///a", "1f2e") == "\033]8;id=1f2e;file:///a\033\\");
}

TEST_CASE("TerminalProtocols.buildHyperlinkOpen_empty_id_matches_id_less_form")
{
    // An empty id must emit no parameters at all, so callers that do not care about link
    // identity keep producing the exact bytes the id-less overload always produced.
    CHECK(buildHyperlinkOpen("https://x.com", "") == buildHyperlinkOpen("https://x.com"));
    CHECK(buildHyperlinkOpen("https://x.com", "") == "\033]8;;https://x.com\033\\");
}

// ============================================================================
// DA1 (Primary Device Attributes) Sixel detection
// ============================================================================

TEST_CASE("TerminalProtocols.da1_reports_sixel_when_parameter_4_present")
{
    CHECK(parseSixelFromDeviceAttributes("\033[?62;4;6c"));
    CHECK(parseSixelFromDeviceAttributes("\033[?63;4c"));
    CHECK(parseSixelFromDeviceAttributes("\033[?4c"));
    CHECK(parseSixelFromDeviceAttributes("\033[?62;1;2;4;6;9;15;22c"));
}

TEST_CASE("TerminalProtocols.da1_reports_no_sixel_when_parameter_4_absent")
{
    CHECK_FALSE(parseSixelFromDeviceAttributes("\033[?62;6c"));
    CHECK_FALSE(parseSixelFromDeviceAttributes("\033[?1;2c"));
    CHECK_FALSE(parseSixelFromDeviceAttributes("\033[?62c"));
}

TEST_CASE("TerminalProtocols.da1_does_not_match_parameter_substrings")
{
    // 14, 40 and 64 all contain '4' but are not the Sixel attribute.
    CHECK_FALSE(parseSixelFromDeviceAttributes("\033[?14;40;64c"));
}

TEST_CASE("TerminalProtocols.da1_accepts_single_byte_csi")
{
    CHECK(parseSixelFromDeviceAttributes("\x9B?62;4c"));
    CHECK_FALSE(parseSixelFromDeviceAttributes("\x9B?62c"));
}

TEST_CASE("TerminalProtocols.da1_rejects_malformed_responses")
{
    CHECK_FALSE(parseSixelFromDeviceAttributes(""));
    CHECK_FALSE(parseSixelFromDeviceAttributes("garbage"));
    CHECK_FALSE(parseSixelFromDeviceAttributes("\033[?62;4")); // truncated: no terminating 'c'
    CHECK_FALSE(parseSixelFromDeviceAttributes("\033[62;4c")); // missing '?' private marker
    CHECK_FALSE(parseSixelFromDeviceAttributes("4"));
}

TEST_CASE("TerminalProtocols.da1_tolerates_surrounding_noise")
{
    // Other terminal replies may arrive in the same read.
    CHECK(parseSixelFromDeviceAttributes("\033[1;1R\033[?62;4;6c"));
}

// ============================================================================
// Mouse tracking modes (DEC 1000, 1002, 1003 with SGR 1006)
// ============================================================================

namespace
{

/// @brief One transition and the exact bytes it writes.
struct MouseTrackingChange
{
    core::tui::MouseTracking from;
    core::tui::MouseTracking to;
    std::string_view bytes;
};

} // namespace

TEST_CASE("TerminalProtocols.mouse_tracking_change_byte_exact")
{
    using enum core::tui::MouseTracking;
    auto const change = GENERATE(values<MouseTrackingChange>({
        { Off, Off, "" },
        { Off, Buttons, "\033[?1000h\033[?1006h" },
        { Off, Drag, "\033[?1002h\033[?1006h" },
        { Off, AnyMotion, "\033[?1003h\033[?1006h" },
        { Buttons, Off, "\033[?1006l\033[?1000l" },
        { Buttons, Buttons, "" },
        { Buttons, Drag, "\033[?1000l\033[?1002h" },
        { Buttons, AnyMotion, "\033[?1000l\033[?1003h" },
        { Drag, Off, "\033[?1006l\033[?1002l" },
        { Drag, Buttons, "\033[?1002l\033[?1000h" },
        { Drag, Drag, "" },
        { Drag, AnyMotion, "\033[?1002l\033[?1003h" },
        { AnyMotion, Off, "\033[?1006l\033[?1003l" },
        { AnyMotion, Buttons, "\033[?1003l\033[?1000h" },
        { AnyMotion, Drag, "\033[?1003l\033[?1002h" },
        { AnyMotion, AnyMotion, "" },
    }));
    CAPTURE(change.from, change.to);
    auto out = std::string {};
    appendMouseTrackingChange(out, change.from, change.to);
    CHECK(out == change.bytes);
}

TEST_CASE("TerminalProtocols.mouse_tracking_change_appends")
{
    auto out = std::string { "x" };
    appendMouseTrackingChange(out, core::tui::MouseTracking::Off, core::tui::MouseTracking::Drag);
    CHECK(out == "x\033[?1002h\033[?1006h");
}

TEST_CASE("TerminalProtocols.mouse_tracking_off_has_no_mode_sequence")
{
    CHECK(mouseTrackingSet(core::tui::MouseTracking::Off).empty());
    CHECK(mouseTrackingReset(core::tui::MouseTracking::Off).empty());
    STATIC_REQUIRE(mouseTrackingSet(core::tui::MouseTracking::Drag) == "\033[?1002h");
}
