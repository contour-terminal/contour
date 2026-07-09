// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/Functions.h>

#include <catch2/catch_test_macros.hpp>

#include <format>

namespace vtbackend
{
// purely for proper diagnostic printing in Catch2
inline std::ostream& operator<<(std::ostream& os, Function const& f)
{
    return os << std::format("{}", f);
}
} // namespace vtbackend

using namespace std;
using namespace vtbackend;

TEST_CASE("Functions.SCOSC", "[Functions]")
{
    SupportedSequences availableSequences;
    // The problem with SCOSC vs DECSLRM is, that the former is a subset of the latter
    // when no arguments are given.
    availableSequences.disableSequence(DECSLRM);
    Function const* f = vtbackend::selectControl(0, 0, 0, 's', availableSequences.activeSequences());
    REQUIRE(f);
    CHECK(*f == SCOSC);
}

TEST_CASE("Functions.DECSLRM", "[Functions]")
{
    // Maybe it is okay to not care about 0 and 1 arguments? Who's doing that?
    SupportedSequences const availableSequences;
    Function const* f = vtbackend::selectControl(0, 2, 0, 's', availableSequences.activeSequences());
    REQUIRE(f);
    CHECK(*f == DECSLRM);
}

TEST_CASE("Functions.OSC1", "[Functions]")
{
    SupportedSequences const availableSequences;
    Function const* osc = vtbackend::selectOSCommand(1, availableSequences.activeSequences());
    REQUIRE(osc);
    CHECK(*osc == SETICON);
}

TEST_CASE("Functions.OSC2", "[Functions]")
{
    SupportedSequences const availableSequences;
    Function const* osc = vtbackend::selectOSCommand(2, availableSequences.activeSequences());
    REQUIRE(osc);
    CHECK(*osc == SETWINTITLE);
}

TEST_CASE("Functions.OSC8", "[Functions]")
{
    SupportedSequences const availableSequences;
    Function const* osc = vtbackend::selectOSCommand(8, availableSequences.activeSequences());
    REQUIRE(osc);
    CHECK(*osc == HYPERLINK);
}

TEST_CASE("Functions.OSC777", "[Functions]")
{
    SupportedSequences const availableSequences;
    Function const* osc = vtbackend::selectOSCommand(777, availableSequences.activeSequences());
    REQUIRE(osc);
    CHECK(*osc == NOTIFY);
}

TEST_CASE("Functions.VTLevelConstrain", "[Functions]")
{
    SupportedSequences availableSequences;
    availableSequences.reset(VTType::VT100);
    Function const* f = vtbackend::selectControl(0, 2, 0, 's', availableSequences.activeSequences());
    REQUIRE(!f);
    availableSequences.reset(VTType::VT420);
    f = vtbackend::selectControl(0, 2, 0, 's', availableSequences.activeSequences());
    REQUIRE(f);
    CHECK(*f == DECSLRM);
}

TEST_CASE("Functions.DCS_GIP", "[Functions]")
{
    // GIP is DCS with intermediate='!' and final='g' (0 args).
    // This tests the binary search leftward-scan fix for ambiguous DCS selectors.
    SupportedSequences const availableSequences;
    auto const* f = vtbackend::select({ .category = FunctionCategory::DCS,
                                        .leader = 0,
                                        .argc = 0,
                                        .intermediate = '!',
                                        .finalSymbol = 'g' },
                                      availableSequences.activeSequences());
    REQUIRE(f);
    CHECK(*f == GIP);
}

TEST_CASE("Functions.EnableAndDisable", "[Functions]")
{
    SupportedSequences availableSequences;
    availableSequences.disableSequence(DECSLRM);
    Function const* f = vtbackend::selectControl(0, 2, 0, 's', availableSequences.activeSequences());
    REQUIRE(!f);
    availableSequences.enableSequence(DECSLRM);
    f = vtbackend::selectControl(0, 2, 0, 's', availableSequences.activeSequences());
    REQUIRE(f);
    CHECK(*f == DECSLRM);
}

TEST_CASE("Functions.CSI_DECAC", "[Functions]")
{
    // DECAC is CSI with intermediate=',' and final='|' (item ; fg ; bg).
    SupportedSequences const availableSequences;
    auto const* f = vtbackend::select({ .category = FunctionCategory::CSI,
                                        .leader = 0,
                                        .argc = 3,
                                        .intermediate = ',',
                                        .finalSymbol = '|' },
                                      availableSequences.activeSequences());
    REQUIRE(f);
    CHECK(*f == DECAC);
}

TEST_CASE("Functions.CSI_DECATC", "[Functions]")
{
    // DECATC is CSI with intermediate=',' and final='}' (attribute ; fg ; bg).
    SupportedSequences const availableSequences;
    auto const* f = vtbackend::select({ .category = FunctionCategory::CSI,
                                        .leader = 0,
                                        .argc = 3,
                                        .intermediate = ',',
                                        .finalSymbol = '}' },
                                      availableSequences.activeSequences());
    REQUIRE(f);
    CHECK(*f == DECATC);
}

TEST_CASE("Functions.DECAC_VTLevelConstrain", "[Functions]")
{
    // DECAC/DECATC are VT525; they must not resolve below that conformance level.
    SupportedSequences availableSequences;
    auto const selector = FunctionSelector {
        .category = FunctionCategory::CSI, .leader = 0, .argc = 3, .intermediate = ',', .finalSymbol = '|'
    };
    availableSequences.reset(VTType::VT510);
    REQUIRE(!vtbackend::select(selector, availableSequences.activeSequences()));
    availableSequences.reset(VTType::VT525);
    auto const* f = vtbackend::select(selector, availableSequences.activeSequences());
    REQUIRE(f);
    CHECK(*f == DECAC);
}

TEST_CASE("Functions.DECATC_VTLevelConstrain", "[Functions]")
{
    // DECATC is VT525; it must not resolve below that conformance level.
    SupportedSequences availableSequences;
    auto const selector = FunctionSelector {
        .category = FunctionCategory::CSI, .leader = 0, .argc = 3, .intermediate = ',', .finalSymbol = '}'
    };
    availableSequences.reset(VTType::VT510);
    REQUIRE(!vtbackend::select(selector, availableSequences.activeSequences()));
    availableSequences.reset(VTType::VT525);
    auto const* f = vtbackend::select(selector, availableSequences.activeSequences());
    REQUIRE(f);
    CHECK(*f == DECATC);
}

TEST_CASE("Functions.CSI_DECATC_ZeroParameterForm", "[Functions]")
{
    // DECATC must resolve with no parameters at all: a lone '0' attribute collapses to zero parameters
    // under the VT convention, and `CSI 0 , }` is the only spelling of "reset the normal-text entry".
    // Registering DECATC with minArgs=1 would drop that sequence before it ever reaches the handler.
    SupportedSequences const availableSequences;
    auto const* f = vtbackend::select({ .category = FunctionCategory::CSI,
                                        .leader = 0,
                                        .argc = 0,
                                        .intermediate = ',',
                                        .finalSymbol = '}' },
                                      availableSequences.activeSequences());
    REQUIRE(f);
    CHECK(*f == DECATC);
}

TEST_CASE("Functions.CSI_DECSTGLT", "[Functions]")
{
    // DECSTGLT is CSI with intermediate=')' and final='{' (VT525).
    SupportedSequences const availableSequences;
    auto const* f = vtbackend::select({ .category = FunctionCategory::CSI,
                                        .leader = 0,
                                        .argc = 1,
                                        .intermediate = ')',
                                        .finalSymbol = '{' },
                                      availableSequences.activeSequences());
    REQUIRE(f);
    CHECK(*f == DECSTGLT);
}

TEST_CASE("Functions.DECSTGLT_VTLevelConstrain", "[Functions]")
{
    // DECSTGLT is VT525; it must not resolve below that conformance level.
    SupportedSequences availableSequences;
    auto const selector = FunctionSelector {
        .category = FunctionCategory::CSI, .leader = 0, .argc = 1, .intermediate = ')', .finalSymbol = '{'
    };
    availableSequences.reset(VTType::VT510);
    REQUIRE(!vtbackend::select(selector, availableSequences.activeSequences()));
    availableSequences.reset(VTType::VT525);
    auto const* f = vtbackend::select(selector, availableSequences.activeSequences());
    REQUIRE(f);
    CHECK(*f == DECSTGLT);
}
