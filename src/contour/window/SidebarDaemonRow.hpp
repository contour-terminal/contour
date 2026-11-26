// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What a daemon row does when it is activated, and which buttons it offers, in every state its link can be
/// in: one table, read by the sidebar's model and tested row by row. And how the row is drawn: its faces per
/// look, its buttons' faces, and the order its parts give way in a narrow sidebar. Qt-free.

#include <contour/config/ChromeTokenGroups.hpp>
#include <contour/window/DaemonLinkLook.hpp>
#include <contour/window/DaemonSection.hpp>
#include <contour/window/GraphemeElide.hpp>
#include <contour/window/LocationChip.hpp>
#include <contour/window/SidebarTree.hpp>
#include <contour/window/TranslateNoop.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>

namespace contour::window
{

/// What activating a daemon row (a click, Return) does.
enum class DaemonRowAction : uint8_t
{
    Nothing = 0, ///< Nothing: a button of the row acts, or there is nothing to do.
    ToggleFold,  ///< Folds or unfolds the daemon's workspaces; the keyboard stays in the sidebar.
    Connect,     ///< Connects to the daemon, asking in this window for what it needs.
    Start,       ///< Starts the local daemon.
};

/// A button a daemon row offers.
enum class DaemonRowButton : uint8_t
{
    None = 0, ///< No button in this place.
    Connect,  ///< Connects to the daemon.
    Start,    ///< Starts the local daemon.
    Cancel,   ///< Ends what is under way: a pending jump into the section, or a connect.
    Remove,   ///< Unlists the daemon for the rest of the run.
};

/// Number of @c DaemonRowButton enumerators, derived from the last one.
inline constexpr std::size_t DaemonRowButtonCount = static_cast<std::size_t>(DaemonRowButton::Remove) + 1;

/// The translation context a daemon row button's words are collected and translated in.
inline constexpr auto DaemonRowContext = std::string_view { "DaemonRow" };

/// When a daemon row button runs its action, whose bound chord its tooltip then names.
enum class DaemonRowCommandWhen : uint8_t
{
    Always = 0,  ///< Whatever the row's state.
    JumpPending, ///< Only while a jump into the row's section is under way: otherwise it does something else.
};

/// How one daemon row button looks: its words while the row has room, else its icon (a one-cell glyph on the
/// Terminal row). Remove has no icon form, as an icon that would say it unambiguously does not exist ("−"
/// reads as minimise or collapse); it gives way before the buttons shrink. Cancel's glyph is the escape mark,
/// as Esc cancels a jump: its cross would read as a failure among the Terminal row's red crosses.
struct DaemonRowButtonFace
{
    DaemonRowButton button = DaemonRowButton::None; ///< The button this row describes.
    std::string_view words {};                      ///< What it says (English source text).
    std::string_view icon {};  ///< The bundled symbolic icon of its icon-only form, if any.
    std::string_view glyph {}; ///< Its icon-only form in one cell on the Terminal row, if any.
    /// The action it runs, whose bound chord its tooltip names; empty for one whose verb needs the row.
    std::string_view command {};
    /// When it runs @c command: Cancel ends a connect, which no action does, while no jump is under way.
    DaemonRowCommandWhen commandWhen = DaemonRowCommandWhen::Always;
};

/// How a text button is drawn in cells: its words in brackets, as the Terminal row draws a check box `[x]`,
/// so two buttons side by side never read as one phrase.
inline constexpr std::string_view CellButtonFormat = "[{}]";

/// The buttons' faces, indexed by @c DaemonRowButton.
inline constexpr auto DaemonRowButtonFaces = std::array<DaemonRowButtonFace, DaemonRowButtonCount> {
    DaemonRowButtonFace { .button = DaemonRowButton::None },
    DaemonRowButtonFace { .button = DaemonRowButton::Connect,
                          .words = QT_TRANSLATE_NOOP("DaemonRow", "Connect"),
                          .icon = "qrc:/contour/icons/network-connect-symbolic.svg",
                          .glyph = "→" },
    DaemonRowButtonFace { .button = DaemonRowButton::Start,
                          .words = QT_TRANSLATE_NOOP("DaemonRow", "Start"),
                          .icon = "qrc:/contour/icons/media-playback-start-symbolic.svg",
                          .glyph = "▶" },
    DaemonRowButtonFace { .button = DaemonRowButton::Cancel,
                          .words = QT_TRANSLATE_NOOP("DaemonRow", "Cancel"),
                          .icon = "qrc:/contour/icons/process-stop-symbolic.svg",
                          .glyph = "⎋",
                          .command = "CancelSwitch",
                          .commandWhen = DaemonRowCommandWhen::JumpPending },
    DaemonRowButtonFace { .button = DaemonRowButton::Remove,
                          .words = QT_TRANSLATE_NOOP("DaemonRow", "Remove"),
                          .icon = {},
                          .glyph = {} },
};

static_assert(std::ranges::all_of(std::views::iota(std::size_t { 0 }, DaemonRowButtonFaces.size()),
                                  [](std::size_t i) {
                                      return DaemonRowButtonFaces[i].button
                                             == static_cast<DaemonRowButton>(i);
                                  }),
              "DaemonRowButtonFaces must list the buttons in enumerator order");

/// @param button A daemon row button.
/// @return How it looks.
[[nodiscard]] constexpr DaemonRowButtonFace const& daemonRowButtonFace(DaemonRowButton button) noexcept
{
    return DaemonRowButtonFaces[static_cast<std::size_t>(button)];
}

/// @param button A daemon row button.
/// @param pending Whether a jump into the row's section is under way.
/// @return The action the button runs now, whose bound chord its tooltip names; empty while it runs none.
[[nodiscard]] constexpr std::string_view daemonRowButtonCommand(DaemonRowButton button,
                                                                RowPending pending) noexcept
{
    auto const& face = daemonRowButtonFace(button);
    switch (face.commandWhen)
    {
        case DaemonRowCommandWhen::Always: return face.command;
        case DaemonRowCommandWhen::JumpPending:
            return pending == RowPending::Yes ? face.command : std::string_view {};
    }
    return {};
}

/// Where a daemon row draws its fold's disclosure.
enum class DaemonRowDisclosure : uint8_t
{
    Leading = 0, ///< Before the name, as the look's workspace rows draw theirs.
    Trailing,    ///< After the name, in the look's own disclosure glyphs.
};

/// The ink a daemon row's name takes.
enum class DaemonRowNameInk : uint8_t
{
    Primary = 0, ///< The row's own ink, as the Terminal row's bold heading at column 0.
    Secondary,   ///< The secondary ink, as a source list's section header: it ranks by being a header.
};

/// How a daemon row is drawn on one sidebar look, beyond what the link table (DaemonLinkLooks) says.
struct DaemonRowFace
{
    config::SidebarLook look = config::SidebarLook::SourceList; ///< The look this row describes.
    /// The bundled icon after the name of this machine's daemon; empty on the Terminal row, whose heading
    /// names the daemon flush at column 0.
    std::string_view localIcon {};
    /// The bundled icon after a host's daemon's name; empty where localIcon is.
    std::string_view remoteIcon {};
    /// The mark the status slot shows while a jump into the section is under way: one cell, the same on every
    /// look, softly pulsing where motion is allowed and still under Reduce Motion.
    std::string_view pendingMark {};
    /// Where the heading draws its fold's disclosure: leading, where the look's workspace rows draw a
    /// chevron, or trailing, after the name, on the Terminal row, whose name stands at column 0 so a daemon
    /// row is never mistaken for a workspace row, yet a folded daemon still differs from one with none.
    DaemonRowDisclosure disclosure = DaemonRowDisclosure::Leading;
    /// The name's ink: the secondary ink on the native rows, whose heading is set in the footnote's size.
    DaemonRowNameInk nameInk = DaemonRowNameInk::Primary;
};

namespace detail
{
    inline constexpr auto LocalDaemonIcon = std::string_view { "qrc:/contour/icons/computer-symbolic.svg" };
    inline constexpr auto RemoteDaemonIcon =
        std::string_view { "qrc:/contour/icons/network-server-symbolic.svg" };
    inline constexpr auto PendingMark = std::string_view { "⧗" };
} // namespace detail

/// The daemon row's faces, in SidebarLook order. The locality icon stands after the name, so the name aligns
/// with its workspaces' names, as a source list's section header leads with its words.
inline constexpr auto DaemonRowFaces = std::array<DaemonRowFace, 4> {
    DaemonRowFace { .look = config::SidebarLook::SourceList,
                    .localIcon = detail::LocalDaemonIcon,
                    .remoteIcon = detail::RemoteDaemonIcon,
                    .pendingMark = detail::PendingMark,
                    .nameInk = DaemonRowNameInk::Secondary },
    DaemonRowFace { .look = config::SidebarLook::NavigationPane,
                    .localIcon = detail::LocalDaemonIcon,
                    .remoteIcon = detail::RemoteDaemonIcon,
                    .pendingMark = detail::PendingMark,
                    .nameInk = DaemonRowNameInk::Secondary },
    DaemonRowFace { .look = config::SidebarLook::HeaderPane,
                    .localIcon = detail::LocalDaemonIcon,
                    .remoteIcon = detail::RemoteDaemonIcon,
                    .pendingMark = detail::PendingMark,
                    .nameInk = DaemonRowNameInk::Secondary },
    DaemonRowFace { .look = config::SidebarLook::CellTree,
                    .pendingMark = detail::PendingMark,
                    .disclosure = DaemonRowDisclosure::Trailing },
};

static_assert(std::ranges::all_of(std::views::iota(std::size_t { 0 }, DaemonRowFaces.size()),
                                  [](std::size_t i) {
                                      return DaemonRowFaces[i].look == static_cast<config::SidebarLook>(i);
                                  }),
              "DaemonRowFaces lists every SidebarLook once, in enumerator order");

/// @param look A sidebar look.
/// @return How a daemon row is drawn on it.
[[nodiscard]] constexpr DaemonRowFace const& daemonRowFace(config::SidebarLook look) noexcept
{
    return DaemonRowFaces[static_cast<std::size_t>(look)];
}

/// How much of a daemon row fits its width, in the order its parts give way (spec §4, Fit): each step keeps
/// what the one before it gave up. The state's words outlast the locality icon, the secondary button and the
/// buttons' words, and the name gives way last, never beyond DaemonRowNameMinimumCells; the primary button is
/// never dropped.
enum class DaemonRowFit : uint8_t
{
    Full =
        0, ///< Everything: the name whole, the locality icon, the link's mark and words, the buttons' words.
    NoLocality,  ///< The locality icon after the name goes first.
    NoSecondary, ///< And the secondary button (Remove), which the row's menus still offer.
    IconButton,  ///< And the primary button shrinks to its icon (its one-cell glyph on the Terminal row).
    NoWords,     ///< And the link's words, into the mark's tooltip; the mark stays.
    NameElided,  ///< And the name elides in the middle, to LocationChipDaemonElidedMax cells.
    NameShort,   ///< And further, to DaemonRowNameMinimumCells: the least of the name the row keeps.
};

/// Number of @c DaemonRowFit enumerators, derived from the last one.
inline constexpr std::size_t DaemonRowFitCount = static_cast<std::size_t>(DaemonRowFit::NameShort) + 1;

/// The fewest cells a daemon row shortens its name to: six of its letters and the ellipsis between them.
inline constexpr std::size_t DaemonRowNameMinimumCells = 7;

/// How much of a daemon's name a fit shows.
enum class DaemonRowNameForm : uint8_t
{
    Whole = 0, ///< Every letter.
    Elided,    ///< Elided in the middle to LocationChipDaemonElidedMax cells.
    Short,     ///< Elided in the middle to DaemonRowNameMinimumCells.
};

/// Where a fit puts the link's words.
enum class DaemonRowWords : uint8_t
{
    Shown = 0, ///< Beside the mark.
    InToolTip, ///< In the mark's tooltip, with the reason.
};

/// How a fit draws the row's buttons.
enum class DaemonRowButtonForm : uint8_t
{
    Words = 0, ///< Each with its words.
    Icons,     ///< Each that has an icon as its icon alone (Remove, which has none, has gone by then).
};

/// Whether a fit keeps the secondary button.
enum class DaemonRowSecondary : uint8_t
{
    Shown = 0, ///< It stays.
    Dropped,   ///< It gives way, after the locality icon.
};

/// Whether a fit keeps the locality icon after the name.
enum class DaemonRowLocalityIcon : uint8_t
{
    Shown = 0, ///< It stays.
    Dropped,   ///< It gives way, first of all.
};

/// One step of the fit order: what the row shows at it.
struct DaemonRowFitStep
{
    DaemonRowFit fit;               ///< The step.
    DaemonRowNameForm name;         ///< How much of the name it shows.
    DaemonRowWords words;           ///< Where the link's words go.
    DaemonRowButtonForm buttons;    ///< How the buttons are drawn.
    DaemonRowSecondary secondary;   ///< Whether the secondary button stays.
    DaemonRowLocalityIcon locality; ///< Whether the locality icon stays.
};

/// The fit order, indexed by @c DaemonRowFit: the first step whose parts fit is the row's.
inline constexpr auto DaemonRowFitSteps = std::array<DaemonRowFitStep, DaemonRowFitCount> {
    DaemonRowFitStep { .fit = DaemonRowFit::Full,
                       .name = DaemonRowNameForm::Whole,
                       .words = DaemonRowWords::Shown,
                       .buttons = DaemonRowButtonForm::Words,
                       .secondary = DaemonRowSecondary::Shown,
                       .locality = DaemonRowLocalityIcon::Shown },
    DaemonRowFitStep { .fit = DaemonRowFit::NoLocality,
                       .name = DaemonRowNameForm::Whole,
                       .words = DaemonRowWords::Shown,
                       .buttons = DaemonRowButtonForm::Words,
                       .secondary = DaemonRowSecondary::Shown,
                       .locality = DaemonRowLocalityIcon::Dropped },
    DaemonRowFitStep { .fit = DaemonRowFit::NoSecondary,
                       .name = DaemonRowNameForm::Whole,
                       .words = DaemonRowWords::Shown,
                       .buttons = DaemonRowButtonForm::Words,
                       .secondary = DaemonRowSecondary::Dropped,
                       .locality = DaemonRowLocalityIcon::Dropped },
    DaemonRowFitStep { .fit = DaemonRowFit::IconButton,
                       .name = DaemonRowNameForm::Whole,
                       .words = DaemonRowWords::Shown,
                       .buttons = DaemonRowButtonForm::Icons,
                       .secondary = DaemonRowSecondary::Dropped,
                       .locality = DaemonRowLocalityIcon::Dropped },
    DaemonRowFitStep { .fit = DaemonRowFit::NoWords,
                       .name = DaemonRowNameForm::Whole,
                       .words = DaemonRowWords::InToolTip,
                       .buttons = DaemonRowButtonForm::Icons,
                       .secondary = DaemonRowSecondary::Dropped,
                       .locality = DaemonRowLocalityIcon::Dropped },
    DaemonRowFitStep { .fit = DaemonRowFit::NameElided,
                       .name = DaemonRowNameForm::Elided,
                       .words = DaemonRowWords::InToolTip,
                       .buttons = DaemonRowButtonForm::Icons,
                       .secondary = DaemonRowSecondary::Dropped,
                       .locality = DaemonRowLocalityIcon::Dropped },
    DaemonRowFitStep { .fit = DaemonRowFit::NameShort,
                       .name = DaemonRowNameForm::Short,
                       .words = DaemonRowWords::InToolTip,
                       .buttons = DaemonRowButtonForm::Icons,
                       .secondary = DaemonRowSecondary::Dropped,
                       .locality = DaemonRowLocalityIcon::Dropped },
};

static_assert(std::ranges::all_of(std::views::iota(std::size_t { 0 }, DaemonRowFitSteps.size()),
                                  [](std::size_t i) {
                                      return DaemonRowFitSteps[i].fit == static_cast<DaemonRowFit>(i);
                                  }),
              "DaemonRowFitSteps must list the fits in enumerator order");

/// @param fit A fit.
/// @return What the row shows at it.
[[nodiscard]] constexpr DaemonRowFitStep const& daemonRowFitStep(DaemonRowFit fit) noexcept
{
    return DaemonRowFitSteps[static_cast<std::size_t>(fit)];
}

/// What a daemon row's parts take, in the units of its width.
struct DaemonRowRoom
{
    double available = 0; ///< The width the row's parts may take.
    double fixed = 0;     ///< What stands whatever the room: the disclosure, the icon, the slot's mark, gaps.
    double locality = 0;  ///< What the locality icon takes of @c fixed, its gap included; 0 for none.
    double name = 0;      ///< The name, whole.
    double elidedName = 0; ///< The name as daemonRowNameText() elides it (DaemonRowNameForm::Elided).
    double shortName = 0;  ///< The name at its shortest (DaemonRowNameForm::Short).
    double words = 0;      ///< The link's words, with their gap; 0 for none.
    double buttonText = 0; ///< The buttons with their words, gaps included; 0 for none.
    double buttonIcon = 0; ///< The buttons in their icon forms, gaps included.
    /// What the secondary button takes of @c buttonText, its gap included; 0 for none.
    double secondaryText = 0;
    /// What the secondary button takes of @c buttonIcon, its gap included; 0 for none.
    double secondaryIcon = 0;
};

/// @param room What the row's parts take.
/// @param step A step of the fit order.
/// @return What the row's parts take at @p step.
[[nodiscard]] constexpr double daemonRowExtentAt(DaemonRowRoom const& room,
                                                 DaemonRowFitStep const& step) noexcept
{
    auto const name = [&] {
        switch (step.name)
        {
            case DaemonRowNameForm::Whole: return room.name;
            case DaemonRowNameForm::Elided: return room.elidedName;
            case DaemonRowNameForm::Short: return room.shortName;
        }
        return room.name;
    }();
    auto const icons = step.buttons == DaemonRowButtonForm::Icons;
    auto const secondary = step.secondary == DaemonRowSecondary::Dropped
                               ? (icons ? room.secondaryIcon : room.secondaryText)
                               : 0.0;
    auto const buttons = (icons ? room.buttonIcon : room.buttonText) - secondary;
    auto const locality = step.locality == DaemonRowLocalityIcon::Dropped ? room.locality : 0.0;
    return room.fixed - locality + name + (step.words == DaemonRowWords::Shown ? room.words : 0) + buttons;
}

/// @param room What the row's parts take.
/// @return The first step of the fit order whose parts fit @p room's width; NameShort when none does.
[[nodiscard]] constexpr DaemonRowFit daemonRowFitOf(DaemonRowRoom const& room) noexcept
{
    auto const fits = std::ranges::find_if(DaemonRowFitSteps, [&](DaemonRowFitStep const& step) {
        return daemonRowExtentAt(room, step) <= room.available;
    });
    return fits != DaemonRowFitSteps.end() ? fits->fit : DaemonRowFit::NameShort;
}

/// @param name A daemon's name.
/// @param fit  How much of its row fits.
/// @return @p name whole while the fit keeps it whole; else elided in the middle, as the location chip elides
///         it, to LocationChipDaemonElidedMax cells, or at the shortest to DaemonRowNameMinimumCells.
[[nodiscard]] inline std::string daemonRowNameText(std::string_view name, DaemonRowFit fit)
{
    switch (daemonRowFitStep(fit).name)
    {
        case DaemonRowNameForm::Whole: return std::string(name);
        case DaemonRowNameForm::Elided:
            return elideMiddle(name, LocationChipDaemonElidedMax, ElideUnit::Cells);
        case DaemonRowNameForm::Short: return elideMiddle(name, DaemonRowNameMinimumCells, ElideUnit::Cells);
    }
    return std::string(name);
}

/// What the tooltip of a daemon row's link mark says.
/// @param words  The link's words as shown (translated); empty for none.
/// @param reason Why the link failed; empty otherwise.
/// @param fit    How much of the row fits.
/// @return The reason while the words stand beside the mark; once they have gone, the words and the reason,
///         joined by LabelSeparator, so what the mark means is always a hover away.
[[nodiscard]] inline std::string daemonRowToolTipOf(std::string_view words,
                                                    std::string_view reason,
                                                    DaemonRowFit fit)
{
    if (daemonRowFitStep(fit).words == DaemonRowWords::Shown || words.empty())
        return std::string(reason);
    if (reason.empty())
        return std::string(words);
    return std::format("{}{}{}", words, LabelSeparator, reason);
}

/// Whether a section holds a tree last known of its daemon, which a daemon whose link failed still folds.
enum class LastKnownTree : uint8_t
{
    Absent = 0, ///< Nothing is known of its workspaces.
    Present,    ///< Its workspaces as last known are listed.
};

/// What a daemon row in one link state does and offers, for one kind of daemon.
struct DaemonRowOffer
{
    DaemonRowAction withTree;               ///< What activating the row does with a last-known tree.
    DaemonRowAction withoutTree;            ///< What it does with none.
    std::array<DaemonRowButton, 2> buttons; ///< Its buttons, leading first; None for an empty place.
};

/// One row of the table: what a daemon row in one link state does and offers, on this machine and on a host.
struct DaemonRowRule
{
    DaemonLinkState state; ///< The link state.
    DaemonRowOffer local;  ///< This machine's daemon, which the row can start and unlist.
    /// A host's daemon: nothing here starts it, and a saved host is not unlisted by Remove (Hide is its
    /// verb), so a host that is not running offers neither.
    DaemonRowOffer remote;
};

namespace detail
{
    /// An offer that is the same for a local and a remote daemon.
    [[nodiscard]] constexpr DaemonRowRule sameEverywhere(DaemonLinkState state, DaemonRowOffer offer) noexcept
    {
        return DaemonRowRule { .state = state, .local = offer, .remote = offer };
    }
} // namespace detail

/// Spec §4's table, one rule per DaemonLinkState, in enumerator order.
inline constexpr std::array<DaemonRowRule, DaemonLinkStateCount> DaemonRowRules {
    detail::sameEverywhere(DaemonLinkState::NotConnected,
                           { .withTree = DaemonRowAction::Connect,
                             .withoutTree = DaemonRowAction::Connect,
                             .buttons = { DaemonRowButton::Connect, DaemonRowButton::None } }),
    detail::sameEverywhere(DaemonLinkState::Connecting,
                           { .withTree = DaemonRowAction::Nothing,
                             .withoutTree = DaemonRowAction::Nothing,
                             .buttons = { DaemonRowButton::Cancel, DaemonRowButton::None } }),
    detail::sameEverywhere(DaemonLinkState::Connected,
                           { .withTree = DaemonRowAction::ToggleFold,
                             .withoutTree = DaemonRowAction::ToggleFold,
                             .buttons = { DaemonRowButton::None, DaemonRowButton::None } }),
    detail::sameEverywhere(DaemonLinkState::Reconnecting,
                           { .withTree = DaemonRowAction::ToggleFold,
                             .withoutTree = DaemonRowAction::ToggleFold,
                             .buttons = { DaemonRowButton::None, DaemonRowButton::None } }),
    detail::sameEverywhere(DaemonLinkState::NeedsInput,
                           { .withTree = DaemonRowAction::Connect,
                             .withoutTree = DaemonRowAction::Connect,
                             .buttons = { DaemonRowButton::Connect, DaemonRowButton::None } }),
    DaemonRowRule { .state = DaemonLinkState::NotRunning,
                    .local = { .withTree = DaemonRowAction::Start,
                               .withoutTree = DaemonRowAction::Start,
                               .buttons = { DaemonRowButton::Start, DaemonRowButton::Remove } },
                    .remote = { .withTree = DaemonRowAction::ToggleFold,
                                .withoutTree = DaemonRowAction::Nothing,
                                .buttons = { DaemonRowButton::None, DaemonRowButton::None } } },
    detail::sameEverywhere(DaemonLinkState::TooOld,
                           { .withTree = DaemonRowAction::ToggleFold,
                             .withoutTree = DaemonRowAction::Nothing,
                             .buttons = { DaemonRowButton::None, DaemonRowButton::None } }),
    detail::sameEverywhere(DaemonLinkState::Failed,
                           { .withTree = DaemonRowAction::ToggleFold,
                             .withoutTree = DaemonRowAction::Nothing,
                             .buttons = { DaemonRowButton::Connect, DaemonRowButton::None } }),
};

static_assert(std::ranges::all_of(std::views::iota(std::size_t { 0 }, DaemonRowRules.size()),
                                  [](std::size_t i) {
                                      return DaemonRowRules[i].state == DaemonLinkState(i);
                                  }),
              "DaemonRowRules must list the link states in enumerator order");

/// A daemon row's own facts, which decide what it does and offers.
struct DaemonRowFacts
{
    DaemonLinkState state = DaemonLinkState::NotConnected; ///< Where its link stands.
    DaemonLocality locality = DaemonLocality::Remote;      ///< Whether it runs on this machine.
    DaemonRowLink link = DaemonRowLink::Observer;          ///< Who holds its link.

    bool operator==(DaemonRowFacts const&) const = default;
};

/// @param row A daemon row.
/// @return Its facts.
[[nodiscard]] constexpr DaemonRowFacts daemonRowFactsOf(SidebarRow const& row) noexcept
{
    return DaemonRowFacts { .state = row.linkState, .locality = row.locality, .link = row.link };
}

/// @param facts The row's facts.
/// @return What its state offers for its locality.
[[nodiscard]] constexpr DaemonRowOffer const& daemonRowOfferOf(DaemonRowFacts const& facts) noexcept
{
    auto const& rule = DaemonRowRules[static_cast<std::size_t>(facts.state)];
    return facts.locality == DaemonLocality::Local ? rule.local : rule.remote;
}

/// @param verb A daemon row's button or action.
/// @param link Who holds the row's link.
/// @return Whether the row can carry it: Connect only where this process holds the link (an observer, or the
///         window's own); another window's link is that window's to seek again.
[[nodiscard]] constexpr bool linkCarries(DaemonRowButton verb, DaemonRowLink link) noexcept
{
    return verb != DaemonRowButton::Connect || link != DaemonRowLink::OtherWindow;
}

/// What activating a daemon row does.
/// @param facts The row's facts.
/// @param tree Whether its section lists a tree last known of it.
/// @return The table's action for its locality and tree; Nothing for a Connect its link cannot carry.
[[nodiscard]] constexpr DaemonRowAction daemonRowActivation(DaemonRowFacts const& facts,
                                                            LastKnownTree tree) noexcept
{
    auto const& offer = daemonRowOfferOf(facts);
    auto const action = tree == LastKnownTree::Present ? offer.withTree : offer.withoutTree;
    if (action == DaemonRowAction::Connect && !linkCarries(DaemonRowButton::Connect, facts.link))
        return DaemonRowAction::Nothing;
    return action;
}

/// The buttons a daemon row offers.
/// @param facts The row's facts.
/// @param pending Whether a jump into the daemon's section is under way.
/// @return The table's buttons for its locality, less what its link cannot carry; while a jump is pending,
///         Cancel alone, where Connect was.
[[nodiscard]] constexpr std::array<DaemonRowButton, 2> daemonRowButtons(DaemonRowFacts const& facts,
                                                                        RowPending pending) noexcept
{
    if (pending == RowPending::Yes)
        return { DaemonRowButton::Cancel, DaemonRowButton::None };
    auto buttons = daemonRowOfferOf(facts).buttons;
    for (auto& button: buttons)
        if (!linkCarries(button, facts.link))
            button = DaemonRowButton::None;
    return buttons;
}

/// Where a daemon row's Connect goes.
enum class ConnectRoute : uint8_t
{
    None = 0,  ///< Nowhere: another window holds the link, and seeks it again from its own chip.
    Sections,  ///< The daemon sections connect an observed daemon (IDaemonSections::connect()).
    Reconnect, ///< The window seeks its own link again (ISidebarHost::reconnect()).
    Prompt,    ///< The window's own link waits for the user: its pending prompt is brought to the front
               ///< (ISidebarHost::raisePrompt()), as seeking the link again would only be refused.
};

/// @param facts A daemon row's facts.
/// @return Where its Connect goes: an observer's to the daemon sections; the window's own link's to a
///         reconnect, or, while that link already waits for the user, to the prompt it waits on.
[[nodiscard]] constexpr ConnectRoute connectRouteOf(DaemonRowFacts const& facts) noexcept
{
    switch (facts.link)
    {
        case DaemonRowLink::Observer: return ConnectRoute::Sections;
        case DaemonRowLink::OwnWindow:
            return facts.state == DaemonLinkState::NeedsInput ? ConnectRoute::Prompt
                                                              : ConnectRoute::Reconnect;
        case DaemonRowLink::OtherWindow: return ConnectRoute::None;
    }
    return ConnectRoute::None;
}

/// What a daemon row's Cancel ends.
enum class CancelRoute : uint8_t
{
    None = 0, ///< Nothing is under way there.
    Jump,     ///< The pending jump into the daemon's section (ISidebarHost::cancelJump()).
    Connect,  ///< The connect still connecting (IDaemonSections::disconnect()).
};

/// Which of what may be under way a daemon row's Cancel ends: a pending jump first, as it is what the user
/// asked for last and what holds the connect.
/// @param row The daemon row's daemon.
/// @param pending Where the pending jump goes, if one is under way.
/// @param state The daemon's link state.
/// @return Jump for a jump into @p row's section; else Connect while it connects; else None.
[[nodiscard]] constexpr CancelRoute cancelRouteOf(DaemonRef const& row,
                                                  std::optional<SidebarTarget> const& pending,
                                                  DaemonLinkState state) noexcept
{
    if (pending && pending->daemon == row)
        return CancelRoute::Jump;
    if (state == DaemonLinkState::Connecting)
        return CancelRoute::Connect;
    return CancelRoute::None;
}

} // namespace contour::window
