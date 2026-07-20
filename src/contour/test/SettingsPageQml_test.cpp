// SPDX-License-Identifier: Apache-2.0
//
// End-to-end offscreen test of the settings page: it instantiates the real SettingsPage.qml against a
// real SettingsController (backed by a file store in a temp directory) and drives it through the QML —
// clicking "New profile", typing a name, clicking "Save As" — then asserts a side file appeared and the
// config picked it up. This proves the QML actually wires its controls to the controller; the
// controller's own logic is unit-tested separately in SettingsController_test.cpp.

#include <contour/Config.h>
#include <contour/GuiConfigStore.h>
#include <contour/SettingsController.h>
#include <contour/test/QmlMessageCapture.h>

#include <QtCore/QTemporaryDir>
#include <QtQml/QQmlComponent>
#include <QtQml/QQmlEngine>
#include <QtQuick/QQuickItem>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <memory>

using namespace contour;

namespace
{

/// Emits @p object's `clicked()` signal, running its QML onClicked handler regardless of the button's
/// `enabled` binding (this drives the wiring directly rather than simulating a pointer event).
void clickButton(QObject* object)
{
    REQUIRE(object != nullptr);
    QMetaObject::invokeMethod(object, "clicked");
}

} // namespace

TEST_CASE("SettingsPage opens on the global settings pane", "[contour][gui][qml][settings]")
{
    contour::test::QmlMessageCapture const warnings;

    QTemporaryDir const dir;
    auto const configDir = std::filesystem::path(dir.path().toStdString());
    auto const configPath = configDir / "contour.yml";
    {
        auto out = std::ofstream(configPath);
        out << "default_profile: main\nprofiles:\n    main:\n        show_title_bar: true\n";
    }

    config::Config cfg;
    config::loadConfigFromFile(cfg, configPath);
    auto store = std::make_shared<FileGuiConfigStore>(configDir);
    auto controller = SettingsController([&]() -> config::Config const& { return cfg; }, store, [&]() {});

    QQmlEngine engine;
    QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/contour/ui/SettingsPage.qml")));
    REQUIRE(component.isReady());

    auto initial = QVariantMap {};
    initial.insert("controller", QVariant::fromValue(&controller));
    std::unique_ptr<QObject> page(component.createWithInitialProperties(initial));
    REQUIRE(page != nullptr);
    auto* item = qobject_cast<QQuickItem*>(page.get());
    REQUIRE(item != nullptr);

    // The page lands on a pane rather than on a "pick something" placeholder, and the nav rail agrees
    // with the pane it is showing.
    CHECK(page->property("editorMode").toString() == "globals");
    CHECK(!page->property("headerTitle").toString().isEmpty());

    auto* globalsButton = item->findChild<QObject*>("globalSettingsButton");
    REQUIRE(globalsButton != nullptr);
    CHECK(globalsButton->property("selected").toBool());

    // Nothing else in the rail claims to be selected at the same time.
    auto* keybindingsButton = item->findChild<QObject*>("keybindingsButton");
    REQUIRE(keybindingsButton != nullptr);
    CHECK(!keybindingsButton->property("selected").toBool());

    CHECK(warnings.count([](QString const& w) {
        return w.contains("TypeError") || w.contains("ReferenceError");
    }) == 0);
}

TEST_CASE("SettingsPage creates a profile through the QML and it lands on disk (offscreen)",
          "[contour][gui][qml][settings]")
{
    contour::test::QmlMessageCapture const warnings;

    QTemporaryDir const dir;
    auto const configDir = std::filesystem::path(dir.path().toStdString());
    auto const configPath = configDir / "contour.yml";
    {
        auto out = std::ofstream(configPath);
        out << "default_profile: main\nprofiles:\n    main:\n        show_title_bar: true\n";
    }

    config::Config cfg;
    config::loadConfigFromFile(cfg, configPath);
    auto store = std::make_shared<FileGuiConfigStore>(configDir);
    auto controller = SettingsController([&]() -> config::Config const& { return cfg; },
                                         store,
                                         [&]() {
                                             cfg = config::Config {};
                                             config::loadConfigFromFile(cfg, configPath);
                                         });

    QQmlEngine engine;
    QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/contour/ui/SettingsPage.qml")));
    REQUIRE(component.isReady());

    auto initial = QVariantMap {};
    initial.insert("controller", QVariant::fromValue(&controller));
    std::unique_ptr<QObject> page(component.createWithInitialProperties(initial));
    REQUIRE(page != nullptr);
    auto* item = qobject_cast<QQuickItem*>(page.get());
    REQUIRE(item != nullptr);

    // Start a new profile through the page's "New profile…" button.
    clickButton(item->findChild<QObject*>("newProfileButton"));
    CHECK(controller.editingProfile().isEmpty()); // an unsaved new draft
    CHECK(!controller.profileFields().isEmpty()); // its fields are exposed for editing

    // Type a name and Save As.
    auto* saveAsField = item->findChild<QObject*>("saveAsField");
    REQUIRE(saveAsField != nullptr);
    saveAsField->setProperty("text", "work");
    clickButton(item->findChild<QObject*>("saveProfileAsButton"));

    // The side file exists, the reloaded config carries it, and the page now edits it.
    CHECK(std::filesystem::exists(configDir / "profiles" / "work.yml"));
    CHECK(cfg.findProfile("work") != nullptr);
    CHECK(controller.editingProfile() == "work");

    // No QML binding errors were raised while driving the page.
    CHECK(warnings.count([](QString const& w) {
        return w.contains("TypeError") || w.contains("ReferenceError");
    }) == 0);
}
