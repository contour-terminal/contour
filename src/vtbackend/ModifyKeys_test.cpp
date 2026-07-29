// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/MockTerm.h>
#include <vtbackend/ModifyKeys.h>

#include <vtpty/MockPty.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <string_view>

using vtbackend::fromModifyKeysResourceNum;
using vtbackend::ModifyKeysAction;
using vtbackend::ModifyKeysRequest;
using vtbackend::modifyKeysRequest;
using vtbackend::ModifyKeysResource;
using vtbackend::toModifyKeysResourceNum;

namespace
{
constexpr auto TestPageSize = vtbackend::PageSize { vtbackend::LineCount(5), vtbackend::ColumnCount(20) };
} // namespace

// The pure decision, driven directly -- no Screen, no Terminal, no Sequence.
TEST_CASE("modifyKeysRequest reads Pp as the RESOURCE, not the value", "[ModifyKeys]")
{
    // This is the whole defect in one assertion. `CSI > 4 ; 2 m` means "set the
    // modifyOtherKeys resource to 2"; while XTMODKEYS was declared with room for a
    // single parameter, the handler read parameter 0 -- the resource selector -- as the
    // level and set 4.
    CHECK(modifyKeysRequest(4, 2)
          == ModifyKeysRequest { .action = ModifyKeysAction::SetOtherKeys, .otherKeysValue = 2 });
    CHECK(modifyKeysRequest(4, 0)
          == ModifyKeysRequest { .action = ModifyKeysAction::SetOtherKeys, .otherKeysValue = 0 });
}

TEST_CASE("modifyKeysRequest ignores the resources Contour does not track", "[ModifyKeys]")
{
    // Recognized, deliberately not applied: Contour always reports modifiers for cursor
    // and function keys. Crucially, `CSI > 2 ; 2 m` (modifyFunctionKeys) must NOT reach
    // modifyOtherKeys -- which is exactly what the one-parameter reading did.
    for (auto const resource: { 0, 1, 2, 3, 6, 7 })
        CHECK(modifyKeysRequest(resource, 2).action == ModifyKeysAction::Ignore);
}

TEST_CASE("modifyKeysRequest rejects resource numbers xterm does not define", "[ModifyKeys]")
{
    // 5 is reserved for input via xterm's string action, which is why the numbering is a
    // table rather than a contiguous cast.
    CHECK(modifyKeysRequest(5, 2).action == ModifyKeysAction::Unknown);
    CHECK(modifyKeysRequest(8, 2).action == ModifyKeysAction::Unknown);
    CHECK(modifyKeysRequest(-1, 2).action == ModifyKeysAction::Unknown);
    CHECK(!fromModifyKeysResourceNum(5).has_value());
}

TEST_CASE("the resource table round-trips every enumerator", "[ModifyKeys]")
{
    for (auto const& row: vtbackend::ModifyKeysResources)
    {
        CHECK(toModifyKeysResourceNum(row.resource) == row.number);
        REQUIRE(fromModifyKeysResourceNum(row.number).has_value());
        CHECK(*fromModifyKeysResourceNum(row.number) == row.resource);
    }
    CHECK(toModifyKeysResourceNum(ModifyKeysResource::OtherKeys) == 4);
}

// And the sequences themselves, through a real terminal -- which covers the parameter
// SHAPES (omitted Pv, omitted Pp, no parameters at all) that the pure decision above
// never sees, since Screen resolves them before calling it.
TEST_CASE("XTMODKEYS and XTRMMODKEYS act on the resource Pp names", "[ModifyKeys]")
{
    auto const [sequence, expected] = GENERATE(table<std::string_view, int>({
        { "\033[>4;1m", 1 },           // assign another level
        { "\033[>4m", 0 },             // reset the named resource (read as a value this SET 4)
        { "\033[>m", 0 },              // reset every resource
        { "\033[>2;2m\033[>1;3m", 2 }, // untracked resources: the level survives
        { "\033[>4n", 0 },             // XTRMMODKEYS: the -1 XTMODKEYS cannot express
        { "\033[>n", 2 },              // XTRMMODKEYS' default Ps is modifyFunctionKeys
    }));

    // Start at level 2, so each case asserts what its sequence does to a NON-default state.
    auto mock = vtbackend::MockTerm { TestPageSize };
    mock.writeToScreen("\033[>4;2m");
    REQUIRE(mock.terminal.modifyOtherKeys() == 2);

    mock.writeToScreen(sequence);
    CHECK(mock.terminal.modifyOtherKeys() == expected);
}

TEST_CASE("XTMODKEYS assigns the level from its second parameter", "[ModifyKeys]")
{
    auto mock = vtbackend::MockTerm { TestPageSize };
    REQUIRE(mock.terminal.modifyOtherKeys() == 0);

    mock.writeToScreen("\033[>4;2m");
    CHECK(mock.terminal.modifyOtherKeys() == 2);
}

TEST_CASE("an out-of-range Pp cannot alias onto a real resource", "[ModifyKeys]")
{
    // A CSI parameter is a uint16_t (the parser clamps at 0xFFFF), and reading Pp as a uint8_t
    // wrapped it modulo 256: `CSI > 260 ; 2 m` became resource 4 — modifyOtherKeys — and silently
    // switched the terminal to the CSI-u encoding, so Ctrl+C stopped interrupting and Ctrl+D
    // stopped sending EOF. modifyKeysRequest() range-checks with std::cmp_equal precisely so an
    // undefined selector falls through to Unknown; xterm likewise ignores what it does not define.
    auto const sequence = GENERATE(std::string_view { "\033[>260;2m" },   // 260 % 256 == 4
                                   std::string_view { "\033[>65540;2m" }, // clamps to 0xFFFF
                                   std::string_view { "\033[>256;2m" },   // 256 % 256 == 0
                                   std::string_view { "\033[>1284;2m" }); // 1284 % 256 == 4

    auto mock = vtbackend::MockTerm { TestPageSize };
    REQUIRE(mock.terminal.modifyOtherKeys() == 0);
    mock.writeToScreen(sequence);
    CHECK(mock.terminal.modifyOtherKeys() == 0);
}

TEST_CASE("an out-of-range XTRMMODKEYS Ps cannot alias either", "[ModifyKeys]")
{
    // The same narrowing on the reset side, where the default value's own type (uint8_t) used to
    // decide the parameter's width.
    auto mock = vtbackend::MockTerm { TestPageSize };
    mock.writeToScreen("\033[>4;2m");
    REQUIRE(mock.terminal.modifyOtherKeys() == 2);

    mock.writeToScreen("\033[>260n"); // 260 % 256 == 4 would have reset modifyOtherKeys
    CHECK(mock.terminal.modifyOtherKeys() == 2);
}
