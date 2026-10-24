// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/screen/ProgramStatusSet.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <ranges>
#include <string>
#include <tuple>
#include <utility>

#include <vtcontrol/EnumTable.hpp>
#include <vthost/control/ProgramStatusConvert.hpp>

using namespace vthost;

namespace
{

/// Whether @p bridge maps every enumerator of both sides, each once, and in step with vtbackend's spelling
/// and vtcontrol's: the row index is both enumerators' value.
template <typename Backend, typename Api, std::size_t N>
bool bridgesInStep(std::array<control::EnumBridge<Backend, Api>, N> const& bridge)
{
    if (vtcontrol::EnumTable<Api>::Rows.size() != N)
        return false;
    for (auto const index: std::views::iota(std::size_t { 0 }, N))
        if (std::to_underlying(bridge[index].backend) != index
            || std::to_underlying(bridge[index].api) != index)
            return false;
    return true;
}

} // namespace

TEST_CASE("the program-status enum bridges cannot drift", "[vthost][control][programstatus]")
{
    CHECK(bridgesInStep(control::ProgramStateBridge));
    CHECK(bridgesInStep(control::BlockedKindBridge));
    CHECK(bridgesInStep(control::ProgramStatusReasonBridge));
    // vtbackend's spelling of each state is the API's: a script reads what the program sent.
    for (auto const& row: vtbackend::ProgramStateNames)
        CHECK(vtcontrol::nameOf(control::toApi(control::ProgramStateBridge, row.value)) == row.name);
    for (auto const& row: vtbackend::BlockedKindNames)
        CHECK(vtcontrol::nameOf(control::toApi(control::BlockedKindBridge, row.value)) == row.name);
    CHECK(control::ProgramStateBridge.size() == vtbackend::ProgramStateNames.size());
}

TEST_CASE("ProgramStatus PDU round-trips and decodes totally", "[vthost][proto][programstatus]")
{
    auto const record = vtbackend::ProgramStatusRecord {
        .id = "deploy/eu",
        .state = vtbackend::ProgramState::Blocked,
        .kind = vtbackend::BlockedKind::Permission,
        .progress = uint8_t { 70 },
        .app = "terraform",
        .title = "Deploy",
        .message = "Apply?",
    };
    CHECK(control::fromWire(control::toWire(record)) == record);

    auto absent = record;
    absent.progress = std::nullopt;
    CHECK(control::toWire(absent).progress == proto::WireProgramStatusRecord::AbsentProgress);
    CHECK(control::fromWire(control::toWire(absent)) == absent);

    SECTION("a state this build does not know drops the record")
    {
        auto wire = control::toWire(record);
        wire.state = 9;
        CHECK(!control::fromWire(wire).has_value());
    }
    SECTION("an unknown kind reads as none, a progress out of range as absent")
    {
        auto wire = control::toWire(record);
        wire.kind = 77;
        wire.progress = 101;
        auto const decoded = control::fromWire(wire);
        REQUIRE(decoded.has_value());
        CHECK(decoded->kind == vtbackend::BlockedKind::None);
        CHECK(!decoded->progress.has_value());
    }
    SECTION("a kind and a progress on a state that takes neither are dropped")
    {
        auto wire = control::toWire(record);
        wire.state = std::to_underlying(vtbackend::ProgramState::Done);
        auto const decoded = control::fromWire(wire);
        REQUIRE(decoded.has_value());
        CHECK(decoded->kind == vtbackend::BlockedKind::None);
        CHECK(!decoded->progress.has_value());
    }
}

TEST_CASE("the control API lists a record with its inherited app", "[vthost][control][programstatus]")
{
    auto set = vtbackend::ProgramStatusSet {};
    std::ignore = set.apply(vtbackend::ProgramStatusRecord { .id = "",
                                                             .state = vtbackend::ProgramState::Working,
                                                             .kind = {},
                                                             .progress = {},
                                                             .app = "deploy",
                                                             .title = {},
                                                             .message = {} });
    std::ignore = set.apply(vtbackend::ProgramStatusRecord { .id = "eu",
                                                             .state = vtbackend::ProgramState::Error,
                                                             .kind = {},
                                                             .progress = {},
                                                             .app = {},
                                                             .title = {},
                                                             .message = "failed" });
    auto const listed = control::toApi(set);
    REQUIRE(listed.size() == 2);
    CHECK(listed[1].id == "eu");
    CHECK(listed[1].app == "deploy");
    CHECK(listed[1].state == vtcontrol::ProgramState::Error);
    CHECK(listed[1].message == "failed");
}
