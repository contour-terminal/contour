// SPDX-License-Identifier: Apache-2.0
//
// The daemon and workspace switcher popup, offscreen: the controller over fake services, and its window as
// the user drives it -- the rows and their states, the keys, a pick, and "New daemon…" -- under every chrome
// row, as the sizes are tokens.

#include <contour/cli/NewDaemon.hpp>
// The actions defined, not only declared: beside vthost::proto's ClosePane and ResizePane (through the switch
// vocabulary), ActionsFwd's declarations alone read as a namespace mix-up to clang-tidy.
#include <contour/config/Actions.hpp>
#include <contour/test/QmlChromeStyle.hpp>
#include <contour/window/PopupParent.hpp>
#include <contour/window/SwitcherController.hpp>

#include <vtpty/ProcessPlacement.hpp>

#include <QtCore/QCoreApplication>
#include <QtGui/QGuiApplication>
#include <QtQml/QQmlEngine>
#include <QtQuick/QQuickItem>
#include <QtQuick/QQuickWindow>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <QtTest/QSignalSpy>
#include <QtTest/QTest>
#include <vtcontrol/ObjectRef.hpp>
#include <vthost/DaemonBinary.hpp>

using namespace contour;
using contour::session::SwitchMode;
using contour::session::SwitchTarget;
using contour::window::SwitcherWorkspaceState;
using vthost::proto::SwitchDaemonKind;

namespace
{

/// Collects the visible items named @p name under @p root in the visual tree.
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

/// @return A workspace's control id, made from @p name: equal for equal names.
[[nodiscard]] vtcontrol::ObjectRef workspaceRef(std::string_view name)
{
    auto ref = vtcontrol::ObjectRef { .kind = vtcontrol::ObjectKind::Workspace, .uuid = {} };
    for (auto const [index, character]: std::views::zip(std::views::iota(std::size_t { 0 }), name))
        ref.uuid.bytes.at(index % ref.uuid.bytes.size()) ^= static_cast<uint8_t>(character);
    return ref;
}

[[nodiscard]] SwitchTarget labelled(std::string name, contour::session::WorkspaceTarget workspace = {})
{
    return SwitchTarget { .kind = SwitchDaemonKind::Label,
                          .daemon = std::move(name),
                          .workspace = std::move(workspace) };
}

/// Discovery that finds `default` and `admin` running.
class FixedDiscovery final: public cli::IDaemonDiscovery
{
  public:
    [[nodiscard]] std::vector<cli::DiscoveredDaemon> discover() override
    {
        return { { .label = "default", .socket = "/run/contour/default", .workspaces = 2 },
                 { .label = "admin", .socket = "/run/contour/admin", .workspaces = 2 } };
    }
};

/// Discovery that holds every caller until released, and counts how many it holds at once: a socket that
/// takes its time to answer, so an opening can come and go while its discovery is under way. Called on the
/// controller's worker threads.
class GatedDiscovery final: public cli::IDaemonDiscovery
{
  public:
    [[nodiscard]] std::vector<cli::DiscoveredDaemon> discover() override
    {
        auto lock = std::unique_lock { _mutex };
        ++_calls;
        ++_running;
        _mostAtOnce = std::max(_mostAtOnce, _running);
        _changed.notify_all();
        _changed.wait(lock, [this] { return _released > 0; });
        --_released;
        --_running;
        return { { .label = "admin", .socket = "/run/contour/admin", .workspaces = 2 } };
    }

    /// Lets one held discovery answer.
    void releaseOne()
    {
        auto const lock = std::lock_guard { _mutex };
        ++_released;
        _changed.notify_all();
    }

    /// @return Whether @p count discoveries were started within three seconds.
    [[nodiscard]] bool awaitCalls(int count)
    {
        auto lock = std::unique_lock { _mutex };
        return _changed.wait_for(lock, std::chrono::seconds(3), [&] { return _calls >= count; });
    }

    [[nodiscard]] int calls() const
    {
        auto const lock = std::lock_guard { _mutex };
        return _calls;
    }

    [[nodiscard]] int mostAtOnce() const
    {
        auto const lock = std::lock_guard { _mutex };
        return _mostAtOnce;
    }

  private:
    mutable std::mutex _mutex;
    std::condition_variable _changed;
    int _calls = 0;
    int _running = 0;
    int _mostAtOnce = 0;
    int _released = 0;
};

/// The controller alone, over @p discovery and services that answer nothing: what discovery does to it.
[[nodiscard]] std::unique_ptr<window::SwitcherController> controllerOver(
    std::shared_ptr<cli::IDaemonDiscovery> discovery)
{
    return std::make_unique<window::SwitcherController>(window::SwitcherServices {
        .discovery = std::move(discovery),
        .savedHosts = [] { return std::vector<window::SwitcherDaemon> {}; },
        .list = [](vtworkspace::WorkspaceId, SwitchTarget const&, window::SwitcherListed const&) {},
        .request = [](vtworkspace::WorkspaceId, SwitchTarget const&, SwitchMode) {},
        .startDaemon =
            [](std::string const&) {
                return []() -> std::expected<cli::NamedDaemonStart, std::string> {
                    return std::unexpected { std::string { "not in this test" } };
                };
            },
    });
}

/// @return Whether @p switcher lists a daemon titled @p title.
[[nodiscard]] bool listsDaemon(window::SwitcherController const& switcher, std::string_view title)
{
    return std::ranges::any_of(switcher.rows(), [&](window::SwitcherRow const& row) {
        return row.kind == window::SwitcherRowKind::Daemon && row.title == title;
    });
}

/// A spawner that records what it starts and has the daemon serve once started: "New daemon…"'s real start
/// (cli::startNamedDaemon) over it. Called on the controller's worker thread.
class RecordingSpawner final: public vthost::DaemonSpawner
{
  public:
    [[nodiscard]] std::expected<void, std::string> spawn(std::span<std::string const> argv,
                                                         vtpty::ProcessPlacement& /*placement*/) override
    {
        auto const lock = std::lock_guard { _mutex };
        _spawned.emplace_back(argv.begin(), argv.end());
        return {};
    }

    [[nodiscard]] vthost::DaemonPresence probe(std::filesystem::path const& /*socketPath*/) override
    {
        auto const lock = std::lock_guard { _mutex };
        return _spawned.empty() ? vthost::DaemonPresence::Absent : vthost::DaemonPresence::Serving;
    }

    [[nodiscard]] std::vector<std::vector<std::string>> spawned() const
    {
        auto const lock = std::lock_guard { _mutex };
        return _spawned;
    }

  private:
    mutable std::mutex _mutex;
    std::vector<std::vector<std::string>> _spawned;
};

/// What the switcher was asked to do.
struct Requested
{
    SwitchTarget target;
    SwitchMode mode = SwitchMode::Switch;
};

/// The window the popup is over.
constexpr auto ThisWindow = vtworkspace::WorkspaceId { 4 };

/// The popup over fake services, in a window under one chrome row.
struct SwitcherFixture
{
    QQmlEngine engine;
    std::vector<Requested> requested;       ///< What the window switcher was asked, in order.
    std::vector<std::string> listed;        ///< The daemons listed, by name, in order.
    std::vector<std::string> startedLabels; ///< The labels a start was prepared for.
    RecordingSpawner spawner;
    vthost::InstalledDaemonBinary binary { "contour" };
    vtpty::NoPlacement placement;
    /// What each daemon's listing answers, by name.
    std::map<std::string, std::vector<window::SwitcherWorkspace>> workspaces {
        { "default",
          { { .ref = workspaceRef("w1"),
              .position = 1,
              .name = "work",
              .tabs = 4,
              .state = SwitcherWorkspaceState::ThisWindow },
            { .ref = workspaceRef("w3"),
              .position = 3,
              .name = "scratch",
              .tabs = 1,
              .state = SwitcherWorkspaceState::Hidden } } },
        { "admin",
          { { .ref = workspaceRef("a1"),
              .position = 1,
              .name = "ops",
              .tabs = 3,
              .state = SwitcherWorkspaceState::Hidden },
            { .ref = workspaceRef("a2"),
              .position = 2,
              .name = "logs",
              .tabs = 1,
              .state = SwitcherWorkspaceState::ShownElsewhere } } },
        { "studio",
          { { .ref = workspaceRef("s1"),
              .position = 1,
              .name = "build",
              .tabs = 2,
              .state = SwitcherWorkspaceState::Hidden } } },
    };
    QObject receiver; ///< Where a listing is answered from, later, as the real one is.
    std::unique_ptr<window::SwitcherController> switcher;
    std::unique_ptr<QQuickWindow> window;

    explicit SwitcherFixture(config::ChromeStyle row = config::ChromeStyle::ContourLinux)
    {
        auto setup = test::defaultChromeSetup();
        setup.row = row;
        test::installChromeStyle(engine, setup);
        switcher = std::make_unique<window::SwitcherController>(window::SwitcherServices {
            .discovery = std::make_shared<FixedDiscovery>(),
            .savedHosts =
                [] {
                    return std::vector<window::SwitcherDaemon> { window::SwitcherDaemon {
                        .address = { .kind = SwitchDaemonKind::Host, .daemon = "studio", .workspace = {} },
                        .workspaceCount = std::nullopt,
                        .tag = "SSH",
                        .expansion = window::SwitcherExpansion::Collapsed,
                        .listing = window::SwitcherListing::NotListed,
                        .failure = {},
                        .workspaces = {} } };
                },
            .list =
                [this](vtworkspace::WorkspaceId asking,
                       SwitchTarget const& daemon,
                       window::SwitcherListed done) {
                    REQUIRE(asking == ThisWindow);
                    listed.push_back(daemon.daemon);
                    QMetaObject::invokeMethod(
                        &receiver,
                        [this, name = daemon.daemon, done = std::move(done)] { done(workspaces.at(name)); },
                        Qt::QueuedConnection);
                },
            .request =
                [this](vtworkspace::WorkspaceId asking, SwitchTarget target, SwitchMode mode) {
                    REQUIRE(asking == ThisWindow);
                    requested.push_back({ std::move(target), mode });
                },
            .startDaemon =
                [this](std::string const& label) {
                    startedLabels.push_back(label);
                    return [this, label] {
                        return cli::startNamedDaemon("/run/contour/" + label,
                                                     { .spawner = spawner,
                                                       .binary = binary,
                                                       .placement = placement,
                                                       .options = {},
                                                       .timeout = std::chrono::seconds { 1 } });
                    };
                },
        });
        window = window::loadSwitcherWindow(engine, *switcher);
        REQUIRE(window != nullptr);
    }

    SwitcherFixture(SwitcherFixture const&) = delete;
    SwitcherFixture& operator=(SwitcherFixture const&) = delete;
    SwitcherFixture(SwitcherFixture&&) = delete;
    SwitcherFixture& operator=(SwitcherFixture&&) = delete;
    ~SwitcherFixture() = default;

    /// Opens the popup over ThisWindow, which is on `default`, and shows it.
    void open(window::SwitcherPrompt prompt = window::SwitcherPrompt::Browse,
              std::optional<window::SwitcherDaemon> pick = std::nullopt,
              SwitchMode mode = SwitchMode::Switch)
    {
        auto const picking = pick.has_value();
        switcher->open(window::SwitcherOpening { .window = ThisWindow,
                                                 .windowDaemon = labelled("default"),
                                                 .mode = mode,
                                                 .prompt = prompt,
                                                 .pick = std::move(pick) });
        window->show();
        REQUIRE(QTest::qWaitForWindowExposed(window.get()));
        if (prompt == window::SwitcherPrompt::NewDaemon)
        {
            REQUIRE(waitUntil([this] {
                return named("switcherLabelField") != nullptr
                       && named("switcherLabelField")->hasActiveFocus();
            }));
            return;
        }
        REQUIRE(waitUntil([this] {
            return named("switcherFilter") != nullptr && named("switcherFilter")->hasActiveFocus();
        }));
        if (!picking)
            // The window's own daemon is listed, and discovery found `admin`.
            REQUIRE(waitUntil(
                [this] { return titles().contains("admin") && titles().contains("3 \"scratch\""); }));
        REQUIRE(waitUntil([this] { return all("switcherRow").size() == switcher->rowCount(); }));
    }

    [[nodiscard]] QQuickItem* named(char const* name) const
    {
        auto const found = all(name);
        return found.isEmpty() ? nullptr : found.front();
    }

    /// The items named @p name, in on-screen order.
    [[nodiscard]] QList<QQuickItem*> all(char const* name) const
    {
        auto found = QList<QQuickItem*> {};
        collectNamed(window->contentItem(), QString::fromLatin1(name), found);
        std::ranges::sort(found, {}, [](QQuickItem const* item) { return item->mapToScene({}).y(); });
        return found;
    }

    /// The rows' titles as drawn.
    [[nodiscard]] QStringList titles() const
    {
        auto texts = QStringList {};
        for (auto const* const item: all("switcherTitle"))
            texts.push_back(item->property("text").toString());
        return texts;
    }

    /// The rows' details as drawn.
    [[nodiscard]] QStringList details() const
    {
        auto texts = QStringList {};
        for (auto const* const item: all("switcherDetail"))
            texts.push_back(item->property("text").toString());
        return texts;
    }

    /// Selects the row titled @p title.
    void selectTitled(QString const& title) const
    {
        auto const& rows = switcher->rows();
        auto const found = std::ranges::find(rows, title.toStdString(), &window::SwitcherRow::title);
        REQUIRE(found != rows.end());
        switcher->setCurrentIndex(static_cast<int>(std::distance(rows.begin(), found)));
    }
};

} // namespace

TEST_CASE("Switcher: the rows are grouped, with each workspace's tabs and state, under every chrome row",
          "[contour][switcher][qml]")
{
    auto const row = GENERATE(config::ChromeStyle::MacOS,
                              config::ChromeStyle::Fluent,
                              config::ChromeStyle::ContourLinux,
                              config::ChromeStyle::Terminal);
    INFO("row " << static_cast<int>(row));
    auto fixture = SwitcherFixture { row };
    fixture.open();

    CHECK(fixture.titles()
          == QStringList { "default",
                           "1 \"work\"",
                           "3 \"scratch\"",
                           "+ new workspace on default",
                           "admin",
                           "New daemon…",
                           "studio" });
    auto const headers = fixture.all("switcherGroupHeader");
    REQUIRE(headers.size() == 2);
    auto const groupTitle = [&](int at) {
        return fixture.switcher
            ->data(fixture.switcher->index(at),
                   static_cast<int>(window::SwitcherController::Roles::GroupTitleRole))
            .toString();
    };
    CHECK(groupTitle(0) == "This computer");
    CHECK(groupTitle(1).isEmpty());
    CHECK(groupTitle(6) == "Saved hosts");
    CHECK(fixture.details().contains("running · 2 workspaces · this window"));
    CHECK(fixture.details().contains("4 tabs · this window"));
    CHECK(fixture.details().contains("1 tab · hidden"));
    CHECK(fixture.details().contains("running · 2 workspaces"));
    // A saved host not yet connected says so, and is not listed until expanded.
    CHECK(fixture.details().contains("not yet connected"));
    CHECK(fixture.listed == std::vector<std::string> { "default" });
    CHECK(fixture.all("switcherTag").size() == 1);
    CHECK(fixture.named("switcherKeys") != nullptr);
    // The window takes its size from tokens, not from literals.
    CHECK(fixture.window->width() > 0);
    CHECK(fixture.window->minimumWidth() == fixture.window->width());
}

TEST_CASE("Switcher: expanding a daemon lists its workspaces; a saved host connects then",
          "[contour][switcher][qml]")
{
    auto fixture = SwitcherFixture {};
    fixture.open();

    fixture.selectTitled("admin");
    QTest::keyClick(fixture.window.get(), Qt::Key_Right);
    REQUIRE(waitUntil([&] { return fixture.titles().contains("2 \"logs\""); }));
    CHECK(fixture.details().contains("3 tabs · hidden"));
    CHECK(fixture.details().contains("1 tab · shown elsewhere"));
    CHECK(fixture.titles().contains("+ new workspace on admin"));

    fixture.selectTitled("studio");
    QTest::keyClick(fixture.window.get(), Qt::Key_Right);
    REQUIRE(waitUntil([&] { return fixture.titles().contains("1 \"build\""); }));
    CHECK(fixture.listed == std::vector<std::string> { "default", "admin", "studio" });

    // Left collapses again, from a workspace row too.
    fixture.selectTitled("1 \"build\"");
    QTest::keyClick(fixture.window.get(), Qt::Key_Left);
    REQUIRE(waitUntil([&] { return !fixture.titles().contains("1 \"build\""); }));
    CHECK(fixture.switcher->rows()[static_cast<std::size_t>(fixture.switcher->currentIndex())].title
          == "studio");
}

TEST_CASE("Switcher: it opens on the first daemon that is not the window's own", "[contour][switcher][qml]")
{
    auto fixture = SwitcherFixture {};
    fixture.open();

    REQUIRE(fixture.switcher->currentIndex() >= 0);
    auto const& selected =
        fixture.switcher->rows()[static_cast<std::size_t>(fixture.switcher->currentIndex())];
    CHECK(selected.kind == window::SwitcherRowKind::Daemon);
    CHECK(selected.title == "admin");

    // A tap on another daemon's disclosure is the user's: the rebuild keeps the selection on that row rather
    // than going back to where the popup started.
    auto const rows = fixture.switcher->rows();
    auto const ownRow = std::ranges::find_if(rows, [](auto const& row) {
        return row.kind == window::SwitcherRowKind::Daemon && row.title == "default";
    });
    REQUIRE(ownRow != rows.end());
    auto const ownIndex = static_cast<int>(std::distance(rows.begin(), ownRow));
    fixture.switcher->toggle(ownIndex);
    auto const& after = fixture.switcher->rows()[static_cast<std::size_t>(fixture.switcher->currentIndex())];
    CHECK(after.title == "default");

    // Asking for the discovery's answer again (a rebuild) keeps it there as well.
    fixture.switcher->toggle(ownIndex);
    CHECK(fixture.switcher->rows()[static_cast<std::size_t>(fixture.switcher->currentIndex())].title
          == "default");
}

TEST_CASE("Switcher: clicking a daemon's disclosure expands it and switches nothing",
          "[contour][switcher][qml]")
{
    auto fixture = SwitcherFixture {};
    fixture.open();
    QSignalSpy const finished { fixture.switcher.get(), &window::SwitcherController::finished };
    auto const rows = fixture.switcher->rows();
    auto const admin = std::ranges::find_if(rows, [](auto const& row) {
        return row.kind == window::SwitcherRowKind::Daemon && row.title == "admin";
    });
    REQUIRE(admin != rows.end());
    REQUIRE(admin->expansion == window::SwitcherExpansion::Collapsed);

    // The disclosure on the "admin" row, as the user clicks it: the row's own tap chooses the row.
    auto const titles = fixture.all("switcherTitle");
    auto const adminTitle = std::ranges::find_if(titles, [](QQuickItem const* item) {
        return item->property("text").toString() == QStringLiteral("admin");
    });
    REQUIRE(adminTitle != titles.end());
    auto const disclosures = fixture.all("switcherDisclosure");
    auto const found = std::ranges::find_if(disclosures, [&](QQuickItem const* item) {
        return item->isVisible() && item->parentItem()->isAncestorOf(*adminTitle);
    });
    REQUIRE(found != disclosures.end());
    auto const* disclosure = *found;
    QTest::mouseClick(
        fixture.window.get(),
        Qt::LeftButton,
        Qt::NoModifier,
        disclosure->mapToScene(QPointF { disclosure->width() / 2, disclosure->height() / 2 }).toPoint());
    QCoreApplication::processEvents();

    CHECK(fixture.requested.empty());
    CHECK(finished.count() == 0);
    auto const after = fixture.switcher->rows();
    auto const expanded = std::ranges::find_if(after, [](auto const& row) {
        return row.kind == window::SwitcherRowKind::Daemon && row.title == "admin";
    });
    REQUIRE(expanded != after.end());
    CHECK(expanded->expansion == window::SwitcherExpansion::Expanded);
}

TEST_CASE("Switcher: a pointer at rest does not take the selection, a pointer that moves does",
          "[contour][switcher][qml]")
{
    auto fixture = SwitcherFixture {};
    fixture.open();
    auto const initial = fixture.switcher->currentIndex();
    auto const rows = fixture.all("switcherRow");
    REQUIRE(rows.size() >= 3);
    auto const centreOf = [](QQuickItem const* item) {
        return item->mapToScene(QPointF { item->width() / 2, item->height() / 2 }).toPoint();
    };
    auto const other = std::ranges::find_if(
        rows, [&](QQuickItem const* item) { return centreOf(item).y() != centreOf(rows[initial]).y(); });
    REQUIRE(other != rows.end());
    auto const otherIndex = static_cast<int>(std::distance(rows.begin(), other));

    // The pointer shows up over a row: where it came to rest is no choice.
    QTest::mouseMove(fixture.window.get(), centreOf(*other));
    QCoreApplication::processEvents();
    CHECK(fixture.switcher->currentIndex() == initial);

    // Moving it is.
    QTest::mouseMove(fixture.window.get(), centreOf(*other) + QPoint { 6, 1 });
    QTest::mouseMove(fixture.window.get(), centreOf(*other) + QPoint { 12, 2 });
    QCoreApplication::processEvents();
    CHECK(fixture.switcher->currentIndex() == otherIndex);
}

TEST_CASE("Switcher: after the pointer moved once, keys and filtering still own the selection",
          "[contour][switcher][qml]")
{
    auto fixture = SwitcherFixture {};
    fixture.open();
    auto const rows = fixture.all("switcherRow");
    REQUIRE(rows.size() >= 4);
    auto const centreOf = [](QQuickItem const* item) {
        return item->mapToScene(QPointF { item->width() / 2, item->height() / 2 }).toPoint();
    };
    auto const spot = centreOf(rows[3]);

    // One nudge over a row, which selects it ...
    QTest::mouseMove(fixture.window.get(), spot);
    QTest::mouseMove(fixture.window.get(), spot + QPoint { 4, 0 });
    QTest::mouseMove(fixture.window.get(), spot + QPoint { 8, 0 });
    QCoreApplication::processEvents();
    REQUIRE(fixture.switcher->currentIndex() == 3);

    // ... then the keys step the selection, the pointer at rest where it is.
    QTest::keyClick(fixture.window.get(), Qt::Key_Down);
    QCoreApplication::processEvents();
    CHECK(fixture.switcher->currentIndex() == 4);
    QTest::keyClick(fixture.window.get(), Qt::Key_Up);
    QTest::keyClick(fixture.window.get(), Qt::Key_Up);
    QCoreApplication::processEvents();
    CHECK(fixture.switcher->currentIndex() == 2);

    // Typing a filter rebuilds the rows under the pointer: the selection is the first match, not the row
    // there.
    for (auto const letter: { 'a', 'd', 'm' })
        QTest::keyClick(fixture.window.get(), letter);
    REQUIRE(waitUntil([&] { return fixture.switcher->filter() == QStringLiteral("adm"); }));
    QCoreApplication::processEvents();
    CHECK(fixture.switcher->currentIndex() == 0);
}

TEST_CASE("Switcher: Return switches, Shift+Return opens, Esc changes nothing", "[contour][switcher][qml]")
{
    auto fixture = SwitcherFixture {};
    fixture.open();
    QSignalSpy const finished { fixture.switcher.get(), &window::SwitcherController::finished };
    QSignalSpy const cancelled { fixture.switcher.get(), &window::SwitcherController::cancelled };

    SECTION("Return")
    {
        fixture.selectTitled("3 \"scratch\"");
        QTest::keyClick(fixture.window.get(), Qt::Key_Return);
        REQUIRE(fixture.requested.size() == 1);
        CHECK(fixture.requested[0].target == labelled("default", workspaceRef("w3")));
        CHECK(fixture.requested[0].mode == SwitchMode::Switch);
        CHECK(finished.count() == 1);
    }
    SECTION("Shift+Return")
    {
        fixture.selectTitled("+ new workspace on default");
        QTest::keyClick(fixture.window.get(), Qt::Key_Return, Qt::ShiftModifier);
        REQUIRE(fixture.requested.size() == 1);
        CHECK(fixture.requested[0].target == labelled("default", session::NewWorkspace {}));
        CHECK(fixture.requested[0].mode == SwitchMode::Open);
    }
    SECTION("Return on an untouched popup switches to the first other daemon")
    {
        QTest::keyClick(fixture.window.get(), Qt::Key_Return);
        REQUIRE(fixture.requested.size() == 1);
        CHECK(fixture.requested[0].target == labelled("admin"));
        CHECK(fixture.requested[0].mode == SwitchMode::Switch);
    }
    SECTION("a daemon row, arrived at with the arrows")
    {
        // The popup opens on the first daemon but the window's own; the list's top is the way to count from.
        fixture.switcher->setCurrentIndex(0);
        QTest::keyClick(fixture.window.get(), Qt::Key_Down);
        QTest::keyClick(fixture.window.get(), Qt::Key_Down);
        QTest::keyClick(fixture.window.get(), Qt::Key_Down);
        QTest::keyClick(fixture.window.get(), Qt::Key_Down);
        QTest::keyClick(fixture.window.get(), Qt::Key_Return);
        REQUIRE(fixture.requested.size() == 1);
        CHECK(fixture.requested[0].target == labelled("admin"));
    }
    SECTION("Esc")
    {
        QTest::keyClick(fixture.window.get(), Qt::Key_Escape);
        CHECK(cancelled.count() == 1);
        CHECK(finished.count() == 0);
        CHECK(fixture.requested.empty());
    }
    SECTION("the filter narrows, and Return takes the match")
    {
        typeText(fixture.window.get(), "scratch");
        REQUIRE(waitUntil([&] { return fixture.titles() == QStringList { "default", "3 \"scratch\"" }; }));
        QTest::keyClick(fixture.window.get(), Qt::Key_Down);
        QTest::keyClick(fixture.window.get(), Qt::Key_Return);
        REQUIRE(fixture.requested.size() == 1);
        CHECK(fixture.requested[0].target == labelled("default", workspaceRef("w3")));
    }
}

TEST_CASE("Switcher: a pick lists the one daemon's workspaces and re-enters with the pick's mode",
          "[contour][switcher][qml]")
{
    auto fixture = SwitcherFixture {};
    auto const mode = GENERATE(SwitchMode::Switch, SwitchMode::Open);
    fixture.open(window::SwitcherPrompt::Browse,
                 window::SwitcherDaemon { .address = labelled("admin"),
                                          .workspaceCount = 2,
                                          .tag = {},
                                          .expansion = window::SwitcherExpansion::Expanded,
                                          .listing = window::SwitcherListing::Listed,
                                          .failure = {},
                                          .workspaces = fixture.workspaces.at("admin") },
                 mode);

    CHECK(fixture.titles() == QStringList { "1 \"ops\"", "2 \"logs\"", "+ new workspace on admin" });
    REQUIRE(fixture.all("switcherGroupHeader").size() == 1);
    CHECK(fixture.switcher->pickTitle() == "Workspaces on admin");
    // Nothing is listed or discovered: the switch has listed it.
    CHECK(fixture.listed.empty());

    SECTION("Return")
    {
        QTest::keyClick(fixture.window.get(), Qt::Key_Down);
        QTest::keyClick(fixture.window.get(), Qt::Key_Return);
        REQUIRE(fixture.requested.size() == 1);
        CHECK(fixture.requested[0].target == labelled("admin", workspaceRef("a2")));
        CHECK(fixture.requested[0].mode == mode);
    }
    SECTION("Esc cancels the switch")
    {
        QTest::keyClick(fixture.window.get(), Qt::Key_Escape);
        CHECK(fixture.requested.empty());
    }
}

TEST_CASE("Switcher: New daemon… starts a persistent daemon and opens a window on it",
          "[contour][switcher][qml][threading]")
{
    auto fixture = SwitcherFixture {};
    QSignalSpy const finished { fixture.switcher.get(), &window::SwitcherController::finished };

    SECTION("from the action")
    {
        fixture.open(window::SwitcherPrompt::NewDaemon);
    }
    SECTION("from its row")
    {
        fixture.open();
        fixture.selectTitled("New daemon…");
        QTest::keyClick(fixture.window.get(), Qt::Key_Return);
        REQUIRE(waitUntil([&] {
            return fixture.named("switcherLabelField") != nullptr
                   && fixture.named("switcherLabelField")->hasActiveFocus();
        }));
    }
    CHECK(fixture.switcher->isAskingLabel());

    typeText(fixture.window.get(), "work2");
    QTest::keyClick(fixture.window.get(), Qt::Key_Return);
    REQUIRE(waitUntil([&] { return !fixture.requested.empty(); }));

    CHECK(fixture.startedLabels == std::vector<std::string> { "work2" });
    auto const spawned = fixture.spawner.spawned();
    REQUIRE(spawned.size() == 1);
    CHECK(std::ranges::contains(spawned.front(), std::string { "--lifecycle=persistent" }));
    REQUIRE(fixture.requested.size() == 1);
    CHECK(fixture.requested[0].target == labelled("work2"));
    CHECK(fixture.requested[0].mode == SwitchMode::Open);
    CHECK(finished.count() == 1);
}

TEST_CASE("Switcher: New daemon… offers to switch to a label already running", "[contour][switcher][qml]")
{
    auto fixture = SwitcherFixture {};
    fixture.open();
    // Discovery has answered: `admin` is running.
    fixture.selectTitled("New daemon…");
    QTest::keyClick(fixture.window.get(), Qt::Key_Return);
    REQUIRE(waitUntil([&] {
        return fixture.named("switcherLabelField") != nullptr
               && fixture.named("switcherLabelField")->hasActiveFocus();
    }));

    typeText(fixture.window.get(), "admin");
    REQUIRE(waitUntil([&] { return fixture.named("switcherSwitchOffer") != nullptr; }));
    CHECK(fixture.named("switcherSwitchOffer")->property("text").toString() == "Switch to daemon admin");
    QTest::keyClick(fixture.window.get(), Qt::Key_Return);
    QTest::qWait(50);
    CHECK(fixture.startedLabels.empty());
    CHECK(fixture.requested.empty());

    QMetaObject::invokeMethod(fixture.named("switcherSwitchOffer"), "clicked");
    REQUIRE(fixture.requested.size() == 1);
    CHECK(fixture.requested[0].target == labelled("admin"));
    CHECK(fixture.requested[0].mode == SwitchMode::Switch);
}

TEST_CASE("Switcher: New daemon… refuses an invalid label inline", "[contour][switcher][qml]")
{
    auto fixture = SwitcherFixture {};
    fixture.open(window::SwitcherPrompt::NewDaemon);
    CHECK(fixture.named("switcherLabelError") == nullptr);

    typeText(fixture.window.get(), "a/b");
    REQUIRE(waitUntil([&] { return fixture.named("switcherLabelError") != nullptr; }));
    CHECK(fixture.named("switcherLabelError")->property("text").toString().contains("letters, digits"));
    QTest::keyClick(fixture.window.get(), Qt::Key_Return);
    QTest::qWait(50);
    CHECK(fixture.startedLabels.empty());
    CHECK(fixture.requested.empty());
    CHECK(fixture.window->isVisible());
    // An empty label is refused on Return too.
    fixture.switcher->submitLabel({});
    CHECK(fixture.switcher->labelError().contains("Type a label"));
}

TEST_CASE("Switcher: reopening it while it looks for daemons starts no second discovery",
          "[contour][switcher][threading]")
{
    // The workspace chip clicked again and again: each click reopens the popup. One discovery serves them
    // all, and its answer reaches the opening under way when it comes.
    auto const discovery = std::make_shared<GatedDiscovery>();
    auto switcher = controllerOver(discovery);
    auto const opening = window::SwitcherOpening { .window = ThisWindow,
                                                   .windowDaemon = labelled("default"),
                                                   .mode = SwitchMode::Switch,
                                                   .prompt = window::SwitcherPrompt::Browse,
                                                   .pick = std::nullopt };
    switcher->open(opening);
    REQUIRE(discovery->awaitCalls(1));
    switcher->open(opening);
    switcher->cancel();
    switcher->open(opening);
    CHECK(discovery->calls() == 1);

    discovery->releaseOne();
    CHECK(waitUntil([&] { return listsDaemon(*switcher, "admin"); }));
    CHECK(discovery->calls() == 1);
    CHECK(discovery->mostAtOnce() == 1);

    // Once it answered, the next opening looks again.
    switcher->open(opening);
    REQUIRE(discovery->awaitCalls(2));
    discovery->releaseOne();
    CHECK(waitUntil([&] { return listsDaemon(*switcher, "admin"); }));
}

TEST_CASE("Switcher: a discovery answering after the popup closed adds nothing",
          "[contour][switcher][threading]")
{
    auto const discovery = std::make_shared<GatedDiscovery>();
    auto switcher = controllerOver(discovery);
    auto const opening = window::SwitcherOpening { .window = ThisWindow,
                                                   .windowDaemon = labelled("default"),
                                                   .mode = SwitchMode::Switch,
                                                   .prompt = window::SwitcherPrompt::Browse,
                                                   .pick = std::nullopt };
    // Opened and closed in quick succession, the discovery under way all along.
    for ([[maybe_unused]] auto const round: std::views::iota(0, 5))
    {
        switcher->open(opening);
        switcher->cancel();
    }
    REQUIRE(discovery->awaitCalls(1));
    discovery->releaseOne();
    // The answer comes back to a closed popup: dropped, and the controller free to look again.
    QTest::qWait(50);
    CHECK_FALSE(listsDaemon(*switcher, "admin"));
    CHECK(discovery->calls() == 1);

    // Once the dropped answer is in, the next opening looks again -- and still only one discovery at a time.
    CHECK(waitUntil([&] {
        switcher->open(opening);
        return discovery->calls() == 2;
    }));
    CHECK(discovery->mostAtOnce() == 1);

    // Closed, then destroyed with its discovery still running: the answer reaches no controller.
    switcher->cancel();
    discovery->releaseOne(); // the destructor joins the worker; it must be free to answer
    switcher.reset();
    QTest::qWait(20);
    CHECK(discovery->calls() == 2);
}

TEST_CASE("Switcher: a popup reopened while it has the focus stays over its window", "[contour][switcher]")
{
    auto terminal = QWindow {};
    auto popup = QWindow {};
    CHECK(window::popupParentFor(&terminal, popup) == &terminal);
    CHECK(window::popupParentFor(nullptr, popup) == nullptr);

    popup.setTransientParent(&terminal);
    // Focused itself, the popup is not made its own parent: it keeps the one it has.
    CHECK(window::popupParentFor(&popup, popup) == &terminal);
}
