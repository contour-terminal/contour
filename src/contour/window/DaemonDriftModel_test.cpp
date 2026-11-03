// SPDX-License-Identifier: Apache-2.0
#include <contour/window/DaemonDriftModel.hpp>
#include <contour/window/WindowLiveness.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>

#include <QtTest/QSignalSpy>

using vthost::client::LayoutAfterRestart;

TEST_CASE("the drift model is hidden until a drift is shown", "[contour][drift]")
{
    auto model = contour::window::DaemonDriftModel {};
    CHECK_FALSE(model.isVisible());
    model.show(vthost::client::assessDrift("0.8.2", "0.8.1", LayoutAfterRestart::Lost));
    CHECK(model.isVisible());
    CHECK(model.text() == QStringLiteral("daemon 0.8.1 is older than this client"));
    CHECK(model.actionLabel() == QStringLiteral("Restart daemon on the new version"));
    CHECK_FALSE(model.confirmation().isEmpty());
}

TEST_CASE("matching releases hide the drift again", "[contour][drift]")
{
    auto model = contour::window::DaemonDriftModel {};
    model.show(vthost::client::assessDrift("0.8.2", "0.8.1", LayoutAfterRestart::Lost));
    model.show(vthost::client::assessDrift("0.8.2", "0.8.2", LayoutAfterRestart::Lost));
    CHECK_FALSE(model.isVisible());
}

TEST_CASE("a restart is requested once until it fails", "[contour][drift]")
{
    auto model = contour::window::DaemonDriftModel {};
    model.show(vthost::client::assessDrift("0.8.2", "0.8.1", LayoutAfterRestart::Lost));
    auto spy = QSignalSpy { &model, &contour::window::DaemonDriftModel::restartRequested };
    model.requestRestart();
    model.requestRestart();
    CHECK(spy.count() == 1);
    CHECK(model.isRestarting());
    CHECK(model.text() == QStringLiteral("Restarting daemon…"));
    model.noteRestartFailed(QStringLiteral("daemon did not start"));
    CHECK_FALSE(model.isRestarting());
    CHECK(model.text().startsWith(QStringLiteral("Restart failed: ")));
    CHECK(model.text().endsWith(QStringLiteral("daemon did not start")));
    CHECK(model.isVisible());

    model.requestRestart();
    CHECK(spy.count() == 2);
}

TEST_CASE("an upgrade is requested once, and its failure leaves the restart on offer", "[contour][drift]")
{
    using contour::window::DriftOffer;
    auto model = contour::window::DaemonDriftModel {};
    model.show(vthost::client::assessDrift("0.8.2", "0.8.1", LayoutAfterRestart::Lost),
               DriftOffer::UpgradeAndRestart);
    CHECK(model.offersUpgrade());
    CHECK(model.upgradeActionLabel() == QStringLiteral("Upgrade daemon (keeps your programs running)"));
    auto spy = QSignalSpy { &model, &contour::window::DaemonDriftModel::upgradeRequested };
    model.requestUpgrade();
    model.requestUpgrade();
    model.requestRestart(); // not while the daemon upgrades
    CHECK(spy.count() == 1);
    CHECK(model.isUpgrading());
    CHECK_FALSE(model.isRestarting());
    CHECK(model.text() == QStringLiteral("Upgrading daemon…"));

    model.noteUpgradeFailed(QStringLiteral("the daemon runs in a sandbox"));
    CHECK_FALSE(model.isUpgrading());
    CHECK_FALSE(model.offersUpgrade());
    CHECK(model.isVisible());
    CHECK(model.text().startsWith(QStringLiteral("Upgrade failed: the daemon runs in a sandbox")));
    model.requestUpgrade(); // the refusal stands: only the restart is left
    CHECK(spy.count() == 1);
}

TEST_CASE("no upgrade is requested where none is offered", "[contour][drift]")
{
    auto model = contour::window::DaemonDriftModel {};
    model.show(vthost::client::assessDrift("0.8.2", "0.8.1", LayoutAfterRestart::Lost));
    CHECK_FALSE(model.offersUpgrade());
    auto spy = QSignalSpy { &model, &contour::window::DaemonDriftModel::upgradeRequested };
    model.requestUpgrade();
    CHECK(spy.count() == 0);
}

TEST_CASE("a daemon another client upgrades reads as upgrading until the drift is shown anew",
          "[contour][drift]")
{
    auto model = contour::window::DaemonDriftModel {};
    model.noteDaemonUpgrading();
    CHECK(model.isVisible());
    CHECK(model.text() == QStringLiteral("Upgrading daemon…"));
    model.show(vthost::client::assessDrift("0.8.2", "0.8.2", LayoutAfterRestart::Lost));
    CHECK_FALSE(model.isUpgrading());
    CHECK_FALSE(model.isVisible());
}

TEST_CASE("a daemon that does not return from an upgrade clears the upgrading line", "[contour][drift]")
{
    auto model = contour::window::DaemonDriftModel {};
    model.show(vthost::client::assessDrift("0.8.2", "0.8.1", LayoutAfterRestart::Lost));
    model.noteDaemonUpgrading();
    REQUIRE(model.isUpgrading());
    model.noteUpgradeEnded();
    CHECK_FALSE(model.isUpgrading());
    CHECK(model.text() == QStringLiteral("daemon 0.8.1 is older than this client"));
}

TEST_CASE("the same drift assessed again keeps the failure of the upgrade", "[contour][drift]")
{
    using contour::window::DriftOffer;
    auto model = contour::window::DaemonDriftModel {};
    auto const drift = [] {
        return vthost::client::assessDrift("0.8.2", "0.8.1", LayoutAfterRestart::Lost);
    };
    model.show(drift(), DriftOffer::UpgradeAndRestart);
    model.requestUpgrade();
    model.noteUpgradeFailed(QStringLiteral("a program is mid-sequence"));
    // The connection came back to the daemon that refused.
    model.show(drift(), DriftOffer::UpgradeAndRestart);
    CHECK_FALSE(model.offersUpgrade());
    CHECK(model.text().contains(QStringLiteral("a program is mid-sequence")));
    // A release that changed is a new situation.
    model.show(vthost::client::assessDrift("0.8.2", "0.8.0", LayoutAfterRestart::Lost),
               DriftOffer::UpgradeAndRestart);
    CHECK(model.offersUpgrade());
    CHECK_FALSE(model.text().contains(QStringLiteral("a program is mid-sequence")));
}

TEST_CASE("only a visible, non-transient window keeps the app alive", "[contour][drift]")
{
    using namespace contour::window;
    using enum WindowVisibility;
    using enum WindowTransience;
    CHECK_FALSE(anyWindowKeepsAppAlive({}));
    auto const closedButAlive = std::array { WindowFacts { Hidden, Independent } };
    CHECK_FALSE(anyWindowKeepsAppAlive(closedButAlive));
    auto const onlyDialog =
        std::array { WindowFacts { Hidden, Independent }, WindowFacts { Shown, Transient } };
    CHECK_FALSE(anyWindowKeepsAppAlive(onlyDialog));
    auto const oneShown =
        std::array { WindowFacts { Hidden, Independent }, WindowFacts { Shown, Independent } };
    CHECK(anyWindowKeepsAppAlive(oneShown));
}

TEST_CASE("an upgrade request stays in flight across the reconnect until its worker reports back",
          "[contour][drift]")
{
    // The daemon refused as it quiesced: the connection came back to the same daemon before the request
    // settled how the upgrade ended. Clicking again meanwhile would ask a second worker while the first runs.
    using contour::window::DriftOffer;
    auto model = contour::window::DaemonDriftModel {};
    auto const drift = [] {
        return vthost::client::assessDrift("0.8.2", "0.8.1", LayoutAfterRestart::Lost);
    };
    model.show(drift(), DriftOffer::UpgradeAndRestart);
    auto upgrades = QSignalSpy { &model, &contour::window::DaemonDriftModel::upgradeRequested };
    auto restarts = QSignalSpy { &model, &contour::window::DaemonDriftModel::restartRequested };
    model.requestUpgrade();
    REQUIRE(upgrades.count() == 1);

    model.show(drift(), DriftOffer::UpgradeAndRestart); // the connection is back
    CHECK_FALSE(model.isUpgrading());
    CHECK_FALSE(model.offersUpgrade());
    model.requestUpgrade();
    model.requestRestart();
    CHECK(upgrades.count() == 1);
    CHECK(restarts.count() == 0);

    SECTION("it failed: the reason is shown, the restart offered")
    {
        model.noteUpgradeFailed(QStringLiteral("the daemon's clients and sessions did not settle in time"));
        CHECK(model.text().contains(QStringLiteral("did not settle in time")));
        model.requestRestart();
        CHECK(restarts.count() == 1);
    }
    SECTION("it succeeded: another may be asked for")
    {
        model.noteUpgradeRequestDone();
        CHECK(model.offersUpgrade());
        model.requestUpgrade();
        CHECK(upgrades.count() == 2);
    }
}

TEST_CASE("the drift follows a connection through an upgrade, with no drift of its own", "[contour][drift]")
{
    // A remote daemon: nothing to offer, so nothing shown -- but its windows wait while it upgrades.
    using contour::window::DriftOffer;
    using contour::window::DriftReassessment;
    using vthost::client::ConnectionState;
    auto model = contour::window::DaemonDriftModel {};
    model.show(vthost::client::DriftNotice {}, DriftOffer::None);
    CHECK_FALSE(model.isVisible());

    CHECK(model.followConnection(ConnectionState::Upgrading) == DriftReassessment::Keep);
    CHECK(model.isVisible());
    CHECK(model.text() == QStringLiteral("Upgrading daemon…"));

    SECTION("the daemon comes back: the drift is assessed anew")
    {
        CHECK(model.followConnection(ConnectionState::Connected) == DriftReassessment::Reassess);
        model.show(vthost::client::DriftNotice {}, DriftOffer::None);
        CHECK_FALSE(model.isVisible());
        CHECK(model.followConnection(ConnectionState::Connected) == DriftReassessment::Keep);
    }
    SECTION("the connection is lost instead: the line goes")
    {
        CHECK(model.followConnection(ConnectionState::Failed) == DriftReassessment::Keep);
        CHECK_FALSE(model.isVisible());
    }
}
