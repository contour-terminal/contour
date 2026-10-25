// SPDX-License-Identifier: Apache-2.0
//
// End-to-end offscreen test of the settings page: it instantiates the real SettingsPage.qml against a
// real SettingsController (backed by a file store in a temp directory) and drives it through the QML —
// clicking "New profile", typing a name, clicking "Save As" — then asserts a side file appeared and the
// config picked it up. This proves the QML actually wires its controls to the controller; the
// controller's own logic is unit-tested separately in SettingsController_test.cpp.

#include <contour/config/Config.hpp>
#include <contour/config/GuiConfigStore.hpp>
#include <contour/display/GpuInventory.hpp>
#include <contour/test/GuiTestFixtures.hpp>
#include <contour/test/QmlChromeStyle.hpp>
#include <contour/test/QmlIncubation.hpp>
#include <contour/test/QmlMessageCapture.hpp>
#include <contour/test/SettingsFixtures.hpp>
#include <contour/window/SettingsController.hpp>
#include <contour/window/SettingsWindowHost.hpp>
#include <contour/window/SettingsWindowRules.hpp>
#include <contour/window/WindowController.hpp>

#include <core/Utils.hpp>

#include <QtCore/QLocale>
#include <QtCore/QPointer>
#include <QtCore/QTemporaryDir>
#include <QtGui/QAccessible>
#include <QtGui/QColor>
#include <QtQml/QQmlComponent>
#include <QtQml/QQmlContext>
#include <QtQml/QQmlEngine>
#include <QtQml/QQmlIncubator>
#include <QtQuick/QQuickItem>
#include <QtQuick/QQuickWindow>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>

#include <QtTest/QSignalSpy>
#include <QtTest/QTest>

using namespace contour;
using contour::test::ControllerFixture;
using contour::test::IncubationDriver;

namespace
{

/// Emits @p object's `clicked()` signal, running its QML onClicked handler regardless of the button's
/// `enabled` binding (this drives the wiring directly rather than simulating a pointer event).
void clickButton(QObject* object)
{
    REQUIRE(object != nullptr);
    QMetaObject::invokeMethod(object, "clicked");
}

/// An active window that shows a settings page, or one row of it, as the user sees it, and that takes the
/// keyboard focus elsewhere when asked.
class TestWindow
{
  public:
    /// Opens an active window of @p width by @p height pixels.
    TestWindow(int width, int height): _owned { std::make_unique<QQuickWindow>() }, _window { _owned.get() }
    {
        _window->resize(width, height);
        activate();
    }

    /// Borrows @p window, resizes it to @p size, and shows and activates it.
    /// @param window The window to drive; it must outlive this one.
    /// @param size   The size to give it.
    TestWindow(QQuickWindow& window, QSize size): _window { &window }
    {
        _window->resize(size);
        activate();
    }

    /// Shows @p item, as wide and as high as the window.
    void show(QQuickItem& item)
    {
        item.setParentItem(_window->contentItem());
        item.setSize(QSizeF(_window->size()));
    }

    /// Gives @p item the keyboard focus, as clicking it would.
    static void focus(QQuickItem& item)
    {
        item.forceActiveFocus();
        REQUIRE(item.hasActiveFocus());
    }

    /// Sends @p key to whatever holds the keyboard focus.
    void press(Qt::Key key) { QTest::keyClick(_window, key); }

    /// Types @p text key by key, as the user would, without committing it.
    void type(std::string_view text)
    {
        for (auto const ch: text)
            QTest::keyClick(_window, ch);
    }

    /// Types @p text over what @p field shows, and commits it with Return.
    void typeOver(QQuickItem& field, std::string_view text)
    {
        focus(field);
        QMetaObject::invokeMethod(&field, "selectAll");
        type(text);
        press(Qt::Key_Return);
    }

    /// Takes the keyboard focus out of the field holding it, as clicking elsewhere would.
    ///
    /// Not onto the window's content item: that is a focus scope, so focusing it hands the focus straight
    /// back to the field inside it.
    void dropFocus() { focus(_focusSink); }

  private:
    /// Parents the focus sink into the window, then shows and activates it.
    void activate()
    {
        _focusSink.setParentItem(_window->contentItem());
        _window->show();
        _window->requestActivate();
        REQUIRE(QTest::qWaitForWindowActive(_window));
    }

    std::unique_ptr<QQuickWindow> _owned; ///< The window, when this one opened it; null when it borrows one.
    QQuickWindow* _window;                ///< The window driven.
    QQuickItem _focusSink;
};

/// The real SettingsPage.qml inside the real Settings window, over a SettingsController, with every delegate
/// it makes incubated.
struct SettingsPageView
{
    QQmlEngine engine;
    IncubationDriver incubation;
    window::SettingsWindowHost host;
    QPointer<QObject> page;

    /// @param controller The controller the page binds to. It must outlive the page, which holds a bare
    ///                   pointer to it.
    explicit SettingsPageView(window::SettingsController* controller):
        host { *controller,
               window::settingsWindowConventionsFor(config::HostPlatform::Other),
               [this](window::SettingsWindowHost& self) { return window::loadSettingsWindow(engine, self); } }
    {
        contour::test::installChromeStyle(engine);
        engine.setIncubationController(&incubation);
        REQUIRE(host.show(nullptr).has_value());
        page = host.window()->findChild<QObject*>(QStringLiteral("settingsPage"));
        REQUIRE(page != nullptr);
        settle();
    }

    /// Lets every pending delegate finish incubating. Call after anything that changes a model.
    void settle() { incubation.settle(); }

    [[nodiscard]] QQuickItem* item() const { return qobject_cast<QQuickItem*>(page.get()); }

    /// @return The Settings window, resized to @p size, shown and active.
    [[nodiscard]] TestWindow activeWindow(QSize size) const { return TestWindow { *host.window(), size }; }
};

/// A settings page over a contour.yml of its own, ready to drive. The controller outlives the page.
struct PageFixture: ControllerFixture, SettingsPageView
{
    /// @param yaml       The contour.yml the page edits.
    /// @param writeStore Where the page writes; null writes the side files beside that contour.yml.
    explicit PageFixture(std::string_view yaml, std::shared_ptr<config::GuiConfigStore> writeStore = nullptr):
        ControllerFixture { yaml, std::move(writeStore) }, SettingsPageView { controller.get() }
    {
    }
};

constexpr auto OneProfile =
    std::string_view { "default_profile: main\nprofiles:\n    main:\n        show_title_bar: true\n" };

/// Builds SettingRow.qml rows on their own, as the page's delegates build them.
struct SettingRowFactory
{
    QQmlEngine engine;
    std::unique_ptr<QQmlComponent> component;

    SettingRowFactory()
    {
        contour::test::installChromeStyle(engine);
        component = std::make_unique<QQmlComponent>(
            &engine, QUrl(QStringLiteral("qrc:/qt/qml/Contour/Ui/SettingRow.qml")));
        REQUIRE(component->isReady());
    }

    /// A row with the initial @p properties; its field key and label are "key" and "Label" unless given.
    [[nodiscard]] std::unique_ptr<QQuickItem> make(QVariantMap properties) const
    {
        if (!properties.contains("fieldKey"))
            properties.insert("fieldKey", "key");
        if (!properties.contains("label"))
            properties.insert("label", "Label");
        auto row = std::unique_ptr<QQuickItem>(
            qobject_cast<QQuickItem*>(component->createWithInitialProperties(properties)));
        REQUIRE(row != nullptr);
        return row;
    }
};

/// Finds the first descendant of @p root satisfying @p predicate, in the VISUAL tree: a Repeater's delegate
/// has a parent item but no QObject parent, so findChild() never sees a row of the page.
/// @param root      The item to search beneath.
/// @param predicate What to look for.
/// @return The item, or nullptr when no descendant satisfies @p predicate.
[[nodiscard]] QQuickItem* findDescendant(QQuickItem const& root, auto const& predicate)
{
    for (auto* child: root.childItems())
    {
        if (predicate(*child))
            return child;
        if (auto* found = findDescendant(*child, predicate); found != nullptr)
            return found;
    }
    return nullptr;
}

/// The page's row for field @p key, or nullptr when the page draws none.
[[nodiscard]] QQuickItem* rowOf(QQuickItem const& page, QString const& key)
{
    return findDescendant(
        page, [&key](QQuickItem const& item) { return item.property("fieldKey").toString() == key; });
}

/// The editor of Qt Quick type @p type -- "QQuickTextField", "QQuickSwitch", "QQuickComboBox" -- in @p row,
/// or nullptr when it has none.
[[nodiscard]] QQuickItem* editorOf(QQuickItem const& row, char const* type)
{
    return findDescendant(row, [type](QQuickItem const& item) { return item.inherits(type); });
}

/// The editor of Qt Quick type @p type in the page's row for field @p key, or nullptr when there is none.
[[nodiscard]] QQuickItem* editorOf(QQuickItem const& page, QString const& key, char const* type)
{
    auto const* row = rowOf(page, key);
    return row != nullptr ? editorOf(*row, type) : nullptr;
}

/// @p item, which the test cannot go on without.
[[nodiscard]] QQuickItem& required(QQuickItem* item)
{
    REQUIRE(item != nullptr);
    return *item;
}

/// The global row @p key of @p controller, as the settings page's Repeater hands it to a SettingRow.
[[nodiscard]] QVariantMap globalRow(window::SettingsController const& controller, QString const& key)
{
    for (auto const& raw: controller.globalFields())
        if (raw.toMap().value("key").toString() == key)
            return raw.toMap();
    return {};
}

/// A GPU inventory listing a hybrid laptop's two GPUs, the discrete one under its long pci.ids title.
class FakeGpuInventory final: public display::IGpuInventory
{
  public:
    [[nodiscard]] std::vector<display::GpuCandidate> list() const override
    {
        return {
            { .title = "Intel integrated GPU",
              .id = { 0x8086, 0xa788 },
              .kind = display::GpuKind::Integrated,
              .output = display::GpuOutput::DrivesDisplay,
              .driver = "i915" },
            { .title = "NVIDIA GeForce RTX 4070 Max-Q / Mobile",
              .id = { 0x10de, 0x2820 },
              .kind = display::GpuKind::Discrete,
              .output = display::GpuOutput::Offscreen,
              .driver = "nouveau" },
        };
    }
};

/// A store that cannot write anything, as one over a configuration directory that is read-only.
class UnwritableStore final: public config::GuiConfigStore
{
  public:
    /// Why every write fails.
    static constexpr auto Reason = std::string_view { "The configuration directory is read-only." };

    [[nodiscard]] std::expected<void, std::string> saveProfile(
        std::string const& /*name*/, config::TerminalProfile const& /*profile*/) override
    {
        return refuse();
    }
    [[nodiscard]] std::expected<void, std::string> deleteProfile(std::string const& /*name*/) override
    {
        return refuse();
    }
    [[nodiscard]] std::expected<void, std::string> saveColorScheme(
        std::string const& /*name*/, vtbackend::ColorPalette const& /*palette*/) override
    {
        return refuse();
    }
    [[nodiscard]] std::expected<void, std::string> deleteColorScheme(std::string const& /*name*/) override
    {
        return refuse();
    }
    [[nodiscard]] std::expected<void, std::string> saveGuiSettings(
        config::GuiManagedSettings const& /*settings*/) override
    {
        return refuse();
    }

  private:
    [[nodiscard]] static std::expected<void, std::string> refuse()
    {
        return std::unexpected(std::string { Reason });
    }
};

/// Records the announcements Qt is handed.
///
/// Static state because QAccessible::installUpdateHandler takes a plain function pointer, so there is nowhere
/// to put a capture.
struct AnnouncementProbe
{
    static inline auto Said = QStringList {};

    static void handle(QAccessibleEvent* event)
    {
        if (event->type() == QAccessible::Announcement)
            Said.push_back(static_cast<QAccessibleAnnouncementEvent*>(event)->message());
    }
};

} // namespace

TEST_CASE("SettingsPage opens on the global settings pane", "[contour][gui][qml][settings]")
{
    contour::test::QmlMessageCapture const warnings;

    auto fx = PageFixture(OneProfile);
    auto* item = fx.item();
    REQUIRE(item != nullptr);

    // The page lands on a pane rather than on a "pick something" placeholder, and the nav rail agrees
    // with the pane it is showing.
    CHECK(fx.page->property("editorMode").toString() == "globals");
    CHECK(!fx.page->property("headerTitle").toString().isEmpty());

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

    auto fx = PageFixture(OneProfile);
    auto* item = fx.item();
    REQUIRE(item != nullptr);

    // Start a new profile through the page's "New profile…" button.
    clickButton(item->findChild<QObject*>("newProfileButton"));
    CHECK(fx.controller->editingProfile().isEmpty()); // an unsaved new draft
    CHECK(!fx.controller->profileFields().isEmpty()); // its fields are exposed for editing

    // Type a name and Save As.
    auto* saveAsField = item->findChild<QObject*>("saveAsField");
    REQUIRE(saveAsField != nullptr);
    saveAsField->setProperty("text", "work");
    clickButton(item->findChild<QObject*>("saveProfileAsButton"));

    // The side file exists, the reloaded config carries it, and the page now edits it.
    CHECK(std::filesystem::exists(fx.configDir / "profiles" / "work.yml"));
    CHECK(fx.cfg.findProfile("work") != nullptr);
    CHECK(fx.controller->editingProfile() == "work");

    // No QML binding errors were raised while driving the page.
    CHECK(warnings.count([](QString const& w) {
        return w.contains("TypeError") || w.contains("ReferenceError");
    }) == 0);
}

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

TEST_CASE("SettingsPage names the gesture that opens links", "[contour][gui][qml][settings][links]")
{
    contour::test::QmlMessageCapture const warnings;

    auto fx = PageFixture(OneProfile);
    auto* item = fx.item();
    REQUIRE(item != nullptr);

    clickButton(item->findChild<QObject*>("newProfileButton"));
    fx.settle();

    // The text is the controller's, which reads input_mapping and the bypass modifier: nothing in the QML
    // spells a gesture.
    auto* label = item->findChild<QObject*>("linkGestureLabel");
    REQUIRE(label != nullptr);
    CHECK(!fx.controller->linkGesture().isEmpty());
    CHECK(label->property("text").toString() == fx.controller->linkGesture());

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
    // NOTE ON WHAT THIS CHECKS. It drives the segment rows' signals rather than the chips: those are the
    // handlers the chips invoke, so the wiring under test is the same. What each chip *renders* is covered by
    // StatusLineChip.qml being a pure function of its inputs, and the editing behaviour itself by
    // SettingsController_test.
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

TEST_CASE("SettingsPage's link pattern list is inert on a read-only profile",
          "[contour][gui][qml][settings][links]")
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

    auto* list = item->findChild<QObject*>("linkPatternList");
    REQUIRE(list != nullptr);
    CHECK_FALSE(list->property("editable").toBool());
    auto const* add = item->findChild<QObject*>("addLinkPatternButton");
    REQUIRE(add != nullptr);
    CHECK_FALSE(add->property("enabled").toBool());

    // Its one write path refuses too, and so would the controller behind it.
    auto const before = fx.controller->linkPatterns();
    QMetaObject::invokeMethod(list,
                              "commit",
                              Q_ARG(QVariant, QVariant { 1 }),
                              Q_ARG(QVariant, QVariant { QStringLiteral("clickable") }),
                              Q_ARG(QVariant, QVariant { false }));
    fx.settle();
    CHECK(fx.controller->linkPatterns() == before);

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("SettingsPage's link pattern list adds, previews and removes patterns",
          "[contour][gui][qml][settings][links]")
{
    contour::test::QmlMessageCapture const warnings;

    auto fx = PageFixture(OneProfile);
    auto* item = fx.item();
    REQUIRE(item != nullptr);

    // A GUI-owned profile, so the draft is editable.
    clickButton(item->findChild<QObject*>("newProfileButton"));
    item->findChild<QObject*>("saveAsField")->setProperty("text", "work");
    clickButton(item->findChild<QObject*>("saveProfileAsButton"));
    REQUIRE(!fx.controller->editingReadOnly());
    fx.page->setProperty("editorMode", "profile");
    fx.settle();

    auto* list = item->findChild<QObject*>("linkPatternList");
    REQUIRE(list != nullptr);
    CHECK(list->property("editable").toBool());

    // The preview shows what the selected row, the url built-in, finds in the sample.
    auto const* preview = item->findChild<QObject*>("linkPatternPreview");
    REQUIRE(preview != nullptr);
    list->setProperty("sample", QStringLiteral("see https://example.com/x and more"));
    fx.settle();
    CHECK(preview->property("text").toString() == "see <u>https://example.com/x</u> and more");

    auto const builtins = fx.controller->linkPatterns().size();
    clickButton(item->findChild<QObject*>("addLinkPatternButton"));
    fx.settle();
    REQUIRE(fx.controller->linkPatterns().size() == builtins + 1);
    auto const added = fx.controller->linkPatterns().back().toMap();

    // The new row says what it lacks, in the chrome style's error colour. A delegate has no QObject parent,
    // so the label is looked for in the visual tree.
    auto const* shown = findDescendant(*item, [](QQuickItem const& label) {
        return label.objectName() == "linkPatternProblem" && !label.property("text").toString().isEmpty();
    });
    REQUIRE(shown != nullptr);
    CHECK(shown->property("text").toString() == added.value("problem").toString());
    auto const* style = fx.engine.rootContext()->contextProperty("chromeStyle").value<QObject*>();
    REQUIRE(style != nullptr);
    CHECK(shown->property("color").value<QColor>() == style->property("errorColor").value<QColor>());

    // Deleting asks first, naming the pattern, and removes it once confirmed.
    QMetaObject::invokeMethod(list,
                              "deleteRequested",
                              Q_ARG(int, static_cast<int>(builtins)),
                              Q_ARG(QString, added.value("name").toString()));
    fx.settle();
    auto* dialog = item->findChild<QObject*>("deleteDialog");
    REQUIRE(dialog != nullptr);
    CHECK(dialog->property("message").toString()
          == QStringLiteral("Remove the link pattern “%1” from this profile?")
                 .arg(added.value("name").toString()));
    QMetaObject::invokeMethod(dialog, "confirmed");
    fx.settle();
    CHECK(fx.controller->linkPatterns().size() == builtins);

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("SettingsPage's link pattern list takes what is typed, one field after the other",
          "[contour][gui][qml][settings][links]")
{
    // Typed for real into the list's own fields: leaving the name field commits it, which changes the model,
    // while the focus is already on its way to the regex field.
    contour::test::QmlMessageCapture const warnings;

    auto fx = PageFixture(OneProfile);
    auto* page = fx.item();
    REQUIRE(page != nullptr);
    clickButton(page->findChild<QObject*>("newProfileButton"));
    page->findChild<QObject*>("saveAsField")->setProperty("text", "work");
    clickButton(page->findChild<QObject*>("saveProfileAsButton"));
    REQUIRE(!fx.controller->editingReadOnly());
    fx.page->setProperty("editorMode", "profile");
    auto window = fx.activeWindow(QSize(1000, 800));
    fx.settle();

    clickButton(page->findChild<QObject*>("addLinkPatternButton"));
    fx.settle();
    auto const row = static_cast<int>(fx.controller->linkPatterns().size()) - 1;
    auto const added = fx.controller->linkPatterns().back().toMap().value("name").toString();

    // The new row's fields, found in the visual tree: a delegate has no QObject parent.
    auto const fieldOf = [&](QString const& objectName) -> QQuickItem& {
        auto* const name = findDescendant(*page, [&](QQuickItem const& item) {
            return item.objectName() == "linkPatternName" && item.property("text").toString() == added;
        });
        return required(findDescendant(required(required(name).parentItem()), [&](QQuickItem const& item) {
            return item.objectName() == objectName;
        }));
    };

    auto* const name = &fieldOf("linkPatternName");
    auto const regex = QPointer<QQuickItem> { &fieldOf("linkPatternRegex") };
    TestWindow::focus(*name);
    QMetaObject::invokeMethod(name, "selectAll");
    window.type("jira");
    regex->forceActiveFocus(); // leaves the name field, which commits it
    fx.settle();
    // The field the focus went to is still there, and still has it.
    REQUIRE_FALSE(regex.isNull());
    CHECK(regex->hasActiveFocus());
    window.type("T-[0-9]");
    window.press(Qt::Key_Return);
    fx.settle();

    auto const stored = fx.controller->linkPatterns().at(row).toMap();
    CHECK(stored.value("name").toString() == "jira");
    CHECK(stored.value("regex").toString() == "T-[0-9]");
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("SettingRow shows the GPU titles in full, and an absent GPU as the current entry",
          "[contour][gui][qml][settings][gpu]")
{
    // Built on its own from the controller's row, exactly as the page's delegate builds it, so the test sets
    // the row's width directly.
    contour::test::QmlMessageCapture const warnings;

    QTemporaryDir const dir;
    auto const configDir = std::filesystem::path(dir.path().toStdU16String());
    auto const configPath = configDir / "contour.yml";
    {
        auto out = std::ofstream(configPath);
        out << "default_profile: main\nrenderer:\n    gpu: 1002:7340\n"
               "profiles:\n    main:\n        show_title_bar: true\n";
    }
    config::Config cfg;
    config::loadConfigFromFile(cfg, configPath);
    auto const controller =
        contour::window::SettingsController([&]() -> config::Config const& { return cfg; },
                                            std::make_shared<config::FileGuiConfigStore>(configDir),
                                            [&]() {},
                                            std::make_shared<FakeGpuInventory>());
    auto const row = globalRow(controller, "renderer.gpu");
    REQUIRE(!row.isEmpty());

    auto const rows = SettingRowFactory {};
    auto initial = QVariantMap {};
    for (auto const* const name: { "label", "help", "type", "value", "options", "optionLabels" })
        initial.insert(QString::fromLatin1(name), row.value(QString::fromLatin1(name)));
    initial.insert("fieldKey", row.value("key"));
    auto const settingRow = rows.make(initial);

    // Laid out for real, in a settings page at a normal window width: the editor's width is the point.
    auto window = TestWindow(900, 200);
    window.show(*settingRow);

    auto& combo = required(editorOf(*settingRow, "QQuickComboBox"));

    // Titles, never ids, and the configured-but-absent GPU stays the current entry.
    auto const labels = row.value("optionLabels").toStringList();
    CHECK(labels.contains("NVIDIA GeForce RTX 4070 Max-Q / Mobile"));
    CHECK(combo.property("count").toInt() == labels.size());
    CHECK(combo.property("currentIndex").toInt() == labels.size() - 1);
    CHECK(combo.property("currentText").toString() == "Unavailable GPU (1002:7340)");
    CHECK(combo.property("displayText").toString() == "Unavailable GPU (1002:7340)");

    // Wide enough for the longest title, so neither the box nor its popup cuts it off. The width
    // follows a layout polish, which is a render-loop pass rather than an event, so this waits for the
    // geometry: a fixed delay is a guess at when the next frame lands, and a loaded runner misses it.
    // 240 is the fixed editor column the box grew past, which cut the RTX title off.
    CHECK(QTest::qWaitFor([&] { return combo.width() > 240; }, 2000));
    CHECK(combo.width() >= combo.implicitWidth());
    INFO("combo width " << combo.width() << ", implicit width " << combo.implicitWidth());

    // A row too narrow for the cap: the box elides the long title rather than cutting it off.
    settingRow->setProperty("value", QStringLiteral("10de:2820"));
    settingRow->setWidth(300);
    CHECK(QTest::qWaitFor([&] { return combo.width() <= 240; }, 2000)); // back to the column's floor
    INFO("narrow combo width " << combo.width());
    CHECK(combo.property("currentText").toString() == "NVIDIA GeForce RTX 4070 Max-Q / Mobile");
    CHECK(combo.property("displayText").toString().endsWith(QChar(0x2026)));

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("SettingsPage renders the command-block settings from the descriptors",
          "[contour][gui][qml][settings]")
{
    // No QML knows about these rows: the profile pane groups whatever `group` a row carries, and the
    // global pane lists whatever globalFields returns. This proves both hold for the new rows.
    contour::test::QmlMessageCapture const warnings;

    auto fx = PageFixture(OneProfile);
    auto* item = fx.item();
    REQUIRE(item != nullptr);
    clickButton(item->findChild<QObject*>("newProfileButton"));
    fx.settle();

    // The page draws a row for each key it lists: a model no delegate shows would pass the other checks.
    auto groupKeys = QStringList {};
    for (auto const& raw: fx.page->property("profileGroups").toList())
        if (raw.toMap().value("title").toString() == "Command blocks")
            for (auto const& field: raw.toMap().value("fields").toList())
                groupKeys.push_back(field.toMap().value("key").toString());
    CHECK(groupKeys.size() == qsizetype { 10 });
    CHECK(groupKeys.contains("sticky_header_mode"));
    for (auto const& key: groupKeys)
        CHECK(rowOf(*item, key) != nullptr);

    // A user who knows the YAML path can search for it on the global pane.
    fx.page->setProperty("filterText", "gutter.");
    fx.settle();
    auto globalKeys = QStringList {};
    for (auto const& raw: fx.page->property("globalFieldsFiltered").toList())
        globalKeys.push_back(raw.toMap().value("key").toString());
    for (auto const* key: { "gutter.exit_status",
                            "gutter.user_marks",
                            "gutter.line_numbers",
                            "gutter.line_number_width",
                            "gutter.timestamps",
                            "gutter.timestamp_format" })
        CHECK(globalKeys.contains(QString::fromLatin1(key)));
    for (auto const& key: globalKeys)
        CHECK(rowOf(*item, key) != nullptr);

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("SettingsPage shows the stored value again when it refuses a typed one",
          "[contour][gui][qml][settings]")
{
    // What the user typed is in the text field, not in the model, and a refused value leaves the model as it
    // was: re-sending it rebuilds no row, so the field went on showing a value that was never saved. Typed
    // for real, into the page's own rows, so the whole path -- editor, page, controller and back -- is the
    // one the user takes.
    contour::test::QmlMessageCapture const warnings;

    auto fx = PageFixture(OneProfile);
    auto* page = fx.item();
    REQUIRE(page != nullptr);
    auto window = fx.activeWindow(QSize(1000, 800));
    fx.settle();

    // Looked up again each time: a model that changed rebuilds the rows.
    auto const shown = [&](QString const& key) {
        return required(editorOf(*page, key, "QQuickTextField")).property("text").toString().toStdString();
    };
    auto const typeInto = [&](QString const& key, std::string_view text) {
        window.typeOver(required(editorOf(*page, key, "QQuickTextField")), text);
        fx.settle();
    };

    SECTION("a global row refuses a width out of range, and says why")
    {
        auto const key = QStringLiteral("gutter.line_number_width");
        auto const stored = shown(key);
        REQUIRE(stored == std::to_string(fx.cfg.gutter.value().lineNumberWidth));

        typeInto(key, "11");
        CHECK(shown(key) == stored);
        REQUIRE(fx.errors.size() == qsizetype { 1 });
        CHECK(fx.errors.first().contains("from 3 to 10"));
        CHECK_FALSE(globalRow(*fx.controller, key).value("overridden").toBool());

        // Leaving the field finishes editing once more. What it shows now is the stored value, which is no
        // change to save: nothing is overridden, and nothing more is reported.
        window.dropFocus();
        fx.settle();
        CHECK(shown(key) == stored);
        CHECK(fx.errors.size() == qsizetype { 1 });
        CHECK_FALSE(globalRow(*fx.controller, key).value("overridden").toBool());
    }

    SECTION("a string row refuses a timestamp format it cannot draw, and says why")
    {
        auto const key = QStringLiteral("gutter.timestamp_format");
        auto const stored = shown(key);
        REQUIRE(stored == fx.cfg.gutter.value().timestampFormat);

        typeInto(key, "%K");
        CHECK(shown(key) == stored);
        REQUIRE(fx.errors.size() == qsizetype { 1 });
        CHECK(fx.errors.first().contains("timestamp format"));
        CHECK_FALSE(globalRow(*fx.controller, key).value("overridden").toBool());
    }

    SECTION("a double row saves what was typed")
    {
        auto const key = QStringLiteral("ui_font_size");
        typeInto(key, "11.5");
        CHECK(fx.cfg.uiFontSize.value() == 11.5);
        CHECK(shown(key) == "11.5");
        CHECK(fx.errors.isEmpty());
    }

    SECTION("a profile row stores a negative minimum duration as zero, and shows the zero")
    {
        clickButton(page->findChild<QObject*>("newProfileButton"));
        fx.page->setProperty("editorMode", "profile");
        fx.settle();
        auto const key = QStringLiteral("notify_on_command_finish_min_duration");
        REQUIRE(shown(key) == "10");

        typeInto(key, "-5");
        CHECK(shown(key) == "0");
        // Again, now that the stored value is already zero: the page re-sends a model that did not change,
        // and the field must still drop what was typed.
        typeInto(key, "-5");
        CHECK(shown(key) == "0");
        // Zero is what the loader makes of a negative duration too, so this is no refusal to report.
        CHECK(fx.errors.isEmpty());
    }

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("SettingsPage says why it refused a value until the next change it saves",
          "[contour][gui][qml][settings]")
{
    contour::test::QmlMessageCapture const warnings;

    auto fx = PageFixture(OneProfile);
    auto* page = fx.item();
    REQUIRE(page != nullptr);
    auto window = fx.activeWindow(QSize(1000, 800));
    fx.settle();

    auto* banner = page->findChild<QQuickItem*>("settingsErrorBanner");
    REQUIRE(banner != nullptr);
    auto* message = page->findChild<QQuickItem*>("settingsErrorMessage");
    REQUIRE(message != nullptr);
    CHECK_FALSE(banner->isVisible());

    auto const typeInto = [&](std::string_view text) {
        window.typeOver(
            required(editorOf(*page, QStringLiteral("gutter.line_number_width"), "QQuickTextField")), text);
        fx.settle();
    };

    typeInto("11");
    CHECK(banner->isVisible());
    auto const shown = message->property("text").toString();
    CHECK(shown.contains("from 3 to 10"));
    // A screen reader is told, too: the reason is the message's accessible name, and it reads as an alert.
    auto* const accessible = QAccessible::queryAccessibleInterface(message);
    REQUIRE(accessible != nullptr);
    CHECK(accessible->text(QAccessible::Name) == shown);
    CHECK(accessible->role() == QAccessible::AlertMessage);

    // The next value the page saves takes the reason away.
    typeInto("8");
    CHECK(fx.cfg.gutter.value().lineNumberWidth == 8);
    CHECK_FALSE(banner->isVisible());

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("SettingsPage puts a switch or a combo box back when it cannot save the change",
          "[contour][gui][qml][settings]")
{
    // Neither editor can send a value the page refuses, but a change it cannot write is refused all the same,
    // and the model is left as it was: nothing rebuilds the row, so only the editor can take the change back.
    contour::test::QmlMessageCapture const warnings;

    auto fx = PageFixture(OneProfile, std::make_shared<UnwritableStore>());
    auto* page = fx.item();
    REQUIRE(page != nullptr);
    auto window = fx.activeWindow(QSize(1000, 800));
    fx.settle();

    // Looked up again each time, as a rebuilt row would be.
    auto const editor = [&page](QString const& key, char const* type) -> QQuickItem& {
        return required(editorOf(*page, key, type));
    };
    auto const press = [&](QQuickItem& item, Qt::Key key) {
        TestWindow::focus(item);
        window.press(key);
        fx.settle();
    };

    SECTION("a switch")
    {
        auto const key = QStringLiteral("reflow_on_resize");
        auto const stored = fx.cfg.reflowOnResize.value();
        REQUIRE(editor(key, "QQuickSwitch").property("checked").toBool() == stored);

        press(editor(key, "QQuickSwitch"), Qt::Key_Space);
        REQUIRE(fx.errors == QStringList { QString::fromUtf8(UnwritableStore::Reason) });
        CHECK(editor(key, "QQuickSwitch").property("checked").toBool() == stored);

        // ... and goes on following the value, as when a Reset moves it.
        required(rowOf(*page, key)).setProperty("value", !stored);
        CHECK(editor(key, "QQuickSwitch").property("checked").toBool() == !stored);
    }

    SECTION("a combo box")
    {
        auto const key = QStringLiteral("gutter.line_numbers");
        REQUIRE(editor(key, "QQuickComboBox").property("currentText").toString() == "off");

        press(editor(key, "QQuickComboBox"), Qt::Key_Down);
        REQUIRE(fx.errors == QStringList { QString::fromUtf8(UnwritableStore::Reason) });
        CHECK(editor(key, "QQuickComboBox").property("currentText").toString() == "off");

        required(rowOf(*page, key)).setProperty("value", "relative");
        CHECK(editor(key, "QQuickComboBox").property("currentText").toString() == "relative");
    }

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("SettingRow shows what is wrong with its value under it", "[contour][gui][qml][settings][links]")
{
    contour::test::QmlMessageCapture const warnings;
    auto const rows = SettingRowFactory {};
    auto const row = rows.make({ { "fieldKey", "file_opener_command" },
                                 { "type", "string" },
                                 { "value", "code {file}" },
                                 { "problem", "an unknown placeholder" } });
    auto const* problem = row->findChild<QQuickItem*>("settingProblem");
    REQUIRE(problem != nullptr);
    CHECK(problem->isVisible());
    CHECK(problem->property("text").toString() == "an unknown placeholder");
    // In the chrome style's error colour, which follows the theme.
    auto const* style = rows.engine.rootContext()->contextProperty("chromeStyle").value<QObject*>();
    REQUIRE(style != nullptr);
    CHECK(problem->property("color").value<QColor>() == style->property("errorColor").value<QColor>());

    row->setProperty("problem", QString {});
    CHECK_FALSE(problem->isVisible());
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("SettingRow's text field follows its value once an edit is committed",
          "[contour][gui][qml][settings]")
{
    // The host answers an edit through `value`: with the value it stored, or by leaving `value` as it was
    // when it refused the edit. Either way the field shows `value` afterwards, and keeps following it.
    contour::test::QmlMessageCapture const warnings;

    auto const rows = SettingRowFactory {};
    auto const row =
        rows.make({ { "fieldKey", "width" }, { "label", "Width" }, { "type", "int" }, { "value", 6 } });
    QSignalSpy const edits(row.get(), SIGNAL(edited(QString, QVariant)));
    auto window = TestWindow(600, 100);
    window.show(*row);
    auto& field = required(editorOf(*row, "QQuickTextField"));

    // Committed, and refused: `value` stays 6, and so does the field.
    window.typeOver(field, "8");
    REQUIRE(edits.size() == qsizetype { 1 });
    CHECK(edits.first().at(1).toInt() == 8);
    CHECK(field.property("text").toString().toStdString() == "6");

    // Leaving the field finishes editing once more, on the 6 it shows now: that is no edit.
    window.dropFocus();
    CHECK_FALSE(field.hasActiveFocus());
    CHECK(edits.size() == qsizetype { 1 });

    // The value moves later, as a Reset moves it: the field still follows.
    row->setProperty("value", 4);
    CHECK(field.property("text").toString().toStdString() == "4");

    // Typing the value the row already holds is no edit.
    window.typeOver(field, "4");
    CHECK(edits.size() == qsizetype { 1 });

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("SettingsPage follows a configuration reload while it is open", "[contour][gui][qml][settings]")
{
    // A reload -- live_config noticing contour.yml change, or ReloadConfig -- refreshes the app's settings
    // page. A row left on the value from before the reload would take typing that value back as no
    // change, and drop it.
    contour::test::QmlMessageCapture const warnings;

    auto testApp = contour::test::ConfiguredTestApp {};
    auto const writeConfig = [&testApp](int lineNumberWidth) {
        auto out = std::ofstream(testApp.configFile());
        out << std::format("default_profile: main\n"
                           "gutter:\n"
                           "    line_number_width: {}\n"
                           "profiles:\n"
                           "    main:\n"
                           "        show_title_bar: true\n",
                           lineNumberWidth);
    };
    writeConfig(6);
    REQUIRE(testApp.manager().reloadAllSessions().has_value());
    auto* settings = &testApp.app().settingsController();

    auto view = SettingsPageView { settings };
    auto* page = view.item();
    REQUIRE(page != nullptr);
    auto window = view.activeWindow(QSize(1000, 800));
    view.settle();

    auto const key = QStringLiteral("gutter.line_number_width");
    auto const field = [&page, &key]() -> QQuickItem& {
        return required(editorOf(*page, key, "QQuickTextField"));
    };
    REQUIRE(field().property("text").toString() == "6");

    writeConfig(8);
    REQUIRE(testApp.manager().reloadAllSessions().has_value());
    view.settle();
    CHECK(field().property("text").toString() == "8");

    // Typing the width from before the reload is a change now, and it is saved.
    window.typeOver(field(), "6");
    view.settle();
    CHECK(testApp.app().config().gutter.value().lineNumberWidth == 6);
    CHECK(globalRow(*settings, key).value("overridden").toBool());

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("SettingRow reads a number with a decimal point in every locale", "[contour][gui][qml][settings]")
{
    // A number field shows its value as JavaScript spells it and reads the typed text back with parseInt or
    // parseFloat, so its validator must read the same: a point for the decimals, and no group separator. One
    // that followed a locale writing a decimal comma took the point for a group separator -- leaving a field
    // that showed 0.5 stored 5, and 1.000 typed into a whole number stored 1.
    contour::test::QmlMessageCapture const warnings;
    auto const previousLocale = QLocale {};
    QLocale::setDefault(QLocale(QLocale::German, QLocale::Germany));
    auto const restoreLocale = core::Finally { [&previousLocale] { QLocale::setDefault(previousLocale); } };

    auto const rows = SettingRowFactory {};
    auto window = TestWindow(600, 100);

    SECTION("a decimal")
    {
        auto const row = rows.make({ { "type", "double" }, { "value", 0.5 } });
        window.show(*row);
        QSignalSpy const edits(row.get(), SIGNAL(edited(QString, QVariant)));
        auto& field = required(editorOf(*row, "QQuickTextField"));
        REQUIRE(field.property("text").toString() == "0.5");

        // Visiting the field is no edit.
        TestWindow::focus(field);
        window.dropFocus();
        CHECK(edits.isEmpty());
        CHECK(field.property("text").toString() == "0.5");

        window.typeOver(field, "0.75");
        REQUIRE(edits.size() == qsizetype { 1 });
        CHECK(edits.at(0).at(1).toDouble() == 0.75);

        // A decimal comma, as the locale writes it, is read the same.
        window.typeOver(field, "0,25");
        REQUIRE(edits.size() == qsizetype { 2 });
        CHECK(edits.at(1).at(1).toDouble() == 0.25);
    }

    SECTION("a whole number")
    {
        auto const row = rows.make({ { "type", "int" }, { "value", 6 } });
        window.show(*row);
        QSignalSpy const edits(row.get(), SIGNAL(edited(QString, QVariant)));
        auto& field = required(editorOf(*row, "QQuickTextField"));

        // Neither separator is a digit, so neither is taken.
        window.typeOver(field, "1.000");
        REQUIRE(edits.size() == qsizetype { 1 });
        CHECK(edits.at(0).at(1).toInt() == 1000);
        window.typeOver(field, "2,000");
        REQUIRE(edits.size() == qsizetype { 2 });
        CHECK(edits.at(1).at(1).toInt() == 2000);

        // A number no setting can hold is no edit, and the field shows the value again.
        window.typeOver(field, "9999999999");
        CHECK(edits.size() == qsizetype { 2 });
        CHECK(field.property("text").toString() == "6");
    }

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("SettingRow sends nothing for a re-picked entry, and Escape puts the stored text back",
          "[contour][gui][qml][settings]")
{
    contour::test::QmlMessageCapture const warnings;

    auto const rows = SettingRowFactory {};
    auto window = TestWindow(600, 100);

    SECTION("a combo box")
    {
        auto const row = rows.make({ { "type", "enum" },
                                     { "value", "relative" },
                                     { "options", QStringList { "off", "relative", "hybrid" } } });
        window.show(*row);
        QSignalSpy const edits(row.get(), SIGNAL(edited(QString, QVariant)));
        auto& combo = required(editorOf(*row, "QQuickComboBox"));
        REQUIRE(combo.property("currentIndex").toInt() == 1);

        // Picking the entry it shows is no change: writing it would override the setting with the value it
        // already has.
        QMetaObject::invokeMethod(&combo, "activated", Q_ARG(int, 1));
        CHECK(edits.isEmpty());

        // Another entry is.
        combo.setProperty("currentIndex", 2);
        QMetaObject::invokeMethod(&combo, "activated", Q_ARG(int, 2));
        REQUIRE(edits.size() == qsizetype { 1 });
        CHECK(edits.first().at(1).toString() == "hybrid");
    }

    for (auto const& [type, value, typed]: std::array {
             std::tuple { "int", QVariant(6), std::string_view { "8" } },
             std::tuple { "double", QVariant(0.5), std::string_view { "0.75" } },
             std::tuple { "string", QVariant(QStringLiteral("less")), std::string_view { "more" } },
         })
    {
        DYNAMIC_SECTION("Escape in a " << type << " field")
        {
            auto const row = rows.make({ { "type", QString::fromLatin1(type) }, { "value", value } });
            window.show(*row);
            QSignalSpy const edits(row.get(), SIGNAL(edited(QString, QVariant)));
            auto& field = required(editorOf(*row, "QQuickTextField"));
            auto const stored = field.property("text").toString();

            TestWindow::focus(field);
            QMetaObject::invokeMethod(&field, "selectAll");
            window.type(typed);
            REQUIRE(field.property("text").toString() != stored);

            window.press(Qt::Key_Escape);
            CHECK(field.property("text").toString() == stored);
            CHECK_FALSE(field.hasActiveFocus());

            // Nothing typed is left behind for the next focus change to commit, and the field still follows
            // the value.
            window.dropFocus();
            CHECK(edits.isEmpty());
            row->setProperty("value", value.toString() + "1");
            CHECK(field.property("text").toString() == value.toString() + "1");
        }
    }

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("SettingsPage announces each refusal, and drops it with the pane or draft it was about",
          "[contour][gui][qml][settings]")
{
    contour::test::QmlMessageCapture const warnings;

    auto fx = PageFixture(OneProfile);
    auto* page = fx.item();
    REQUIRE(page != nullptr);
    auto window = fx.activeWindow(QSize(1000, 800));
    fx.settle();

    auto* banner = page->findChild<QQuickItem*>("settingsErrorBanner");
    REQUIRE(banner != nullptr);

    SECTION("a screen reader hears every refusal, a repeated one too")
    {
        AnnouncementProbe::Said.clear();
        auto* const previous = QAccessible::installUpdateHandler(&AnnouncementProbe::handle);
        for ([[maybe_unused]] auto const attempt: { 1, 2 })
        {
            window.typeOver(
                required(editorOf(*page, QStringLiteral("gutter.line_number_width"), "QQuickTextField")),
                "11");
            fx.settle();
        }
        QAccessible::installUpdateHandler(previous);
        REQUIRE(AnnouncementProbe::Said.size() == qsizetype { 2 });
        CHECK(AnnouncementProbe::Said.at(0).contains("from 3 to 10"));
        CHECK(AnnouncementProbe::Said.at(1) == AnnouncementProbe::Said.at(0));
    }

    SECTION("another pane drops it")
    {
        REQUIRE_FALSE(fx.controller->setGlobalField("gutter.line_number_width", 11));
        REQUIRE(banner->isVisible());
        fx.page->setProperty("editorMode", "keybindings");
        CHECK_FALSE(banner->isVisible());
    }

    SECTION("a change to the profile draft drops it")
    {
        fx.controller->newProfile("main");
        fx.page->setProperty("editorMode", "profile");
        REQUIRE_FALSE(fx.controller->saveProfileAs(""));
        REQUIRE(banner->isVisible());
        fx.controller->setProfileField("maximized", true);
        CHECK_FALSE(banner->isVisible());
    }

    SECTION("a change to the colour scheme draft drops it")
    {
        fx.controller->newColorScheme("");
        fx.page->setProperty("editorMode", "scheme");
        REQUIRE_FALSE(fx.controller->saveColorScheme(""));
        REQUIRE(banner->isVisible());
        fx.controller->setSchemeColor("background", "#102030");
        CHECK_FALSE(banner->isVisible());
    }

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("ColorSchemeEditor pins a derived colour on Return only, and lets it follow again",
          "[contour][gui][qml][settings]")
{
    contour::test::QmlMessageCapture const warnings;

    auto fx = PageFixture(OneProfile);
    auto* page = fx.item();
    REQUIRE(page != nullptr);
    auto window = fx.activeWindow(QSize(1000, 800));
    clickButton(page->findChild<QObject*>("newSchemeButton"));
    fx.settle();

    auto const key = QStringLiteral("gutter_text");
    auto const sourceOf = [&fx, &key]() {
        for (auto const& raw: fx.controller->schemeColors())
            if (raw.toMap().value("key").toString() == key)
                return raw.toMap().value("colorSource").toString();
        return QString {};
    };
    // Looked up again each time: a changed draft rebuilds the swatches.
    auto const partOf = [&page, &key](char const* name) -> QQuickItem& {
        auto& swatch = required(findDescendant(*page, [&key](QQuickItem const& candidate) {
            return candidate.property("modelData").toMap().value("key").toString() == key;
        }));
        return required(findDescendant(swatch, [name](QQuickItem const& candidate) {
            return candidate.objectName() == QLatin1String(name);
        }));
    };
    REQUIRE(sourceOf() == "derived");
    CHECK(partOf("schemeColorFollows").isVisible());
    CHECK(partOf("schemeColorFollows").property("text").toString() == "Follows the scheme's other colors");
    CHECK_FALSE(partOf("followSchemeColorButton").isVisible());

    // Passing through the field is no edit.
    TestWindow::focus(partOf("schemeColorField"));
    window.dropFocus();
    fx.settle();
    CHECK(sourceOf() == "derived");

    // Return commits the colour shown, which keeps it from now on.
    TestWindow::focus(partOf("schemeColorField"));
    window.press(Qt::Key_Return);
    fx.settle();
    CHECK(sourceOf() == "pinned");
    CHECK_FALSE(partOf("schemeColorFollows").isVisible());
    CHECK(partOf("followSchemeColorButton").isVisible());
    auto* const followAccessible = QAccessible::queryAccessibleInterface(&partOf("followSchemeColorButton"));
    REQUIRE(followAccessible != nullptr);
    CHECK(followAccessible->text(QAccessible::Name) == "Follow the scheme's other colors again");

    clickButton(&partOf("followSchemeColorButton"));
    fx.settle();
    CHECK(sourceOf() == "derived");

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}
