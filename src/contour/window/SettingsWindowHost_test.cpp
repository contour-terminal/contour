// SPDX-License-Identifier: Apache-2.0
//
// The Settings window and its host, offscreen: the real SettingsWindow.qml over a real SettingsController,
// shown, hidden and shown again as the app's entry points will ask for it.

#include <contour/config/ChromeTokenGroups.hpp>
#include <contour/config/UiStyle.hpp>
#include <contour/test/QmlChromeStyle.hpp>
#include <contour/test/QmlIncubation.hpp>
#include <contour/test/QmlMessageCapture.hpp>
#include <contour/test/SettingsFixtures.hpp>
#include <contour/window/SettingsController.hpp>
#include <contour/window/SettingsWindowHost.hpp>
#include <contour/window/SettingsWindowRules.hpp>
#include <contour/window/UiStyleProvider.hpp>
#include <contour/window/WindowLivenessQt.hpp>

#include <QtCore/QCoreApplication>
#include <QtGui/QGuiApplication>
#include <QtGui/QKeySequence>
#include <QtGui/QPalette>
#include <QtGui/QScreen>
#include <QtQml/QQmlContext>
#include <QtQml/QQmlEngine>
#include <QtQuick/QQuickItem>
#include <QtQuick/QQuickWindow>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <expected>
#include <format>
#include <memory>
#include <string_view>

#include <QtTest/QSignalSpy>
#include <QtTest/QTest>

using namespace contour;
using contour::test::ControllerFixture;
using contour::window::SettingsWindowError;
using contour::window::SettingsWindowHost;

namespace
{

constexpr auto OneProfile =
    std::string_view { "default_profile: main\nprofiles:\n    main:\n        show_title_bar: true\n" };

/// A Settings window host over a controller of its own, on an engine carrying the chrome tokens.
struct HostFixture: ControllerFixture
{
    QQmlEngine engine;
    test::IncubationDriver incubation;
    int factoryCalls = 0; ///< How often the host asked for its window to be built.
    SettingsWindowHost host;

    /// @param conventions The conventions the host presents the window with.
    /// @param style       The chrome style the engine's tokens resolve.
    explicit HostFixture(window::SettingsWindowConventions conventions =
                             window::settingsWindowConventionsFor(config::HostPlatform::Other),
                         config::UiStyle style = config::UiStyle::Native):
        ControllerFixture { OneProfile, nullptr },
        host { *controller, conventions, [this](SettingsWindowHost& self) {
                  ++factoryCalls;
                  return window::loadSettingsWindow(engine, self);
              } }
    {
        test::installChromeStyle(engine, style);
        engine.setIncubationController(&incubation);
    }

    /// Shows the window, which the case cannot go on without.
    /// @return The window.
    QQuickWindow& show()
    {
        REQUIRE(host.show(nullptr).has_value());
        incubation.settle();
        REQUIRE(host.window() != nullptr);
        return *host.window();
    }

    /// @return The settings page inside the window.
    [[nodiscard]] QQuickItem& page() const
    {
        REQUIRE(host.window() != nullptr);
        auto* item = host.window()->findChild<QQuickItem*>("settingsPage");
        REQUIRE(item != nullptr);
        return *item;
    }

    /// Opens a new profile draft and edits its first scalar field to a value it did not hold.
    /// @return The key of the edited field.
    [[nodiscard]] QString draftAProfileField()
    {
        auto* newProfile = page().findChild<QObject*>("newProfileButton");
        REQUIRE(newProfile != nullptr);
        QMetaObject::invokeMethod(newProfile, "clicked");
        incubation.settle();

        for (auto const& raw: controller->profileFields())
        {
            auto const row = raw.toMap();
            auto const type = row.value("type").toString();
            auto value = QVariant {};
            if (type == "string")
                value = row.value("value").toString() + QStringLiteral("x");
            else if (type == "int")
                value = row.value("value").toInt() + 1;
            else
                continue;
            auto const key = row.value("key").toString();
            controller->setProfileField(key, value);
            return key;
        }
        FAIL("the profile draft offers no scalar field");
        return {};
    }

    /// @param key A field of the profile draft.
    /// @return Its value in the draft, or a null variant when the draft has no such field.
    [[nodiscard]] QVariant profileFieldValue(QString const& key) const
    {
        for (auto const& raw: controller->profileFields())
            if (raw.toMap().value("key").toString() == key)
                return raw.toMap().value("value");
        return {};
    }

    /// @return The provider the engine publishes as `chromeStyle`.
    [[nodiscard]] window::UiStyleProvider& provider() const
    {
        auto* provider = engine.rootContext()
                             ->contextProperty(QStringLiteral("chromeStyle"))
                             .value<window::UiStyleProvider*>();
        REQUIRE(provider != nullptr);
        return *provider;
    }
};

/// @return The size the window asks for, as its `preferredSize` property gives it.
[[nodiscard]] QSizeF preferredSizeOf(QQuickWindow const& window)
{
    return window.property("preferredSize").toSizeF();
}

} // namespace

TEST_CASE("the Settings window opens once, however often it is asked for",
          "[contour][gui][qml][settingswindow][host]")
{
    contour::test::QmlMessageCapture const warnings;
    auto fx = HostFixture {};

    auto& first = fx.show();
    auto& second = fx.show();
    CHECK(fx.factoryCalls == 1);
    CHECK(&first == &second);
    CHECK(second.isVisible());
    CHECK(second.color() == QGuiApplication::palette().color(QPalette::Active, QPalette::Window));

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("the Settings window comes back in front", "[contour][gui][qml][settingswindow][host]")
{
    contour::test::QmlMessageCapture const warnings;
    auto fx = HostFixture {};
    auto& settings = fx.show();

    SECTION("over another window")
    {
        auto other = QQuickWindow {};
        other.show();
        other.requestActivate();
        REQUIRE(QTest::qWaitForWindowActive(&other));

        REQUIRE(fx.host.show(nullptr).has_value());
        CHECK(QTest::qWaitForWindowActive(&settings));
    }

    SECTION("out of the minimised state, and still maximised")
    {
        // Maximised beneath the minimised state, so a show() that went through showNormal() would fail.
        settings.setWindowStates(Qt::WindowMaximized | Qt::WindowMinimized);
        REQUIRE(fx.host.show(nullptr).has_value());
        CHECK_FALSE(settings.windowStates().testFlag(Qt::WindowMinimized));
        CHECK(settings.windowStates().testFlag(Qt::WindowMaximized));
    }

    SECTION("still maximised")
    {
        settings.setWindowStates(Qt::WindowMaximized);
        REQUIRE(fx.host.show(nullptr).has_value());
        CHECK(settings.windowStates() == Qt::WindowMaximized);
    }

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("the Settings window is placed on first show", "[contour][gui][qml][settingswindow][host]")
{
    contour::test::QmlMessageCapture const warnings;
    auto fx = HostFixture {};
    auto& settings = fx.show();

    auto const anchor = QGuiApplication::primaryScreen()->availableGeometry();
    auto const expected = window::settingsWindowGeometry({ .preferred = preferredSizeOf(settings).toSize(),
                                                           .minimum = settings.minimumSize(),
                                                           .previous = std::nullopt,
                                                           .anchor = anchor,
                                                           .screens = { anchor },
                                                           .frame = settings.frameMargins() });
    CHECK(settings.geometry() == expected);

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("a chrome republish keeps the Settings window's size", "[contour][gui][qml][settingswindow][host]")
{
    contour::test::QmlMessageCapture const warnings;
    auto fx = HostFixture {};
    auto& settings = fx.show();
    auto const userSize = QSize(750, 500);
    settings.resize(userSize);

    // As ChromeStyleHost republishes it: a new provider, under the same name.
    auto* republished = new window::UiStyleProvider(fx.provider().inputs(), &fx.engine);
    fx.engine.rootContext()->setContextProperty(QStringLiteral("chromeStyle"), republished);
    QCoreApplication::processEvents();

    CHECK(settings.size() == userSize);

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("the Settings window takes the keyboard focus", "[contour][gui][qml][settingswindow][host]")
{
    contour::test::QmlMessageCapture const warnings;
    auto fx = HostFixture {};
    auto& settings = fx.show();

    REQUIRE(QTest::qWaitForWindowActive(&settings));
    CHECK(fx.page().hasActiveFocus());

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("the Close shortcut closes the Settings window", "[contour][gui][qml][settingswindow][host]")
{
    contour::test::QmlMessageCapture const warnings;
    auto fx = HostFixture {};
    auto& settings = fx.show();

    REQUIRE(QTest::qWaitForWindowActive(fx.host.window()));
    QTest::keySequence(&settings, QKeySequence::Close);
    CHECK_FALSE(settings.isVisible());
    CHECK(fx.host.window() == &settings);

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("closing Settings after the last terminal window lets the app quit",
          "[contour][gui][qml][settingswindow][host]")
{
    // Ruling D1: Settings keeps the app running after the last terminal window closes, until Settings
    // closes too. Qt's quit-on-last-window rule runs on a close, never on a hide, so the Close shortcut must
    // close the window as its title bar would.
    contour::test::QmlMessageCapture const warnings;
    auto fx = HostFixture {};
    // What ContourGuiApp's quitIfWindowless() judges by: is any independent top-level window still shown?
    auto const appStaysAlive = []() {
        return window::anyWindowKeepsAppAlive(window::windowFactsOf(QGuiApplication::topLevelWindows()));
    };

    auto terminal = std::make_unique<QQuickWindow>();
    terminal->show();
    auto& settings = fx.show();
    REQUIRE(terminal->close());
    terminal.reset();
    CHECK(appStaysAlive());

    settings.requestActivate();
    REQUIRE(QTest::qWaitForWindowActive(&settings));
    QTest::keySequence(&settings, QKeySequence::Close);
    CHECK(settings.handle() == nullptr); // closed, as Qt's quit rule needs; a hidden window keeps its handle
    CHECK_FALSE(appStaysAlive());

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("closing the Settings window keeps the window, its pane and its draft",
          "[contour][gui][qml][settingswindow][host]")
{
    contour::test::QmlMessageCapture const warnings;
    auto fx = HostFixture {};
    auto& settings = fx.show();

    auto const key = fx.draftAProfileField();
    auto const editing = fx.controller->editingProfile();
    auto const drafted = fx.profileFieldValue(key);
    REQUIRE(fx.page().property("editorMode").toString() == "profile");

    // The state the window reopens in: the one it closed in.
    auto states = Qt::WindowStates { Qt::WindowNoState };
    SECTION("through the page's Close button and the Close shortcut")
    {
        fx.host.closeSettings();
    }
    SECTION("through the title bar's close button, which destroys the platform window")
    {
        REQUIRE(settings.close());
        CHECK(settings.handle() == nullptr);
    }
    SECTION("while maximised, which it reopens as")
    {
        states = Qt::WindowMaximized;
        settings.setWindowStates(states);
        REQUIRE(settings.close());
    }
    REQUIRE_FALSE(settings.isVisible());
    CHECK(&fx.show() == &settings);
    CHECK(settings.windowStates() == states);

    CHECK(fx.page().property("editorMode").toString() == "profile");
    CHECK(fx.controller->editingProfile() == editing);
    CHECK(fx.profileFieldValue(key) == drafted);

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("asking for the open Settings window keeps what is being edited",
          "[contour][gui][qml][settingswindow][host]")
{
    // A config reload refreshes the controller already; a show of the window on screen must not, or the
    // rows rebuild under the field being typed into.
    contour::test::QmlMessageCapture const warnings;
    auto fx = HostFixture {};
    auto const& settings = fx.show();
    auto const key = fx.draftAProfileField();
    auto const drafted = fx.profileFieldValue(key);
    auto const changes = QSignalSpy(fx.controller.get(), &window::SettingsController::changed);

    REQUIRE(settings.isVisible());
    REQUIRE(fx.host.show(nullptr).has_value());
    CHECK(changes.count() == 0);
    CHECK(fx.profileFieldValue(key) == drafted);

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("the Settings window follows its host's conventions", "[contour][gui][qml][settingswindow][host]")
{
    contour::test::QmlMessageCapture const warnings;

    for (auto const& row: window::SettingsWindowConventionRows)
    {
        INFO("host " << static_cast<int>(row.host));
        auto fx = HostFixture { row };
        auto const& settings = fx.show();

        CHECK(settings.title().toStdString() == row.title);
        switch (row.buttons)
        {
            case window::SettingsWindowButtons::CloseOnly:
                CHECK_FALSE(settings.flags().testFlag(Qt::WindowMinimizeButtonHint));
                CHECK_FALSE(settings.flags().testFlag(Qt::WindowMaximizeButtonHint));
                break;
            case window::SettingsWindowButtons::All: CHECK(settings.flags() == Qt::Window); break;
        }
    }

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("the Settings window's Terminal row counts it in cells",
          "[contour][gui][qml][settingswindow][host]")
{
    contour::test::QmlMessageCapture const warnings;
    auto const& row = config::ChromeTokenRows[static_cast<std::size_t>(config::ChromeStyle::Terminal)];
    auto fx = HostFixture { window::settingsWindowConventionsFor(config::HostPlatform::Other),
                            config::UiStyle::Terminal };
    auto const& settings = fx.show();

    CHECK(preferredSizeOf(settings).width()
          == Catch::Approx(row.size.settingsWidth * fx.provider().widthQuantum()));
    CHECK(settings.minimumHeight() == Catch::Approx(row.size.settingsMinHeight * fx.provider().cellHeight()));

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("the Settings window keeps the app alive", "[contour][gui][qml][settingswindow][host]")
{
    contour::test::QmlMessageCapture const warnings;
    auto fx = HostFixture {};

    CHECK(fx.show().transientParent() == nullptr);

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("a Settings window factory that fails leaves no window",
          "[contour][gui][qml][settingswindow][host]")
{
    auto fx = ControllerFixture { OneProfile, nullptr };
    auto host = SettingsWindowHost {
        *fx.controller,
        window::settingsWindowConventionsFor(config::HostPlatform::Other),
        [](SettingsWindowHost&) -> std::expected<std::unique_ptr<QQuickWindow>, SettingsWindowError> {
            return std::unexpected { SettingsWindowError::LoadFailed };
        }
    };

    auto const shown = host.show(nullptr);
    REQUIRE_FALSE(shown.has_value());
    CHECK(shown.error() == SettingsWindowError::LoadFailed);
    CHECK(std::format("{}", shown.error()) == "LoadFailed");
    CHECK(host.window() == nullptr);
}
