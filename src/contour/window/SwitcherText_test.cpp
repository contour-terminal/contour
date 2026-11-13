// SPDX-License-Identifier: Apache-2.0
//
// What a switcher row says, its footer, the field's hint and the New Daemon verdict: plain text from plain
// data, checked with no window and no Qt.

#include <contour/window/SwitcherText.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <format>
#include <string>
#include <vector>

#include <vthost/SocketPath.hpp>

using namespace contour::window;
using namespace std::chrono_literals;
using contour::command::ShortcutChord;
using contour::command::ShortcutStyle;
using contour::session::SwitchMode;
using contour::session::SwitchTarget;
using vthost::proto::SwitchDaemonKind;

namespace
{

constexpr auto Now = std::chrono::sys_days { std::chrono::year { 2026 } / std::chrono::November / 12 };

[[nodiscard]] SwitchTarget label(std::string name)
{
    return SwitchTarget { .kind = SwitchDaemonKind::Label, .daemon = std::move(name), .workspace = {} };
}

[[nodiscard]] SwitcherStatus statusOf(Urgency urgency, std::string git, ReasonParts reason = {})
{
    auto status = SwitcherStatus {};
    status.urgency = urgency;
    status.gitText = std::move(git);
    status.reason = std::move(reason);
    return status;
}

[[nodiscard]] SwitcherWorkspace workspace(uint32_t position,
                                          std::string name,
                                          std::size_t tabs,
                                          SwitcherStatus status)
{
    auto each = SwitcherWorkspace {};
    each.position = position;
    each.name = std::move(name);
    each.tabs = tabs;
    each.status = std::move(status);
    return each;
}

/// `work` (the window's) and `home`.
[[nodiscard]] std::vector<SwitcherDaemon> daemons()
{
    auto work = SwitcherDaemon {};
    work.address = label("work");
    work.listing = SwitcherListing::Listed;
    work.workspaces = { workspace(1, "contour", 3, statusOf(Urgency::Working, "main ↑2")),
                        workspace(2, "api-gateway", 1, statusOf(Urgency::Idle, {})) };
    auto home = SwitcherDaemon {};
    home.address = label("home");
    home.listing = SwitcherListing::Listed;
    auto failed = statusOf(
        Urgency::Failed, "main", { .label = "exit 1", .separator = " · ", .command = "make deploy" });
    // As another daemon's failed command reads: no age beside the reason (the sidebar's finished reason).
    home.workspaces = { workspace(1, "api-docs", 2, failed),
                        workspace(2, "a", 1, {}),
                        workspace(3, "b", 1, {}),
                        workspace(4, "c", 1, {}) };
    return { work, home };
}

[[nodiscard]] SwitcherRow const& rowTitled(std::vector<SwitcherRow> const& rows, std::string_view title)
{
    auto const found = std::ranges::find(rows, title, &SwitcherRow::title);
    REQUIRE(found != rows.end());
    return *found;
}

[[nodiscard]] std::string key(vtbackend::Key input, ShortcutStyle style, vtbackend::Modifiers modifiers = {})
{
    return contour::command::shortcutText(ShortcutChord { .modifiers = modifiers, .input = input }, style);
}

} // namespace

TEST_CASE("switcherDetail shows git and tabs, or the reason and time when the row asks for the user",
          "[switcher]")
{
    auto list = daemons();
    auto const inputs = SwitcherInputs { .daemons = list };
    auto const top = switcherRows(inputs, SwitcherView {});
    CHECK(switcherDetail(rowTitled(top, "contour"), inputs, Now)
          == SwitcherDetail { .text = "main ↑2 · 3 tabs", .separator = {}, .command = {}, .tail = {} });
    CHECK(switcherDetail(rowTitled(top, "api-gateway"), inputs, Now)
          == SwitcherDetail { .text = "1 tab", .separator = {}, .command = {}, .tail = {} });
    CHECK(switcherDetail(rowTitled(top, "home"), inputs, Now)
          == SwitcherDetail { .text = "4 workspaces", .separator = {}, .command = {}, .tail = {} });
    CHECK(switcherDetail(rowTitled(top, "New Workspace"), inputs, Now).text == "on work");

    auto const level = switcherRows(inputs, SwitcherView { .level = SwitcherLevel::Daemon, .daemon = 1 });
    CHECK(switcherDetail(rowTitled(level, "api-docs"), inputs, Now)
          == SwitcherDetail {
              .text = "main · exit 1", .separator = " · ", .command = "make deploy", .tail = {} });

    SECTION("a reason with a time ends in it")
    {
        auto blocked = statusOf(
            Urgency::NeedsYou, {}, { .label = "Permission · Bash: ", .separator = {}, .command = "rm -rf" });
        blocked.time.kind = TimeKind::Live;
        blocked.since = Now - 120s;
        list[1].workspaces[0].status = blocked;
        auto const asking =
            switcherRows(inputs, SwitcherView { .level = SwitcherLevel::Daemon, .daemon = 1 });
        CHECK(switcherDetail(rowTitled(asking, "api-docs"), inputs, Now)
              == SwitcherDetail { .text = "Permission · Bash: ",
                                  .separator = {},
                                  .command = "rm -rf",
                                  .tail = " · " + switcherTimeText(blocked, Now) });
        CHECK_FALSE(switcherTimeText(blocked, Now).empty());
    }

    SECTION("a daemon whose listing failed says why")
    {
        list[1].listing = SwitcherListing::Failed;
        list[1].failure = "connection refused";
        list[1].workspaces.clear();
        auto const failed = switcherRows(inputs, SwitcherView {});
        CHECK(switcherDetail(rowTitled(failed, "home"), inputs, Now).text == "connection refused");
    }

    SECTION("a daemon not listed yet says so")
    {
        list[1].listing = SwitcherListing::Listing;
        list[1].workspaces.clear();
        CHECK(switcherDetail(rowTitled(switcherRows(inputs, SwitcherView {}), "home"), inputs, Now).text
              == "Looking…");
    }

    SECTION("a saved host never reached says it is not connected")
    {
        auto host = SwitcherDaemon {};
        host.address = SwitchTarget { .kind = SwitchDaemonKind::Host, .daemon = "ci-box", .workspace = {} };
        list.push_back(host);
        auto const withHost = SwitcherInputs { .daemons = list };
        CHECK(switcherDetail(rowTitled(switcherRows(withHost, SwitcherView {}), "ci-box"), withHost, Now).text
              == "Not connected");

        // One a window of this application is on says so.
        list.back().link = SwitcherHostLink::Connected;
        auto const connected = SwitcherInputs { .daemons = list };
        CHECK(
            switcherDetail(rowTitled(switcherRows(connected, SwitcherView {}), "ci-box"), connected, Now).text
            == "Connected");
    }

    SECTION("one workspace is singular")
    {
        list[1].workspaces.resize(1);
        CHECK(switcherDetail(rowTitled(switcherRows(inputs, SwitcherView {}), "home"), inputs, Now).text
              == "1 workspace");
    }
}

TEST_CASE("SwitcherDetailRules show the reason from DoneUnseen up", "[switcher]")
{
    STATIC_REQUIRE(SwitcherDetailRules.size() == static_cast<std::size_t>(Urgency::NeedsYou) + 1);
    CHECK(switcherDetailRule(Urgency::Working).reason == ShowReason::No);
    CHECK(switcherDetailRule(Urgency::UnseenOutput).reason == ShowReason::No);
    CHECK(switcherDetailRule(Urgency::DoneUnseen).reason == ShowReason::Yes);
    CHECK(switcherDetailRule(Urgency::NeedsYou).reason == ShowReason::Yes);
}

TEST_CASE("switcherDescription names what Return does", "[switcher]")
{
    auto const list = daemons();
    auto const inputs = SwitcherInputs { .daemons = list };
    auto const top = switcherRows(inputs, SwitcherView {});
    auto const describe = [&](std::vector<SwitcherRow> const& rows,
                              std::string_view title,
                              SwitcherView const& view,
                              SwitchMode mode = SwitchMode::Switch) {
        return switcherDescription(rowTitled(rows, title), inputs, view, mode);
    };

    CHECK(describe(top, "api-gateway", {}) == "Switch this window to api-gateway");
    CHECK(describe(top, "api-gateway", {}, SwitchMode::Open) == "Open api-gateway in a new window");
    CHECK(describe(top, "home", {}) == "Show the workspaces on home");
    CHECK(describe(top, "New Workspace", {}) == "Create a workspace on work");
    CHECK(describe(top, "New Daemon…", {}) == "Starts a new daemon with one workspace");

    auto const filteredView = SwitcherView { .filter = "api" };
    auto const filtered = switcherRows(inputs, filteredView);
    CHECK(describe(filtered, "api-gateway", filteredView) == "Switch this window to api-gateway on work");
    CHECK(describe(filtered, "api-docs", filteredView) == "Move this window to home · api-docs");

    auto const levelView = SwitcherView { .level = SwitcherLevel::Daemon, .daemon = 1 };
    CHECK(describe(switcherRows(inputs, levelView), "api-docs", levelView)
          == "Move this window to home · api-docs");

    auto const chipView = SwitcherView { .origin = SwitcherOrigin::Chip, .scope = SwitcherScope::OneDaemon };
    auto const chip = switcherRows(inputs, chipView);
    CHECK(describe(chip, "contour", chipView) == "contour · main ↑2");
    CHECK(describe(chip, "api-gateway", chipView) == "api-gateway");
    CHECK(describe(chip, "All daemons…", chipView) == "Show every daemon");

    auto const newDaemonView = SwitcherView { .level = SwitcherLevel::NewDaemon, .filter = "research" };
    auto const newDaemon = switcherRows(inputs, newDaemonView);
    REQUIRE(newDaemon.size() == 1);
    CHECK(switcherDescription(newDaemon[0], inputs, newDaemonView, SwitchMode::Switch)
          == "Starts a new daemon with one workspace");

    // A label already running: Return switches to it, and the footer says so.
    auto const runningView = SwitcherView { .level = SwitcherLevel::NewDaemon, .filter = "home" };
    auto const running = switcherRows(inputs, runningView);
    REQUIRE(running.size() == 1);
    CHECK(switcherDescription(running[0], inputs, runningView, SwitchMode::Switch)
          == "Switch this window to daemon “home”");
}

TEST_CASE("switcherFooterHints spells the keys per style", "[switcher]")
{
    using vtbackend::Key;
    using vtbackend::Modifier;
    for (auto const style: { ShortcutStyle::MacGlyphs, ShortcutStyle::Text })
    {
        auto const enter = key(Key::Enter, style);
        auto const shiftEnter = key(Key::Enter, style, Modifier::Shift);
        auto const escape = key(Key::Escape, style);
        CHECK(switcherFooterHints(
                  SwitcherHintSubject::Workspace, SwitcherCommand::Close, style, SwitchMode::Switch)
              == std::format("{} Switch · {} New Window · {} Close", enter, shiftEnter, escape));
        CHECK(switcherFooterHints(
                  SwitcherHintSubject::NewWorkspace, SwitcherCommand::ClearFilter, style, SwitchMode::Switch)
              == std::format("{} Create · {} Create in New Window · {} Clear", enter, shiftEnter, escape));
        CHECK(
            switcherFooterHints(SwitcherHintSubject::None, SwitcherCommand::Close, style, SwitchMode::Switch)
            == std::format("{} Close", escape));
    }
    CHECK(switcherFooterHints(SwitcherHintSubject::Workspace,
                              SwitcherCommand::Close,
                              ShortcutStyle::MacGlyphs,
                              SwitchMode::Switch)
          == "↩ Switch · ⇧↩ New Window · ⎋ Close");

    auto const daemon = switcherFooterHints(
        SwitcherHintSubject::Daemon, SwitcherCommand::Back, ShortcutStyle::MacGlyphs, SwitchMode::Switch);
    // As the palette's level footer: Escape is Back, named once (Backspace in an empty field does the same).
    CHECK(daemon == "↩ Open · ⇧↩ New Window · → Open daemon · ⎋ Back");
    CHECK(switcherFooterHints(SwitcherHintSubject::NewDaemon,
                              SwitcherCommand::Close,
                              ShortcutStyle::MacGlyphs,
                              SwitchMode::Switch)
          == "↩ Open · → Open · ⎋ Close");
}

TEST_CASE("an Open pick's footer names what Return does, and no Shift+Return", "[switcher]")
{
    auto const style = ShortcutStyle::MacGlyphs;
    CHECK(switcherFooterHints(SwitcherHintSubject::Workspace, SwitcherCommand::Close, style, SwitchMode::Open)
          == "↩ New Window · ⎋ Close");
    CHECK(switcherFooterHints(
              SwitcherHintSubject::NewWorkspace, SwitcherCommand::Close, style, SwitchMode::Open)
          == "↩ Create in New Window · ⎋ Close");
    // Return on a daemon drills in whatever the mode.
    CHECK(switcherFooterHints(SwitcherHintSubject::Daemon, SwitcherCommand::Close, style, SwitchMode::Open)
          == switcherFooterHints(
              SwitcherHintSubject::Daemon, SwitcherCommand::Close, style, SwitchMode::Switch));
}

TEST_CASE("Escape on the New Daemon level clears the label first, then goes back", "[switcher]")
{
    auto context = SwitcherKeyContext { .filter = FilterText::Typed,
                                        .caret = CaretAt::End,
                                        .level = SwitcherLevel::NewDaemon,
                                        .origin = SwitcherOrigin::Centred,
                                        .scope = SwitcherScope::Everything,
                                        .selected = SwitcherRowKind::LabelAction };
    auto const style = ShortcutStyle::MacGlyphs;
    CHECK(switcherEscapeCommand(context) == SwitcherCommand::ClearFilter);
    CHECK(switcherFooterHints(
              SwitcherHintSubject::CreateDaemon, switcherEscapeCommand(context), style, SwitchMode::Switch)
          == "↩ Create · ⇧↩ Create in New Window · ⎋ Clear");
    context.filter = FilterText::Empty;
    context.selected = std::nullopt;
    CHECK(switcherEscapeCommand(context) == SwitcherCommand::Back);
    CHECK(switcherFooterHints(
              SwitcherHintSubject::None, switcherEscapeCommand(context), style, SwitchMode::Switch)
          == "⎋ Back");
}

TEST_CASE("a narrow footer gives Shift+Return up first, then →, and keeps Return and Escape", "[switcher]")
{
    auto const style = ShortcutStyle::MacGlyphs;
    CHECK(
        switcherFooterHintSets(SwitcherHintSubject::Daemon, SwitcherCommand::Back, style, SwitchMode::Switch)
        == std::vector<std::string> { "↩ Open · ⇧↩ New Window · → Open daemon · ⎋ Back",
                                      "↩ Open · → Open daemon · ⎋ Back",
                                      "↩ Open · ⎋ Back" });
    CHECK(switcherFooterHintSets(
              SwitcherHintSubject::Workspace, SwitcherCommand::Close, ShortcutStyle::Text, SwitchMode::Switch)
          == std::vector<std::string> { "Enter Switch · Shift+Enter New Window · Escape Close",
                                        "Enter Switch · Escape Close" });
    // Nothing to give up: one set.
    CHECK(switcherFooterHintSets(SwitcherHintSubject::None, SwitcherCommand::Close, style, SwitchMode::Switch)
          == std::vector<std::string> { "⎋ Close" });
    // The widest set is the footer's hints.
    for (auto const subject: { SwitcherHintSubject::Workspace, SwitcherHintSubject::NewDaemon })
        CHECK(switcherFooterHintSets(subject, SwitcherCommand::Close, style, SwitchMode::Switch).front()
              == switcherFooterHints(subject, SwitcherCommand::Close, style, SwitchMode::Switch));
    // Escape is never given up: every set ends in it.
    for (auto const& hints: switcherFooterHintSets(
             SwitcherHintSubject::NewWorkspace, SwitcherCommand::Close, style, SwitchMode::Switch))
        CHECK(hints.ends_with("⎋ Close"));
}

TEST_CASE("a row's Shift+Return hint is there exactly when Shift+Return opens a new window", "[switcher]")
{
    for (auto const& rule: SwitcherRowKindRules)
    {
        INFO("kind " << static_cast<int>(rule.kind));
        auto const context = SwitcherKeyContext { .filter = FilterText::Empty,
                                                  .caret = CaretAt::StartAndEnd,
                                                  .level = SwitcherLevel::Top,
                                                  .origin = SwitcherOrigin::Centred,
                                                  .scope = SwitcherScope::Everything,
                                                  .selected = rule.kind };
        auto const opens = switcherKeyCommand(SwitcherKey::Return, SwitcherKeyModifier::Shift, context)
                           == SwitcherCommand::ActivateInNewWindow;
        // The label action's hints are its verdict's: both verdicts that make a row offer Shift+Return.
        auto const subjects =
            rule.kind == SwitcherRowKind::LabelAction
                ? std::vector { SwitcherHintSubject::CreateDaemon, SwitcherHintSubject::SwitchToRunning }
                : std::vector { rule.hints };
        for (auto const subject: subjects)
            CHECK(SwitcherHintRows.at(static_cast<std::size_t>(subject)).shiftEnterVerb.empty() == !opens);
    }
}

TEST_CASE("switcherHintSubject follows the selected row", "[switcher]")
{
    auto row = SwitcherRow {};
    CHECK(switcherHintSubject(row) == SwitcherHintSubject::Workspace);
    row.kind = SwitcherRowKind::AllDaemons;
    CHECK(switcherHintSubject(row) == SwitcherHintSubject::AllDaemons);
    row.kind = SwitcherRowKind::Discovering;
    CHECK(switcherHintSubject(row) == SwitcherHintSubject::None);
}

TEST_CASE("the field's hint, the empty text and the level title follow the view", "[switcher]")
{
    CHECK(switcherHintText(SwitcherView {}, "work") == "Switch workspace or daemon…");
    CHECK(switcherHintText(SwitcherView { .origin = SwitcherOrigin::Chip }, "work")
          == "Switch workspace or daemon…");
    CHECK(switcherHintText(SwitcherView { .level = SwitcherLevel::Daemon, .daemon = 1 }, "home")
          == "Filter workspaces on home…");
    CHECK(switcherHintText(SwitcherView { .origin = SwitcherOrigin::Chip, .scope = SwitcherScope::OneDaemon },
                           "work")
          == "Filter workspaces on work…");
    CHECK(switcherHintText(SwitcherView { .level = SwitcherLevel::NewDaemon }, "work")
          == "Label, such as work2");

    CHECK(switcherEmptyText("zzz") == "No workspace matches “zzz”");

    CHECK(switcherLevelTitle(SwitcherView {}, "work").empty());
    CHECK(switcherLevelTitle(SwitcherView { .level = SwitcherLevel::Daemon, .daemon = 1 }, "home") == "home");
    CHECK(switcherLevelTitle(SwitcherView { .level = SwitcherLevel::NewDaemon }, "work") == "New Daemon");
}

TEST_CASE("switcherLabelState judges the label as it is typed", "[switcher]")
{
    auto const running = std::vector<std::string> { "work" };
    auto const style = ShortcutStyle::MacGlyphs;

    CHECK(switcherLabelState("", running, style)
          == SwitcherLabelState { .verdict = LabelVerdict::Empty,
                                  .message = {},
                                  .actionTitle = {},
                                  .equivalentCommand = {},
                                  .subject = SwitcherHintSubject::None });

    CHECK(switcherLabelState("research", running, style)
          == SwitcherLabelState { .verdict = LabelVerdict::Available,
                                  .message = "Available",
                                  .actionTitle = "Start daemon “research” and switch this window to it",
                                  .equivalentCommand = "contour daemon --label research",
                                  .subject = SwitcherHintSubject::CreateDaemon });

    CHECK(switcherLabelState("work", running, style)
          == SwitcherLabelState { .verdict = LabelVerdict::Running,
                                  .message =
                                      "A daemon named “work” is already running — ↩ switches to it instead",
                                  .actionTitle = "Switch this window to daemon “work”",
                                  .equivalentCommand = {},
                                  .subject = SwitcherHintSubject::SwitchToRunning });
    CHECK(switcherLabelState("work", running, ShortcutStyle::Text).message
          == std::format("A daemon named “work” is already running — {} switches to it instead",
                         key(vtbackend::Key::Enter, ShortcutStyle::Text)));

    CHECK(switcherLabelState("a b", running, style)
          == SwitcherLabelState { .verdict = LabelVerdict::Invalid,
                                  .message = vthost::describe(vthost::SocketLabelError::InvalidCharacter),
                                  .actionTitle = {},
                                  .equivalentCommand = {},
                                  .subject = SwitcherHintSubject::None });
}

TEST_CASE("All daemons… shows the chord bound to ShowSwitcher, and nothing while none is", "[switcher]")
{
    auto list = daemons();
    auto const chip = SwitcherView { .origin = SwitcherOrigin::Chip, .scope = SwitcherScope::OneDaemon };

    auto const bound = SwitcherInputs { .daemons = list, .showSwitcherShortcut = "Ctrl+Shift+R" };
    CHECK(switcherDetail(rowTitled(switcherRows(bound, chip), "All daemons…"), bound, Now)
          == SwitcherDetail { .text = "Ctrl+Shift+R", .separator = {}, .command = {}, .tail = {} });

    auto const unbound = SwitcherInputs { .daemons = list };
    CHECK(switcherDetail(rowTitled(switcherRows(unbound, chip), "All daemons…"), unbound, Now)
          == SwitcherDetail {});
}
