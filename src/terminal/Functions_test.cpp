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
#include <terminal/Functions.h>

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
