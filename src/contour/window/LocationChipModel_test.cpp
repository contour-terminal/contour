// SPDX-License-Identifier: Apache-2.0
#include <contour/window/LocationChipModel.hpp>
#include <contour/window/LocationChipQt.hpp>

#include <QtCore/QVariantMap>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
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

TEST_CASE("LocationChipModel: an unmeasured chip falls to its least step, never to the full one",
          "[LocationChipModel]")
{
    constexpr auto Least = static_cast<int>(LocationChipFit::WorkspaceElided);
    // Nothing measured, no room: not the full step (a missing width must not read as zero).
    CHECK(LocationChipModel::fitFor({}) == Least);
    // Room but no widths: no step has a width, so none fits.
    CHECK(LocationChipModel::fitFor(QVariantMap { { "available", 500 } }) == Least);
    // Widths but no room: a missing `available` is no room.
    CHECK(LocationChipModel::fitFor(QVariantMap { { "full", 300 }, { "workspaceElided", 80 } }) == Least);
    // A step QML did not measure is skipped, the ones it did measure still win.
    CHECK(LocationChipModel::fitFor(QVariantMap { { "dotOnly", 110 }, { "available", 120 } })
          == static_cast<int>(LocationChipFit::DotOnly));
    // The daemon-only control's least step keeps its name.
    CHECK(LocationChipModel::fitForDaemonOnly({}) == static_cast<int>(LocationChipFit::DaemonElided));
}

TEST_CASE("measuredStepWidth: an unmeasured step does not fit, for the chip and the band allocation alike",
          "[LocationChipModel]")
{
    // The one rule both LocationChipModel::fitFor and UiStyleProvider::allocateBand read widths by.
    auto const widths = QVariantMap { { "full", 300 }, { "dotOnly", 0 } };
    CHECK(contour::window::measuredStepWidth(widths, "full") == 300.0);
    CHECK(contour::window::measuredStepWidth(widths, "dotOnly") == 0.0);
    CHECK(std::isinf(contour::window::measuredStepWidth(widths, "noStateText")));
}

TEST_CASE("LocationChipModel: a narrow daemon-only row still shows a local daemon's name",
          "[LocationChipModel]")
{
    auto inputs = remoteDaemon();
    inputs.daemon = "local";
    inputs.locality = DaemonLocality::Local;
    auto const model = LocationChipModel { inputs };
    auto const widths = QVariantMap { { "full", 200 },   { "noStateText", 150 },    { "daemonElided", 120 },
                                      { "dotOnly", 40 }, { "workspaceElided", 30 }, { "available", 50 } };
    CHECK(LocationChipModel::fitFor(widths) == static_cast<int>(LocationChipFit::DotOnly));
    auto const fit = LocationChipModel::fitForDaemonOnly(widths);
    CHECK(fit == static_cast<int>(LocationChipFit::DaemonElided));
    CHECK(stepText(model, static_cast<LocationChipFit>(fit), "daemon") == "local");
}

TEST_CASE("LocationChipModel: a host that is no daemon is named, with no workspace segment",
          "[LocationChipModel]")
{
    auto inputs = remoteDaemon();
    inputs.hosting = DaemonHosting::OtherHost;
    inputs.daemon = "tmuxhost";
    inputs.state = HostChipState::Reconnecting;
    inputs.centre = BarCentre::Tabs;
    auto model = LocationChipModel { inputs };
    CHECK_FALSE(model.workspaceShown());
    CHECK(model.place() == "band");
    CHECK(model.dotShown());
    CHECK(stepText(model, LocationChipFit::Full, "daemon") == "tmuxhost");
    CHECK(stepText(model, LocationChipFit::Full, "status") == "Reconnecting…");

    // With no label to name, there is no chip.
    inputs.daemon.clear();
    model.setInputs(inputs);
    CHECK(model.place() == "none");

    // A rebind that moves no label or state still tells what serves the window, and where the chip shows.
    inputs = remoteDaemon();
    model.setInputs(inputs);
    auto surface = QSignalSpy { &model, &LocationChipModel::surfaceChanged };
    inputs.hosting = DaemonHosting::OtherHost;
    model.setInputs(inputs);
    CHECK(surface.count() == 1);
    CHECK_FALSE(model.workspaceShown());
}
