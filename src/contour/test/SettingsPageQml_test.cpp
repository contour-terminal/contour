// SPDX-License-Identifier: Apache-2.0
//
// End-to-end offscreen test of the settings page: it instantiates the real SettingsPage.qml against a
// real SettingsController (backed by a file store in a temp directory) and drives it through the QML —
// clicking "New profile", typing a name, clicking "Save As" — then asserts a side file appeared and the
// config picked it up. This proves the QML actually wires its controls to the controller; the
// controller's own logic is unit-tested separately in SettingsController_test.cpp.

#include <contour/config/Config.hpp>
#include <contour/config/ConfigEnum.hpp>
#include <contour/config/GuiConfigStore.hpp>
#include <contour/config/UiStyle.hpp>
#include <contour/display/GpuInventory.hpp>
#include <contour/platform/ColorConversion.hpp>
#include <contour/session/TerminalSession.hpp>
#include <contour/test/GuiTestFixtures.hpp>
#include <contour/test/InMemoryGuiStateStore.hpp>
#include <contour/test/QmlChromeStyle.hpp>
#include <contour/test/QmlIncubation.hpp>
#include <contour/test/QmlMessageCapture.hpp>
#include <contour/test/QmlMocks.hpp>
#include <contour/test/SettingsFixtures.hpp>
#include <contour/window/SettingsController.hpp>
#include <contour/window/SettingsWindowHost.hpp>
#include <contour/window/SettingsWindowRules.hpp>
#include <contour/window/WindowController.hpp>

#include <vtbackend/core/ColorPalette.hpp>

#include <core/Utils.hpp>
#include <core/platform/NativeFileSystem.hpp>

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
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>

#include <algorithm>
#include <array>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <ranges>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include <QtTest/QSignalSpy>
#include <QtTest/QTest>
#include <vthost/testing/FaultyFileSystem.hpp>

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

/// The chrome setup of @p row, with the default scheme.
[[nodiscard]] contour::test::ChromeSetup rowSetup(config::ChromeStyle row)
{
    auto setup = contour::test::defaultChromeSetup();
    setup.row = row;
    return setup;
}

/// The Terminal row painted with the built-in colour scheme @p schemeName.
[[nodiscard]] contour::test::ChromeSetup terminalRowSetup(std::string_view schemeName)
{
    auto palette = vtbackend::ColorPalette {};
    REQUIRE(vtbackend::defaultColorPalettes(schemeName, palette));
    auto setup = rowSetup(config::ChromeStyle::Terminal);
    setup.scheme = config::chromeSchemeColorsOf(palette);
    return setup;
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

    /// @return What holds the keyboard focus: the focused item, or the window itself.
    [[nodiscard]] QObject* focusObject() const { return _window->focusObject(); }

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

/// Every descendant of @p root satisfying @p predicate, in the visual tree's order.
[[nodiscard]] std::vector<QQuickItem*> findDescendants(QQuickItem const& root, auto const& predicate)
{
    auto found = std::vector<QQuickItem*> {};
    for (auto* child: root.childItems())
    {
        if (predicate(*child))
            found.push_back(child);
        auto const inside = findDescendants(*child, predicate);
        found.insert(found.end(), inside.begin(), inside.end());
    }
    return found;
}

/// @p item, which the test cannot go on without.
[[nodiscard]] QQuickItem& required(QQuickItem* item)
{
    REQUIRE(item != nullptr);
    return *item;
}

/// Whether the page under test is given a preview terminal.
enum class PreviewSource : uint8_t
{
    None = 0,  ///< No factory: `host.preview` is null, as in the snapshot scene.
    Recording, ///< A factory that records the profile it is asked to run and makes no session.
};

/// The real SettingsPage.qml inside the real Settings window, over a SettingsController, with every delegate
/// it makes incubated.
struct SettingsPageView
{
    QQmlEngine engine;
    IncubationDriver incubation;
    contour::test::InMemoryGuiStateStore state; ///< What the window remembers: nothing, until it closes.
    std::vector<std::string> previewed;         ///< The profiles the recording preview was asked to run.
    window::SettingsWindowHost host;
    QPointer<QObject> page;
    int configurationEdits = 0;              ///< How often "Edit contour.yml…" asked for the editor.
    std::vector<std::string> openedInNewTab; ///< The profiles "Open in new tab" asked for.

    /// @param controller The controller the page binds to. It must outlive the page, which holds a bare
    ///                   pointer to it.
    /// @param setup      The chrome the page is drawn with.
    /// @param preview    Whether the host has a preview terminal.
    explicit SettingsPageView(
        window::SettingsController* controller,
        contour::test::ChromeSetup const& setup = rowSetup(config::ChromeStyle::ContourLinux),
        PreviewSource preview = PreviewSource::None):
        host { *controller,
               window::settingsWindowConventionsFor(config::HostPlatform::Other),
               [this](window::SettingsWindowHost& self) { return window::loadSettingsWindow(engine, self); },
               state,
               preview == PreviewSource::Recording
                   ? window::SettingsPreview::SessionFactory { [this](config::Config const& /*config*/,
                                                                      std::string const& profile) {
                         previewed.push_back(profile);
                         return std::unique_ptr<session::TerminalSession> {};
                     } }
                   : window::SettingsPreview::SessionFactory {},
               [this] { ++configurationEdits; },
               [this](std::string const& profile) { openedInNewTab.push_back(profile); } }
    {
        // The preview is a terminal display; the test engine has no real one.
        qmlRegisterType<contour::test::StubContourTerminal>("Contour.Terminal", 1, 0, "ContourTerminal");
        contour::test::installChromeStyle(engine, setup);
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
    /// @param setup      The chrome the page is drawn with.
    /// @param preview    Whether the host has a preview terminal.
    explicit PageFixture(
        std::string_view yaml,
        std::shared_ptr<config::GuiConfigStore> writeStore = nullptr,
        contour::test::ChromeSetup const& setup = rowSetup(config::ChromeStyle::ContourLinux),
        PreviewSource preview = PreviewSource::None):
        ControllerFixture { yaml, std::move(writeStore) },
        SettingsPageView { controller.get(), setup, preview }
    {
    }

    /// Opens the page @p id names, as the sidebar does, and lets what it built settle.
    /// @return Whether the page knows it.
    bool open(QString const& id)
    {
        auto opened = QVariant {};
        REQUIRE(QMetaObject::invokeMethod(
            page.get(), "openPage", Q_RETURN_ARG(QVariant, opened), Q_ARG(QVariant, QVariant { id })));
        settle();
        return opened.toBool();
    }

    /// Opens the page that holds the global field @p key.
    void openGlobal(QString const& key)
    {
        auto group = -1;
        for (auto const& raw: controller->globalFields())
            if (raw.toMap().value("key").toString() == key)
                group = raw.toMap().value("group").toInt();
        REQUIRE(group >= 0);
        for (auto const& rawSection: controller->sections())
            for (auto const& rawGroup: rawSection.toMap().value("groups").toList())
                if (rawGroup.toMap().value("id").toInt() == group)
                    REQUIRE(open("section:" + rawSection.toMap().value("id").toString()));
    }

    /// Adds a profile through the sidebar's "+", which opens its page, and lets the debounce write it, which
    /// is when its row joins the sidebar.
    void addProfile()
    {
        clickButton(&required(byName("addProfileButton")));
        scheduler->fire();
        settle();
    }

    /// @return The item named @p name beneath the page, or nullptr.
    [[nodiscard]] QQuickItem* byName(QString const& name) const
    {
        return findDescendant(
            *item(), [&name](QQuickItem const& candidate) { return candidate.objectName() == name; });
    }

    /// @return The control of field @p key, or nullptr when the page draws none.
    [[nodiscard]] QQuickItem* controlOf(QString const& key) const { return byName("settingsControl:" + key); }

    /// @return The text field inside control @p key, or nullptr.
    [[nodiscard]] QQuickItem* fieldOf(QString const& key) const
    {
        auto const* control = controlOf(key);
        return control != nullptr ? findDescendant(*control,
                                                   [](QQuickItem const& candidate) {
                                                       return candidate.inherits("QQuickTextField");
                                                   })
                                  : nullptr;
    }
};

constexpr auto OneProfile =
    std::string_view { "default_profile: main\nprofiles:\n    main:\n        show_title_bar: true\n" };

/// Builds SettingsRow.qml rows on their own, as a group's Repeater builds them.
struct SettingsRowFactory
{
    QQmlEngine engine;
    std::unique_ptr<QQmlComponent> component;

    SettingsRowFactory()
    {
        contour::test::installChromeStyle(engine);
        component = std::make_unique<QQmlComponent>(
            &engine, QUrl(QStringLiteral("qrc:/qt/qml/Contour/Ui/SettingsRow.qml")));
        INFO("component errors: " << component->errorString().toStdString());
        REQUIRE(component->isReady());
    }

    /// A row of @p field, a map as the controller lists them; its key and label are "key" and "Label" unless
    /// given, and its control a text field unless it names another.
    [[nodiscard]] std::unique_ptr<QQuickItem> make(QVariantMap field, bool editable = true) const
    {
        if (!field.contains("key"))
            field.insert("key", "key");
        if (!field.contains("label"))
            field.insert("label", "Label");
        auto row =
            std::unique_ptr<QQuickItem>(qobject_cast<QQuickItem*>(component->createWithInitialProperties(
                QVariantMap { { "field", field }, { "editable", editable } })));
        REQUIRE(row != nullptr);
        return row;
    }
};

/// The global row @p key of @p controller.
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

    /// What the read-only directory holds, as the loader read it.
    std::map<config::SideFile, std::string> files;

    [[nodiscard]] std::expected<std::optional<std::string>, std::string> read(
        config::SideFile const& file) const override
    {
        auto const found = files.find(file);
        return found != files.end() ? std::optional { found->second } : std::nullopt;
    }
    [[nodiscard]] std::expected<void, std::string> write(config::SideFile const& /*file*/,
                                                         std::string_view /*bytes*/) override
    {
        return refuse();
    }
    [[nodiscard]] std::expected<void, std::string> remove(config::SideFile const& /*file*/) override
    {
        return refuse();
    }
    [[nodiscard]] std::expected<std::vector<config::SideFile>, std::string> list(
        config::SideFileKind /*kind*/) const override
    {
        return std::vector<config::SideFile> {};
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

/// The text of the one text field inside @p control, or an empty string.
[[nodiscard]] std::string shown(QQuickItem const* control)
{
    REQUIRE(control != nullptr);
    return required(findDescendant(*control,
                                   [](QQuickItem const& item) { return item.inherits("QQuickTextField"); }))
        .property("text")
        .toString()
        .toStdString();
}

/// Whether @p item lies inside the visible part of the Flickable @p scroll holds it.
[[nodiscard]] bool isInView(QQuickItem const& item, QQuickItem const& scroll)
{
    auto const top = item.mapToItem(&scroll, QPointF { 0, 0 }).y();
    return top >= 0 && top + item.height() <= scroll.height();
}

} // namespace

TEST_CASE("Settings lists the five sections, the profiles, then Edit contour.yml…",
          "[contour][gui][qml][settings]")
{
    auto const row = GENERATE(config::ChromeStyle::MacOS, config::ChromeStyle::Terminal);
    contour::test::QmlMessageCapture const warnings;
    auto fx = PageFixture(OneProfile, nullptr, rowSetup(row));
    fx.settle();

    // Opens on the first section, and the sidebar agrees.
    CHECK(fx.page->property("currentPage").toString() == "section:general");

    auto const* list = fx.byName("settingsSidebarList");
    REQUIRE(list != nullptr);
    auto const rows =
        findDescendants(*list->property("contentItem").value<QQuickItem*>(), [](QQuickItem const& item) {
            auto const name = item.objectName();
            return name.startsWith("sidebarRow:") || name.startsWith("profileRow:")
                   || name == "editConfigRow";
        });
    auto names = QStringList {};
    for (auto const* sidebarRow: rows)
        names << sidebarRow->objectName();
    auto expected = QStringList {};
    for (auto const& section: fx.controller->sections())
        expected << "sidebarRow:" + section.toMap().value("id").toString();
    CHECK(expected.size() == qsizetype { 5 });
    INFO("sidebar: " << names.join(", ").toStdString());
    for (auto const& profile: fx.controller->profiles())
        expected << "profileRow:" + profile.toMap().value("name").toString();
    expected << "editConfigRow";
    CHECK(names == expected);
    CHECK(names.front() == "sidebarRow:general");
    CHECK(names.at(4) == "sidebarRow:profiles");
    CHECK(names.back() == "editConfigRow");

    // The selected row is the page shown, and nothing else claims to be.
    CHECK(fx.byName("sidebarRow:general")->property("isSelected").toBool());
    CHECK_FALSE(fx.byName("sidebarRow:appearance")->property("isSelected").toBool());
    CHECK_FALSE(fx.byName("profileRow:main")->property("isSelected").toBool());

    // "Edit contour.yml…" opens the editor, and the host is where the OS is reached.
    REQUIRE(fx.configurationEdits == 0);
    auto window = fx.activeWindow(QSize(1000, 800));
    fx.settle();
    QTest::mouseClick(fx.host.window(),
                      Qt::LeftButton,
                      Qt::NoModifier,
                      fx.byName("editConfigRow")->mapToScene(QPointF { 4, 4 }).toPoint());
    CHECK(fx.configurationEdits == 1);

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("Settings creates a profile through the sidebar's plus and it lands on disk (offscreen)",
          "[contour][gui][qml][settings]")
{
    contour::test::QmlMessageCapture const warnings;
    auto fx = PageFixture(OneProfile);
    fx.addProfile();

    // Staged under "New Profile", opened, and shown.
    CHECK(fx.controller->editingProfile() == "New Profile");
    CHECK(fx.page->property("currentPage").toString() == "profile:New Profile");
    CHECK_FALSE(fx.controller->editingReadOnly());
    CHECK(fx.byName("profileRow:New Profile")->property("isSelected").toBool());

    // The debounce wrote it: the side file exists, and the reloaded config carries it.
    CHECK(std::filesystem::exists(fx.configDir / "profiles" / "New Profile.yml"));
    CHECK(fx.cfg.findProfile("New Profile") != nullptr);

    SECTION("the minus asks first, and removes it once confirmed")
    {
        clickButton(fx.byName("removeProfileButton"));
        fx.settle();
        auto* dialog = fx.page->findChild<QObject*>("deleteDialog");
        REQUIRE(dialog != nullptr);
        CHECK(dialog->property("message").toString().contains("New Profile"));
        QMetaObject::invokeMethod(dialog, "confirmed");
        fx.scheduler->fire();
        fx.settle();
        CHECK(fx.cfg.findProfile("New Profile") == nullptr);
        // Its page is gone with it.
        CHECK(fx.page->property("currentPage").toString() == "section:profiles");
    }
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("a control change reaches the controller and Revert enables", "[contour][gui][qml][settings]")
{
    auto const row = GENERATE(config::ChromeStyle::MacOS, config::ChromeStyle::Terminal);
    contour::test::QmlMessageCapture const warnings;
    auto fx = PageFixture(OneProfile, nullptr, rowSetup(row));
    auto* revert = fx.byName("revertChangesButton");
    REQUIRE(revert != nullptr);
    CHECK_FALSE(revert->isEnabled());

    auto const key = QStringLiteral("reflow_on_resize");
    auto const before = fx.cfg.reflowOnResize.value();
    auto* toggle = fx.controlOf(key);
    REQUIRE(toggle != nullptr);
    REQUIRE(toggle->property("checked").toBool() == before);

    // As the user does it: focus the switch and press Space.
    auto window = fx.activeWindow(QSize(1000, 800));
    TestWindow::focus(*toggle);
    window.press(Qt::Key_Space);
    fx.settle();
    CHECK(revert->isEnabled());
    CHECK(globalRow(*fx.controller, key).value("overridden").toBool());

    fx.scheduler->fire();
    fx.settle();
    CHECK(fx.cfg.reflowOnResize.value() == !before);
    CHECK(fx.controlOf(key)->property("checked").toBool() == !before);
    CHECK(revert->isEnabled()); // written, and still Revert's to put back

    clickButton(revert);
    fx.settle();
    CHECK(fx.controlOf(key)->property("checked").toBool() == before);
    CHECK_FALSE(revert->isEnabled());
    CHECK(fx.cfg.reflowOnResize.value() == before);

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("a contour.yml profile shows its banner and disabled controls", "[contour][gui][qml][settings]")
{
    // Review Focus 3.
    auto const row = GENERATE(config::ChromeStyle::MacOS, config::ChromeStyle::Terminal);
    contour::test::QmlMessageCapture const warnings;
    auto fx = PageFixture(OneProfile, nullptr, rowSetup(row));
    auto* banner = fx.byName("readOnlyBanner");
    REQUIRE(banner != nullptr);
    CHECK_FALSE(banner->isVisible()); // on a section page

    REQUIRE(fx.open("profile:main"));
    CHECK(banner->isVisible());
    CHECK(banner->property("text").toString().contains("Defined in contour.yml"));

    auto const controls = findDescendants(
        *fx.item(), [](QQuickItem const& item) { return item.objectName().startsWith("settingsControl:"); });
    REQUIRE_FALSE(controls.empty());
    for (auto const* control: controls)
    {
        INFO(control->objectName().toStdString());
        CHECK_FALSE(control->isEnabled());
    }

    auto* duplicate = fx.byName("duplicateToEditButton");
    REQUIRE(duplicate != nullptr);
    clickButton(duplicate);
    fx.scheduler->fire();
    fx.settle();
    CHECK(fx.controller->editingProfile() == "main copy");
    CHECK(fx.page->property("currentPage").toString() == "profile:main copy");
    CHECK(fx.byName("profileRow:main copy")->property("isSelected").toBool());
    CHECK_FALSE(banner->isVisible());
    for (auto const* control: findDescendants(*fx.item(), [](QQuickItem const& item) {
             return item.objectName().startsWith("settingsControl:");
         }))
        CHECK(control->isEnabled());

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("a locked configuration disables every control and hides Revert", "[contour][gui][qml][settings]")
{
    contour::test::QmlMessageCapture const warnings;
    auto fx = PageFixture("default_profile: main\ngui_config_locked: true\nprofiles:\n    main:\n"
                          "        show_title_bar: true\n");
    REQUIRE(fx.controller->locked());

    auto* locked = fx.byName("lockedBanner");
    REQUIRE(locked != nullptr);
    CHECK(locked->isVisible());
    CHECK_FALSE(fx.byName("revertChangesButton")->isVisible());
    CHECK_FALSE(fx.byName("addProfileButton")->isEnabled());

    for (auto const* id: { "section:general", "profile:main" })
    {
        INFO(id);
        REQUIRE(fx.open(id));
        auto const controls = findDescendants(*fx.item(), [](QQuickItem const& item) {
            return item.objectName().startsWith("settingsControl:");
        });
        REQUIRE_FALSE(controls.empty());
        for (auto const* control: controls)
            CHECK_FALSE(control->isEnabled());
    }
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("an overridden global shows Modified · Reset", "[contour][gui][qml][settings]")
{
    contour::test::QmlMessageCapture const warnings;
    auto fx = PageFixture(OneProfile);
    {
        auto out = std::ofstream(fx.configDir / "settings.yml");
        out << "theme: dark\n";
    }
    fx.reload();
    fx.settle();
    fx.openGlobal("theme");

    auto* reset = fx.byName("modifiedResetButton:theme");
    REQUIRE(reset != nullptr);
    CHECK(reset->isVisible());
    CHECK(reset->property("text").toString() == "Modified · Reset");
    // A setting nothing overrides has none.
    CHECK_FALSE(fx.byName("modifiedResetButton:ui_style")->isVisible());

    clickButton(reset);
    fx.scheduler->fire();
    fx.settle();
    CHECK_FALSE(globalRow(*fx.controller, "theme").value("overridden").toBool());
    CHECK_FALSE(fx.byName("modifiedResetButton:theme")->isVisible());

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("search finds a row on another page", "[contour][gui][qml][settings]")
{
    contour::test::QmlMessageCapture const warnings;
    auto fx = PageFixture(OneProfile);
    REQUIRE(fx.open("profile:main"));
    REQUIRE(fx.open("section:general"));
    auto window = fx.activeWindow(QSize(1000, 800));
    fx.settle();

    auto* search = fx.byName("settingsSearchField");
    REQUIRE(search != nullptr);
    TestWindow::focus(*search);
    window.type("scrollback");
    fx.settle();

    auto* results = fx.byName("settingsSearchResults");
    REQUIRE(results != nullptr);
    CHECK(results->isVisible());
    CHECK(results->property("count").toInt() > 0);
    CHECK_FALSE(fx.byName("settingsSidebarList")->isVisible());

    window.press(Qt::Key_Return);
    fx.settle();
    // The profile page, with the row of the first result in view.
    auto const page = fx.page->property("currentPage").toString();
    CHECK(page == "profile:main");
    auto const hit = fx.controller->searchSettings("scrollback").first().toMap();
    auto* row = fx.byName("settingsRow:" + hit.value("key").toString());
    REQUIRE(row != nullptr);
    auto* scroll = fx.byName("settingsPageLoader")->property("item").value<QQuickItem*>();
    REQUIRE(scroll != nullptr);
    CHECK(isInView(*row, *scroll));

    // Escape clears the search, and the sidebar is back.
    TestWindow::focus(*search);
    window.press(Qt::Key_Escape);
    CHECK(search->property("text").toString().isEmpty());
    CHECK(fx.byName("settingsSidebarList")->isVisible());

    SECTION("a query that finds nothing says so")
    {
        TestWindow::focus(*search);
        window.type("asparagus");
        CHECK(fx.byName("settingsNoResults")->isVisible());
    }
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("the keyboard alone reaches every section and every control", "[contour][gui][qml][settings]")
{
    // Review Focus 5.
    contour::test::QmlMessageCapture const warnings;
    auto fx = PageFixture(OneProfile);
    auto window = fx.activeWindow(QSize(1000, 800));
    fx.settle();

    auto* sidebar = fx.byName("settingsSidebarList");
    REQUIRE(sidebar != nullptr);
    TestWindow::focus(*sidebar);
    for ([[maybe_unused]] auto const press: std::views::iota(0, 4))
        window.press(Qt::Key_Down);
    fx.settle();
    CHECK(fx.page->property("currentPage").toString() == "section:shell-integration");
    CHECK(fx.byName("sidebarRow:shell-integration")->property("isSelected").toBool());

    // What the page lists, in the order of the controller's list: every global field of the section's groups.
    auto expected = QStringList {};
    auto const sections = fx.controller->sections();
    auto const section = std::ranges::find_if(sections, [](QVariant const& raw) {
        return raw.toMap().value("id").toString() == "shell-integration";
    });
    REQUIRE(section != sections.end());
    for (auto const& group: section->toMap().value("groups").toList())
        for (auto const& field: fx.controller->globalFields())
            if (field.toMap().value("group").toInt() == group.toMap().value("id").toInt())
                expected << field.toMap().value("key").toString();
    REQUIRE(expected.size() > 1);
    {
        auto inListOrder = QStringList {};
        for (auto const& field: fx.controller->globalFields())
            if (expected.contains(field.toMap().value("key").toString()))
                inListOrder << field.toMap().value("key").toString();
        CHECK(inListOrder == expected);
    }

    // Tab leaves the sidebar and walks the page's controls, each exactly once.
    auto visited = QStringList {};
    auto const controlOfFocus = [&window]() -> QString {
        for (auto* item = qobject_cast<QQuickItem*>(window.focusObject()); item != nullptr;
             item = item->parentItem())
            if (item->objectName().startsWith("settingsControl:"))
                return item->objectName().mid(QStringLiteral("settingsControl:").size());
        return {};
    };
    for ([[maybe_unused]] auto const press: std::views::iota(0, (4 * static_cast<int>(expected.size())) + 16))
    {
        window.press(Qt::Key_Tab);
        auto const key = controlOfFocus();
        if (!key.isEmpty() && (visited.isEmpty() || visited.back() != key))
            visited << key;
    }
    // After the last control focus goes round again, so the first lap is what is compared.
    visited = visited.mid(0, expected.size());
    CHECK(visited == expected);

    // Space on a focused switch toggles it.
    auto const key = QStringLiteral("gutter.exit_status");
    REQUIRE(expected.contains(key));
    auto* toggle = fx.controlOf(key);
    REQUIRE(toggle != nullptr);
    auto const before = toggle->property("checked").toBool();
    TestWindow::focus(*toggle);
    window.press(Qt::Key_Space);
    fx.settle();
    CHECK(globalRow(*fx.controller, key).value("overridden").toBool());
    fx.scheduler->fire();
    fx.settle();
    CHECK(fx.controlOf(key)->property("checked").toBool() == !before);

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("the Terminal row's page fills the window", "[contour][gui][qml][settings]")
{
    // R-o: the page is the window's content, whatever size the window has -- not a whole number of cells.
    contour::test::QmlMessageCapture const warnings;
    auto fx = PageFixture(OneProfile, nullptr, rowSetup(config::ChromeStyle::Terminal));
    auto* settings = fx.host.window();
    REQUIRE(settings != nullptr);
    auto window = fx.activeWindow(QSize(1872, 1430));
    fx.settle();

    CHECK(fx.item()->height() == settings->contentItem()->height());
    CHECK(fx.item()->width() == settings->contentItem()->width());
    CHECK(fx.item()->height() == 1430);
    CHECK(fx.item()->width() == 1872);

    // The page's bottom is the window's: no band of cells is left below it.
    auto* loader = fx.byName("settingsPageLoader");
    REQUIRE(loader != nullptr);
    CHECK(loader->mapToItem(fx.item(), QPointF { 0, loader->height() }).y() == 1430);
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("every settings row's control carries its label and help", "[contour][gui][qml][settings][a11y]")
{
    // Review Focus 5. A row's visible label is a sibling of its control, so without a name of its own the
    // control reaches a screen reader as an unlabelled control that can only say what KIND it is. Asserted
    // for every row of General and of a profile page, so a new kind of row cannot reintroduce it.
    auto const row = GENERATE(config::ChromeStyle::MacOS, config::ChromeStyle::Terminal);
    contour::test::QmlMessageCapture const warnings;
    auto fx = PageFixture(OneProfile, nullptr, rowSetup(row));

    auto const checkPage = [&fx](QString const& id) {
        INFO("page: " << id.toStdString());
        REQUIRE(fx.open(id));
        auto const rows = findDescendants(
            *fx.item(), [](QQuickItem const& item) { return item.objectName().startsWith("settingsRow:"); });
        REQUIRE_FALSE(rows.empty());
        for (auto const* settingsRow: rows)
        {
            INFO(settingsRow->objectName().toStdString());
            auto const key = settingsRow->objectName().mid(QStringLiteral("settingsRow:").size());
            auto const label = settingsRow->property("field").toMap().value("label").toString();
            auto const help = settingsRow->property("field").toMap().value("help").toString();
            auto* control = fx.controlOf(key);
            REQUIRE(control != nullptr);
            auto* const accessible = QAccessible::queryAccessibleInterface(control);
            REQUIRE(accessible != nullptr);
            CHECK(accessible->text(QAccessible::Name) == label);
            CHECK(accessible->text(QAccessible::Description) == help);
        }
    };
    checkPage("section:general");
    checkPage("profile:main");

    // The sidebar rows are list items named by their titles, inside a list named "Settings sections".
    REQUIRE(fx.open("section:general"));
    auto* sidebar = fx.byName("settingsSidebarList");
    REQUIRE(sidebar != nullptr);
    auto* const list = QAccessible::queryAccessibleInterface(sidebar);
    REQUIRE(list != nullptr);
    CHECK(list->role() == QAccessible::List);
    CHECK(list->text(QAccessible::Name) == "Settings sections");
    for (auto const& raw: fx.controller->sections())
    {
        auto* sidebarRow = fx.byName("sidebarRow:" + raw.toMap().value("id").toString());
        REQUIRE(sidebarRow != nullptr);
        auto* const accessible = QAccessible::queryAccessibleInterface(sidebarRow);
        REQUIRE(accessible != nullptr);
        CHECK(accessible->role() == QAccessible::ListItem);
        CHECK(accessible->text(QAccessible::Name) == raw.toMap().value("title").toString());
    }
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

namespace
{
/// @p top painted over @p bottom.
[[nodiscard]] QColor over(QColor top, QColor bottom)
{
    auto const alpha = top.alphaF();
    return QColor::fromRgbF((top.redF() * alpha) + (bottom.redF() * (1 - alpha)),
                            (top.greenF() * alpha) + (bottom.greenF() * (1 - alpha)),
                            (top.blueF() * alpha) + (bottom.blueF() * (1 - alpha)));
}

/// The contrast ratio of @p ink over @p ground.
[[nodiscard]] double contrast(QColor ink, QColor ground)
{
    return vtbackend::contrastRatio(contour::platform::toRGBColor(over(ink, ground)),
                                    contour::platform::toRGBColor(ground));
}

/// The rows the contrast test visits: the three native rows, then every bundled colour scheme on the Terminal
/// row.
[[nodiscard]] std::vector<std::string_view> contrastRows()
{
    auto names = std::vector<std::string_view> { "macos", "fluent", "linux" };
    for (auto const name: vtbackend::defaultColorPaletteNames())
        names.push_back(name);
    return names;
}
} // namespace

TEST_CASE("every ink of the Settings page reads on what it sits on, on every row, scheme and appearance",
          "[contour][gui][qml][settings][contrast]")
{
    // Review Focus 3, for what Task 10 draws: the rows' labels and help, the group titles, "Modified ·
    // Reset", a row's problem, the page title, and the sidebar's rows.
    auto const appearance = GENERATE(vtbackend::ColorPreference::Light, vtbackend::ColorPreference::Dark);
    auto const rowName = GENERATE(from_range(contrastRows()));
    INFO("appearance: " << (appearance == vtbackend::ColorPreference::Light ? "light" : "dark")
                        << ", row: " << rowName);
    contour::test::ScopedChromeAppearance const forced { appearance };
    contour::test::QmlMessageCapture const warnings;

    auto setup = rowSetup(config::ChromeStyle::ContourLinux);
    if (rowName == "macos")
        setup.row = config::ChromeStyle::MacOS;
    else if (rowName == "fluent")
        setup.row = config::ChromeStyle::Fluent;
    else if (rowName == "linux")
        setup.row = config::ChromeStyle::ContourLinux;
    else
        setup = terminalRowSetup(rowName);
    auto const isTerminal = setup.row == config::ChromeStyle::Terminal;

    auto fx = PageFixture(OneProfile, nullptr, setup);
    {
        auto out = std::ofstream(fx.configDir / "settings.yml");
        out << "theme: dark\n"; // an override, so a row offers "Modified · Reset"
    }
    fx.reload();
    fx.openGlobal("theme");

    auto const token = [&fx](char const* name) {
        auto* style =
            fx.engine.rootContext()->contextProperty(QStringLiteral("chromeStyle")).value<QObject*>();
        return style->property("colors").value<QObject*>()->property(name).value<QColor>();
    };
    auto const colorOf = [](QQuickItem const* item) {
        return item->property("color").value<QColor>();
    };
    auto const window = QGuiApplication::palette().color(QPalette::Window);
    auto const page = over(token("settingsPageFill"), window);
    auto const group = over(token("settingsGroupFill"), page);
    auto const sidebar = over(token("settingsSidebarFill"), window);
    constexpr auto TextContrast = 4.5;

    // A row's texts, on the group.
    auto* row = fx.byName("settingsRow:theme");
    REQUIRE(row != nullptr);
    auto const part = [&](QQuickItem const& in, char const* name) -> QQuickItem const& {
        return required(findDescendant(
            in, [name](QQuickItem const& item) { return item.objectName() == QLatin1String(name); }));
    };
    CHECK(contrast(colorOf(&part(*row, "settingsRowLabel")), group) >= TextContrast);
    CHECK(contrast(colorOf(&part(*row, "settingsRowHelp")), group) >= TextContrast);
    // The reason a value is refused, in the chrome's error colour.
    auto* problem = const_cast<QQuickItem*>(&part(*row, "settingsRowProblem"));
    row->setProperty("field", [&] {
        auto field = row->property("field").toMap();
        field.insert("problem", "an unknown placeholder");
        return field;
    }());
    REQUIRE(problem->isVisible());
    CHECK(contrast(colorOf(problem), group) >= TextContrast);

    // "Modified · Reset": the label of the button, which sits on the group.
    auto const* reset = fx.byName("modifiedResetButton:theme");
    REQUIRE(reset != nullptr);
    REQUIRE(reset->isVisible());
    CHECK(contrast(colorOf(reset->property("contentItem").value<QQuickItem*>()), group) >= TextContrast);

    // The group's title sits on the page, and the page's title on its header.
    CHECK(contrast(colorOf(&part(*fx.item(), "settingsGroupTitle")), page) >= TextContrast);
    auto const header = over(colorOf(fx.byName("settingsHeader")), window);
    CHECK(contrast(colorOf(&part(*fx.item(), "settingsPageTitle")), header) >= TextContrast);

    // The sidebar: an unselected row's title, on the sidebar. The selected row is the OS's own highlight pair
    // on the native rows (the ruling of Task 8), so only the Terminal row's, which the scheme decides, is
    // held to the ratio.
    auto const* keyboardRow = fx.byName("sidebarRow:keyboard");
    REQUIRE(keyboardRow != nullptr);
    auto const* title = findDescendant(*keyboardRow, [](QQuickItem const& item) {
        return item.inherits("QQuickText") && item.property("text").toString() == "Keyboard";
    });
    REQUIRE(title != nullptr);
    CHECK(contrast(colorOf(title), sidebar) >= TextContrast);
    auto const* selectedRow = fx.byName("sidebarRow:appearance"); // the page of the global it opened
    if (isTerminal)
        CHECK(contrast(token("settingsSelectionText"), over(colorOf(selectedRow), sidebar)) >= TextContrast);
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("a profile page groups its fields, and the groups lose none", "[contour][gui][qml][settings]")
{
    contour::test::QmlMessageCapture const warnings;
    auto fx = PageFixture(OneProfile);
    fx.addProfile();

    // A group none of whose fields the profile has -- the colours, which have their own page -- is not shown.
    auto const groups = findDescendants(*fx.item(), [](QQuickItem const& item) {
        return item.objectName().startsWith("settingsGroup:") && item.isVisible();
    });
    REQUIRE(groups.size() > 1);
    auto total = 0;
    for (auto const* group: groups)
    {
        INFO(group->objectName().toStdString());
        auto const rows = findDescendants(
            *group, [](QQuickItem const& item) { return item.objectName().startsWith("settingsRow:"); });
        CHECK_FALSE(rows.empty()); // an empty group is not shown
        total += static_cast<int>(rows.size());
        auto const* title = findDescendant(
            *group, [](QQuickItem const& item) { return item.objectName() == "settingsGroupTitle"; });
        REQUIRE(title != nullptr);
        CHECK_FALSE(title->property("text").toString().isEmpty());
    }
    CHECK(total == fx.controller->profileFields().size());
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("a profile page opens the pages of what is not a scalar", "[contour][gui][qml][settings]")
{
    contour::test::QmlMessageCapture const warnings;
    auto fx = PageFixture(OneProfile);
    fx.addProfile();
    auto const profile = QStringLiteral("profile:New Profile");

    for (auto const* kind: { "schemes", "links", "statusline" })
    {
        INFO(kind);
        REQUIRE(fx.open(profile));
        auto* link = fx.byName(QStringLiteral("profileSubpage:") + QString::fromLatin1(kind));
        REQUIRE(link != nullptr);
        QMetaObject::invokeMethod(link, "forceActiveFocus");
        auto window = fx.activeWindow(QSize(1000, 800));
        TestWindow::focus(*link);
        window.press(Qt::Key_Return);
        fx.settle();
        CHECK(fx.page->property("currentPage").toString() == profile + ":" + QString::fromLatin1(kind));
        CHECK(fx.byName("profileRow:New Profile")->property("isSelected").toBool());
    }
    CHECK_FALSE(fx.open(profile + ":nonsense"));
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("a profile page opens on a live preview of the profile", "[contour][gui][qml][settings][preview]")
{
    auto const row = GENERATE(config::ChromeStyle::MacOS, config::ChromeStyle::Terminal);
    contour::test::QmlMessageCapture const warnings;
    auto fx = PageFixture(OneProfile, nullptr, rowSetup(row), PreviewSource::Recording);
    REQUIRE(fx.previewed.empty());

    REQUIRE(fx.open("profile:main"));

    // Opening the page asks the preview to show the profile.
    REQUIRE_FALSE(fx.previewed.empty());
    CHECK(fx.previewed.back() == "main");

    // The hero is the style's token high, and its caption is the preview's.
    auto const* hero = fx.byName("settingsPreview");
    REQUIRE(hero != nullptr);
    CHECK(hero->isVisible());
    auto const* style = fx.engine.rootContext()->contextProperty("chromeStyle").value<QObject*>();
    auto const previewHeight =
        style->property("settings").value<QObject*>()->property("previewHeight").toReal();
    CHECK(previewHeight > 0);
    CHECK(hero->height() == previewHeight);
    auto const* caption = fx.byName("settingsPreviewCaption");
    REQUIRE(caption != nullptr);
    REQUIRE_FALSE(fx.host.preview()->caption().isEmpty());
    CHECK(caption->property("text").toString() == fx.host.preview()->caption());

    // Nothing in it takes the keyboard, and a screen reader hears it as a picture named by its caption.
    CHECK_FALSE(hero->property("enabled").toBool());
    CHECK_FALSE(hero->property("activeFocusOnTab").toBool());
    auto const* accessible = QAccessible::queryAccessibleInterface(const_cast<QQuickItem*>(hero));
    REQUIRE(accessible != nullptr);
    CHECK(accessible->role() == QAccessible::Graphic);
    CHECK(accessible->text(QAccessible::Name) == fx.host.preview()->caption());
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("Fluent's profile page offers Duplicate and Open in new tab beside the preview",
          "[contour][gui][qml][settings][preview]")
{
    contour::test::QmlMessageCapture const warnings;
    auto fx =
        PageFixture(OneProfile, nullptr, rowSetup(config::ChromeStyle::Fluent), PreviewSource::Recording);
    REQUIRE(fx.open("profile:main"));

    auto const* open = fx.byName("previewOpenInNewTabButton");
    REQUIRE(open != nullptr);
    CHECK(open->isVisible());
    CHECK(open->property("text").toString() == "Open in new tab");
    clickButton(const_cast<QQuickItem*>(open));
    CHECK(fx.openedInNewTab == std::vector<std::string> { "main" });

    // Duplicate works on a read-only profile too: it is how one is made editable.
    auto const* duplicate = fx.byName("previewDuplicateButton");
    REQUIRE(duplicate != nullptr);
    CHECK(duplicate->property("text").toString() == "Duplicate");
    clickButton(const_cast<QQuickItem*>(duplicate));
    fx.settle();
    CHECK(fx.controller->editingProfile() == "main copy");
    CHECK(fx.page->property("currentPage").toString() == "profile:main copy");
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("the other rows' profile pages have no such buttons", "[contour][gui][qml][settings][preview]")
{
    auto const row = GENERATE(
        config::ChromeStyle::MacOS, config::ChromeStyle::ContourLinux, config::ChromeStyle::Terminal);
    auto fx = PageFixture(OneProfile, nullptr, rowSetup(row), PreviewSource::Recording);
    REQUIRE(fx.open("profile:main"));
    for (auto const* name: { "previewOpenInNewTabButton", "previewDuplicateButton" })
    {
        auto const* button = fx.byName(QString::fromLatin1(name));
        REQUIRE(button != nullptr);
        CHECK_FALSE(button->isVisible());
    }
}

TEST_CASE("a profile page without a preview terminal still shows the frame, silently",
          "[contour][gui][qml][settings][preview]")
{
    auto const row = GENERATE(config::ChromeStyle::MacOS, config::ChromeStyle::Terminal);
    contour::test::QmlMessageCapture const warnings;
    auto fx = PageFixture(OneProfile, nullptr, rowSetup(row));
    REQUIRE(fx.host.preview() == nullptr);

    REQUIRE(fx.open("profile:main"));

    auto const* hero = fx.byName("settingsPreview");
    REQUIRE(hero != nullptr);
    CHECK(hero->isVisible());
    CHECK(hero->height() > 0);
    CHECK(fx.byName("settingsPreviewCaption")->property("text").toString().isEmpty());
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("the Links page names the gesture that opens links", "[contour][gui][qml][settings][links]")
{
    contour::test::QmlMessageCapture const warnings;
    auto fx = PageFixture(OneProfile);
    fx.addProfile();
    REQUIRE(fx.open("profile:New Profile:links"));

    // The text is the controller's, which reads input_mapping and the bypass modifier: nothing in the QML
    // spells a gesture.
    auto const* label = fx.byName("linkGestureLabel");
    REQUIRE(label != nullptr);
    CHECK(!fx.controller->linkGesture().isEmpty());
    CHECK(label->property("text").toString() == fx.controller->linkGesture());
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("the Status Line page's indicator editor is wired to the controller",
          "[contour][gui][qml][settings]")
{
    // NOTE ON WHAT THIS CHECKS. It drives the segment rows' signals rather than the chips: those are the
    // handlers the chips invoke, so the wiring under test is the same. What each chip *renders* is covered by
    // StatusLineChip.qml being a pure function of its inputs, and the editing behaviour itself by
    // SettingsController_test.
    contour::test::QmlMessageCapture const warnings;
    auto fx = PageFixture(OneProfile);
    fx.addProfile(); // a GUI-owned profile, so the draft is editable
    REQUIRE_FALSE(fx.controller->editingReadOnly());
    REQUIRE(fx.open("profile:New Profile:statusline"));
    auto* item = fx.item();

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

TEST_CASE("the Status Line page's indicator editor is inert on a read-only profile",
          "[contour][gui][qml][settings]")
{
    contour::test::QmlMessageCapture const warnings;
    auto fx = PageFixture(OneProfile);

    // "main" comes from contour.yml, so the page shows it read-only.
    REQUIRE(fx.open("profile:main:statusline"));
    REQUIRE(fx.controller->editingReadOnly());

    auto* left = fx.item()->findChild<QObject*>("indicatorSegmentLeft");
    REQUIRE(left != nullptr);
    CHECK(!left->property("editable").toBool()); // the chips and Add button are disabled

    // And driving the handlers anyway changes nothing: the controller is the boundary that has to hold, not
    // the enabled state of a button.
    auto const before = fx.controller->indicatorSegment(0);
    QMetaObject::invokeMethod(left, "removeRequested", Q_ARG(int, 0));
    QMetaObject::invokeMethod(left, "rawCommitted", Q_ARG(QString, QStringLiteral("{Clock}")));
    fx.settle();
    CHECK(fx.controller->indicatorSegment(0) == before);
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("the Links page's pattern list is inert on a read-only profile",
          "[contour][gui][qml][settings][links]")
{
    contour::test::QmlMessageCapture const warnings;
    auto fx = PageFixture(OneProfile);
    REQUIRE(fx.open("profile:main:links"));
    REQUIRE(fx.controller->editingReadOnly());

    auto* list = fx.item()->findChild<QObject*>("linkPatternList");
    REQUIRE(list != nullptr);
    CHECK_FALSE(list->property("editable").toBool());
    auto const* add = fx.item()->findChild<QObject*>("addLinkPatternButton");
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

TEST_CASE("the Links page's pattern list adds, previews and removes patterns",
          "[contour][gui][qml][settings][links]")
{
    contour::test::QmlMessageCapture const warnings;
    auto fx = PageFixture(OneProfile);
    fx.addProfile();
    REQUIRE_FALSE(fx.controller->editingReadOnly());
    REQUIRE(fx.open("profile:New Profile:links"));
    auto* item = fx.item();

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
    auto const* shownProblem = findDescendant(*item, [](QQuickItem const& label) {
        return label.objectName() == "linkPatternProblem" && !label.property("text").toString().isEmpty();
    });
    REQUIRE(shownProblem != nullptr);
    CHECK(shownProblem->property("text").toString() == added.value("problem").toString());
    auto const* style = fx.engine.rootContext()->contextProperty("chromeStyle").value<QObject*>();
    REQUIRE(style != nullptr);
    CHECK(shownProblem->property("color").value<QColor>() == style->property("errorColor").value<QColor>());

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

TEST_CASE("the Links page's pattern list takes what is typed, one field after the other",
          "[contour][gui][qml][settings][links]")
{
    // Typed for real into the list's own fields: leaving the name field commits it, which changes the model,
    // while the focus is already on its way to the regex field.
    contour::test::QmlMessageCapture const warnings;
    auto fx = PageFixture(OneProfile);
    fx.addProfile();
    REQUIRE_FALSE(fx.controller->editingReadOnly());
    REQUIRE(fx.open("profile:New Profile:links"));
    auto* page = fx.item();
    auto window = fx.activeWindow(QSize(1000, 800));
    fx.settle();

    clickButton(page->findChild<QObject*>("addLinkPatternButton"));
    fx.settle();
    auto const row = static_cast<int>(fx.controller->linkPatterns().size()) - 1;
    auto const added = fx.controller->linkPatterns().back().toMap().value("name").toString();

    // The new row's fields, found in the visual tree: a delegate has no QObject parent.
    auto const fieldNamed = [&](QString const& objectName) -> QQuickItem& {
        auto* const name = findDescendant(*page, [&](QQuickItem const& item) {
            return item.objectName() == "linkPatternName" && item.property("text").toString() == added;
        });
        return required(findDescendant(required(required(name).parentItem()), [&](QQuickItem const& item) {
            return item.objectName() == objectName;
        }));
    };

    auto* const name = &fieldNamed("linkPatternName");
    auto const regex = QPointer<QQuickItem> { &fieldNamed("linkPatternRegex") };
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

TEST_CASE("a pop-up row shows the GPU titles in full, and an absent GPU as the current entry",
          "[contour][gui][qml][settings][gpu]")
{
    // Built on its own from the controller's row, exactly as a page's group builds it.
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
    auto const fileSystem = core::platform::NativeFileSystem {};
    auto const controller = contour::window::SettingsController(
        [&]() -> config::Config const& { return cfg; },
        window::SettingsControllerServices {
            .store = std::make_shared<config::FileGuiConfigStore>(fileSystem, configDir),
            .reload = [] { return std::expected<void, std::string> {}; },
            .gpuInventory = std::make_shared<FakeGpuInventory>(),
            .scheduler = std::make_shared<contour::test::ManualSaveScheduler>(),
        },
        vtbackend::ColorPreference::Dark);
    auto const gpu = globalRow(controller, "renderer.gpu");
    REQUIRE(!gpu.isEmpty());

    auto const rows = SettingsRowFactory {};
    auto const settingsRow = rows.make(gpu);
    auto window = TestWindow(900, 200);
    window.show(*settingsRow);

    // The control is a pop-up, as the titles are too long for segments.
    auto* popUp = settingsRow->findChild<QQuickItem*>(QStringLiteral("settingsControl:renderer.gpu"));
    REQUIRE(popUp != nullptr);
    CHECK(gpu.value("controlName").toString() == "popUp");

    // Titles, never ids, and the configured-but-absent GPU stays the current entry.
    auto const labels = gpu.value("optionLabels").toStringList();
    CHECK(labels.contains("NVIDIA GeForce RTX 4070 Max-Q / Mobile"));
    auto* caption =
        findDescendant(*popUp, [](QQuickItem const& item) { return item.objectName() == "popUpCaption"; });
    REQUIRE(caption != nullptr);
    CHECK(caption->property("text").toString() == "Unavailable GPU (1002:7340)");

    // Wide enough for the longest title, so neither the button nor its list cuts it off.
    auto const longest = *std::ranges::max_element(
        labels, [](QString const& a, QString const& b) { return a.size() < b.size(); });
    TestWindow::focus(*popUp);
    QMetaObject::invokeMethod(popUp, "open");
    auto* popup = popUp->findChild<QObject*>(QStringLiteral("popUpPopup"));
    REQUIRE(popup != nullptr);
    REQUIRE(QTest::qWaitFor([&] { return popup->property("opened").toBool(); }, 2000));
    auto const rowLabels = [&] {
        auto* list = popup->findChild<QQuickItem*>(QStringLiteral("popUpList"));
        auto items = findDescendants(*list->property("contentItem").value<QQuickItem*>(),
                                     [](QQuickItem const& item) { return item.objectName() == "popUpRow"; });
        std::ranges::sort(items, [](QQuickItem const* a, QQuickItem const* b) { return a->y() < b->y(); });
        auto found = QStringList {};
        for (auto* child: items)
            found << child->findChild<QObject*>(QStringLiteral("popUpRowLabel"))->property("text").toString();
        return found;
    };
    REQUIRE(QTest::qWaitFor([&] { return rowLabels().size() == labels.size(); }, 2000));
    CHECK(rowLabels().contains(longest));
    CHECK(rowLabels() == labels);
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("a settings page lists the command-block settings from the descriptors",
          "[contour][gui][qml][settings]")
{
    // No QML knows about these rows: a profile page draws whatever `group` a row carries, and a section page
    // whatever globalFields returns. This proves both hold for the command-block rows.
    contour::test::QmlMessageCapture const warnings;
    auto fx = PageFixture(OneProfile);
    fx.addProfile();

    auto groupKeys = QStringList {};
    for (auto const& raw: fx.controller->profileFields())
        if (raw.toMap().value("groupTitle").toString() == "Command blocks")
            groupKeys.push_back(raw.toMap().value("key").toString());
    CHECK(groupKeys.size() == qsizetype { 12 });
    CHECK(groupKeys.contains("sticky_header_mode"));
    for (auto const& key: groupKeys)
        CHECK(fx.byName("settingsRow:" + key) != nullptr);

    // The gutter's rows are on the Shell Integration page.
    REQUIRE(fx.open("section:shell-integration"));
    for (auto const* key: { "gutter.exit_status",
                            "gutter.user_marks",
                            "gutter.line_numbers",
                            "gutter.line_number_width",
                            "gutter.timestamps",
                            "gutter.timestamp_format" })
        CHECK(fx.byName("settingsRow:" + QString::fromLatin1(key)) != nullptr);
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("a stepper holds a typed value to its range, and a field puts the stored text back",
          "[contour][gui][qml][settings]")
{
    // What the user typed is in the field, not in the model, and a refused value leaves the model as it was:
    // re-sending it rebuilds no row, so a field that kept the typed text would go on showing a value that was
    // never saved. Typed for real, into the page's own rows, so the whole path -- control, page, controller
    // and back -- is the one the user takes.
    contour::test::QmlMessageCapture const warnings;
    auto fx = PageFixture(OneProfile);
    auto window = fx.activeWindow(QSize(1000, 800));
    fx.settle();

    auto const typeInto = [&](QString const& key, std::string_view text) {
        window.typeOver(required(fx.fieldOf(key)), text);
        fx.settle();
    };

    SECTION("a width out of range is held to the range")
    {
        auto const key = QStringLiteral("gutter.line_number_width");
        fx.openGlobal(key);
        auto const stored = shown(fx.controlOf(key));
        REQUIRE(stored == std::to_string(fx.cfg.gutter.value().lineNumberWidth));
        auto const row = globalRow(*fx.controller, key);
        REQUIRE(row.value("max").toDouble() == 10.0);

        typeInto(key, "11");
        fx.scheduler->fire();
        fx.settle();
        CHECK(fx.cfg.gutter.value().lineNumberWidth == 10);
        CHECK(shown(fx.controlOf(key)) == "10");
        CHECK(fx.errors.isEmpty());

        // Leaving the field finishes editing once more: what it shows is the stored value, no change.
        window.dropFocus();
        fx.settle();
        CHECK(fx.errors.isEmpty());
    }

    SECTION("a string row refuses a timestamp format it cannot draw, and says why")
    {
        auto const key = QStringLiteral("gutter.timestamp_format");
        fx.openGlobal(key);
        auto const stored = shown(fx.controlOf(key));
        REQUIRE(stored == fx.cfg.gutter.value().timestampFormat);

        typeInto(key, "%K");
        CHECK(shown(fx.controlOf(key)) == stored);
        REQUIRE(fx.errors.size() == qsizetype { 1 });
        CHECK(fx.errors.first().contains("timestamp format"));
        CHECK_FALSE(globalRow(*fx.controller, key).value("overridden").toBool());
    }

    SECTION("a decimal row saves what was typed")
    {
        auto const key = QStringLiteral("ui_font_size");
        fx.openGlobal(key);
        typeInto(key, "11.5");
        fx.scheduler->fire();
        CHECK(fx.cfg.uiFontSize.value() == 11.5);
        CHECK(shown(fx.controlOf(key)) == "11.5");
        CHECK(fx.errors.isEmpty());
    }

    SECTION("a profile row stores a negative minimum duration as zero, and shows the zero")
    {
        fx.addProfile();
        auto const key = QStringLiteral("notify_on_command_finish_min_duration");
        REQUIRE(shown(fx.controlOf(key)) == "10");

        typeInto(key, "-5");
        CHECK(shown(fx.controlOf(key)) == "0");
        // Again, now that the stored value is already zero: nothing changes, and the field still drops what
        // was typed.
        typeInto(key, "-5");
        CHECK(shown(fx.controlOf(key)) == "0");
        CHECK(fx.errors.isEmpty());
    }

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("the page says why it refused a value until the next change it saves",
          "[contour][gui][qml][settings]")
{
    contour::test::QmlMessageCapture const warnings;
    auto fx = PageFixture(OneProfile);
    auto window = fx.activeWindow(QSize(1000, 800));
    fx.settle();

    auto* banner = fx.byName("settingsErrorBanner");
    REQUIRE(banner != nullptr);
    CHECK_FALSE(banner->isVisible());

    auto const key = QStringLiteral("gutter.timestamp_format");
    fx.openGlobal(key);
    auto const typeInto = [&](std::string_view text) {
        window.typeOver(required(fx.fieldOf(key)), text);
        fx.settle();
    };

    typeInto("%K");
    CHECK(banner->isVisible());
    auto const message = banner->property("text").toString();
    CHECK(message.contains("timestamp format"));
    // A screen reader is told, too: the reason is the banner's accessible name, and it reads as an alert.
    auto* const accessible = QAccessible::queryAccessibleInterface(banner);
    REQUIRE(accessible != nullptr);
    CHECK(accessible->text(QAccessible::Name) == message);
    CHECK(accessible->role() == QAccessible::AlertMessage);

    // The next value the page saves takes the reason away.
    typeInto("%H");
    fx.scheduler->fire();
    CHECK(fx.cfg.gutter.value().timestampFormat == "%H");
    CHECK_FALSE(banner->isVisible());

    // So does dismissing it.
    typeInto("%K");
    REQUIRE(banner->isVisible());
    clickButton(
        findDescendant(*banner, [](QQuickItem const& item) { return item.objectName() == "bannerDismiss"; }));
    CHECK_FALSE(banner->isVisible());
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("a switch or a segmented control is put back when the page cannot save the change",
          "[contour][gui][qml][settings]")
{
    // Neither control can send a value the page refuses, but a change it cannot write is refused all the same
    // once the debounce writes it: the settings.yml draft goes back to what is in effect, and the row with
    // it.
    contour::test::QmlMessageCapture const warnings;
    auto fx = PageFixture(OneProfile, std::make_shared<UnwritableStore>());
    auto window = fx.activeWindow(QSize(1000, 800));
    fx.settle();

    SECTION("a switch")
    {
        auto const key = QStringLiteral("reflow_on_resize");
        auto const stored = fx.cfg.reflowOnResize.value();
        REQUIRE(required(fx.controlOf(key)).property("checked").toBool() == stored);

        TestWindow::focus(required(fx.controlOf(key)));
        window.press(Qt::Key_Space);
        fx.settle();
        fx.scheduler->fire();
        fx.settle();
        REQUIRE(fx.errors.size() == qsizetype { 1 });
        CHECK(fx.errors.front().contains(QString::fromUtf8(UnwritableStore::Reason)));
        CHECK(required(fx.controlOf(key)).property("checked").toBool() == stored);
    }

    SECTION("a segmented control")
    {
        // The first global setting that is edited with one.
        auto key = QString {};
        for (auto const& raw: fx.controller->globalFields())
            if (key.isEmpty() && raw.toMap().value("controlName").toString() == "segmented")
                key = raw.toMap().value("key").toString();
        REQUIRE_FALSE(key.isEmpty());
        fx.openGlobal(key);
        auto const stored = required(fx.controlOf(key)).property("value").toString();

        TestWindow::focus(required(fx.controlOf(key)));
        window.press(Qt::Key_Right);
        fx.settle();
        fx.scheduler->fire();
        fx.settle();
        REQUIRE(fx.errors.size() == qsizetype { 1 });
        CHECK(fx.errors.front().contains(QString::fromUtf8(UnwritableStore::Reason)));
        CHECK(required(fx.controlOf(key)).property("value").toString() == stored);
    }
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

namespace
{
/// Opens a side-file profile `work`, a copy of `main`, on @p fx's controller, and commits a font size the
/// write of which then fails; asserts what the page shows afterwards.
/// @param fx     The fixture, whose store cannot write.
/// @param reason What the failure must name.
/// @param mirror Puts `work.yml`'s bytes where the store reads them too: the loader reads the real disk, the
///               store its own files, and both must hold what the other does.
void checkUnwritableRollsBack(ControllerFixture& fx,
                              QString const& reason,
                              std::function<void(std::string const&)> const& mirror)
{
    {
        auto const bytes = config::emitProfileYaml(*fx.cfg.findProfile("main"));
        std::filesystem::create_directories(fx.configDir / "profiles");
        auto out = std::ofstream(fx.configDir / "profiles" / "work.yml");
        out << bytes;
        mirror(bytes);
    }
    fx.reload();
    fx.controller->editProfile("work");
    REQUIRE_FALSE(fx.controller->editingReadOnly());
    auto const fontSize = [&fx] {
        for (auto const& raw: fx.controller->profileFields())
            if (raw.toMap().value("key").toString() == "font_size")
                return raw.toMap().value("value").toDouble();
        return 0.0;
    };
    auto const inEffect = fx.cfg.findProfile("work")->fonts.value().size.pt;
    REQUIRE(inEffect != 15.0);

    fx.controller->commitProfileField("font_size", 15.0);
    REQUIRE(fontSize() == 15.0);
    CHECK(fx.controller->hasChanges());
    fx.scheduler->fire();

    REQUIRE(fx.errors.size() == qsizetype { 1 });
    CHECK(fx.errors.front().contains(reason));
    CHECK(fx.errors.front().contains("profiles/work.yml"));
    CHECK(fontSize() == inEffect);
    CHECK_FALSE(fx.controller->hasChanges());
}
} // namespace

TEST_CASE("SettingsController: an unwritable directory rolls the control back", "[settings]")
{
    // Review Focus 3: the write fails with the OS's reason, and the row shows the value still in effect.
    SECTION("a store that refuses every write")
    {
        auto const store = std::make_shared<UnwritableStore>();
        auto fx = ControllerFixture(OneProfile, store);
        checkUnwritableRollsBack(
            fx, QString::fromUtf8(UnwritableStore::Reason), [&store](std::string const& bytes) {
                store->files.insert_or_assign(config::profileFile("work"), bytes);
            });
    }

    SECTION("a file system that refuses the replace, through the durable write")
    {
        auto const files = vthost::testing::FaultyFileSystem {
            [](vthost::testing::FaultOp op, std::filesystem::path const&) {
                return op == vthost::testing::FaultOp::Rename;
            },
        };
        auto fx = ControllerFixture(OneProfile, files);
        checkUnwritableRollsBack(fx, QStringLiteral("Permission denied"), [&](std::string const& bytes) {
            REQUIRE(files.createDirectories(fx.configDir / "profiles").has_value());
            REQUIRE(files.writeFile(fx.configDir / "profiles" / "work.yml", bytes).has_value());
        });
        // The refused write left no temporary behind: the profile is all there is.
        auto const listed = files.listDirectory(fx.configDir / "profiles");
        REQUIRE(listed.has_value());
        REQUIRE(listed->size() == 1);
        CHECK(listed->front().path.filename() == "work.yml");
    }
}

TEST_CASE("a row shows what is wrong with its value under it", "[contour][gui][qml][settings][links]")
{
    contour::test::QmlMessageCapture const warnings;
    auto const rows = SettingsRowFactory {};
    auto const row = rows.make({ { "key", "file_opener_command" },
                                 { "controlName", "textField" },
                                 { "value", "code {file}" },
                                 { "problem", "an unknown placeholder" } });
    auto const* problem = findDescendant(
        *row, [](QQuickItem const& item) { return item.objectName() == "settingsRowProblem"; });
    REQUIRE(problem != nullptr);
    CHECK(problem->isVisible());
    CHECK(problem->property("text").toString() == "an unknown placeholder");
    // In the chrome style's danger ink, which follows the theme.
    auto* style = rows.engine.rootContext()->contextProperty("chromeStyle").value<QObject*>();
    REQUIRE(style != nullptr);
    auto danger = QColor {};
    REQUIRE(QMetaObject::invokeMethod(style,
                                      "statusInk",
                                      Q_RETURN_ARG(QColor, danger),
                                      Q_ARG(int, style->property("statusDanger").toInt())));
    CHECK(problem->property("color").value<QColor>() == danger);

    row->setProperty("field",
                     QVariantMap { { "key", "file_opener_command" },
                                   { "label", "Label" },
                                   { "controlName", "textField" },
                                   { "value", "code {file}" } });
    CHECK_FALSE(problem->isVisible());
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("a row's text field follows its value once an edit is committed", "[contour][gui][qml][settings]")
{
    // The host answers an edit through `field.value`: with the value it stored, or by leaving it as it was
    // when it refused the edit. Either way the field shows it afterwards, and keeps following it.
    contour::test::QmlMessageCapture const warnings;
    auto const rows = SettingsRowFactory {};
    auto const field = [](QString const& value) {
        return QVariantMap { { "key", "word_delimiters" },
                             { "label", "Word delimiters" },
                             { "controlName", "textField" },
                             { "value", value } };
    };
    auto const row = rows.make(field("abc"));
    QSignalSpy const commits(row.get(), SIGNAL(committed(QString, QVariant)));
    auto window = TestWindow(600, 100);
    window.show(*row);
    auto& entry = required(
        findDescendant(*row, [](QQuickItem const& item) { return item.inherits("QQuickTextField"); }));

    // Committed, and refused: `value` stays, and so does the field.
    window.typeOver(entry, "xyz");
    REQUIRE(commits.size() == qsizetype { 1 });
    CHECK(commits.first().at(1).toString() == "xyz");
    CHECK(entry.property("text").toString() == "abc");

    // Leaving the field finishes editing once more, on what it shows now: that is no edit.
    window.dropFocus();
    CHECK_FALSE(entry.hasActiveFocus());
    CHECK(commits.size() == qsizetype { 1 });

    // The value moves later, as a Reset moves it: the field still follows.
    row->setProperty("field", field("def"));
    CHECK(entry.property("text").toString() == "def");

    // Typing the value the row already holds is no edit.
    window.typeOver(entry, "def");
    CHECK(commits.size() == qsizetype { 1 });
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("the page follows a configuration reload while it is open", "[contour][gui][qml][settings]")
{
    // A reload -- live_config noticing contour.yml change, or ReloadConfig -- refreshes the app's settings
    // page. A row left on the value from before the reload would take typing that value back as no change,
    // and drop it.
    contour::test::QmlMessageCapture const warnings;

    // This machine's file system: Settings writes the override that the loader then reads from the disk.
    auto testApp = contour::test::ConfiguredTestApp { nullptr,
                                                      contour::platform::nullAppearanceServices(),
                                                      contour::test::TestApp::SessionFiles::ThisMachine };
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
    REQUIRE(QMetaObject::invokeMethod(
        view.page.get(), "openPage", Q_ARG(QVariant, QStringLiteral("section:shell-integration"))));
    view.settle();

    auto const key = QStringLiteral("gutter.line_number_width");
    auto const field = [&]() -> QQuickItem& {
        return required(
            findDescendant(required(findDescendant(*page,
                                                   [&key](QQuickItem const& item) {
                                                       return item.objectName() == "settingsControl:" + key;
                                                   })),
                           [](QQuickItem const& item) { return item.inherits("QQuickTextField"); }));
    };
    REQUIRE(field().property("text").toString() == "6");

    writeConfig(8);
    REQUIRE(testApp.manager().reloadAllSessions().has_value());
    view.settle();
    CHECK(field().property("text").toString() == "8");

    // Typing the width from before the reload is a change now, and it is saved.
    window.typeOver(field(), "6");
    view.settle();
    settings->flushPendingChanges();
    view.settle();
    CHECK(testApp.app().config().gutter.value().lineNumberWidth == 6);
    CHECK(globalRow(*settings, key).value("overridden").toBool());
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("a number is typed with a decimal point in every locale", "[contour][gui][qml][settings]")
{
    // A stepper's field shows its value as JavaScript spells it and reads the typed text back with Number(),
    // so what it lets be typed must read the same: a point (or a comma) for the decimals, and no group
    // separator. One that followed a locale writing a decimal comma took the point for a group separator --
    // leaving a field that showed 0.5 stored 5, and 1.000 typed into a whole number stored 1.
    contour::test::QmlMessageCapture const warnings;
    auto const previousLocale = QLocale {};
    QLocale::setDefault(QLocale(QLocale::German, QLocale::Germany));
    auto const restoreLocale = core::Finally { [&previousLocale] { QLocale::setDefault(previousLocale); } };

    auto const rows = SettingsRowFactory {};
    auto window = TestWindow(600, 100);

    SECTION("a decimal")
    {
        auto const row = rows.make({ { "controlName", "stepper" },
                                     { "value", 0.5 },
                                     { "min", 0.0 },
                                     { "max", 10.0 },
                                     { "step", 0.5 },
                                     { "decimals", 1 },
                                     { "unitText", "" } });
        window.show(*row);
        QSignalSpy const commits(row.get(), SIGNAL(committed(QString, QVariant)));
        auto& entry = required(
            findDescendant(*row, [](QQuickItem const& item) { return item.inherits("QQuickTextField"); }));
        REQUIRE(entry.property("text").toString() == "0.5");

        // Visiting the field is no edit.
        TestWindow::focus(entry);
        window.dropFocus();
        CHECK(commits.isEmpty());
        CHECK(entry.property("text").toString() == "0.5");

        window.typeOver(entry, "0.7");
        REQUIRE(commits.size() == qsizetype { 1 });
        CHECK(commits.at(0).at(1).toDouble() == 0.7);

        // A decimal comma, as the locale writes it, is read the same.
        window.typeOver(entry, "0,2");
        REQUIRE(commits.size() == qsizetype { 2 });
        CHECK(commits.at(1).at(1).toDouble() == 0.2);
    }

    SECTION("a whole number")
    {
        auto const row = rows.make({ { "controlName", "stepper" },
                                     { "value", 6 },
                                     { "min", -std::numeric_limits<double>::infinity() },
                                     { "max", std::numeric_limits<double>::infinity() },
                                     { "step", 1.0 },
                                     { "decimals", 0 },
                                     { "unitText", "" } });
        window.show(*row);
        QSignalSpy const commits(row.get(), SIGNAL(committed(QString, QVariant)));
        auto& entry = required(
            findDescendant(*row, [](QQuickItem const& item) { return item.inherits("QQuickTextField"); }));

        // Neither separator is a digit, so neither is taken.
        window.typeOver(entry, "1.000");
        REQUIRE(commits.size() == qsizetype { 1 });
        CHECK(commits.at(0).at(1).toInt() == 1000);
        window.typeOver(entry, "2,000");
        REQUIRE(commits.size() == qsizetype { 2 });
        CHECK(commits.at(1).at(1).toInt() == 2000);
    }
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("a row sends nothing for a re-picked entry, and Escape puts the stored text back",
          "[contour][gui][qml][settings]")
{
    contour::test::QmlMessageCapture const warnings;
    auto const rows = SettingsRowFactory {};
    auto window = TestWindow(600, 100);

    for (auto const* control: { "segmented", "popUp" })
    {
        DYNAMIC_SECTION("a " << control)
        {
            auto const row = rows.make({ { "controlName", control },
                                         { "value", "relative" },
                                         { "options", QStringList { "off", "relative", "hybrid" } },
                                         { "optionLabels", QStringList { "Off", "Relative", "Hybrid" } } });
            window.show(*row);
            QSignalSpy const commits(row.get(), SIGNAL(committed(QString, QVariant)));
            auto* picker = row->findChild<QQuickItem*>(QStringLiteral("settingsControl:key"));
            REQUIRE(picker != nullptr);

            // Picking the entry it shows is no change: writing it would override the setting with the value
            // it already has.
            QMetaObject::invokeMethod(picker, "picked", Q_ARG(QString, QStringLiteral("relative")));
            CHECK(commits.isEmpty());

            // Another entry is.
            QMetaObject::invokeMethod(picker, "picked", Q_ARG(QString, QStringLiteral("hybrid")));
            REQUIRE(commits.size() == qsizetype { 1 });
            CHECK(commits.first().at(1).toString() == "hybrid");
        }
    }

    for (auto const& [control, value, typed]: std::array {
             std::tuple { "stepper", QVariant(6), std::string_view { "8" } },
             std::tuple { "textField", QVariant(QStringLiteral("less")), std::string_view { "more" } },
         })
    {
        DYNAMIC_SECTION("Escape in a " << control << " field")
        {
            auto const row = rows.make({ { "controlName", control },
                                         { "value", value },
                                         { "min", 0.0 },
                                         { "max", 100.0 },
                                         { "step", 1.0 },
                                         { "decimals", 0 },
                                         { "unitText", "" } });
            window.show(*row);
            QSignalSpy const commits(row.get(), SIGNAL(committed(QString, QVariant)));
            auto& entry = required(findDescendant(
                *row, [](QQuickItem const& item) { return item.inherits("QQuickTextField"); }));
            auto const stored = entry.property("text").toString();

            TestWindow::focus(entry);
            QMetaObject::invokeMethod(&entry, "selectAll");
            window.type(typed);
            REQUIRE(entry.property("text").toString() != stored);

            window.press(Qt::Key_Escape);
            CHECK(entry.property("text").toString() == stored);
            CHECK_FALSE(entry.hasActiveFocus());

            // Nothing typed is left behind for the next focus change to commit.
            window.dropFocus();
            CHECK(commits.isEmpty());
        }
    }
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("the page drops a refusal with the page or draft it was about", "[contour][gui][qml][settings]")
{
    contour::test::QmlMessageCapture const warnings;
    auto fx = PageFixture(OneProfile);
    auto window = fx.activeWindow(QSize(1000, 800));
    fx.settle();

    auto* banner = fx.byName("settingsErrorBanner");
    REQUIRE(banner != nullptr);

    SECTION("a screen reader hears every refusal, a repeated one too")
    {
        auto const key = QStringLiteral("gutter.timestamp_format");
        fx.openGlobal(key);
        AnnouncementProbe::Said.clear();
        auto* const previous = QAccessible::installUpdateHandler(&AnnouncementProbe::handle);
        for ([[maybe_unused]] auto const attempt: { 1, 2 })
        {
            window.typeOver(required(fx.fieldOf(key)), "%K");
            fx.settle();
        }
        QAccessible::installUpdateHandler(previous);
        REQUIRE(AnnouncementProbe::Said.size() == qsizetype { 2 });
        CHECK(AnnouncementProbe::Said.at(0).contains("timestamp format"));
        CHECK(AnnouncementProbe::Said.at(1) == AnnouncementProbe::Said.at(0));
    }

    SECTION("another page drops it")
    {
        fx.controller->commitGlobalField("gutter.timestamp_format", "%K");
        REQUIRE(banner->isVisible());
        REQUIRE(fx.open("section:keyboard"));
        CHECK_FALSE(banner->isVisible());
    }

    SECTION("a change to the profile draft drops it")
    {
        fx.addProfile();
        fx.controller->commitGlobalField("gutter.timestamp_format", "%K");
        REQUIRE(banner->isVisible());
        fx.controller->commitProfileField("maximized", true);
        CHECK_FALSE(banner->isVisible());
    }

    SECTION("a change to the colour scheme draft drops it")
    {
        fx.controller->duplicateScheme("default");
        fx.controller->commitGlobalField("gutter.timestamp_format", "%K");
        REQUIRE(banner->isVisible());
        fx.controller->commitSchemeColor("background", "#102030");
        CHECK_FALSE(banner->isVisible());
    }
    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("ColorSchemeEditor pins a derived colour on Return only, and lets it follow again",
          "[contour][gui][qml][settings]")
{
    contour::test::QmlMessageCapture const warnings;
    auto fx = PageFixture(OneProfile);
    fx.controller->duplicateScheme("default"); // an editable scheme, opened for editing

    // The editor on its own, over the controller, in an active window.
    QQmlComponent component(&fx.engine, QUrl(QStringLiteral("qrc:/qt/qml/Contour/Ui/ColorSchemeEditor.qml")));
    INFO("component errors: " << component.errorString().toStdString());
    REQUIRE(component.isReady());
    auto editor = std::unique_ptr<QQuickItem>(qobject_cast<QQuickItem*>(component.createWithInitialProperties(
        QVariantMap { { "controller", QVariant::fromValue(static_cast<QObject*>(fx.controller.get())) } })));
    REQUIRE(editor != nullptr);
    auto window = TestWindow(1000, 800);
    window.show(*editor);
    fx.settle();

    auto const key = QStringLiteral("gutter_text");
    auto const sourceOf = [&fx, &key]() {
        for (auto const& raw: fx.controller->schemeColors())
            if (raw.toMap().value("key").toString() == key)
                return raw.toMap().value("colorSource").toString();
        return QString {};
    };
    // Looked up again each time: a changed draft rebuilds the swatches.
    auto const partOf = [&](char const* name) -> QQuickItem& {
        auto& swatch = required(findDescendant(*editor, [&key](QQuickItem const& candidate) {
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
