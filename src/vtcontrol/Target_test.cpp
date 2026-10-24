// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <vector>

#include <vtcontrol/Target.hpp>
#include <vtcontrol/testing/SequentialUuidSource.hpp>

namespace
{
using vtcontrol::ControlErrorCode;
using vtcontrol::ObjectKind;
using vtcontrol::ObjectRef;
using vtcontrol::TargetCandidate;

struct Fixture
{
    vtcontrol::testing::SequentialUuidSource uuids;
    TargetCandidate a { .ref = { ObjectKind::Pane, uuids.next() },
                        .label = "logs",
                        .position = std::nullopt };
    TargetCandidate b { .ref = { ObjectKind::Pane, uuids.next() },
                        .label = "Logs",
                        .position = std::nullopt };
    TargetCandidate c { .ref = { ObjectKind::Pane, uuids.next() },
                        .label = "build",
                        .position = std::nullopt };
    TargetCandidate t1 { .ref = { ObjectKind::Tab, uuids.next() }, .label = "", .position = 1 };
    TargetCandidate t2 { .ref = { ObjectKind::Tab, uuids.next() }, .label = "", .position = 2 };
    std::vector<TargetCandidate> panes { a, b, c };
    std::vector<TargetCandidate> tabs { t1, t2 };
};
} // namespace

TEST_CASE("resolveTarget: id, label, position, suffix, ambiguity", "[vtcontrol][target]")
{
    auto const f = Fixture {};
    CHECK(resolveTarget(ObjectKind::Pane, toString(f.a.ref), f.panes) == f.a.ref);
    CHECK(resolveTarget(ObjectKind::Pane, "build", f.panes) == f.c.ref);
    auto const ambiguous = resolveTarget(ObjectKind::Pane, "LOGS", f.panes);
    REQUIRE_FALSE(ambiguous);
    CHECK(ambiguous.error().code == ControlErrorCode::Ambiguous);
    CHECK(ambiguous.error().candidates.size() == 2);
    CHECK(ambiguous.error().candidates[0] == toString(f.a.ref));
    CHECK(resolveTarget(ObjectKind::Tab, "2", f.tabs) == f.t2.ref);
    CHECK(resolveTarget(ObjectKind::Pane, "2", f.panes).error().code == ControlErrorCode::NotFound);
    CHECK(resolveTarget(ObjectKind::Pane, toString(f.c.ref.uuid).substr(30), f.panes)
          == f.c.ref); // 6-digit suffix
    CHECK(resolveTarget(ObjectKind::Pane, "abc", f.panes).error().code == ControlErrorCode::NotFound);
    CHECK(resolveTarget(ObjectKind::Pane, toString(f.t1.ref), f.panes).error().code
          == ControlErrorCode::NotFound);
}

TEST_CASE("resolveTarget: a suffix shared by two objects is ambiguous, an unknown spec names itself",
          "[vtcontrol][target]")
{
    auto f = Fixture {};
    // Make a and b share their last digits.
    f.panes[1].ref.uuid = f.panes[0].ref.uuid;
    f.panes[1].ref.uuid.bytes[0] = 0xff;
    auto const suffix = toString(f.a.ref.uuid).substr(28);
    auto const ambiguous = resolveTarget(ObjectKind::Pane, suffix, f.panes);
    REQUIRE_FALSE(ambiguous);
    CHECK(ambiguous.error().code == ControlErrorCode::Ambiguous);

    auto const missing = resolveTarget(ObjectKind::Pane, "nonesuch", f.panes);
    REQUIRE_FALSE(missing);
    CHECK(missing.error().message.contains("nonesuch"));
}

TEST_CASE("resolveTarget: an id wins over a label that spells it", "[vtcontrol][target]")
{
    auto f = Fixture {};
    f.panes[2].label = toString(f.a.ref);
    CHECK(resolveTarget(ObjectKind::Pane, toString(f.a.ref), f.panes) == f.a.ref);
}
