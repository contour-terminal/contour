// SPDX-License-Identifier: Apache-2.0
#include <contour/window/LocationChip.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>

using namespace contour::window;
using contour::session::SwitchTarget;
using vthost::proto::SwitchDaemonKind;

TEST_CASE("locationChip.daemonName", "[LocationChip]")
{
    auto const target = [](SwitchDaemonKind kind, std::string daemon) {
        return SwitchTarget { .kind = kind, .daemon = std::move(daemon) };
    };
    CHECK(locationDaemonName(target(SwitchDaemonKind::Label, "default"), "") == "local");
    CHECK(locationDaemonName(target(SwitchDaemonKind::Label, "work"), "") == "work");
    CHECK(locationDaemonName(target(SwitchDaemonKind::Label, "local"), "") == "local");
    CHECK(locationDaemonName(target(SwitchDaemonKind::Same, ""), "") == "daemon");
    CHECK(locationDaemonName(target(SwitchDaemonKind::Host, "starfive"), "starfive") == "starfive");
    CHECK(locationDaemonName(target(SwitchDaemonKind::Host, "sf"), "me@starfive") == "me@starfive");
}

TEST_CASE("locationChip.daemonDot", "[LocationChip]")
{
    CHECK(daemonDotOf(DaemonLocality::Remote, HostChipState::Connected) == DaemonDot::Shown);
    CHECK(daemonDotOf(DaemonLocality::Local, HostChipState::Connected) == DaemonDot::Hidden);
    CHECK(daemonDotOf(DaemonLocality::Local, HostChipState::Reconnecting) == DaemonDot::Shown);
    CHECK(daemonDotOf(DaemonLocality::Local, HostChipState::Disconnected) == DaemonDot::Shown);
}

TEST_CASE("locationChip.steps", "[LocationChip]")
{
    // The table is every fit in enumerator order, with distinct keys: it is the one place the elision order
    // and the width keys live.
    STATIC_REQUIRE(LocationChipSteps.size() == LocationChipFitCount);
    for (auto const index: std::views::iota(size_t { 0 }, LocationChipSteps.size()))
        CHECK(std::to_underlying(LocationChipSteps[index].fit) == index);
    CHECK(std::ranges::adjacent_find(LocationChipSteps, {}, &LocationChipStep::key)
          == LocationChipSteps.end());
    // Each row's key reads its own width: widthOf names the step, and only that step fits.
    for (auto const& step: LocationChipSteps)
        CHECK(locationChipFit([&](std::string_view key) { return key == step.key ? 10.0 : 1000.0; }, 10)
              == step.fit);
}

TEST_CASE("locationChip.fit", "[LocationChip]")
{
    auto const widthOf = [](std::string_view key) {
        constexpr auto Widths = std::array { std::pair { std::string_view { "full" }, 300.0 },
                                             std::pair { std::string_view { "noStateText" }, 220.0 },
                                             std::pair { std::string_view { "daemonElided" }, 160.0 } };
        auto const found = std::ranges::find(Widths, key, &std::pair<std::string_view, double>::first);
        return found != Widths.end() ? found->second : 110.0;
    };
    CHECK(locationChipFit(widthOf, 400) == LocationChipFit::Full);
    CHECK(locationChipFit(widthOf, 300) == LocationChipFit::Full);
    CHECK(locationChipFit(widthOf, 250) == LocationChipFit::NoStateText);
    CHECK(locationChipFit(widthOf, 200) == LocationChipFit::DaemonElided);
    CHECK(locationChipFit(widthOf, 120) == LocationChipFit::DotOnly);
    CHECK(locationChipFit(widthOf, 90) == LocationChipFit::WorkspaceElided);
    CHECK(locationChipFit(widthOf, 10) == LocationChipFit::WorkspaceElided);
}

TEST_CASE("locationChip.daemonOnlyFitKeepsTheName", "[LocationChip]")
{
    auto const widthOf = [](std::string_view key) {
        constexpr auto Widths = std::array { std::pair { std::string_view { "full" }, 300.0 },
                                             std::pair { std::string_view { "noStateText" }, 220.0 },
                                             std::pair { std::string_view { "daemonElided" }, 160.0 } };
        auto const found = std::ranges::find(Widths, key, &std::pair<std::string_view, double>::first);
        return found != Widths.end() ? found->second : 20.0;
    };
    constexpr auto DaemonOnly = ChipSegments::DaemonOnly;
    // The same room, two answers: the chip with a workspace may drop to the dot, the daemon-only one never
    // shows a bare caret, so it stays at its elided name however narrow the row.
    CHECK(locationChipFit(widthOf, 100) == LocationChipFit::DotOnly);
    CHECK(locationChipFit(widthOf, 100, DaemonOnly) == LocationChipFit::DaemonElided);
    CHECK(locationChipFit(widthOf, 0, DaemonOnly) == LocationChipFit::DaemonElided);
    CHECK(locationChipFit(widthOf, 400, DaemonOnly) == LocationChipFit::Full);
    CHECK(locationChipFit(widthOf, 250, DaemonOnly) == LocationChipFit::NoStateText);
    CHECK(locationChipLeastFit() == LocationChipFit::WorkspaceElided);
    CHECK(locationChipLeastFit(DaemonOnly) == LocationChipFit::DaemonElided);
    // What the least step says still names the daemon.
    CHECK_FALSE(locationChipDaemonText("starfive-build-01", locationChipLeastFit(DaemonOnly)).empty());
}

TEST_CASE("locationChip.stepTexts", "[LocationChip]")
{
    auto const texts =
        locationChipStepTexts(HostChipState::Reconnecting, "work-build-server", "billing-and-invoices");
    REQUIRE(texts.size() == LocationChipSteps.size());
    for (auto const index: std::views::iota(size_t { 0 }, texts.size()))
    {
        CHECK(texts[index].fit == LocationChipSteps[index].fit);
        CHECK(texts[index].key == LocationChipSteps[index].key);
        // Each row says what the per-fit functions say.
        CHECK(texts[index].daemon == locationChipDaemonText("work-build-server", texts[index].fit));
        CHECK(texts[index].status == locationChipStatusText(HostChipState::Reconnecting, texts[index].fit));
        CHECK(texts[index].workspace == locationChipWorkspaceText("billing-and-invoices", texts[index].fit));
        CHECK(texts[index].tooltip
              == locationChipToolTip(HostChipState::Reconnecting, texts[index].fit, "work-build-server"));
    }
    CHECK(texts[std::to_underlying(LocationChipFit::DaemonElided)].daemon == "work-…rver");
    CHECK(texts[std::to_underlying(LocationChipFit::WorkspaceElided)].workspace == "billin…");
}

TEST_CASE("locationChip.accessibleName", "[LocationChip]")
{
    // The name is what the chip shows; status goes in the description (ui-ux rule 8.2).
    CHECK(locationChipAccessibleName("starfive", "billing") == "Workspace billing on starfive");
    CHECK(locationChipAccessibleName("", "billing") == "Workspace billing");
    CHECK(locationChipDaemonAccessibleName("local") == "Daemon local");
}

TEST_CASE("locationChip.accessibleDescription", "[LocationChip]")
{
    constexpr auto Idle = Urgency::Idle;
    constexpr auto Collapsed = Disclosure::Collapsed;
    CHECK(locationChipAccessibleDescription(HostChipState::Connected, Idle, Collapsed) == "collapsed");
    CHECK(locationChipAccessibleDescription(HostChipState::Connected, Idle, Disclosure::Expanded)
          == "expanded");
    CHECK(locationChipAccessibleDescription(HostChipState::Reconnecting, Idle, Collapsed)
          == "Reconnecting, collapsed");
    CHECK(locationChipAccessibleDescription(HostChipState::NeedsAttention, Idle, Collapsed)
          == "Reconnecting, needs your attention, collapsed");
    CHECK(locationChipAccessibleDescription(HostChipState::Disconnected, Idle, Collapsed)
          == "Disconnected, collapsed");
    // What another workspace asks is said while the marker shows, between the state and the disclosure.
    CHECK(locationChipAccessibleDescription(HostChipState::Connected, Urgency::NeedsYou, Collapsed)
          == "Another workspace needs you, collapsed");
    CHECK(locationChipAccessibleDescription(HostChipState::Reconnecting, Urgency::Failed, Collapsed)
          == "Reconnecting, another workspace has failed, collapsed");
    CHECK(locationChipAccessibleDescription(HostChipState::Connected, Urgency::DoneUnseen, Collapsed)
          == "Another workspace is done, collapsed");
    // What does not mark the chip is not said either.
    CHECK(locationChipAccessibleDescription(HostChipState::Connected, Urgency::Working, Collapsed)
          == "collapsed");
    CHECK(locationChipAccessibleDescription(HostChipState::Connected, Urgency::UnseenOutput, Collapsed)
          == "collapsed");
}

TEST_CASE("locationChip.translation", "[LocationChip]")
{
    // Every sentence goes through the translator, whole, with its arguments filled in afterwards.
    struct Shouted
    {
        [[nodiscard]] std::string operator()(std::string_view sentence) const
        {
            auto text = std::string(sentence);
            std::ranges::transform(text, text.begin(), [](char c) { return c == 'a' ? 'A' : c; });
            return text;
        }

        // A sentence several controls share: the context says where it is translated, the test does not care.
        [[nodiscard]] std::string operator()(std::string_view /*context*/, std::string_view sentence) const
        {
            return (*this)(sentence);
        }
    };
    auto const shouted = Shouted {};
    CHECK(locationChipAccessibleName("starfive", "billing", shouted) == "WorkspAce billing on starfive");
    CHECK(locationChipToolTip(HostChipState::Reconnecting, LocationChipFit::DotOnly, "starfive", shouted)
          == "Reconnecting to starfive · Switch workspAce or dAemon");
    CHECK(locationChipAccessibleDescription(
              HostChipState::Connected, Urgency::NeedsYou, Disclosure::Collapsed, shouted)
          == "Another workspAce needs you, collApsed");
    CHECK(locationChipStatusText(HostChipState::Disconnected, LocationChipFit::Full, shouted)
          == "Disconnected");
    // A marker-like text in an argument is left alone.
    CHECK(locationChipAccessibleName("a%2b", "w%2", SourceText {}) == "Workspace w%2 on a%2b");
}

TEST_CASE("locationChip.stateGlyph", "[LocationChip]")
{
    CHECK(daemonStateGlyph(HostChipState::Connected) == "●");
    CHECK(daemonStateGlyph(HostChipState::Reconnecting) == "◑");
    CHECK(daemonStateGlyph(HostChipState::Disconnected) == "✕");
    CHECK(daemonStateGlyph(HostChipState::NeedsAttention) == "!");
}

TEST_CASE("locationChip.toolTip", "[LocationChip]")
{
    CHECK(locationChipToolTip(HostChipState::Reconnecting, LocationChipFit::NoStateText, "starfive")
          == "Reconnecting to starfive · Switch workspace or daemon");
    CHECK(locationChipToolTip(HostChipState::NeedsAttention, LocationChipFit::DotOnly, "starfive")
          == "Reconnecting to starfive needs your attention · Switch workspace or daemon");
    CHECK(locationChipToolTip(HostChipState::Disconnected, LocationChipFit::DaemonElided, "starfive")
          == "Disconnected from starfive · Switch workspace or daemon");
    CHECK(locationChipToolTip(HostChipState::Reconnecting, LocationChipFit::Full, "starfive")
          == "Switch workspace or daemon");
    CHECK(locationChipToolTip(HostChipState::Connected, LocationChipFit::DotOnly, "local")
          == "Switch workspace or daemon");
    CHECK(locationChipToolTip(HostChipState::Reconnecting, LocationChipFit::DotOnly, "")
          == "Switch workspace");
}

TEST_CASE("locationChip.place", "[LocationChip]")
{
    using P = LocationChipPlace;
    struct Row
    {
        BarPresence band;
        BarCentre centre;
        DaemonHosting hosting;
        SidebarShown sidebar;
        LocationChipPlace expected;
    };
    constexpr auto Daemon = DaemonHosting::Daemon;
    constexpr auto None = DaemonHosting::InProcess;
    constexpr auto OtherHost = DaemonHosting::OtherHost;
    constexpr auto Off = SidebarShown::No;
    constexpr auto On = SidebarShown::Yes;
    constexpr auto Shown = BarPresence::Shown;
    constexpr auto Hidden = BarPresence::Hidden;
    auto const rows = std::array {
        // A daemon's chip follows the band's centre.
        Row { Shown, BarCentre::ChipAndTabs, Daemon, Off, P::Band },
        Row { Shown, BarCentre::Tabs, Daemon, Off, P::Band },
        Row { Shown, BarCentre::Title, Daemon, Off, P::Band },
        Row { Shown, BarCentre::Empty, Daemon, Off, P::Band },
        Row { BarPresence::Pinned, BarCentre::Tabs, Daemon, Off, P::Band },
        Row { BarPresence::RevealOnEdge, BarCentre::Title, Daemon, Off, P::Band },
        Row { Shown, BarCentre::Breadcrumb, Daemon, On, P::Sidebar },
        // The sidebar's head holds the daemon only while the sidebar shows; hidden, the band does.
        Row { Shown, BarCentre::Breadcrumb, Daemon, Off, P::Band },
        // With no band the sidebar names the daemon, or nothing does.
        Row { Hidden, BarCentre::Tabs, Daemon, On, P::Sidebar },
        Row { Hidden, BarCentre::Tabs, Daemon, Off, P::None },
        // Without a daemon (in-process) only the workspace-chip centre shows.
        Row { Shown, BarCentre::ChipAndTabs, None, Off, P::Band },
        Row { Shown, BarCentre::Tabs, None, Off, P::None },
        Row { Shown, BarCentre::Title, None, Off, P::None },
        Row { Shown, BarCentre::Empty, None, Off, P::None },
        Row { Shown, BarCentre::Breadcrumb, None, On, P::None },
        Row { Hidden, BarCentre::Tabs, None, On, P::None },
        Row { Hidden, BarCentre::ChipAndTabs, None, Off, P::None },
        // A host that is no daemon (a tmux attach) names its host, and so follows the daemon's rules.
        Row { Shown, BarCentre::ChipAndTabs, OtherHost, Off, P::Band },
        Row { Shown, BarCentre::Tabs, OtherHost, Off, P::Band },
        Row { Shown, BarCentre::Title, OtherHost, Off, P::Band },
        Row { Shown, BarCentre::Breadcrumb, OtherHost, On, P::Sidebar },
        Row { Shown, BarCentre::Breadcrumb, OtherHost, Off, P::Band },
        Row { Hidden, BarCentre::Tabs, OtherHost, On, P::Sidebar },
        Row { Hidden, BarCentre::Tabs, OtherHost, Off, P::None },
    };
    for (auto const& row: rows)
        CHECK(locationChipPlace(row.band, row.centre, row.hosting, row.sidebar) == row.expected);
}

TEST_CASE("locationChip.placeName", "[LocationChip]")
{
    CHECK(locationChipPlaceName(LocationChipPlace::None) == "none");
    CHECK(locationChipPlaceName(LocationChipPlace::Band) == "band");
    CHECK(locationChipPlaceName(LocationChipPlace::Sidebar) == "sidebar");
}

TEST_CASE("locationChip.daemonText", "[LocationChip]")
{
    constexpr auto Long = std::string_view { "starfive-build-01" };
    CHECK(locationChipDaemonText(Long, LocationChipFit::Full) == Long);
    CHECK(locationChipDaemonText(Long, LocationChipFit::NoStateText) == Long);
    CHECK(locationChipDaemonText(Long, LocationChipFit::DaemonElided) == "starf…d-01");
    CHECK(locationChipDaemonText("local", LocationChipFit::DaemonElided) == "local");
    CHECK(locationChipDaemonText("0123456789", LocationChipFit::DaemonElided) == "0123456789");
    CHECK(locationChipDaemonText(Long, LocationChipFit::DotOnly).empty());
    CHECK(locationChipDaemonText(Long, LocationChipFit::WorkspaceElided).empty());
    // Characters, not bytes: eleven two-byte characters elide to ten characters.
    CHECK(locationChipDaemonText("ééééééééééé", LocationChipFit::DaemonElided) == "ééééé…éééé");
    // Cells, not characters: ten CJK characters take twenty cells, and a wide character is never halved, so
    // the head and the tail each keep as many whole characters as their cells hold (four cells each, with the
    // ellipsis's one making the ten).
    CHECK(locationChipDaemonText("日本語日本語日本語日", LocationChipFit::DaemonElided) == "日本…語日");
    // A grapheme cluster is never split: combining marks stay with their base, a ZWJ sequence stays whole.
    auto const combining =
        std::string("e\u0301e\u0301e\u0301e\u0301e\u0301e\u0301e\u0301e\u0301e\u0301e\u0301e\u0301");
    CHECK(locationChipDaemonText(combining, LocationChipFit::DaemonElided)
          == "e\u0301e\u0301e\u0301e\u0301e\u0301…e\u0301e\u0301e\u0301e\u0301");
    auto const family = std::string("\U0001F468\u200D\U0001F469\u200D\U0001F467");
    auto const elided = locationChipDaemonText(family + family + family + family + family + family,
                                               LocationChipFit::DaemonElided);
    CHECK(elided == family + family + "…" + family + family);
}

TEST_CASE("locationChip.statusText", "[LocationChip]")
{
    CHECK(locationChipStatusText(HostChipState::Reconnecting, LocationChipFit::Full) == "Reconnecting…");
    CHECK(locationChipStatusText(HostChipState::Disconnected, LocationChipFit::Full) == "Disconnected");
    CHECK(locationChipStatusText(HostChipState::Connected, LocationChipFit::Full).empty());
    for (auto const fit: { LocationChipFit::NoStateText,
                           LocationChipFit::DaemonElided,
                           LocationChipFit::DotOnly,
                           LocationChipFit::WorkspaceElided })
        CHECK(locationChipStatusText(HostChipState::Reconnecting, fit).empty());
}

TEST_CASE("locationChip.workspaceText", "[LocationChip]")
{
    for (auto const fit: { LocationChipFit::Full,
                           LocationChipFit::NoStateText,
                           LocationChipFit::DaemonElided,
                           LocationChipFit::DotOnly })
        CHECK(locationChipWorkspaceText("billing-and-invoices", fit) == "billing-and-invoices");
    CHECK(locationChipWorkspaceText("billing-and-invoices", LocationChipFit::WorkspaceElided) == "billin…");
    // A name at or under the floor is never touched, and the floor is never undercut.
    CHECK(locationChipWorkspaceText("billin", LocationChipFit::WorkspaceElided) == "billin");
    CHECK(locationChipWorkspaceText("api", LocationChipFit::WorkspaceElided) == "api");
    CHECK(locationChipWorkspaceText("ééééééé", LocationChipFit::WorkspaceElided) == "éééééé…");
    // The floor is six cells: six CJK characters take twelve, so the least kept is three of them.
    CHECK(locationChipWorkspaceText("日本語日本語", LocationChipFit::WorkspaceElided) == "日本語…");
    CHECK(locationChipWorkspaceText("日本語", LocationChipFit::WorkspaceElided) == "日本語");
    // Combining marks and a ZWJ sequence are never split, whatever the floor lands on.
    auto const combining = std::string("e\u0301e\u0301e\u0301e\u0301e\u0301e\u0301e\u0301e\u0301");
    CHECK(locationChipWorkspaceText(combining, LocationChipFit::WorkspaceElided)
          == "e\u0301e\u0301e\u0301e\u0301e\u0301e\u0301…");
    auto const family = std::string("\U0001F468\u200D\U0001F469\u200D\U0001F467");
    CHECK(locationChipWorkspaceText(family + family + family + family, LocationChipFit::WorkspaceElided)
          == family + family + family + "…");
}

TEST_CASE("daemonHosting.policy", "[LocationChip]")
{
    // Every hosting has a row, in enumerator order, and the chip and the switcher read the same one.
    CHECK(DaemonHostingPolicies.size() == DaemonHostingCount);
    for (auto const& row: DaemonHostingPolicies)
        CHECK(&daemonHostingPolicy(row.hosting) == &row);
    auto const& daemon = daemonHostingPolicy(DaemonHosting::Daemon);
    CHECK(daemon.naming == ChipNaming::NamesDaemon);
    CHECK(daemon.reach == SwitcherReach::AsAsked);
    auto const& inProcess = daemonHostingPolicy(DaemonHosting::InProcess);
    CHECK(inProcess.naming == ChipNaming::WorkspaceOnly);
    CHECK(inProcess.reach == SwitcherReach::OwnWorkspacesOnly);
    CHECK(daemon.segments == ChipSegments::DaemonAndWorkspace);
    // A host that is no daemon is named by its host label alone, and keeps the scope asked, as it always has.
    auto const& other = daemonHostingPolicy(DaemonHosting::OtherHost);
    CHECK(other.naming == ChipNaming::NamesDaemon);
    CHECK(other.segments == ChipSegments::DaemonOnly);
    CHECK(other.reach == SwitcherReach::AsAsked);
}

TEST_CASE("locationChip.otherWorkspaceClauseIsTheTranslatorsToCase", "[LocationChip]")
{
    // A clause that follows another part of the description is its own translatable sentence: whether it
    // starts in upper or lower case is the translation's call (a German main clause starts in upper case),
    // never something the code changes after the fact. The stand-in translation answers in capitals, so a
    // case the code imposed would show.
    struct Capitals
    {
        [[nodiscard]] std::string operator()(std::string_view sentence) const
        {
            if (sentence == "Another workspace has failed" || sentence == "another workspace has failed")
                return "ANOTHER WORKSPACE HAS FAILED";
            if (sentence == "Reconnecting")
                return "RECONNECTING";
            return std::string(sentence);
        }

        [[nodiscard]] std::string operator()(std::string_view /*context*/, std::string_view sentence) const
        {
            return (*this)(sentence);
        }
    };
    CHECK(locationChipAccessibleDescription(
              HostChipState::Reconnecting, Urgency::Failed, Disclosure::Collapsed, Capitals {})
          == "RECONNECTING, ANOTHER WORKSPACE HAS FAILED, collapsed");
    // Led by nothing, the clause starts the description, and says its leading form.
    CHECK(locationChipAccessibleDescription(
              HostChipState::Connected, Urgency::Failed, Disclosure::Collapsed, Capitals {})
          == "ANOTHER WORKSPACE HAS FAILED, collapsed");
    // In the source language the mid-list form is lower case, as it always read.
    CHECK(locationChipAccessibleDescription(
              HostChipState::Disconnected, Urgency::NeedsYou, Disclosure::Collapsed)
          == "Disconnected, another workspace needs you, collapsed");
}
