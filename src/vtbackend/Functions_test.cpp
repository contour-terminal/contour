// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/Functions.h>

#include <fmt/format.h>

#include <catch2/catch.hpp>

namespace terminal
{
// purely for proper diagnostic printing in Catch2
inline std::ostream& operator<<(std::ostream& os, FunctionDefinition const& f)
{
    return os << fmt::format("{}", f);
}
} // namespace terminal

using namespace std;
using namespace terminal;

TEST_CASE("Functions.SCOSC", "[Functions]")
{
    FunctionDefinition const* f = terminal::selectControl(0, 0, 0, 's');
    REQUIRE(f);
    CHECK(*f == SCOSC);
}

TEST_CASE("Functions.DECSLRM", "[Functions]")
{
    // Maybe it is okay to not care about 0 and 1 arguments? Who's doing that?
    FunctionDefinition const* f = terminal::selectControl(0, 2, 0, 's');
    REQUIRE(f);
    CHECK(*f == DECSLRM);
}

TEST_CASE("Functions.OSC1", "[Functions]")
{
    FunctionDefinition const* osc = terminal::selectOSCommand(1);
    REQUIRE(osc);
    CHECK(*osc == SETICON);
}

TEST_CASE("Functions.OSC2", "[Functions]")
{
    FunctionDefinition const* osc = terminal::selectOSCommand(2);
    REQUIRE(osc);
    CHECK(*osc == SETWINTITLE);
}

TEST_CASE("Functions.OSC8", "[Functions]")
{
    FunctionDefinition const* osc = terminal::selectOSCommand(8);
    REQUIRE(osc);
    CHECK(*osc == HYPERLINK);
}

TEST_CASE("Functions.OSC777", "[Functions]")
{
    FunctionDefinition const* osc = terminal::selectOSCommand(777);
    REQUIRE(osc);
    CHECK(*osc == NOTIFY);
}
