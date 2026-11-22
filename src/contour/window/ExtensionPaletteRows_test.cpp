// SPDX-License-Identifier: Apache-2.0
//
// A daemon extension's actions as a window's own command palette offers them: the presence the window's
// binding holds, through the window's palette sources and the palette tree, to the rows QML lists, and from
// a picked row back to the binding as an invocation.

#include <contour/command/Shortcut.hpp>
#include <contour/test/AppFixture.hpp>
#include <contour/test/CoreFixtures.hpp>
#include <contour/test/GuiTestFixtures.hpp>
#include <contour/window/CommandPaletteModel.hpp>
#include <contour/window/WindowController.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

#include <vthost/proto/Pdu.hpp>

using contour::session::SwitchTarget;
using contour::test::DaemonNamedBinding;
using contour::test::MockPtySessionFactory;
using contour::test::ScopedController;
using contour::test::TestApp;
using contour::window::CommandPaletteModel;
using vthost::proto::WirePresenceState;

namespace
{

/// @return The orchestrator as its daemon reports it in @p state, declaring open, refresh and show-blocked.
[[nodiscard]] vthost::proto::ExtensionPresence orchestratorIn(WirePresenceState state)
{
    return vthost::proto::ExtensionPresence {
        .extensions = { vthost::proto::WireExtensionPresence {
            .extension = "orchestrator",
            .title = "Agents",
            .state = state,
            .rows = { vthost::proto::WireActionRow { .id = "open", .title = "Open Agents" },
                      vthost::proto::WireActionRow { .id = "refresh", .title = "Refresh Agents" },
                      vthost::proto::WireActionRow { .id = "show-blocked", .title = "Show blocked runs" } },
        } },
    };
}

/// A window with one tab, bound to a daemon whose extensions are @c binding.presence.
struct PaletteRig
{
    MockPtySessionFactory* factory = nullptr;
    TestApp app;
    DaemonNamedBinding binding;
    ScopedController window;

    PaletteRig():
        app { [this] {
            auto made = std::make_unique<MockPtySessionFactory>();
            factory = made.get();
            return made;
        }() },
        binding { *factory,
                  SwitchTarget {
                      .kind = vthost::proto::SwitchDaemonKind::Name, .daemon = "default", .workspace = {} } },
        window { app.manager() }
    {
        // The user binds the orchestrator's `open` to a chord, as the gate's configuration does.
        app.app().config().inputMappings = contour::test::loadConfigFromYaml(R"(
input_mapping:
    - { mods: [Control, Shift], key: F12, action: ExtensionAction, extension: orchestrator, id: open }
)")
                                               .inputMappings;
        app.manager().bindWindow(window.id, binding);
        window->createNewTab();
    }

    /// @return What the palette shows as the chord bound to the command @p id; empty when unbound.
    [[nodiscard]] std::string shortcutOf(std::string const& id)
    {
        auto const& config = app.app().config();
        auto const index = contour::command::shortcutIndex(
            config, contour::command::shortcutStyleFor(config.keyboardPlatformKind));
        auto const found = index.find(id);
        return found != index.end() ? found->second : std::string {};
    }

    /// Opens the window's palette, as View → Command Palette… does, and types @p filter.
    [[nodiscard]] CommandPaletteModel& openPalette(QString const& filter) const
    {
        window->openCommandPalette();
        auto& palette = *window->commandPalette();
        palette.setFilter(filter);
        return palette;
    }
};

/// One row of the palette, as the QML delegate reads it.
struct ListedRow
{
    std::string id;
    std::string title;
    std::string shortcut;    ///< The chord column.
    std::string description; ///< What the footer says of the selected row.
    bool operator==(ListedRow const&) const = default;
};

/// @return Every row @p palette lists, top to bottom.
[[nodiscard]] std::vector<ListedRow> rowsOf(CommandPaletteModel const& palette)
{
    auto const role = [&](int row, CommandPaletteModel::Roles which) {
        return palette.data(palette.index(row, 0), static_cast<int>(which)).toString().toStdString();
    };
    auto rows = std::vector<ListedRow> {};
    for (auto const row: std::views::iota(0, palette.rowCount()))
        rows.push_back(ListedRow { .id = role(row, CommandPaletteModel::Roles::IdRole),
                                   .title = role(row, CommandPaletteModel::Roles::TitleRole),
                                   .shortcut = role(row, CommandPaletteModel::Roles::ShortcutRole),
                                   .description = role(row, CommandPaletteModel::Roles::DescriptionRole) });
    return rows;
}

/// @return The row of @p rows whose id is @p id, if listed.
[[nodiscard]] std::optional<ListedRow> rowWithId(std::vector<ListedRow> const& rows, std::string_view id)
{
    auto const found = std::ranges::find(rows, id, &ListedRow::id);
    return found != rows.end() ? std::optional { *found } : std::nullopt;
}

} // namespace

TEST_CASE("a running extension's actions are palette rows under their own titles",
          "[contour][palette][presence]")
{
    auto rig = PaletteRig {};
    rig.binding.presence = orchestratorIn(WirePresenceState::Running);

    auto const description = std::string("From the orchestrator daemon extension.");
    auto const openAgents = ListedRow { .id = "ExtensionAction:orchestrator:open",
                                        .title = "Open Agents",
                                        .shortcut = rig.shortcutOf("ExtensionAction:orchestrator:open"),
                                        .description = description };
    // The binding's chord is the row's, not a second row's.
    REQUIRE_FALSE(openAgents.shortcut.empty());

    SECTION("listed with no filter, among the daemon's rows, once")
    {
        auto const rows = rowsOf(rig.openPalette({}));
        CHECK(rowWithId(rows, "ExtensionAction:orchestrator:open") == openAgents);
        CHECK(std::ranges::count(rows, openAgents.id, &ListedRow::id) == 1);
        CHECK(rowWithId(rows, "ExtensionAction:orchestrator:refresh")
              == ListedRow { .id = "ExtensionAction:orchestrator:refresh",
                             .title = "Refresh Agents",
                             .shortcut = {},
                             .description = description });
        // No Extension Action drill-in: every extension action the window knows has a row of its own.
        CHECK(std::ranges::count(rows, std::string("ExtensionAction"), &ListedRow::id) == 0);
    }

    SECTION("found by the words of their titles")
    {
        auto const rows = rowsOf(rig.openPalette(QStringLiteral("Agents")));
        CHECK(rowWithId(rows, "ExtensionAction:orchestrator:open") == openAgents);
        CHECK(std::ranges::count(rows, openAgents.id, &ListedRow::id) == 1);
        CHECK(rowWithId(rows, "ExtensionAction:orchestrator:refresh").has_value());
        auto const blocked = rowsOf(rig.openPalette(QStringLiteral("blocked")));
        REQUIRE_FALSE(blocked.empty());
        CHECK(blocked.front().title == "Show blocked runs");
    }

    SECTION("picked, the row's action reaches the daemon behind the window")
    {
        auto const found = rowsOf(rig.openPalette(QStringLiteral("Open Agents")));
        REQUIRE_FALSE(found.empty());
        CHECK(found.front().title == "Open Agents");
        rig.window->runCommand(QString::fromStdString(found.front().id));
        CHECK(rig.binding.invocations
              == std::vector<std::pair<std::string, std::string>> { { "orchestrator", "open" } });
    }
}

TEST_CASE("an extension awaiting its grant or disabled is one palette row under its own title",
          "[contour][palette][presence]")
{
    struct Case
    {
        WirePresenceState state;
        char const* filter;
        std::string_view title;
    };
    for (auto const& row: { Case { .state = WirePresenceState::AwaitingGrant,
                                   .filter = "grant",
                                   .title = "Agents: grant access…" },
                            Case { .state = WirePresenceState::Disabled,
                                   .filter = "restart",
                                   .title = "Agents: restart extension" } })
    {
        INFO(row.title);
        auto rig = PaletteRig {};
        rig.binding.presence = orchestratorIn(row.state);
        auto const rows = rowsOf(rig.openPalette(QString::fromUtf8(row.filter)));
        CHECK(rowWithId(rows, "ExtensionAction:orchestrator:")
              == ListedRow { .id = "ExtensionAction:orchestrator:",
                             .title = std::string(row.title),
                             .shortcut = {},
                             .description = "From the orchestrator daemon extension." });
    }
}
