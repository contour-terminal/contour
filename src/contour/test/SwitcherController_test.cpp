// SPDX-License-Identifier: Apache-2.0
//
// The switcher popup's controller over fake services, with no window: its levels, the status its rows show,
// what its keys do, and what it does when a daemon answers late, wrongly or never.

// The actions defined, not only declared: beside vthost::proto's ClosePane and ResizePane (through the switch
// vocabulary), ActionsFwd's declarations alone read as a namespace mix-up to clang-tidy.
#include <contour/config/Actions.hpp>
#include <contour/test/SwitcherFakes.hpp>
#include <contour/window/SwitcherController.hpp>

#include <QtCore/QCoreApplication>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

#include <QtTest/QSignalSpy>
#include <QtTest/QTest>

using namespace contour;
using contour::session::SwitchMode;
using contour::session::SwitchTarget;
using contour::test::switcherLabelled;
using contour::test::switcherWaitUntil;
using contour::test::switcherWorkspaceRef;
using contour::window::SwitcherRowKind;
using contour::window::SwitcherWorkspaceState;
using contour::window::Urgency;
using vthost::proto::SwitchDaemonKind;

namespace
{

/// The window the popup is over.
constexpr auto ThisWindow = vtworkspace::WorkspaceId { 4 };

[[nodiscard]] window::SwitcherWorkspace workspace(
    uint64_t id,
    uint32_t position,
    std::string name,
    SwitcherWorkspaceState state = SwitcherWorkspaceState::Hidden,
    Urgency urgency = Urgency::Idle)
{
    auto status = window::SwitcherStatus {};
    status.urgency = urgency;
    return window::SwitcherWorkspace { .ref = switcherWorkspaceRef(name),
                                       .position = position,
                                       .name = std::move(name),
                                       .tabs = 1,
                                       .state = state,
                                       .status = std::move(status),
                                       .id = id };
}

/// Boards 1-3: the window is on `work` showing `contour`; `home` and `lab` are other local daemons, `ci-box`
/// a saved host.
[[nodiscard]] std::vector<window::SwitcherWorkspace> workWorkspaces()
{
    return { workspace(1, 1, "contour", SwitcherWorkspaceState::ThisWindow, Urgency::Working),
             workspace(2, 2, "api-gateway", SwitcherWorkspaceState::Hidden, Urgency::NeedsYou),
             workspace(3, 3, "docs"),
             workspace(4, 4, "scratch") };
}

/// The controller over a world, opened over ThisWindow on `work`.
struct Switcher
{
    test::SwitcherWorld world;
    std::unique_ptr<window::SwitcherController> controller;

    Switcher()
    {
        world.found = { { .label = "home", .socket = "/run/contour/home", .workspaces = 2 },
                        { .label = "lab", .socket = "/run/contour/lab", .workspaces = 1 },
                        { .label = "work", .socket = "/run/contour/work", .workspaces = 4 } };
        world.workspaces = { { "home", { workspace(11, 1, "api-docs"), workspace(12, 2, "dotfiles") } },
                             { "lab", { workspace(21, 1, "rapid-proto") } },
                             { "ci-box", { workspace(31, 1, "ci-runner") } } };
        world.hosts = { window::SwitcherDaemon {
            .address = { .kind = SwitchDaemonKind::Host, .daemon = "ci-box", .workspace = {} },
            .workspaceCount = std::nullopt,
            .tag = "SSH",
            .listing = window::SwitcherListing::NotListed,
            .failure = {},
            .workspaces = {} } };
        controller = std::make_unique<window::SwitcherController>(world.services());
    }

    /// Opens the popup as @p origin opens it, with @p scope, and waits for every local daemon's listing.
    void open(window::SwitcherOrigin origin = window::SwitcherOrigin::Centred,
              window::SwitcherScope scope = window::SwitcherScope::Everything,
              window::SwitcherPrompt prompt = window::SwitcherPrompt::Browse) const
    {
        controller->open(window::SwitcherOpening { .window = ThisWindow,
                                                   .windowDaemon = switcherLabelled("work"),
                                                   .mode = SwitchMode::Switch,
                                                   .prompt = prompt,
                                                   .pick = std::nullopt,
                                                   .scope = scope,
                                                   .windowWorkspaces = workWorkspaces(),
                                                   .origin = origin });
        if (scope == window::SwitcherScope::Everything)
            REQUIRE(settled());
    }

    /// @return Whether discovery answered and every local daemon not held back was listed.
    [[nodiscard]] bool settled() const
    {
        return switcherWaitUntil([this] {
            return !controller->isDiscovering()
                   && std::ranges::all_of(controller->daemons(), [&](window::SwitcherDaemon const& each) {
                          return each.address.kind != SwitchDaemonKind::Label
                                 || std::ranges::contains(world.heldBack, each.address.daemon)
                                 || each.listing == window::SwitcherListing::Listed
                                 || each.listing == window::SwitcherListing::Failed;
                      });
        });
    }

    [[nodiscard]] std::vector<std::string> titles() const
    {
        return std::ranges::to<std::vector>(controller->rows()
                                            | std::views::transform(&window::SwitcherRow::title));
    }

    /// @return The index of the row of @p kind titled @p title.
    [[nodiscard]] int rowOf(std::string_view title, SwitcherRowKind kind = SwitcherRowKind::Workspace) const
    {
        auto const& rows = controller->rows();
        auto const found = std::ranges::find_if(
            rows, [&](window::SwitcherRow const& row) { return row.title == title && row.kind == kind; });
        REQUIRE(found != rows.end());
        return static_cast<int>(std::distance(rows.begin(), found));
    }

    void select(std::string_view title, SwitcherRowKind kind = SwitcherRowKind::Workspace) const
    {
        controller->setCurrentIndex(rowOf(title, kind));
    }

    [[nodiscard]] window::SwitcherRow const& selected() const
    {
        REQUIRE(controller->currentIndex() >= 0);
        return controller->rows().at(static_cast<std::size_t>(controller->currentIndex()));
    }

    [[nodiscard]] QString role(int row, window::SwitcherController::Roles role) const
    {
        return controller->data(controller->index(row), static_cast<int>(role)).toString();
    }

    /// Presses @p key in the field, its caret at the end of what is typed.
    bool press(int key, Qt::KeyboardModifiers modifiers = Qt::NoModifier) const
    {
        return controller->handleKey(
            key, static_cast<int>(modifiers), static_cast<int>(controller->filter().size()));
    }
};

} // namespace

TEST_CASE("Switcher: the field's name is notified with the daemon level's title", "[contour][switcher]")
{
    auto s = Switcher {};
    s.open();
    s.select("home", SwitcherRowKind::Daemon);
    // What a binding reads when it is told: the level is entered before its title is decided.
    auto heard = std::vector<QString> {};
    QObject::connect(s.controller.get(), &window::SwitcherController::presentationChanged, [&] {
        heard.push_back(s.controller->fieldName());
    });

    CHECK(s.press(Qt::Key_Return));
    REQUIRE_FALSE(heard.empty());
    CHECK(heard.back() == "Switch to a workspace of home");
    CHECK(s.controller->fieldName() == "Switch to a workspace of home");

    CHECK(s.press(Qt::Key_Escape));
    CHECK(heard.back() == "Switch workspace or daemon");
}

TEST_CASE("Switcher: Return on a daemon drills in; Escape comes back to the daemon row",
          "[contour][switcher]")
{
    auto s = Switcher {};
    s.open();
    s.select("home", SwitcherRowKind::Daemon);

    CHECK(s.press(Qt::Key_Return));
    CHECK(s.controller->level() == static_cast<int>(window::SwitcherLevel::Daemon));
    CHECK(s.controller->levelTitle() == "home");
    CHECK(s.titles() == std::vector<std::string> { "api-docs", "dotfiles", "New Workspace" });
    CHECK(s.controller->rows().front().groupTitle == "Workspaces on home");
    CHECK(s.controller->hintText() == "Filter workspaces on home…");

    CHECK(s.press(Qt::Key_Escape));
    CHECK(s.controller->level() == static_cast<int>(window::SwitcherLevel::Top));
    CHECK(s.selected().title == "home");
    CHECK(s.selected().kind == SwitcherRowKind::Daemon);

    SECTION("→ drills in as well, and ⌫ in the empty field comes back")
    {
        CHECK(s.press(Qt::Key_Right));
        CHECK(s.controller->level() == static_cast<int>(window::SwitcherLevel::Daemon));
        CHECK(s.press(Qt::Key_Backspace));
        CHECK(s.selected().title == "home");
    }
    SECTION("Alt+⌫ is the field's")
    {
        CHECK(s.press(Qt::Key_Right));
        CHECK_FALSE(s.press(Qt::Key_Backspace, Qt::AltModifier));
        CHECK(s.controller->level() == static_cast<int>(window::SwitcherLevel::Daemon));
    }
}

TEST_CASE("Switcher: a window reaching no other daemon lists its own workspaces and looks for none",
          "[contour][switcher]")
{
    auto s = Switcher {};
    s.controller->open(window::SwitcherOpening { .window = ThisWindow,
                                                 .windowDaemon = switcherLabelled("work"),
                                                 .mode = SwitchMode::Switch,
                                                 .prompt = window::SwitcherPrompt::Browse,
                                                 .pick = std::nullopt,
                                                 .scope = window::SwitcherScope::Everything,
                                                 .windowWorkspaces = workWorkspaces(),
                                                 .origin = window::SwitcherOrigin::Chip,
                                                 .reach = window::SwitcherReach::WindowOnly });
    auto const titles = s.titles();
    CHECK(std::ranges::contains(titles, "contour"));
    CHECK(std::ranges::contains(titles, "New Workspace"));
    CHECK_FALSE(std::ranges::contains(titles, "All daemons…"));
    CHECK_FALSE(std::ranges::contains(titles, "Looking for other daemons…"));
    CHECK_FALSE(std::ranges::contains(titles, "home"));
    CHECK(s.world.listed.empty());
}

TEST_CASE("Switcher: typing lists the matches of every listed local daemon", "[contour][switcher]")
{
    auto s = Switcher {};
    s.open();
    // Every local daemon was listed as it opened; the saved host was not.
    CHECK(std::ranges::contains(s.world.listed, "home"));
    CHECK(std::ranges::contains(s.world.listed, "lab"));
    CHECK_FALSE(std::ranges::contains(s.world.listed, "ci-box"));
    CHECK_FALSE(std::ranges::contains(s.world.listed, "work"));

    s.controller->setFilter("api");
    CHECK(s.titles()
          == std::vector<std::string> { "api-gateway", "api-docs", "rapid-proto", "New Workspace “api”" });
    CHECK(s.role(1, window::SwitcherController::Roles::PillTextRole) == "home");
    CHECK(s.controller
              ->data(s.controller->index(0),
                     static_cast<int>(window::SwitcherController::Roles::TitleMatchesRole))
              .toList()
          == QVariantList { 0, 1, 2 });
    // The best match is selected, and the footer names its daemon.
    CHECK(s.selected().title == "api-gateway");
    CHECK(s.controller->footerDescription() == "Switch this window to api-gateway on work");
}

TEST_CASE("Switcher: a daemon whose listing fails says why and the rest still filters", "[contour][switcher]")
{
    auto s = Switcher {};
    s.world.failures = { { "home", "connection refused" } };
    s.open();

    auto const home = s.rowOf("home", SwitcherRowKind::Daemon);
    CHECK(s.role(home, window::SwitcherController::Roles::DetailTextRole) == "connection refused");

    s.controller->setFilter("pro");
    CHECK(s.titles() == std::vector<std::string> { "rapid-proto", "New Workspace “pro”" });

    // Drilled into, it says why there is nothing, and asks again.
    s.controller->setFilter({});
    s.select("home", SwitcherRowKind::Daemon);
    s.world.failures.clear();
    s.world.heldBack = { "home" };
    CHECK(s.press(Qt::Key_Return));
    CHECK(s.titles() == std::vector<std::string> { "Connecting to home…", "New Workspace" });
    s.world.failures = { { "home", "connection refused" } };
    s.world.answer("home");
    CHECK(s.titles()
          == std::vector<std::string> { "Could not list home: connection refused", "New Workspace" });
}

TEST_CASE("Switcher: backing out of a connecting host drops its answer", "[contour][switcher]")
{
    auto s = Switcher {};
    s.world.heldBack = { "ci-box" };
    s.open();
    auto const before = s.titles();
    s.select("ci-box", SwitcherRowKind::Daemon);

    CHECK(s.press(Qt::Key_Return));
    CHECK(s.world.listed.back() == "ci-box");
    CHECK(s.titles() == std::vector<std::string> { "Connecting to ci-box…", "New Workspace" });
    // Typing never waits for it.
    s.controller->setFilter("x");
    s.controller->setFilter({});

    CHECK(s.press(Qt::Key_Escape));
    CHECK(s.controller->level() == static_cast<int>(window::SwitcherLevel::Top));
    s.world.answer("ci-box");
    QCoreApplication::processEvents();
    CHECK(s.titles() == before);
    CHECK(s.controller->level() == static_cast<int>(window::SwitcherLevel::Top));
    CHECK(
        s.role(s.rowOf("ci-box", SwitcherRowKind::Daemon), window::SwitcherController::Roles::DetailTextRole)
        == "Not connected");
}

TEST_CASE("Switcher: a status update keeps the selection on its workspace", "[contour][switcher]")
{
    auto s = Switcher {};
    s.open();
    s.controller->setCurrentIndex(2);
    auto const target = s.selected().target;
    REQUIRE(s.selected().title == "docs");
    QSignalSpy const reset { s.controller.get(), &QAbstractItemModel::modelReset };

    SECTION("the same rows saying something new are updated in place")
    {
        auto updated = workWorkspaces();
        updated[2].status.urgency = Urgency::Failed;
        updated[2].status.glyph = window::TabBadgeKind::Error;
        s.controller->updateWindowWorkspaces(updated);
        CHECK(s.selected().target == target);
        CHECK(s.selected().status.urgency == Urgency::Failed);
        CHECK(reset.isEmpty());
    }
    SECTION("reordered, the selection follows its workspace")
    {
        auto reordered = workWorkspaces();
        std::ranges::reverse(reordered);
        s.controller->updateWindowWorkspaces(reordered);
        CHECK(s.selected().target == target);
        CHECK(s.selected().title == "docs");
    }
}

TEST_CASE("Switcher: New Workspace “q” creates a labelled workspace and switches this window",
          "[contour][switcher]")
{
    auto s = Switcher {};
    s.open();
    QSignalSpy const finished { s.controller.get(), &window::SwitcherController::finished };
    s.controller->setFilter("q");
    s.select("New Workspace “q”", SwitcherRowKind::NewWorkspace);

    SECTION("Return switches this window")
    {
        CHECK(s.press(Qt::Key_Return));
        REQUIRE(s.world.created.size() == 1);
        CHECK(s.world.created[0].daemon == switcherLabelled("work"));
        CHECK(s.world.created[0].label == "q");
        CHECK(s.world.created[0].mode == SwitchMode::Switch);
        CHECK(finished.size() == 1);
        CHECK(s.world.requested.empty());
    }
    SECTION("Shift+Return opens a new window")
    {
        CHECK(s.press(Qt::Key_Return, Qt::ShiftModifier));
        REQUIRE(s.world.created.size() == 1);
        CHECK(s.world.created[0].mode == SwitchMode::Open);
        CHECK(finished.size() == 1);
    }

    SECTION("with nothing typed it is a plain new workspace")
    {
        auto plain = Switcher {};
        plain.open();
        plain.select("New Workspace", SwitcherRowKind::NewWorkspace);
        CHECK(plain.press(Qt::Key_Return));
        REQUIRE(plain.world.requested.size() == 1);
        CHECK(plain.world.requested[0].target == switcherLabelled("work", session::NewWorkspace {}));
    }
}

TEST_CASE("Switcher: New Daemon starts the daemon and switches this window; Shift opens a new one",
          "[contour][switcher]")
{
    auto s = Switcher {};
    s.open(window::SwitcherOrigin::Centred,
           window::SwitcherScope::Everything,
           window::SwitcherPrompt::NewDaemon);
    CHECK(s.controller->level() == static_cast<int>(window::SwitcherLevel::NewDaemon));
    CHECK(s.controller->levelTitle() == "New Daemon");

    s.controller->checkLabel("research");
    CHECK(s.controller->labelVerdict() == static_cast<int>(window::LabelVerdict::Available));
    CHECK(s.controller->labelMessage() == "Available");
    CHECK(s.controller->labelCommand() == "contour daemon --label research");
    REQUIRE(s.controller->rows().size() == 1);
    CHECK(s.selected().kind == SwitcherRowKind::LabelAction);

    auto const shifted = GENERATE(false, true);
    CHECK(s.press(Qt::Key_Return, shifted ? Qt::ShiftModifier : Qt::NoModifier));
    REQUIRE(switcherWaitUntil([&] { return !s.world.requested.empty(); }));
    CHECK(s.world.startedLabels == std::vector<std::string> { "research" });
    CHECK(s.world.requested[0].target == switcherLabelled("research", session::AnyWorkspace {}));
    CHECK(s.world.requested[0].mode == (shifted ? SwitchMode::Open : SwitchMode::Switch));

    SECTION("a label already running switches to it, starting nothing")
    {
        auto running = Switcher {};
        running.open(window::SwitcherOrigin::Centred,
                     window::SwitcherScope::Everything,
                     window::SwitcherPrompt::NewDaemon);
        running.controller->checkLabel("home");
        CHECK(running.controller->labelVerdict() == static_cast<int>(window::LabelVerdict::Running));
        CHECK(running.controller->footerDescription() == "Switch this window to daemon “home”");
        CHECK(running.press(Qt::Key_Return));
        CHECK(running.world.startedLabels.empty());
        REQUIRE(running.world.requested.size() == 1);
        CHECK(running.world.requested[0].target == switcherLabelled("home", session::AnyWorkspace {}));
    }
    SECTION("an invalid label offers nothing, and Escape clears it, then goes back")
    {
        auto invalid = Switcher {};
        invalid.open(window::SwitcherOrigin::Centred,
                     window::SwitcherScope::Everything,
                     window::SwitcherPrompt::NewDaemon);
        invalid.controller->checkLabel("a b");
        CHECK(invalid.controller->labelVerdict() == static_cast<int>(window::LabelVerdict::Invalid));
        CHECK(invalid.controller->rows().empty());
        CHECK(invalid.press(Qt::Key_Return));
        CHECK(invalid.world.startedLabels.empty());
        CHECK(invalid.press(Qt::Key_Escape));
        CHECK(invalid.controller->filter().isEmpty());
        CHECK(invalid.controller->level() == static_cast<int>(window::SwitcherLevel::NewDaemon));
        CHECK(invalid.press(Qt::Key_Escape));
        CHECK(invalid.controller->level() == static_cast<int>(window::SwitcherLevel::Top));
    }
}

TEST_CASE("Switcher: the footer follows the selected row", "[contour][switcher]")
{
    auto s = Switcher {};
    s.open();
    using window::SwitcherCommand;
    using window::SwitcherHintSubject;

    // Untouched, it opens on the first workspace this window does not show.
    CHECK(s.selected().title == "api-gateway");
    CHECK(s.controller->footerDescription() == "Switch this window to api-gateway");
    CHECK(s.controller->footerHints().toStdString()
          == window::switcherFooterHints(
              SwitcherHintSubject::Workspace, SwitcherCommand::Close, s.world.style, SwitchMode::Switch));

    s.select("home", SwitcherRowKind::Daemon);
    CHECK(s.controller->footerDescription() == "Show the workspaces on home");
    CHECK(s.controller->footerHints().toStdString()
          == window::switcherFooterHints(
              SwitcherHintSubject::Daemon, SwitcherCommand::Close, s.world.style, SwitchMode::Switch));

    s.controller->setFilter("zzz");
    CHECK(s.selected().kind == SwitcherRowKind::NewWorkspace);
    CHECK(s.controller->emptyText() == "No workspace matches “zzz”");
    CHECK(s.controller->footerHints().toStdString()
          == window::switcherFooterHints(SwitcherHintSubject::NewWorkspace,
                                         SwitcherCommand::ClearFilter,
                                         s.world.style,
                                         SwitchMode::Switch));
}

TEST_CASE("Switcher: the chip scope widens to every daemon and Escape narrows it back", "[contour][switcher]")
{
    auto s = Switcher {};
    QSignalSpy const cancelled { s.controller.get(), &window::SwitcherController::cancelled };
    s.open(window::SwitcherOrigin::Chip, window::SwitcherScope::OneDaemon);

    // The window's own daemon is its sidebar's: listed, never "Connecting to work…".
    CHECK(s.titles()
          == std::vector<std::string> {
              "contour", "api-gateway", "docs", "scratch", "New Workspace", "All daemons…" });
    CHECK(s.world.listed.empty());
    CHECK(s.controller->hintText() == "Filter workspaces on work…");
    CHECK(s.controller->footerDescription() == "api-gateway");

    s.select("All daemons…", SwitcherRowKind::AllDaemons);
    CHECK(s.press(Qt::Key_Return));
    CHECK(s.controller->scope() == window::SwitcherScope::Everything);
    REQUIRE(s.settled());
    CHECK(std::ranges::contains(s.titles(), "home"));
    CHECK(std::ranges::contains(s.titles(), "ci-box"));

    CHECK(s.press(Qt::Key_Escape));
    CHECK(s.controller->scope() == window::SwitcherScope::OneDaemon);
    CHECK(s.selected().kind == SwitcherRowKind::AllDaemons);
    CHECK(cancelled.isEmpty());
    CHECK(s.press(Qt::Key_Escape));
    CHECK(cancelled.size() == 1);
}

TEST_CASE("Switcher: the selection stays on New Daemon… while the looking row comes and goes",
          "[contour][switcher]")
{
    auto s = Switcher {};
    auto const discovery = std::make_shared<test::GatedDiscovery>();
    auto services = s.world.services();
    services.discovery = discovery;
    s.controller = std::make_unique<window::SwitcherController>(std::move(services));
    s.controller->open(window::SwitcherOpening { .window = ThisWindow,
                                                 .windowDaemon = switcherLabelled("work"),
                                                 .mode = SwitchMode::Switch,
                                                 .prompt = window::SwitcherPrompt::Browse,
                                                 .pick = std::nullopt,
                                                 .scope = window::SwitcherScope::Everything,
                                                 .windowWorkspaces = workWorkspaces(),
                                                 .origin = window::SwitcherOrigin::Centred });
    REQUIRE(std::ranges::contains(s.titles(), "Looking for other daemons…"));
    s.select("New Daemon…", SwitcherRowKind::NewDaemon);
    s.controller->updateWindowWorkspaces(workWorkspaces());
    CHECK(s.selected().kind == SwitcherRowKind::NewDaemon);

    REQUIRE(discovery->awaitCalls(1));
    discovery->releaseOne();
    REQUIRE(switcherWaitUntil([&] { return !s.controller->isDiscovering(); }));
    CHECK(s.selected().kind == SwitcherRowKind::NewDaemon);
}

TEST_CASE("Switcher: a workspace row's menu leads with the switch, then the sidebar's rows",
          "[contour][switcher]")
{
    auto s = Switcher {};
    s.open();
    QSignalSpy const asked { s.controller.get(), &window::SwitcherController::rowMenuRequested };
    s.select("docs");
    CHECK(s.press(Qt::Key_F10, Qt::ShiftModifier));
    REQUIRE(asked.size() == 1);
    auto const row = asked.front().front().toInt();

    auto const menu = s.controller->rowMenu(row);
    REQUIRE(menu.size() > 3);
    CHECK(menu[0].toMap()["title"] == "Switch to docs");
    CHECK(menu[1].toMap()["title"] == "Open in New Window");
    CHECK(menu[2].toMap()["kind"] == "separator");

    SECTION("a sidebar row runs on that workspace")
    {
        auto const command = std::ranges::find_if(menu | std::views::drop(3), [](QVariant const& each) {
            return each.toMap()["kind"] == "command";
        });
        REQUIRE(command != (menu | std::views::drop(3)).end());
        s.controller->triggerRowMenu(row, command->toMap()["actionId"].toInt());
        REQUIRE(s.world.ran.size() == 1);
        CHECK(s.world.ran[0].workspace == 3);
    }
    SECTION("the leading row switches")
    {
        s.controller->triggerRowMenu(row, menu[1].toMap()["actionId"].toInt());
        REQUIRE(s.world.requested.size() == 1);
        CHECK(s.world.requested[0].mode == SwitchMode::Open);
    }
    SECTION("another daemon's workspace offers the switch alone")
    {
        s.controller->setFilter("dotf");
        CHECK(s.controller->rowMenu(s.rowOf("dotfiles")).size() == 2);
    }
    SECTION("a pick whose row went away ends the opening")
    {
        // The workspace closed while its menu was up: the pick finds nothing to act on, and the switcher,
        // which the menu's pick already closed, must still let go of the window.
        QSignalSpy const cancelled { s.controller.get(), &window::SwitcherController::cancelled };
        auto remaining = workWorkspaces();
        std::erase_if(remaining, [](window::SwitcherWorkspace const& each) { return each.name == "docs"; });
        s.controller->updateWindowWorkspaces(remaining);
        REQUIRE_FALSE(std::ranges::contains(s.titles(), "docs"));

        auto const firstSidebarRow =
            std::ranges::find_if(menu | std::views::drop(3),
                                 [](QVariant const& each) { return each.toMap()["kind"] == "command"; });
        REQUIRE(firstSidebarRow != (menu | std::views::drop(3)).end());
        for (auto const id:
             { menu[0].toMap()["actionId"].toInt(), firstSidebarRow->toMap()["actionId"].toInt() })
        {
            s.controller->triggerRowMenu(row, id);
        }
        CHECK(cancelled.size() == 2);
        CHECK(s.world.ran.empty());
        CHECK(s.world.requested.empty());
    }
    SECTION("a pick with no menu behind it ends the opening")
    {
        QSignalSpy const cancelled { s.controller.get(), &window::SwitcherController::cancelled };
        s.controller->triggerRowMenu(row, -1);
        CHECK(cancelled.size() == 1);
    }
}

TEST_CASE("Switcher: reopening it while it looks for daemons starts no second discovery",
          "[contour][switcher][threading]")
{
    // The workspace chip clicked again and again: each click reopens the popup. One discovery serves them
    // all, and its answer reaches the opening under way when it comes.
    test::SwitcherWorld world;
    auto services = world.services();
    auto const discovery = std::make_shared<test::GatedDiscovery>();
    services.discovery = discovery;
    auto switcher = std::make_unique<window::SwitcherController>(std::move(services));
    auto const opening = window::SwitcherOpening { .window = ThisWindow,
                                                   .windowDaemon = switcherLabelled("default"),
                                                   .mode = SwitchMode::Switch,
                                                   .prompt = window::SwitcherPrompt::Browse,
                                                   .pick = std::nullopt };
    auto const listsAdmin = [&] {
        return std::ranges::any_of(switcher->rows(), [](window::SwitcherRow const& row) {
            return row.kind == SwitcherRowKind::Daemon && row.title == "admin";
        });
    };
    switcher->open(opening);
    REQUIRE(discovery->awaitCalls(1));
    switcher->open(opening);
    switcher->cancel();
    switcher->open(opening);
    CHECK(discovery->calls() == 1);

    discovery->releaseOne();
    CHECK(switcherWaitUntil(listsAdmin));
    CHECK(discovery->calls() == 1);
    CHECK(discovery->mostAtOnce() == 1);

    // Once it answered, the next opening looks again.
    switcher->open(opening);
    REQUIRE(discovery->awaitCalls(2));
    discovery->releaseOne();
    CHECK(switcherWaitUntil(listsAdmin));
}

TEST_CASE("Switcher: a discovery answering after the popup closed adds nothing",
          "[contour][switcher][threading]")
{
    test::SwitcherWorld world;
    auto services = world.services();
    auto const discovery = std::make_shared<test::GatedDiscovery>();
    services.discovery = discovery;
    auto switcher = std::make_unique<window::SwitcherController>(std::move(services));
    auto const opening = window::SwitcherOpening { .window = ThisWindow,
                                                   .windowDaemon = switcherLabelled("default"),
                                                   .mode = SwitchMode::Switch,
                                                   .prompt = window::SwitcherPrompt::Browse,
                                                   .pick = std::nullopt };
    auto const listsAdmin = [&] {
        return std::ranges::any_of(switcher->rows(), [](window::SwitcherRow const& row) {
            return row.kind == SwitcherRowKind::Daemon && row.title == "admin";
        });
    };
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
    CHECK_FALSE(listsAdmin());
    CHECK(discovery->calls() == 1);

    // Once the dropped answer is in, the next opening looks again -- and still only one discovery at a time.
    CHECK(switcherWaitUntil([&] {
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

TEST_CASE("Switcher: the daemon drilled into stays it while a discovery reorders the daemons",
          "[contour][switcher][threading]")
{
    // [work, ci-box] while discovery runs; drilled into ci-box; then discovery puts home and lab before it.
    auto s = Switcher {};
    auto services = s.world.services();
    auto const discovery = std::make_shared<test::GatedDiscovery>(s.world.found);
    services.discovery = discovery;
    s.controller = std::make_unique<window::SwitcherController>(std::move(services));
    s.world.heldBack = { "ci-box" };
    s.controller->open(window::SwitcherOpening { .window = ThisWindow,
                                                 .windowDaemon = switcherLabelled("work"),
                                                 .mode = SwitchMode::Switch,
                                                 .prompt = window::SwitcherPrompt::Browse,
                                                 .pick = std::nullopt,
                                                 .scope = window::SwitcherScope::Everything,
                                                 .windowWorkspaces = workWorkspaces(),
                                                 .origin = window::SwitcherOrigin::Centred });
    REQUIRE(discovery->awaitCalls(1));
    s.select("ci-box", SwitcherRowKind::Daemon);
    CHECK(s.press(Qt::Key_Return));
    CHECK(s.controller->levelTitle() == "ci-box");

    discovery->releaseOne();
    REQUIRE(switcherWaitUntil([&] { return !s.controller->isDiscovering(); }));
    CHECK(s.controller->levelTitle() == "ci-box");
    CHECK(s.titles() == std::vector<std::string> { "Connecting to ci-box…", "New Workspace" });

    // Escape leaves ci-box, wherever it is listed now: its late answer is dropped.
    CHECK(s.press(Qt::Key_Escape));
    CHECK(s.controller->level() == static_cast<int>(window::SwitcherLevel::Top));
    s.world.answer("ci-box");
    QCoreApplication::processEvents();
    CHECK(
        s.role(s.rowOf("ci-box", SwitcherRowKind::Daemon), window::SwitcherController::Roles::DetailTextRole)
        == "Not connected");
}

TEST_CASE("Switcher: All daemons… shows the chord bound to ShowSwitcher", "[contour][switcher]")
{
    auto const bound = GENERATE(as<std::string> {}, "Ctrl+Shift+R", "");
    auto s = Switcher {};
    auto services = s.world.services();
    services.showSwitcherShortcut = [bound] {
        return std::string { bound };
    };
    s.controller = std::make_unique<window::SwitcherController>(std::move(services));
    s.open(window::SwitcherOrigin::Chip, window::SwitcherScope::OneDaemon);
    CHECK(s.role(s.rowOf("All daemons…", SwitcherRowKind::AllDaemons),
                 window::SwitcherController::Roles::DetailTextRole)
          == QString::fromStdString(bound));
}

TEST_CASE("Switcher: the level and discovery tell QML of their own changes", "[contour][switcher]")
{
    auto s = Switcher {};
    auto levels = QSignalSpy { s.controller.get(), &window::SwitcherController::levelChanged };
    auto discovering = QSignalSpy { s.controller.get(), &window::SwitcherController::discoveringChanged };
    s.open();
    CHECK(discovering.count() >= 2); // looking, then settled
    CHECK(s.controller->fieldName() == "Switch workspace or daemon");

    s.select("New Daemon…", SwitcherRowKind::NewDaemon);
    CHECK(s.press(Qt::Key_Return));
    CHECK(levels.count() == 1);
    CHECK(s.controller->isAskingLabel());
    CHECK(s.controller->fieldName() == "Label of the new daemon");

    CHECK(s.press(Qt::Key_Escape));
    CHECK(levels.count() == 2);
    s.select("home", SwitcherRowKind::Daemon);
    CHECK(s.press(Qt::Key_Return));
    CHECK(levels.count() == 3);
    CHECK(s.controller->fieldName() == "Switch to a workspace of home");
}

TEST_CASE("Switcher: a row's description says its git, its reason and its place", "[contour][switcher]")
{
    auto s = Switcher {};
    auto own = workWorkspaces();
    own[1].status.gitText = "main ↑2";
    own[1].status.reason = { .label = "exit 1", .separator = " · ", .command = "make deploy" };
    own[1].status.urgency = Urgency::Failed;
    s.open();
    s.controller->updateWindowWorkspaces(own);
    auto const description =
        s.role(s.rowOf("api-gateway"), window::SwitcherController::Roles::AccessibleDescriptionRole);
    CHECK(description.startsWith("workspace"));
    CHECK(description.contains("main ↑2"));
    CHECK(description.contains("exit 1 · make deploy"));
    CHECK(s.role(s.rowOf("contour"), window::SwitcherController::Roles::AccessibleDescriptionRole)
              .contains("current"));
}

TEST_CASE("Switcher: a start is dropped once its level is left or its label changed",
          "[contour][switcher][threading]")
{
    auto s = Switcher {};
    auto start = test::GatedStart {};
    auto services = s.world.services();
    services.startDaemon = [&start](std::string const&) {
        return start.job();
    };
    s.controller = std::make_unique<window::SwitcherController>(std::move(services));
    s.open(window::SwitcherOrigin::Centred,
           window::SwitcherScope::Everything,
           window::SwitcherPrompt::NewDaemon);
    s.controller->checkLabel("research");
    CHECK(s.press(Qt::Key_Return));
    REQUIRE(start.awaitStarted());
    CHECK(s.controller->isBusy());

    auto const how = GENERATE(as<std::string> {}, "left", "relabelled", "abandoned");
    if (how == "left")
    {
        CHECK(s.press(Qt::Key_Escape)); // the label first
        CHECK(s.press(Qt::Key_Escape)); // then the level
        CHECK(s.controller->level() == static_cast<int>(window::SwitcherLevel::Top));
    }
    else if (how == "relabelled")
        s.controller->checkLabel("research2");
    else
        s.controller->abandon();
    CHECK_FALSE(s.controller->isBusy());

    start.release();
    QTest::qWait(100);
    QCoreApplication::processEvents();
    CHECK(s.world.requested.empty());
}
