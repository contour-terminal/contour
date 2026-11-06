// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <string_view>

#include <vthost/handover/HandoverToken.hpp>

using namespace vthost::handover;

TEST_CASE("the memory object a POSIX image is in round-trips through its token", "[vthost][handover]")
{
    auto const memory = vtpty::CrossingHandle { 7 };
    auto const token = handoverToken(memory);
    CHECK(token == "fd=7");
    CHECK(parseHandoverDescriptor(token) == memory);
    CHECK(handoverArgument(token) == "--handover=fd=7");
    CHECK(handoverTokenOf(handoverArgument(token)) == token);
}

TEST_CASE("a token that does not name one descriptor names none", "[vthost][handover]")
{
    auto const value = GENERATE(as<std::string_view> {},
                                "",
                                "7",
                                "fd=",
                                "fd=-1",
                                "fd=0",
                                "fd=2",
                                "fd=+7",
                                "fd=7x",
                                "fd=99999999999999999999",
                                "fd=2147483648",
                                "fd=4294967297",
                                "handles=1,2,3,4");
    CAPTURE(value);
    CHECK_FALSE(parseHandoverDescriptor(value).has_value());
}

TEST_CASE("the handles a successor is handed round-trip through their token", "[vthost][handover]")
{
    auto const handles = SuccessorHandles { .image = vtpty::CrossingHandle { 0x1A4 },
                                            .ready = vtpty::CrossingHandle { 0x2B8 },
                                            .commit = vtpty::CrossingHandle { 0x2BC },
                                            .predecessor = vtpty::CrossingHandle { 0x7FFF'FFF0 } };
    auto const token = handoverToken(handles);
    CHECK(token == "handles=420,696,700,2147483632");
    CHECK(parseHandoverHandles(token) == handles);
}

TEST_CASE("a token that does not name four handles names none", "[vthost][handover]")
{
    auto const value = GENERATE(as<std::string_view> {},
                                "",
                                "1,2,3,4",
                                "fd=3",
                                "handles=",
                                "handles=1,2,3",
                                "handles=1,2,3,4,5",
                                "handles=1,2,x,4",
                                "handles=1,0,3,4",
                                "handles=-1,2,3,4",
                                "handles=1,2,3,4,");
    CAPTURE(value);
    CHECK_FALSE(parseHandoverHandles(value).has_value());
}

TEST_CASE("an argument that is not --handover= carries no token", "[vthost][handover]")
{
    auto const value = GENERATE(as<std::string_view> {}, "", "--handover", "--label", "-handover=fd=7");
    CAPTURE(value);
    CHECK_FALSE(handoverTokenOf(value).has_value());
}
