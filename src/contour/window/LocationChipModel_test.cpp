// SPDX-License-Identifier: Apache-2.0
#include <contour/window/LocationChipModel.hpp>

#include <QtCore/QVariantMap>

#include <catch2/catch_test_macros.hpp>

#include <utility>

#include <QtTest/QSignalSpy>

using namespace contour::window;

namespace
{

/// @return A remote daemon's chip, connected, on a band that holds the chip.
[[nodiscard]] LocationChipInputs remoteDaemon()
{
    return LocationChipInputs { .daemon = "starfive",
                                .hosting = DaemonHosting::Daemon,
                                .locality = DaemonLocality::Remote,
                                .state = HostChipState::Connected,
                                .workspace = "billing",
                                .band = BarPresence::Shown,
                                .centre = BarCentre::Tabs };
}

/// @param model The chip.
/// @param fit   A step.
/// @return The text of @p model's step @p fit named @p field.
[[nodiscard]] QString stepText(LocationChipModel const& model, LocationChipFit fit, QString const& field)
{
    return model.steps().at(static_cast<qsizetype>(fit)).toMap().value(field).toString();
}

} // namespace

TEST_CASE("LocationChipModel: the surface follows the inputs", "[LocationChipModel]")
{
    auto const model = LocationChipModel { remoteDaemon() };
    CHECK(model.place() == "band");
    CHECK(model.daemonName() == "starfive");
    CHECK(model.dotShown());
    CHECK(model.stateGlyph() == "●");
    CHECK(model.accessibleName() == "Workspace billing on starfive");
    CHECK(model.accessibleDescription() == "collapsed");
    CHECK(model.daemonAccessibleName() == "Daemon starfive");
    CHECK(model.steps().size() == static_cast<qsizetype>(LocationChipSteps.size()));
    CHECK(stepText(model, LocationChipFit::Full, "workspace") == "billing");
    CHECK(stepText(model, LocationChipFit::DotOnly, "daemon").isEmpty());
}

TEST_CASE("LocationChipModel: an in-process window names no daemon and shows no dot", "[LocationChipModel]")
{
    auto inputs = remoteDaemon();
    inputs.daemon.clear();
    inputs.hosting = DaemonHosting::InProcess;
    inputs.state = HostChipState::Disconnected;
    auto const model = LocationChipModel { inputs };
    CHECK(model.daemonName().isEmpty());
    CHECK_FALSE(model.dotShown());
    CHECK(model.place() == "none");
    CHECK(stepText(model, LocationChipFit::Full, "tooltip") == "Switch workspace");
}

TEST_CASE("LocationChipModel: each group of readers hears only what they read", "[LocationChipModel]")
{
    auto model = LocationChipModel { remoteDaemon() };
    auto place = QSignalSpy { &model, &LocationChipModel::placeChanged };
    auto surface = QSignalSpy { &model, &LocationChipModel::surfaceChanged };
    auto accessibility = QSignalSpy { &model, &LocationChipModel::accessibilityChanged };

    SECTION("the same inputs change nothing")
    {
        model.setInputs(remoteDaemon());
        CHECK(place.count() + surface.count() + accessibility.count() == 0);
    }

    SECTION("another workspace's urgency reaches the accessible words alone")
    {
        auto inputs = remoteDaemon();
        inputs.otherWorkspaces = Urgency::NeedsYou;
        model.setInputs(inputs);
        CHECK(accessibility.count() == 1);
        CHECK(surface.count() == 0);
        CHECK(place.count() == 0);
        CHECK(model.accessibleDescription() == "Another workspace needs you, collapsed");
    }

    SECTION("the switcher opening reaches the accessible words alone")
    {
        auto inputs = remoteDaemon();
        inputs.disclosure = Disclosure::Expanded;
        model.setInputs(inputs);
        CHECK(accessibility.count() == 1);
        CHECK(surface.count() == 0);
        CHECK(model.accessibleDescription() == "expanded");
    }

    SECTION("the sidebar's visibility reaches the place alone, and only when it moves the chip")
    {
        auto inputs = remoteDaemon();
        inputs.sidebar = SidebarShown::Yes; // the band still holds the tabs: the chip stays where it was
        model.setInputs(inputs);
        CHECK(place.count() == 0);
        inputs.centre = BarCentre::Breadcrumb;
        model.setInputs(inputs);
        CHECK(place.count() == 1);
        CHECK(model.place() == "sidebar");
        CHECK(surface.count() == 0);
    }

    SECTION("the connection reaches the surface and the words")
    {
        auto inputs = remoteDaemon();
        inputs.state = HostChipState::Reconnecting;
        model.setInputs(inputs);
        CHECK(surface.count() == 1);
        CHECK(accessibility.count() == 1);
        CHECK(model.stateGlyph() == "◑");
        CHECK(stepText(model, LocationChipFit::Full, "status") == "Reconnecting…");
        CHECK(stepText(model, LocationChipFit::NoStateText, "status").isEmpty());
    }

    SECTION("the workspace's name reaches the surface and the words")
    {
        auto inputs = remoteDaemon();
        inputs.workspace = "billing-and-invoices";
        model.setInputs(inputs);
        CHECK(surface.count() == 1);
        CHECK(stepText(model, LocationChipFit::WorkspaceElided, "workspace") == "billin…");
    }
}

TEST_CASE("LocationChipModel: fitFor takes the first step the room admits", "[LocationChipModel]")
{
    auto const widths = [](double available) {
        return QVariantMap { { "full", 300 },    { "noStateText", 220 },    { "daemonElided", 160 },
                             { "dotOnly", 110 }, { "workspaceElided", 80 }, { "available", available } };
    };
    CHECK(LocationChipModel::fitFor(widths(400)) == static_cast<int>(LocationChipFit::Full));
    CHECK(LocationChipModel::fitFor(widths(250)) == static_cast<int>(LocationChipFit::NoStateText));
    CHECK(LocationChipModel::fitFor(widths(120)) == static_cast<int>(LocationChipFit::DotOnly));
    CHECK(LocationChipModel::fitFor(widths(10)) == static_cast<int>(LocationChipFit::WorkspaceElided));
}
