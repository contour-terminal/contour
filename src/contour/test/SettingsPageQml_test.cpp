// SPDX-License-Identifier: Apache-2.0
//
// End-to-end offscreen test of the settings page: it instantiates the real SettingsPage.qml against a
// real SettingsController (backed by a file store in a temp directory) and drives it through the QML —
// clicking "New profile", typing a name, clicking "Save As" — then asserts a side file appeared and the
// config picked it up. This proves the QML actually wires its controls to the controller; the
// controller's own logic is unit-tested separately in SettingsController_test.cpp.

#include <contour/config/Config.hpp>
#include <contour/config/GuiConfigStore.hpp>
#include <contour/test/QmlChromeStyle.hpp>
#include <contour/test/QmlMessageCapture.hpp>
#include <contour/window/SettingsController.hpp>

#include <QtCore/QTemporaryDir>
#include <QtQml/QQmlComponent>
#include <QtQml/QQmlEngine>
#include <QtQml/QQmlIncubator>
#include <QtQuick/QQuickItem>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string_view>

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
    auto store = std::make_shared<config::FileGuiConfigStore>(configDir);
    auto controller =
        contour::window::SettingsController([&]() -> config::Config const& { return cfg; }, store, [&]() {});

    QQmlEngine engine;
    contour::test::installChromeStyle(engine);
    QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/qt/qml/Contour/Ui/SettingsPage.qml")));
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
    auto store = std::make_shared<config::FileGuiConfigStore>(configDir);
    auto controller = contour::window::SettingsController([&]() -> config::Config const& { return cfg; },
                                                          store,
                                                          [&]() {
                                                              cfg = config::Config {};
                                                              config::loadConfigFromFile(cfg, configPath);
                                                          });

    QQmlEngine engine;
    contour::test::installChromeStyle(engine);
    QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/qt/qml/Contour/Ui/SettingsPage.qml")));
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

namespace
{

/// Drives QML object incubation to completion.
///
/// A component built by QQmlComponent creates nested objects through the engine's incubator, and
/// asynchronously while it is itself being created. Asynchronous incubation only progresses when something
/// drives it, which a bare QQmlEngine has nothing to do; this supplies that.
///
/// It does NOT make Repeater delegates appear. Nothing available here does: in this harness a Repeater
/// creates no items at all — even `Repeater { model: 3; delegate: Rectangle {} }` yields none, reporting
/// the right `count` alongside a null `itemAt(0)`, with or without a QQuickWindow. So a test here can
/// assert on statically declared objects and on page/controller state, but never on a delegate. Where a
/// delegate's behaviour matters, drive the signal it would have invoked (see the indicator editor tests)
/// or cover it in SettingsController_test instead.
struct IncubationDriver: QQmlIncubationController
{
    void settle()
    {
        // Bounded, so an incubation that genuinely never finishes fails the test instead of hanging it.
        for (auto guard = 0; incubatingObjectCount() > 0 && guard < 1000; ++guard)
            incubateFor(5);
    }
};

/// Builds a page over a config with one profile, ready to drive. Keeps the controller alive alongside the
/// page (the page holds a bare pointer to it).
struct PageFixture
{
    QTemporaryDir dir;
    config::Config cfg;
    std::filesystem::path configDir;
    std::filesystem::path configPath;
    std::shared_ptr<config::FileGuiConfigStore> store;
    std::unique_ptr<contour::window::SettingsController> controller;
    QQmlEngine engine;
    IncubationDriver incubation;
    std::unique_ptr<QQmlComponent> component;
    std::unique_ptr<QObject> page;

    /// Lets every pending delegate finish incubating. Call after anything that changes a model.
    void settle() { incubation.settle(); }

    explicit PageFixture(std::string_view yaml)
    {
        configDir = std::filesystem::path(dir.path().toStdString());
        configPath = configDir / "contour.yml";
        {
            auto out = std::ofstream(configPath);
            out << yaml;
        }
        config::loadConfigFromFile(cfg, configPath);
        store = std::make_shared<config::FileGuiConfigStore>(configDir);
        controller = std::make_unique<contour::window::SettingsController>(
            [this]() -> config::Config const& { return cfg; },
            store,
            [this]() {
                cfg = config::Config {};
                config::loadConfigFromFile(cfg, configPath);
            });

        contour::test::installChromeStyle(engine);
        engine.setIncubationController(&incubation);
        component = std::make_unique<QQmlComponent>(
            &engine, QUrl(QStringLiteral("qrc:/qt/qml/Contour/Ui/SettingsPage.qml")));
        REQUIRE(component->isReady());
        auto initial = QVariantMap {};
        initial.insert("controller", QVariant::fromValue(controller.get()));
        page.reset(component->createWithInitialProperties(initial));
        REQUIRE(page != nullptr);
        settle();
    }

    [[nodiscard]] QQuickItem* item() const { return qobject_cast<QQuickItem*>(page.get()); }
};

constexpr auto OneProfile =
    std::string_view { "default_profile: main\nprofiles:\n    main:\n        show_title_bar: true\n" };

} // namespace

TEST_CASE("SettingsPage groups the profile fields into collapsible sections", "[contour][gui][qml][settings]")
{
    contour::test::QmlMessageCapture const warnings;

    auto fx = PageFixture(OneProfile);
    auto* item = fx.item();
    REQUIRE(item != nullptr);

    clickButton(item->findChild<QObject*>("newProfileButton"));
    fx.settle();

    // The grouped model the pane renders: several groups, each with fields, built from the `group` each
    // field carries rather than from anything matched on in QML.
    auto const groups = fx.page->property("profileGroups").toList();
    REQUIRE(groups.size() > 1);
    auto totalFields = 0;
    for (auto const& raw: groups)
    {
        auto const group = raw.toMap();
        CHECK(!group.value("title").toString().isEmpty());
        CHECK(!group.value("glyph").toString().isEmpty());
        auto const fields = group.value("fields").toList();
        CHECK(!fields.isEmpty());
        totalFields += static_cast<int>(fields.size());
    }
    CHECK(totalFields == fx.controller->profileFields().size()); // grouping loses no field

    // A group is open by default and closes when its header asks to.
    auto const firstTitle = groups.first().toMap().value("title").toString();
    CHECK(fx.page->property("expandedGroups").toMap().isEmpty());
    QVariant expanded;
    QMetaObject::invokeMethod(
        fx.page.get(), "groupExpanded", Q_RETURN_ARG(QVariant, expanded), Q_ARG(QVariant, firstTitle));
    CHECK(expanded.toBool());

    QMetaObject::invokeMethod(fx.page.get(), "toggleGroup", Q_ARG(QVariant, firstTitle));
    QMetaObject::invokeMethod(
        fx.page.get(), "groupExpanded", Q_RETURN_ARG(QVariant, expanded), Q_ARG(QVariant, firstTitle));
    CHECK(!expanded.toBool());

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("SettingsPage filters the fields and reopens what it matched", "[contour][gui][qml][settings]")
{
    contour::test::QmlMessageCapture const warnings;

    auto fx = PageFixture(OneProfile);
    auto* item = fx.item();
    REQUIRE(item != nullptr);
    clickButton(item->findChild<QObject*>("newProfileButton"));
    fx.settle();

    auto const unfiltered = fx.page->property("visibleFieldCount").toInt();
    REQUIRE(unfiltered > 0);

    // A filter that matches something narrows the list without emptying it.
    fx.page->setProperty("filterText", "cursor");
    fx.settle();
    CHECK(fx.page->property("filtering").toBool());
    auto const filtered = fx.page->property("visibleFieldCount").toInt();
    CHECK(filtered > 0);
    CHECK(filtered < unfiltered);

    // Every surviving field really does match, and every surviving group really has survivors.
    for (auto const& raw: fx.page->property("profileGroups").toList())
    {
        auto const fields = raw.toMap().value("fields").toList();
        CHECK(!fields.isEmpty());
        for (auto const& fieldRaw: fields)
        {
            auto const field = fieldRaw.toMap();
            auto const haystack = (field.value("label").toString() + field.value("help").toString()
                                   + field.value("key").toString())
                                      .toLower();
            INFO("field: " << field.value("key").toString().toStdString());
            CHECK(haystack.contains("cursor"));
        }
    }

    // A collapsed group reopens while a search is running: a hit you cannot see is not a hit.
    auto const title = fx.page->property("profileGroups").toList().first().toMap().value("title").toString();
    QMetaObject::invokeMethod(fx.page.get(), "toggleGroup", Q_ARG(QVariant, title));
    QVariant expanded;
    QMetaObject::invokeMethod(
        fx.page.get(), "groupExpanded", Q_RETURN_ARG(QVariant, expanded), Q_ARG(QVariant, title));
    CHECK(expanded.toBool());

    SECTION("a filter that matches nothing says so")
    {
        // A real word, so the spell checker is happy, and one no setting label, help text or key contains.
        fx.page->setProperty("filterText", "asparagus");
        CHECK(fx.page->property("visibleFieldCount").toInt() == 0);
        auto* hint = item->findChild<QObject*>("noFieldMatchesLabel");
        REQUIRE(hint != nullptr);
        CHECK(hint->property("visible").toBool());
    }

    SECTION("clearing the filter restores every field")
    {
        fx.page->setProperty("filterText", "");
        CHECK(!fx.page->property("filtering").toBool());
        CHECK(fx.page->property("visibleFieldCount").toInt() == unfiltered);
    }

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("SettingsPage's indicator editor is wired to the controller", "[contour][gui][qml][settings]")
{
    // NOTE ON WHAT THIS CAN CHECK. Repeater delegates are not instantiated in this harness at all — a bare
    // `Repeater { model: 3; delegate: Rectangle {} }` yields zero items here, with the right `count` and a
    // null `itemAt()`, because a QQmlEngine driving a component outside a QML application engine never
    // creates them. So the chips themselves are unreachable from a test, and this drives the segment rows'
    // signals directly instead: those are the handlers the chips invoke, so the wiring under test is the
    // same. What each chip *renders* is covered by StatusLineChip.qml being a pure function of its inputs,
    // and the editing behaviour itself by SettingsController_test.
    contour::test::QmlMessageCapture const warnings;

    auto fx = PageFixture(OneProfile);
    auto* item = fx.item();
    REQUIRE(item != nullptr);

    // A GUI-owned profile, so the draft is editable (a contour.yml one is shown read-only).
    clickButton(item->findChild<QObject*>("newProfileButton"));
    item->findChild<QObject*>("saveAsField")->setProperty("text", "work");
    clickButton(item->findChild<QObject*>("saveProfileAsButton"));
    REQUIRE(fx.controller->editingProfile() == "work");
    REQUIRE(!fx.controller->editingReadOnly());
    fx.page->setProperty("editorMode", "profile");
    fx.settle();

    // All three segment rows exist, and each shows exactly what the controller parsed for it.
    for (auto const& [objectName, segmentIndex]: std::array {
             std::pair { "indicatorSegmentLeft", 0 },
             std::pair { "indicatorSegmentMiddle", 1 },
             std::pair { "indicatorSegmentRight", 2 },
         })
    {
        auto* row = item->findChild<QObject*>(QString::fromLatin1(objectName));
        INFO("segment row: " << objectName);
        REQUIRE(row != nullptr);
        CHECK(row->property("editable").toBool());
        CHECK(row->property("rawTemplate").toString() == fx.controller->indicatorSegment(segmentIndex));
        CHECK(row->property("items").toList().size()
              == fx.controller->parseIndicatorSegment(fx.controller->indicatorSegment(segmentIndex)).size());
        // The picker offers the whole vocabulary, read from vtbackend rather than listed in QML.
        CHECK(row->property("placeholders").toList().size() == fx.controller->indicatorPlaceholders().size());
        CHECK(!row->property("flagKeys").toList().isEmpty());
    }

    auto* left = item->findChild<QObject*>("indicatorSegmentLeft");
    REQUIRE(left != nullptr);

    SECTION("removing a placeholder rewrites the template")
    {
        auto const before = fx.controller->indicatorSegment(0);
        auto const countBefore = fx.controller->parseIndicatorSegment(before).size();
        REQUIRE(countBefore > 1);

        QMetaObject::invokeMethod(left, "removeRequested", Q_ARG(int, 0));
        fx.settle();

        auto const after = fx.controller->indicatorSegment(0);
        CHECK(after != before);
        CHECK(fx.controller->parseIndicatorSegment(after).size() == countBefore - 1);
        // Whatever survived is still made of real placeholders: nothing degraded into literal text.
        for (auto const& raw: fx.controller->parseIndicatorSegment(after))
            CHECK(!raw.toMap().value("type").toString().isEmpty());
    }

    SECTION("adding a placeholder appends it")
    {
        auto const catalog = fx.controller->indicatorPlaceholders();
        REQUIRE(!catalog.isEmpty());
        auto clock = QVariantMap {};
        for (auto const& raw: catalog)
            if (raw.toMap().value("type").toString() == "Clock")
                clock = raw.toMap();
        REQUIRE(!clock.isEmpty());

        auto const countBefore =
            fx.controller->parseIndicatorSegment(fx.controller->indicatorSegment(1)).size();
        QMetaObject::invokeMethod(left, "addRequested", Q_ARG(QVariant, QVariant(clock)));
        fx.settle();

        auto const items = fx.controller->parseIndicatorSegment(fx.controller->indicatorSegment(0));
        CHECK(items.last().toMap().value("type").toString() == "Clock");
        // ... and only the segment that was asked changed.
        CHECK(fx.controller->parseIndicatorSegment(fx.controller->indicatorSegment(1)).size() == countBefore);
    }

    SECTION("the raw template field writes straight through")
    {
        QMetaObject::invokeMethod(left, "rawCommitted", Q_ARG(QString, QStringLiteral("{Clock:Bold}")));
        fx.settle();
        CHECK(fx.controller->indicatorSegment(0) == "{Clock:Bold}");
        auto const items = fx.controller->parseIndicatorSegment(fx.controller->indicatorSegment(0));
        REQUIRE(items.size() == 1);
        CHECK(items.first().toMap().value("type").toString() == "Clock");
    }

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("SettingsPage's indicator editor is inert on a read-only profile", "[contour][gui][qml][settings]")
{
    contour::test::QmlMessageCapture const warnings;

    auto fx = PageFixture(OneProfile);
    auto* item = fx.item();
    REQUIRE(item != nullptr);

    // "main" comes from contour.yml, so the page shows it read-only.
    fx.controller->editProfile("main");
    REQUIRE(fx.controller->editingReadOnly());
    fx.page->setProperty("editorMode", "profile");
    fx.settle();

    auto* left = item->findChild<QObject*>("indicatorSegmentLeft");
    REQUIRE(left != nullptr);
    CHECK(!left->property("editable").toBool()); // the chips and Add button are disabled

    // And driving the handlers anyway changes nothing: the controller is the boundary that has to hold,
    // not the enabled state of a button.
    auto const before = fx.controller->indicatorSegment(0);
    QMetaObject::invokeMethod(left, "removeRequested", Q_ARG(int, 0));
    QMetaObject::invokeMethod(left, "rawCommitted", Q_ARG(QString, QStringLiteral("{Clock}")));
    fx.settle();
    CHECK(fx.controller->indicatorSegment(0) == before);

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}
