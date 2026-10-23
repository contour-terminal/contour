// SPDX-License-Identifier: Apache-2.0
//
// What events record into the state an extension's `state` call answers with.
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <utility>
#include <vector>

#include <vtextension/HostState.hpp>

using namespace vtextension;

TEST_CASE("HostState.records", "[vtextension]")
{
    auto state = StateSnapshot {};
    recordEvent(state, CwdChanged { .cwd = "/x" }, 4);
    recordEvent(state, TitleChanged { .title = "t" }, 4);
    recordEvent(state, Resized { .size = SurfaceSize { .cols = 10, .rows = 1 } }, 4);
    recordEvent(state, ReplyBytes { .bytes = { 1 } }, 4);
    CHECK(state.cwd == "/x");
    CHECK(state.title == "t");
    CHECK(state.size == SurfaceSize { .cols = 10, .rows = 1 });
}

TEST_CASE("HostState.userVarsCapped", "[vtextension]")
{
    // Names beyond the cap are dropped; known names keep updating.
    using Vars = std::vector<std::pair<std::string, std::string>>;
    auto state = StateSnapshot {};
    recordEvent(state, UserVarChanged { .name = "a", .value = "1" }, 2);
    recordEvent(state, UserVarChanged { .name = "b", .value = "1" }, 2);
    recordEvent(state, UserVarChanged { .name = "c", .value = "1" }, 2);
    CHECK(state.userVars == Vars { { "a", "1" }, { "b", "1" } });
    recordEvent(state, UserVarChanged { .name = "a", .value = "2" }, 2);
    CHECK(state.userVars == Vars { { "a", "2" }, { "b", "1" } });
}
