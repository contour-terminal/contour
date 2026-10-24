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
