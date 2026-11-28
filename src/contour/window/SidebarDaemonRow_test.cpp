// SPDX-License-Identifier: Apache-2.0
#include <contour/window/SidebarControls.hpp>
#include <contour/window/SidebarDaemonRow.hpp>
#include <contour/window/TabBadge.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

using namespace contour::window;

namespace
{

/// @return Every link state, in enumerator order.
[[nodiscard]] auto allLinkStates()
{
    return std::views::iota(std::size_t { 0 }, DaemonLinkStateCount)
           | std::views::transform([](std::size_t i) { return static_cast<DaemonLinkState>(i); });
}

[[nodiscard]] DaemonRef work()
{
    return localDaemonRef("work");
}

[[nodiscard]] DaemonRef starfive()
{
    return hostDaemonRef("starfive");
}

} // namespace

TEST_CASE("the daemon row's activation follows the table, state by state", "[sidebar][daemon-row]")
{
    // Spec §4: what a click or Return on a daemon row does, with and without a tree last known of it, for a
    // daemon of this machine and for a host's.
    struct Expected
    {
        DaemonLinkState state;
        DaemonRowAction localWithTree;
        DaemonRowAction localWithoutTree;
        DaemonRowAction remoteWithTree;
        DaemonRowAction remoteWithoutTree;
    };
    using enum DaemonRowAction;
    constexpr auto Table = std::array {
        Expected { DaemonLinkState::NotConnected, Connect, Connect, Connect, Connect },
        Expected { DaemonLinkState::Connecting, Nothing, Nothing, Nothing, Nothing },
        Expected { DaemonLinkState::Connected, ToggleFold, ToggleFold, ToggleFold, ToggleFold },
        Expected { DaemonLinkState::Reconnecting, ToggleFold, ToggleFold, ToggleFold, ToggleFold },
        Expected { DaemonLinkState::NeedsInput, Connect, Connect, Connect, Connect },
        // Nothing here starts a host's daemon: its row folds what it last knew, and does nothing without.
        Expected { DaemonLinkState::NotRunning, Start, Start, ToggleFold, Nothing },
        Expected { DaemonLinkState::TooOld, ToggleFold, Nothing, ToggleFold, Nothing },
        Expected { DaemonLinkState::Failed, ToggleFold, Nothing, ToggleFold, Nothing },
    };
    static_assert(Table.size() == DaemonLinkStateCount);
    for (auto const state: allLinkStates())
    {
        auto const& row = Table[static_cast<std::size_t>(state)];
        INFO(std::format("state {}", static_cast<int>(state)));
        REQUIRE(row.state == state);
        auto const local = DaemonRowFacts { .state = state, .locality = DaemonLocality::Local };
        auto const remote = DaemonRowFacts { .state = state, .locality = DaemonLocality::Remote };
        CHECK(daemonRowActivation(local, LastKnownTree::Present) == row.localWithTree);
        CHECK(daemonRowActivation(local, LastKnownTree::Absent) == row.localWithoutTree);
        CHECK(daemonRowActivation(remote, LastKnownTree::Present) == row.remoteWithTree);
        CHECK(daemonRowActivation(remote, LastKnownTree::Absent) == row.remoteWithoutTree);
    }
}

TEST_CASE("a daemon row offers the buttons its state names", "[sidebar][daemon-row]")
{
    using enum DaemonRowButton;
    using Buttons = std::array<DaemonRowButton, 2>;
    auto const local = [](DaemonLinkState state) {
        return DaemonRowFacts { .state = state, .locality = DaemonLocality::Local };
    };
    CHECK(daemonRowButtons(local(DaemonLinkState::NotRunning), RowPending::No) == Buttons { Start, Remove });
    CHECK(daemonRowButtons(local(DaemonLinkState::Connecting), RowPending::No) == Buttons { Cancel, None });
    CHECK(daemonRowButtons(local(DaemonLinkState::NotConnected), RowPending::No)
          == Buttons { Connect, None });
    CHECK(daemonRowButtons(local(DaemonLinkState::NeedsInput), RowPending::No) == Buttons { Connect, None });
    CHECK(daemonRowButtons(local(DaemonLinkState::Connected), RowPending::No) == Buttons { None, None });

    // A pending jump into the section offers Cancel alone, where Connect was, whatever the link says.
    for (auto const state: allLinkStates())
    {
        INFO(std::format("state {}", static_cast<int>(state)));
        CHECK(daemonRowButtons(local(state), RowPending::Yes) == Buttons { Cancel, None });
    }
}

TEST_CASE("a host that is not running offers Connect, neither Start nor Remove", "[sidebar][daemon-row]")
{
    // A remote relay's "daemon absent" reads Not running, but nothing here starts a host's daemon, and Remove
    // is for this machine's daemons alone: a saved host is hidden, not unlisted. Connect dials it without
    // starting it, and finds it once it runs.
    using enum DaemonRowButton;
    using Buttons = std::array<DaemonRowButton, 2>;
    auto const host =
        DaemonRowFacts { .state = DaemonLinkState::NotRunning, .locality = DaemonLocality::Remote };
    CHECK(daemonRowButtons(host, RowPending::No) == Buttons { Connect, None });
    CHECK(daemonRowActivation(host, LastKnownTree::Absent) == DaemonRowAction::Nothing);
    // A host too old offers Connect too, which finds it once it was upgraded; this machine's is dialled
    // again by the periodic refresh, and offers nothing.
    auto const tooOldHost =
        DaemonRowFacts { .state = DaemonLinkState::TooOld, .locality = DaemonLocality::Remote };
    auto const tooOldHere =
        DaemonRowFacts { .state = DaemonLinkState::TooOld, .locality = DaemonLocality::Local };
    CHECK(daemonRowButtons(tooOldHost, RowPending::No) == Buttons { Connect, None });
    CHECK(daemonRowButtons(tooOldHere, RowPending::No) == Buttons { None, None });
    // Every other state offers a host what it offers this machine.
    for (auto const state: allLinkStates())
    {
        if (state == DaemonLinkState::NotRunning || state == DaemonLinkState::TooOld)
            continue;
        INFO(std::format("state {}", static_cast<int>(state)));
        auto const local = DaemonRowFacts { .state = state, .locality = DaemonLocality::Local };
        auto const remote = DaemonRowFacts { .state = state, .locality = DaemonLocality::Remote };
        CHECK(daemonRowButtons(remote, RowPending::No) == daemonRowButtons(local, RowPending::No));
    }
}

TEST_CASE("Connect is offered only where this process holds the link", "[sidebar][daemon-row]")
{
    using enum DaemonRowButton;
    using Buttons = std::array<DaemonRowButton, 2>;
    for (auto const state:
         { DaemonLinkState::Failed, DaemonLinkState::NeedsInput, DaemonLinkState::NotConnected })
    {
        INFO(std::format("state {}", static_cast<int>(state)));
        auto facts = DaemonRowFacts { .state = state, .locality = DaemonLocality::Local };
        // An observer, and the window's own link, which the row seeks again as the chip does.
        for (auto const link: { DaemonRowLink::Observer, DaemonRowLink::OwnWindow })
        {
            facts.link = link;
            CHECK(daemonRowButtons(facts, RowPending::No) == Buttons { Connect, None });
        }
        // Another window's link is that window's to seek again: a Connect here would do nothing.
        facts.link = DaemonRowLink::OtherWindow;
        CHECK(daemonRowButtons(facts, RowPending::No) == Buttons { None, None });
        CHECK(daemonRowActivation(facts, LastKnownTree::Absent) != DaemonRowAction::Connect);
        CHECK(daemonRowActivation(facts, LastKnownTree::Present) != DaemonRowAction::Connect);
    }
}

TEST_CASE("Cancel ends a pending jump before a connect", "[sidebar][daemon-row]")
{
    auto const intoWork = SidebarTarget {
        .daemon = work(), .instance = DaemonInstance { 7 }, .workspace = 3, .tab = std::nullopt, .pane = {}
    };
    auto const here = PendingJumps { .here = intoWork, .newWindow = std::nullopt };
    auto const opening = PendingJumps { .here = std::nullopt, .newWindow = intoWork };

    // A jump into the row's section is what Cancel ends, even while its daemon connects for it.
    CHECK(cancelRouteOf(work(), here, DaemonLinkState::Connecting) == CancelRoute::JumpHere);
    CHECK(cancelRouteOf(work(), here, DaemonLinkState::Connected) == CancelRoute::JumpHere);
    // An Open in New Window into it is ended from its row too; the jump in this window first.
    CHECK(cancelRouteOf(work(), opening, DaemonLinkState::Connecting) == CancelRoute::JumpNewWindow);
    CHECK(cancelRouteOf(
              work(), PendingJumps { .here = intoWork, .newWindow = intoWork }, DaemonLinkState::Connected)
          == CancelRoute::JumpHere);
    // A jump elsewhere leaves this row's connect to its Cancel.
    CHECK(cancelRouteOf(starfive(), here, DaemonLinkState::Connecting) == CancelRoute::Connect);
    CHECK(cancelRouteOf(starfive(), opening, DaemonLinkState::Connecting) == CancelRoute::Connect);
    CHECK(cancelRouteOf(starfive(), PendingJumps {}, DaemonLinkState::Connecting) == CancelRoute::Connect);
    // Nothing under way: nothing to cancel.
    CHECK(cancelRouteOf(starfive(), PendingJumps {}, DaemonLinkState::Connected) == CancelRoute::None);
    CHECK(cancelRouteOf(starfive(), here, DaemonLinkState::NotRunning) == CancelRoute::None);
}

TEST_CASE("daemon row faces differ from every tab-status and link glyph, per look", "[sidebar][daemon-row]")
{
    using contour::config::SidebarLook;
    for (auto const index: std::views::iota(std::size_t { 0 }, DaemonRowFaces.size()))
    {
        auto const look = static_cast<SidebarLook>(index);
        auto const& face = daemonRowFace(look);
        INFO(std::format("look {}", index));
        REQUIRE(face.look == look);

        // What else the sidebar draws in a cell on this look: the tabs' statuses, the link marks, the tree's
        // connectors and folds, and the sidebar's own controls.
        auto taken = std::vector<std::string_view> { "├", "└", "│", "▾", "▸" };
        for (auto const kind: std::views::iota(std::size_t { 1 }, TabBadgeKindCount))
            taken.push_back(tabBadgeGlyph(static_cast<TabBadgeKind>(kind), look));
        for (auto const state: allLinkStates())
            taken.push_back(linkLookOf(state, look).glyph);
        for (auto const control: std::views::iota(std::size_t { 0 }, SidebarControlCount))
            taken.push_back(sidebarControlFace(look, static_cast<SidebarControl>(control)).glyph);

        // Remove, which has no glyph, keeps its words in cells.
        auto marks = std::vector<std::string_view> { face.pendingMark };
        for (auto const button: std::views::iota(std::size_t { 1 }, DaemonRowButtonCount))
            if (auto const glyph = daemonRowButtonFace(static_cast<DaemonRowButton>(button)).glyph;
                !glyph.empty())
                marks.push_back(glyph);
        CHECK_FALSE(face.pendingMark.empty());
        for (auto const mark: marks)
        {
            INFO(std::format("mark {}", mark));
            CHECK(std::ranges::find(taken, mark) == taken.end());
        }
        // The marks differ from each other too.
        for (auto const i: std::views::iota(std::size_t { 0 }, marks.size()))
            for (auto const j: std::views::iota(i + 1, marks.size()))
                CHECK(marks[i] != marks[j]);

        // A native look names the locality by an icon; the Terminal row's name stands at column 0, unmarked,
        // its disclosure after it, where the native looks draw theirs before the name as their workspaces do.
        auto const native = look != SidebarLook::CellTree;
        CHECK(face.localIcon.empty() == !native);
        CHECK(face.remoteIcon.empty() == !native);
        CHECK((native ? face.localIcon != face.remoteIcon : true));
        CHECK((face.disclosure == DaemonRowDisclosure::Leading) == native);
    }
}

TEST_CASE("no two marks a daemon row draws are confusable", "[sidebar][daemon-row]")
{
    // Glyphs that read as one another at a row's size: shapes that differ only by a dotted outline, a size or
    // a stroke. A daemon row draws its link's mark, the urgency beneath it, its pending mark, its buttons'
    // glyphs and the ellipsis of an elided name, so no pair may be among them on any look. The link's ⨯ and a
    // tab's ✕ are kept apart on purpose and both mean "failed", so they are not a pair.
    using contour::config::SidebarLook;
    constexpr auto Confusables = std::array {
        std::pair { std::string_view { "○" }, std::string_view { "◌" } },
        std::pair { std::string_view { "…" }, std::string_view { "⋯" } },
        std::pair { std::string_view { "◐" }, std::string_view { "◑" } },
        std::pair { std::string_view { "◔" }, std::string_view { "◑" } },
        std::pair { std::string_view { "⊗" }, std::string_view { "⨯" } },
        std::pair { std::string_view { "⊗" }, std::string_view { "✕" } },
        std::pair { std::string_view { "⊗" }, std::string_view { "⊘" } },
        std::pair { std::string_view { "▷" }, std::string_view { "▸" } },
        std::pair { std::string_view { "⧗" }, std::string_view { "⧖" } },
        std::pair { std::string_view { "→" }, std::string_view { "⇢" } },
        std::pair { std::string_view { "−" }, std::string_view { "–" } },
        std::pair { std::string_view { "-" }, std::string_view { "–" } },
    };
    for (auto const index: std::views::iota(std::size_t { 0 }, DaemonRowFaces.size()))
    {
        auto const look = static_cast<SidebarLook>(index);
        auto const& face = daemonRowFace(look);
        INFO(std::format("look {}", index));
        auto drawn = std::vector<std::string_view> { face.pendingMark, "…" };
        for (auto const state: allLinkStates())
            drawn.push_back(linkLookOf(state, look).glyph);
        for (auto const kind: std::views::iota(std::size_t { 1 }, TabBadgeKindCount))
            drawn.push_back(tabBadgeGlyph(static_cast<TabBadgeKind>(kind), look));
        for (auto const button: std::views::iota(std::size_t { 1 }, DaemonRowButtonCount))
            drawn.push_back(daemonRowButtonFace(static_cast<DaemonRowButton>(button)).glyph);
        for (auto const& [first, second]: Confusables)
        {
            INFO(std::format("{} and {}", first, second));
            CHECK_FALSE((std::ranges::contains(drawn, first) && std::ranges::contains(drawn, second)));
        }
    }
}

TEST_CASE("every button that can be primary shrinks to an icon and a one-cell glyph", "[sidebar][daemon-row]")
{
    // The buttons shrink before the words leave the row, so each that can stand first has both forms. Remove
    // is always second and gives way before they shrink: its "−" would read as minimise or collapse, so it
    // has none. Cancel's glyph is the escape mark, not a cross, which reads as a failure on the Terminal row.
    for (auto const button: { DaemonRowButton::Connect, DaemonRowButton::Start, DaemonRowButton::Cancel })
    {
        auto const& face = daemonRowButtonFace(button);
        INFO(face.words);
        CHECK_FALSE(face.icon.empty());
        auto const clusters = clustersOf(face.glyph);
        CHECK(unitsOf(clusters, ElideUnit::Cells) == 1);
        CHECK(clusters.size() == 1);
    }
    CHECK(daemonRowButtonFace(DaemonRowButton::Remove).icon.empty());
    CHECK(daemonRowButtonFace(DaemonRowButton::Remove).glyph.empty());
    CHECK(daemonRowButtonFace(DaemonRowButton::Cancel).glyph == "⎋");
}

TEST_CASE("Cancel names its action's chord only while a jump is under way", "[sidebar][daemon-row]")
{
    // While a jump into the section is pending, Cancel is CancelSwitch, and its tooltip names that chord.
    CHECK(daemonRowButtonCommand(DaemonRowButton::Cancel, RowPending::Yes) == "CancelSwitch");
    // Otherwise it ends a connect, which CancelSwitch does not: its tooltip names no chord.
    CHECK(daemonRowButtonCommand(DaemonRowButton::Cancel, RowPending::No).empty());
    // The verbs that need their row run no action, pending or not.
    for (auto const button: { DaemonRowButton::Connect, DaemonRowButton::Start, DaemonRowButton::Remove })
        for (auto const pending: { RowPending::No, RowPending::Yes })
            CHECK(daemonRowButtonCommand(button, pending).empty());
}

TEST_CASE("Connect on the window's own link that needs the user raises its prompt", "[sidebar][daemon-row]")
{
    auto facts = DaemonRowFacts { .state = DaemonLinkState::NeedsInput,
                                  .locality = DaemonLocality::Local,
                                  .link = DaemonRowLink::OwnWindow };
    // Seeking that link again would be refused ("already being sought"): the prompt it waits on is the
    // answer.
    CHECK(connectRouteOf(facts) == ConnectRoute::Prompt);
    for (auto const state: { DaemonLinkState::Failed, DaemonLinkState::NotConnected })
    {
        facts.state = state;
        CHECK(connectRouteOf(facts) == ConnectRoute::Reconnect);
    }
    // An observer connects through the daemon sections whatever its state; another window's link not here.
    for (auto const state: { DaemonLinkState::NeedsInput, DaemonLinkState::Failed })
    {
        facts.state = state;
        facts.link = DaemonRowLink::Observer;
        CHECK(connectRouteOf(facts) == ConnectRoute::Sections);
        facts.link = DaemonRowLink::OtherWindow;
        CHECK(connectRouteOf(facts) == ConnectRoute::None);
    }
}

TEST_CASE("a native look heads a section in the secondary ink", "[sidebar][daemon-row]")
{
    using contour::config::SidebarLook;
    // A source list's section header ranks by being a header, not by its ink; the Terminal row's bold name
    // at column 0 keeps the row's own.
    for (auto const look: { SidebarLook::SourceList, SidebarLook::NavigationPane, SidebarLook::HeaderPane })
        CHECK(daemonRowFace(look).nameInk == DaemonRowNameInk::Secondary);
    CHECK(daemonRowFace(SidebarLook::CellTree).nameInk == DaemonRowNameInk::Primary);
}

TEST_CASE("the daemon row gives way in its documented order", "[sidebar][daemon-row]")
{
    // Fixed parts 40 (a locality icon of 20 among them), a name of 100 (30 elided, 20 at its shortest), words
    // 50, two buttons of 100 with their words and 60 in their icon forms, of which the secondary takes 40 and
    // 25.
    auto room = DaemonRowRoom { .available = 290,
                                .fixed = 40,
                                .locality = 20,
                                .name = 100,
                                .elidedName = 30,
                                .shortName = 20,
                                .words = 50,
                                .buttonText = 100,
                                .buttonIcon = 60,
                                .secondaryText = 40,
                                .secondaryIcon = 25 };
    CHECK(daemonRowFitOf(room) == DaemonRowFit::Full);
    room.available = 289; // the locality icon goes first
    CHECK(daemonRowFitOf(room) == DaemonRowFit::NoLocality);
    room.available = 270;
    CHECK(daemonRowFitOf(room) == DaemonRowFit::NoLocality);
    room.available = 269; // then the secondary button, its words and all
    CHECK(daemonRowFitOf(room) == DaemonRowFit::NoSecondary);
    room.available = 230;
    CHECK(daemonRowFitOf(room) == DaemonRowFit::NoSecondary);
    room.available = 229; // then the primary button shrinks to its icon
    CHECK(daemonRowFitOf(room) == DaemonRowFit::IconButton);
    room.available = 205;
    CHECK(daemonRowFitOf(room) == DaemonRowFit::IconButton);
    room.available = 204; // then the words, into the mark's tooltip
    CHECK(daemonRowFitOf(room) == DaemonRowFit::NoWords);
    room.available = 155;
    CHECK(daemonRowFitOf(room) == DaemonRowFit::NoWords);
    room.available = 154; // then the name elides in the middle
    CHECK(daemonRowFitOf(room) == DaemonRowFit::NameElided);
    room.available = 85;
    CHECK(daemonRowFitOf(room) == DaemonRowFit::NameElided);
    room.available = 84; // to its shortest, and no further: the primary button stays
    CHECK(daemonRowFitOf(room) == DaemonRowFit::NameShort);
    room.available = 0;
    CHECK(daemonRowFitOf(room) == DaemonRowFit::NameShort);

    // A row with no words skips their step; one with no button keeps its name whole longer.
    room = DaemonRowRoom { .available = 140, .fixed = 40, .name = 100, .elidedName = 30, .shortName = 20 };
    CHECK(daemonRowFitOf(room) == DaemonRowFit::Full);
    room.available = 139;
    CHECK(daemonRowFitOf(room) == DaemonRowFit::NameElided);
}

TEST_CASE("the state's words outlast the locality icon, the secondary button and the buttons' words",
          "[sidebar][daemon-row]")
{
    // The words go only once the locality icon and the secondary button have gone and the primary button has
    // shrunk to its icon, so an ordinary name keeps them at the default width; the name gives way last.
    auto const firstWithout = [](auto const& kept) {
        return std::ranges::find_if_not(DaemonRowFitSteps, kept)->fit;
    };
    auto const words =
        firstWithout([](DaemonRowFitStep const& s) { return s.words == DaemonRowWords::Shown; });
    auto const locality =
        firstWithout([](DaemonRowFitStep const& s) { return s.locality == DaemonRowLocalityIcon::Shown; });
    auto const secondary =
        firstWithout([](DaemonRowFitStep const& s) { return s.secondary == DaemonRowSecondary::Shown; });
    auto const icons =
        firstWithout([](DaemonRowFitStep const& s) { return s.buttons == DaemonRowButtonForm::Words; });
    auto const name =
        firstWithout([](DaemonRowFitStep const& s) { return s.name == DaemonRowNameForm::Whole; });
    CHECK(locality < secondary);
    CHECK(secondary < icons);
    CHECK(icons < words);
    CHECK(words < name);
    CHECK(daemonRowFitStep(DaemonRowFit::NameShort).name == DaemonRowNameForm::Short);
}

TEST_CASE("each fit step shows what the order leaves", "[sidebar][daemon-row]")
{
    // Every step keeps what the one before it gave up: once gone, a part stays gone.
    auto const rank = [](DaemonRowFitStep const& step) {
        return std::array { static_cast<int>(step.name),
                            static_cast<int>(step.words),
                            static_cast<int>(step.buttons),
                            static_cast<int>(step.secondary),
                            static_cast<int>(step.locality) };
    };
    for (auto const i: std::views::iota(std::size_t { 1 }, DaemonRowFitSteps.size()))
    {
        INFO(std::format("step {}", i));
        auto const before = rank(DaemonRowFitSteps[i - 1]);
        auto const after = rank(DaemonRowFitSteps[i]);
        CHECK(before != after);
        for (auto const part: std::views::iota(std::size_t { 0 }, before.size()))
            CHECK(after[part] >= before[part]);
    }
}

TEST_CASE("a long daemon name elides in the middle", "[sidebar][daemon-row]")
{
    constexpr auto Long = std::string_view { "build-farm-eu-west" };
    for (auto const fit: { DaemonRowFit::Full,
                           DaemonRowFit::NoLocality,
                           DaemonRowFit::NoSecondary,
                           DaemonRowFit::NoWords,
                           DaemonRowFit::IconButton })
        CHECK(daemonRowNameText(Long, fit) == Long);
    auto const elided = daemonRowNameText(Long, DaemonRowFit::NameElided);
    INFO(elided);
    // As the location chip elides a daemon's name: its ends kept, the middle given up for one ellipsis.
    CHECK(elided == elideMiddle(Long, LocationChipDaemonElidedMax, ElideUnit::Cells));
    CHECK(elided.starts_with("b"));
    CHECK(elided.ends_with("t"));
    CHECK(elided.contains("…"));
    // At its shortest it keeps six of its letters, three at either end.
    auto const shortest = daemonRowNameText(Long, DaemonRowFit::NameShort);
    CHECK(shortest == elideMiddle(Long, DaemonRowNameMinimumCells, ElideUnit::Cells));
    CHECK(shortest.starts_with(Long.substr(0, 3)));
    CHECK(shortest.ends_with(Long.substr(Long.size() - 3)));
    // A name that fits keeps every letter, whatever the fit.
    CHECK(daemonRowNameText("work", DaemonRowFit::NameShort) == "work");
}

TEST_CASE("once the words leave the row, the mark's tooltip says them", "[sidebar][daemon-row]")
{
    // While the words stand beside the mark the tooltip is the reason alone.
    CHECK(daemonRowToolTipOf("Failed", "host unreachable", DaemonRowFit::NoSecondary) == "host unreachable");
    CHECK(daemonRowToolTipOf("Connecting…", "", DaemonRowFit::Full).empty());
    // Once they have gone it says them first, so a first-time user learns what the mark means.
    CHECK(daemonRowToolTipOf("Failed", "host unreachable", DaemonRowFit::NoWords)
          == "Failed · host unreachable");
    CHECK(daemonRowToolTipOf("Not running", "", DaemonRowFit::NameShort) == "Not running");
    CHECK(daemonRowToolTipOf("", "", DaemonRowFit::NoWords).empty());
}

TEST_CASE("Cancel of a connect is offered only on an observer's row", "[sidebar][daemon-row]")
{
    // A window adopting the daemon holds its connection, Connecting: the rows of the other windows say so but
    // offer no Cancel, which would end no connect (only the daemon sections' observer connects end).
    using enum DaemonRowButton;
    using Buttons = std::array<DaemonRowButton, 2>;
    auto facts = DaemonRowFacts { .state = DaemonLinkState::Connecting, .locality = DaemonLocality::Remote };
    facts.link = DaemonRowLink::Observer;
    CHECK(daemonRowButtons(facts, RowPending::No) == Buttons { Cancel, None });
    for (auto const link: { DaemonRowLink::OwnWindow, DaemonRowLink::OtherWindow })
    {
        INFO(std::format("link {}", static_cast<int>(link)));
        facts.link = link;
        CHECK(daemonRowButtons(facts, RowPending::No) == Buttons { None, None });
        // The window whose jump it is cancels that jump, whoever holds the link.
        CHECK(daemonRowButtons(facts, RowPending::Yes) == Buttons { Cancel, None });
    }
}
