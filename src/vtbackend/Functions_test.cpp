// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/Functions.h>

#include <fmt/format.h>

#include <catch2/catch_test_macros.hpp>

namespace vtbackend
{
// purely for proper diagnostic printing in Catch2
inline std::ostream& operator<<(std::ostream& os, FunctionDefinition const& f)
{
    return os << fmt::format("{}", f);
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
    FunctionDefinition const* f =
        vtbackend::selectControl(0, 0, 0, 's', availableSequences.activeSequences());
    REQUIRE(f);
    CHECK(*f == SCOSC);
}

TEST_CASE("Functions.DECSLRM", "[Functions]")
{
    // Maybe it is okay to not care about 0 and 1 arguments? Who's doing that?
    SupportedSequences const availableSequences;
    FunctionDefinition const* f =
        vtbackend::selectControl(0, 2, 0, 's', availableSequences.activeSequences());
    REQUIRE(f);
    CHECK(*f == DECSLRM);
}

TEST_CASE("Functions.OSC1", "[Functions]")
{
    SupportedSequences const availableSequences;
    FunctionDefinition const* osc = vtbackend::selectOSCommand(1, availableSequences.activeSequences());
    REQUIRE(osc);
    CHECK(*osc == SETICON);
}

TEST_CASE("Functions.OSC2", "[Functions]")
{
    SupportedSequences const availableSequences;
    FunctionDefinition const* osc = vtbackend::selectOSCommand(2, availableSequences.activeSequences());
    REQUIRE(osc);
    CHECK(*osc == SETWINTITLE);
}

TEST_CASE("Functions.OSC8", "[Functions]")
{
    SupportedSequences const availableSequences;
    FunctionDefinition const* osc = vtbackend::selectOSCommand(8, availableSequences.activeSequences());
    REQUIRE(osc);
    CHECK(*osc == HYPERLINK);
}

TEST_CASE("Functions.OSC777", "[Functions]")
{
    SupportedSequences const availableSequences;
    FunctionDefinition const* osc = vtbackend::selectOSCommand(777, availableSequences.activeSequences());
    REQUIRE(osc);
    CHECK(*osc == NOTIFY);
}

TEST_CASE("Functions.VTLevelConstrain", "[Functions]")
{
    SupportedSequences availableSequences;
    availableSequences.reset(VTType::VT100);
    FunctionDefinition const* f =
        vtbackend::selectControl(0, 2, 0, 's', availableSequences.activeSequences());
    REQUIRE(!f);
    availableSequences.reset(VTType::VT420);
    f = vtbackend::selectControl(0, 2, 0, 's', availableSequences.activeSequences());
    REQUIRE(f);
    CHECK(*f == DECSLRM);
}

TEST_CASE("Functions.EnableAndDisable", "[Functions]")
{
    SupportedSequences availableSequences;
    availableSequences.disableSequence(DECSLRM);
    FunctionDefinition const* f =
        vtbackend::selectControl(0, 2, 0, 's', availableSequences.activeSequences());
    REQUIRE(!f);
    availableSequences.enableSequence(DECSLRM);
    f = vtbackend::selectControl(0, 2, 0, 's', availableSequences.activeSequences());
    REQUIRE(f);
    CHECK(*f == DECSLRM);
}
