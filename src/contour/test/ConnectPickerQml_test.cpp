// SPDX-License-Identifier: Apache-2.0
//
// The Connect picker, offscreen: the controller over fake services, and its window as the user drives it --
// under every chrome row, as the sizes are tokens.

#include <contour/cli/ConnectList.hpp>
#include <contour/config/Config.hpp>
#include <contour/config/LaunchTarget.hpp>
#include <contour/test/InMemoryGuiStateStore.hpp>
#include <contour/test/QmlChromeStyle.hpp>
#include <contour/window/ConnectPickerController.hpp>

#include <QtCore/QCoreApplication>
#include <QtCore/QEventLoop>
#include <QtGui/QGuiApplication>
#include <QtQml/QQmlEngine>
#include <QtQuick/QQuickItem>
#include <QtQuick/QQuickWindow>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <QtTest/QSignalSpy>
#include <QtTest/QTest>

using namespace contour;

namespace
{

/// Collects the visible items named @p name under @p root in the visual tree: delegates are parented to their
/// view there, whatever their QObject parent is.
void collectNamed(QQuickItem* root, QString const& name, QList<QQuickItem*>& found)
{
    if (root->objectName() == name && root->isVisible())
        found.push_back(root);
    for (auto* const child: root->childItems())
        collectNamed(child, name, found);
}

/// Spins the event loop until @p predicate holds or three seconds have passed.
template <typename Predicate>
[[nodiscard]] bool waitUntil(Predicate const& predicate)
{
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!predicate() && std::chrono::steady_clock::now() < deadline)
        QTest::qWait(10);
    return predicate();
}

/// Types @p text into @p window, one key at a time.
void typeText(QWindow* window, std::string_view text)
{
    for (auto const character: text)
        QTest::keyClick(window, character);
}

/// The picker over fake services, in a window under one chrome row.
struct PickerFixture
{
    std::shared_ptr<std::atomic<bool>> daemonGate; ///< When set, the daemon probe waits until it holds true.
    config::Config configuration;
    test::InMemoryGuiStateStore state;
    std::vector<std::string> launched;      ///< The targets a connection was started for.
    std::vector<std::string> checked;       ///< The targets that were checked.
    std::vector<std::string> settingsPages; ///< The pages Settings was asked to show.
    std::optional<std::string> failure;     ///< What the check answers for a host.
    std::vector<window::StartPrompt> prompts;
    std::shared_ptr<std::atomic<bool>>
        checkGate; ///< When set, a check waits until it holds true. ///< What the start-up switch stored.
    QQmlEngine engine;
    std::unique_ptr<window::ConnectPickerController> picker;
    std::unique_ptr<QQuickWindow> window;

    explicit PickerFixture(config::ChromeStyle row = config::ChromeStyle::ContourLinux,
                           cli::QuickSelectKey quickKey = cli::QuickSelectKey::Alt,
                           std::shared_ptr<std::atomic<bool>> daemonProbeGate = nullptr):
        daemonGate(std::move(daemonProbeGate))
    {
        auto setup = test::defaultChromeSetup();
        setup.row = row;
        test::installChromeStyle(engine, setup);
        configuration.hosts.value()["starfive"].url = config::parseHostUrl("ssh://me@starfive").value();
        configuration.hosts.value()["build-box"].url = config::parseHostUrl("ssh://build.lan").value();
        picker = std::make_unique<window::ConnectPickerController>(window::ConnectPickerServices {
            .config = [this]() -> config::Config const& { return configuration; },
            .daemon =
                [gate = daemonGate] {
                    return [gate] {
                        while (gate && !gate->load())
                            std::this_thread::sleep_for(std::chrono::milliseconds(5));
                        return cli::LocalDaemonState { .run = cli::DaemonRunState::Running, .sessions = 3 };
                    };
                },
            .check =
                [this](config::LaunchTarget const& target) {
                    checked.push_back(config::toString(target));
                    return [answer = failure, gate = checkGate]() -> std::expected<void, std::string> {
                        while (gate && !gate->load())
                            std::this_thread::sleep_for(std::chrono::milliseconds(5));
                        if (answer)
                            return std::unexpected { *answer };
                        return {};
                    };
                },
            .launch = [this](config::LaunchTarget const& target) -> std::expected<void, std::string> {
                launched.push_back(config::toString(target));
                return {};
            },
            .showSettingsPage = [this](std::string const& page) { settingsPages.push_back(page); },
            .setStartPrompt = [this](window::StartPrompt prompt) { prompts.push_back(prompt); },
            .state = state,
            .quickKey = quickKey });
        window = window::loadConnectPickerWindow(engine, *picker);
        REQUIRE(window != nullptr);
        window->show();
        REQUIRE(QTest::qWaitForWindowExposed(window.get()));
        REQUIRE(waitUntil([this] { return field() != nullptr && field()->hasActiveFocus(); }));
        // The list builds its rows on the first frames.
        REQUIRE(waitUntil([this] { return all("connectRow").size() == picker->rowCount(); }));
        // The daemon's state arrives from the probe's worker thread.
        if (!daemonGate)
            REQUIRE(waitUntil([this] {
                return all("connectDetail").front()->property("text").toString().contains("running");
            }));
    }

    [[nodiscard]] QQuickItem* named(char const* name) const
    {
        auto const found = all(name);
        return found.isEmpty() ? nullptr : found.front();
    }

    [[nodiscard]] QQuickItem* field() const { return named("connectFilter"); }

    /// The items named @p name in the list, in row order.
    [[nodiscard]] QList<QQuickItem*> all(char const* name) const
    {
        auto found = QList<QQuickItem*> {};
        collectNamed(window->contentItem(), QString::fromLatin1(name), found);
        std::ranges::sort(found, {}, [](QQuickItem const* item) { return item->mapToScene({}).y(); });
        return found;
    }

    /// Waits until @p predicate holds.
    template <typename Predicate>
    void waitFor(Predicate const& predicate) const
    {
        REQUIRE(waitUntil(predicate));
    }
};
} // namespace

TEST_CASE("ConnectPicker: the window shows the targets under every chrome row", "[contour][connect][qml]")
{
    auto const row = GENERATE(config::ChromeStyle::MacOS,
                              config::ChromeStyle::Fluent,
                              config::ChromeStyle::ContourLinux,
                              config::ChromeStyle::Terminal);
    INFO("row " << static_cast<int>(row));
    auto fixture = PickerFixture { row };

    CHECK(fixture.picker->rowCount() == 4);
    CHECK(fixture.all("connectRow").size() == 4);
    auto const titles = fixture.all("connectTitle");
    REQUIRE(titles.size() == 4);
    CHECK(titles[0]->property("text").toString() == "Persistent sessions");
    CHECK(titles[1]->property("text").toString() == "This window only");
    CHECK(fixture.all("connectDetail")[0]->property("text").toString().contains("running · 3 sessions"));
    CHECK(fixture.all("connectGroupHeader").size() == 2);
    CHECK(fixture.all("connectTag").size() == 2);
    // The window takes its size from tokens, not from literals.
    CHECK(fixture.window->width() > 0);
    CHECK(fixture.window->height() > 0);
    CHECK(fixture.window->minimumWidth() == fixture.window->width());
}

TEST_CASE("ConnectPicker: every row shows its quick-select key, per platform", "[contour][connect][qml]")
{
    SECTION("Alt elsewhere")
    {
        auto fixture = PickerFixture { config::ChromeStyle::ContourLinux, cli::QuickSelectKey::Alt };
        auto const keys = fixture.all("connectQuick");
        REQUIRE(keys.size() == 4);
        CHECK(keys[0]->property("text").toString() == "Alt+1");
        CHECK(keys[3]->property("text").toString() == "Alt+4");
    }
    SECTION("Command on macOS")
    {
        auto fixture = PickerFixture { config::ChromeStyle::MacOS, cli::QuickSelectKey::Command };
        CHECK(fixture.all("connectQuick")[1]->property("text").toString() == "⌘2");
    }
}

TEST_CASE("ConnectPicker: Alt+digit connects at once and plain digits stay typable",
          "[contour][connect][qml]")
{
    auto fixture = PickerFixture {};

    SECTION("a plain digit is text")
    {
        typeText(fixture.window.get(), "2");
        CHECK(fixture.picker->filter() == "2");
        CHECK(fixture.launched.empty());
    }

    SECTION("Alt+2 picks the second row")
    {
        QSignalSpy const connected { fixture.picker.get(), &window::ConnectPickerController::connected };
        QTest::keyClick(fixture.window.get(), Qt::Key_2, Qt::AltModifier);
        fixture.waitFor([&] { return !fixture.launched.empty(); });
        CHECK(fixture.launched == std::vector<std::string> { "local" });
        CHECK(connected.count() == 1);
        CHECK(fixture.picker->filter().isEmpty());
    }

    SECTION("filtering renumbers: Alt+1 reaches the only match")
    {
        typeText(fixture.window.get(), "star");
        REQUIRE(fixture.picker->rowCount() == 1);
        QTest::keyClick(fixture.window.get(), Qt::Key_1, Qt::AltModifier);
        fixture.waitFor([&] { return !fixture.launched.empty(); });
        CHECK(fixture.launched == std::vector<std::string> { "starfive" });
        CHECK(fixture.checked == std::vector<std::string> { "starfive" });
    }

    SECTION("a digit past the last row does nothing")
    {
        QTest::keyClick(fixture.window.get(), Qt::Key_9, Qt::AltModifier);
        CHECK(fixture.launched.empty());
    }
}

TEST_CASE("ConnectPicker: the keyboard moves, connects and cancels", "[contour][connect][qml]")
{
    auto fixture = PickerFixture {};
    CHECK(fixture.picker->currentIndex() == 0);
    QTest::keyClick(fixture.window.get(), Qt::Key_Down);
    CHECK(fixture.picker->currentIndex() == 1);
    QTest::keyClick(fixture.window.get(), Qt::Key_Up);
    QTest::keyClick(fixture.window.get(), Qt::Key_Up);
    CHECK(fixture.picker->currentIndex() == 0);

    QTest::keyClick(fixture.window.get(), Qt::Key_Down);
    QTest::keyClick(fixture.window.get(), Qt::Key_Down);
    QTest::keyClick(fixture.window.get(), Qt::Key_Return);
    fixture.waitFor([&] { return !fixture.launched.empty(); });
    CHECK(fixture.launched == std::vector<std::string> { "build-box" });
    CHECK(fixture.state.state.lastConnectTarget == "build-box");

    QSignalSpy const cancelled { fixture.picker.get(), &window::ConnectPickerController::cancelled };
    QTest::keyClick(fixture.window.get(), Qt::Key_Escape);
    CHECK(cancelled.count() == 1);
}

TEST_CASE("ConnectPicker: the selection starts on the last used target", "[contour][connect][qml]")
{
    auto fixture = PickerFixture {};
    fixture.state.state.lastConnectTarget = "starfive";
    fixture.picker->refresh();
    CHECK(fixture.picker->currentIndex() == 3);
}

TEST_CASE("ConnectPicker: a typed address adds a leading connect row", "[contour][connect][qml]")
{
    auto fixture = PickerFixture {};
    typeText(fixture.window.get(), "ssh://me@pi4.lan");
    REQUIRE(fixture.picker->rowCount() >= 1);
    auto const titles = fixture.all("connectTitle");
    REQUIRE_FALSE(titles.isEmpty());
    CHECK(titles[0]->property("text").toString() == "Connect to ssh://me@pi4.lan");
    CHECK(fixture.all("connectDetail")[0]->property("text").toString() == "Not saved");

    QTest::keyClick(fixture.window.get(), Qt::Key_Return);
    fixture.waitFor([&] { return !fixture.launched.empty(); });
    CHECK(fixture.launched == std::vector<std::string> { "ssh://me@pi4.lan" });
}

TEST_CASE("ConnectPicker: a failed connection stays under its row with Retry and Edit Host…",
          "[contour][connect][qml]")
{
    auto fixture = PickerFixture {};
    fixture.failure = "Contour isn't installed on starfive (contour: command not found).";
    typeText(fixture.window.get(), "star");
    QTest::keyClick(fixture.window.get(), Qt::Key_Return);
    fixture.waitFor([&] { return !fixture.picker->failure().isEmpty(); });

    CHECK(fixture.launched.empty());
    CHECK(fixture.picker->failedTarget() == "starfive");
    CHECK(fixture.picker->failureCanEditHost());
    fixture.waitFor([&] { return !fixture.all("connectFailureText").isEmpty(); });
    auto const text = fixture.all("connectFailureText");
    REQUIRE(text.size() == 1);
    CHECK(text[0]->property("text").toString().contains("Contour isn't installed on starfive"));
    CHECK(fixture.named("connectRetry") != nullptr);
    CHECK(fixture.named("connectEditHost") != nullptr);
    CHECK(fixture.window->isVisible());

    SECTION("Retry connects again, and a host that answers now is launched")
    {
        fixture.failure.reset();
        QMetaObject::invokeMethod(fixture.named("connectRetry"), "clicked");
        fixture.waitFor([&] { return !fixture.launched.empty(); });
        CHECK(fixture.launched == std::vector<std::string> { "starfive" });
        CHECK(fixture.picker->failure().isEmpty());
    }

    SECTION("Edit Host… opens Settings on that host")
    {
        QMetaObject::invokeMethod(fixture.named("connectEditHost"), "clicked");
        CHECK(fixture.settingsPages == std::vector<std::string> { "host:starfive" });
    }
}

TEST_CASE("ConnectPicker: the footer manages hosts and stores the start-up switch", "[contour][connect][qml]")
{
    auto fixture = PickerFixture {};
    QMetaObject::invokeMethod(fixture.named("connectManageHosts"), "clicked");
    CHECK(fixture.settingsPages == std::vector<std::string> { "section:hosts" });

    CHECK_FALSE(fixture.picker->askEveryTime());
    QMetaObject::invokeMethod(fixture.named("connectAskSwitch"), "toggle");
    CHECK(fixture.prompts == std::vector<window::StartPrompt> { window::StartPrompt::Ask });
}

TEST_CASE("ConnectPicker: Return on the first row connects to the daemon", "[contour][connect][qml]")
{
    auto fixture = PickerFixture {};
    QTest::keyClick(fixture.window.get(), Qt::Key_Return);
    fixture.waitFor([&] { return !fixture.launched.empty(); });
    CHECK(fixture.launched == std::vector<std::string> { "daemon" });
}

TEST_CASE("ConnectPicker: cancelling a check in flight starts nothing", "[contour][connect][qml]")
{
    auto release = std::make_shared<std::atomic<bool>>(false);
    // A host whose check waits until released, as a slow ssh does.
    auto slow = PickerFixture {};
    slow.checkGate = release;
    typeText(slow.window.get(), "star");
    QTest::keyClick(slow.window.get(), Qt::Key_Return);
    slow.waitFor([&] { return slow.picker->isBusy(); });
    slow.picker->cancel();
    CHECK_FALSE(slow.picker->isBusy());
    release->store(true);
    QTest::qWait(200);
    CHECK(slow.launched.empty());
}

TEST_CASE("ConnectPicker: the start-up switch moves when it is toggled", "[contour][connect][qml]")
{
    auto fixture = PickerFixture {};
    auto* const toggle = fixture.named("connectAskSwitch");
    REQUIRE(toggle != nullptr);
    CHECK_FALSE(toggle->property("checked").toBool());
    QMetaObject::invokeMethod(toggle, "toggle");
    CHECK(toggle->property("checked").toBool());
    QMetaObject::invokeMethod(toggle, "toggle");
    CHECK_FALSE(toggle->property("checked").toBool());
}

TEST_CASE("ConnectPicker: a daemon that never answers does not hold the picker", "[contour][connect][qml]")
{
    auto release = std::make_shared<std::atomic<bool>>(false);
    auto fixture = PickerFixture { config::ChromeStyle::ContourLinux, cli::QuickSelectKey::Alt, release };
    // The picker is up, typable and says it is checking, while the probe is still asking.
    CHECK(fixture.all("connectDetail").front()->property("text").toString().contains("checking"));
    typeText(fixture.window.get(), "star");
    CHECK(fixture.picker->rowCount() == 1);
    fixture.picker->setFilter("");
    release->store(true);
    fixture.waitFor([&] {
        return fixture.all("connectDetail")
            .front()
            ->property("text")
            .toString()
            .contains("running · 3 sessions");
    });
}
