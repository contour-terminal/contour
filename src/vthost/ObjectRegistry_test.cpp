// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <memory>

#include <vtcontrol/testing/SequentialUuidSource.hpp>
#include <vthost/ObjectRegistry.hpp>

namespace
{
using vtcontrol::ObjectKind;
using vtcontrol::ObjectRef;
using vtworkspace::SessionId;
using vtworkspace::TabId;
using vtworkspace::WindowId;

[[nodiscard]] vthost::ObjectRegistry makeRegistry()
{
    return vthost::ObjectRegistry { std::make_shared<vtcontrol::testing::SequentialUuidSource>() };
}
} // namespace

TEST_CASE("ObjectRegistry mints distinct, increasing uuids", "[vthost][ids]")
{
    auto registry = makeRegistry();
    auto const a = registry.mint();
    auto const b = registry.mint();
    CHECK(a < b);
}

TEST_CASE("ObjectRegistry maps ids to uuids and back, per kind", "[vthost][ids]")
{
    auto registry = makeRegistry();
    auto const window = registry.mint();
    auto const tab = registry.mint();
    auto const pane = registry.mint();
    registry.bind(WindowId { 1 }, window);
    registry.bind(TabId { 1 }, tab);
    registry.bind(SessionId { 1 }, pane);

    // The same small integer in three kinds names three objects.
    CHECK(registry.uuidOf(WindowId { 1 }) == window);
    CHECK(registry.uuidOf(TabId { 1 }) == tab);
    CHECK(registry.uuidOf(SessionId { 1 }) == pane);
    CHECK_FALSE(registry.uuidOf(SessionId { 2 }));

    auto const found = registry.find(ObjectRef { ObjectKind::Pane, pane });
    REQUIRE(found);
    CHECK(std::get<SessionId>(*found) == SessionId { 1 });
    CHECK(std::get<TabId>(*registry.find(ObjectRef { ObjectKind::Tab, tab })) == TabId { 1 });
    CHECK(std::get<WindowId>(*registry.find(ObjectRef { ObjectKind::Window, window })) == WindowId { 1 });

    // A uuid of one kind is not found under another.
    CHECK_FALSE(registry.find(ObjectRef { ObjectKind::Tab, pane }));
}

TEST_CASE("ObjectRegistry forgets an object with its label", "[vthost][ids]")
{
    auto registry = makeRegistry();
    auto const pane = registry.mint();
    auto const ref = ObjectRef { ObjectKind::Pane, pane };
    registry.bind(SessionId { 7 }, pane);
    registry.setLabel(ref, "logs");
    REQUIRE(registry.labelOf(ref) == "logs");

    registry.forget(SessionId { 7 });

    CHECK_FALSE(registry.uuidOf(SessionId { 7 }));
    CHECK_FALSE(registry.find(ref));
    CHECK(registry.labelOf(ref).empty());
    registry.forget(SessionId { 7 }); // a second time is a no-op
}

TEST_CASE("ObjectRegistry labels: set, replace, clear, and ignore the unknown", "[vthost][ids]")
{
    auto registry = makeRegistry();
    auto const tab = registry.mint();
    auto const ref = ObjectRef { ObjectKind::Tab, tab };
    registry.bind(TabId { 3 }, tab);

    CHECK(registry.labelOf(ref).empty());
    registry.setLabel(ref, "build");
    CHECK(registry.labelOf(ref) == "build");
    registry.setLabel(ref, "test");
    CHECK(registry.labelOf(ref) == "test");
    registry.setLabel(ref, "");
    CHECK(registry.labelOf(ref).empty());

    auto const stranger = ObjectRef { ObjectKind::Tab, registry.mint() };
    registry.setLabel(stranger, "ghost");
    CHECK(registry.labelOf(stranger).empty());
}

TEST_CASE("ObjectRegistry rebinding replaces", "[vthost][ids]")
{
    auto registry = makeRegistry();
    auto const first = registry.mint();
    auto const second = registry.mint();
    registry.bind(SessionId { 1 }, first);
    registry.bind(SessionId { 1 }, second);
    CHECK(registry.uuidOf(SessionId { 1 }) == second);
    CHECK_FALSE(registry.find(ObjectRef { ObjectKind::Pane, first }));
    CHECK(registry.find(ObjectRef { ObjectKind::Pane, second }));
}
