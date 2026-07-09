// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/Color.h>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <vtmux/Pane.h>
#include <vtmux/Primitives.h>

namespace vtmux
{

/// Who assigned a tab's color.
///
/// The enumerators are ordered by **ascending precedence**: when more than one source has assigned a
/// color, the last one wins. That single rule is the whole resolution logic — Tab::color() is a reverse
/// scan over this list — so a third source is introduced by adding an enumerator at the right position,
/// not by editing precedence checks scattered across the model.
enum class TabColorSource : uint8_t
{
    Application, //!< Assigned by the terminal application via DECAC item 2 (window frame).
    User,        //!< Chosen by the user via the tab's color flyout. Overrides Application.
};

/// Number of tab-color sources, i.e. the extent of Tab's per-source color storage. Derived from the
/// last enumerator so that adding a source does not require updating a hand-written count.
inline constexpr size_t TabColorSourceCount = std::to_underlying(TabColorSource::User) + 1;

/// A tab: a named, optionally colored container for a binary split tree of panes.
///
/// The tab owns the root Pane, tracks which leaf is active (with an MRU list for focus
/// navigation), and — importantly — owns the authoritative **runtime title** and **runtime color**.
/// These live here, in Qt-free vtmux, deliberately *below* the GUI: the Contour GUI and any future
/// network-connected client are pure views of this state. They read title()/color() and request
/// mutations (setRuntimeTitle/setColor); they never hold their own authoritative copy. That is what
/// lets every client agree on the same title and color.
///
/// The color is stored per @ref TabColorSource rather than as a single slot, so that a color the user
/// picked and a color an application assigned coexist and neither destroys the other.
class Tab
{
  public:
    /// Resolves a session's own (program-driven) title, given its SessionId. Supplied by the host
    /// because the title source (vtbackend::Terminal) lives outside this Qt-free model.
    using SessionTitleResolver = std::function<std::string(SessionId)>;

    /// The label used for a tab whose active pane is part of a multi-pane split and which has no
    /// runtime title override. Mirrors Windows Terminal's "MultiplePanes".
    static constexpr std::string_view MultiplePanesLabel = "Multiple panes";

    /// Constructs a tab with a single leaf pane.
    Tab(TabId id, PaneId rootPaneId, SessionId session):
        _id { id }, _root { std::make_unique<Pane>(rootPaneId, session) }, _activeLeaf { _root.get() }
    {
        _mru.push_back(rootPaneId);
    }

    Tab(Tab const&) = delete;
    Tab& operator=(Tab const&) = delete;
    Tab(Tab&&) = delete;
    Tab& operator=(Tab&&) = delete;
    ~Tab() = default;

    [[nodiscard]] TabId id() const noexcept { return _id; }
    [[nodiscard]] Pane* rootPane() const noexcept { return _root.get(); }
    [[nodiscard]] Pane* activePane() const noexcept { return _activeLeaf; }
    [[nodiscard]] bool hasMultiplePanes() const noexcept { return !_root->isLeaf(); }
    [[nodiscard]] int paneCount() const noexcept { return _root->leafCount(); }

    /// Sets the active leaf and moves it to the front of the MRU list. @p leaf must be a leaf in
    /// this tab's tree.
    void setActivePane(Pane* leaf);

    /// Splits the active pane along @p direction, creating a new leaf for @p newSession.
    ///
    /// @param direction    Horizontal or Vertical.
    /// @param splitNodeId  Id the promoted node adopts (see Pane::split).
    /// @param newLeafId    Id of the new leaf.
    /// @param newSession   Session for the new leaf.
    /// @param ratio        First child's space share.
    /// @return The new leaf (which also becomes the active pane).
    Pane* splitActivePane(
        SplitState direction, PaneId splitNodeId, PaneId newLeafId, SessionId newSession, double ratio = 0.5);

    /// Closes @p leaf, absorbing its sibling. The active pane and MRU are updated. Returns the
    /// SessionId of the closed leaf so the caller can tear down that session.
    ///
    /// @pre @p leaf is not the root (closing the last pane is the owning model's job — it closes the
    ///      whole tab). Use isLastPane() to check.
    SessionId closePane(Pane* leaf);

    /// Whether @p leaf is the only pane in the tab (so closing it means closing the tab).
    [[nodiscard]] bool isLastPane(Pane const* leaf) const noexcept { return leaf == _root.get(); }

    /// Moves focus from the active pane in @p direction, if there is a neighbor. Returns the new
    /// active leaf, or nullptr if focus did not move.
    Pane* focusDirection(FocusDirection direction);

    /// Flips the orientation of the active pane's parent split (Horizontal<->Vertical).
    ///
    /// @return The split node whose orientation flipped, or nullptr if the active pane is the tab's
    ///         only pane (no parent split to flip).
    Pane* toggleActivePaneOrientation();

    /// Swaps the active pane with its neighbor in @p direction (the two terminals trade slots),
    /// keeping the active session focused in its new slot.
    ///
    /// Only the sessions move; both PaneIds stay put (see Pane::swapLeafPayload). The active pane
    /// follows its session, so after the swap the user is still "on" the terminal they were editing,
    /// now living where the neighbor was.
    /// @param direction The direction of the neighbor to swap with.
    /// @return A pair {a, b} of the two leaves whose sessions were swapped (a is the previously
    ///         active leaf), or {nullptr, nullptr} if there is no neighbor.
    std::pair<Pane*, Pane*> swapActivePane(FocusDirection direction);

    /// Moves the active pane across its neighbor in @p direction, re-parenting it in the tree.
    ///
    /// Unlike swapActivePane (which only trades sessions in place), this changes the tree topology:
    /// the active leaf is removed from its current split (which collapses onto its sibling), then the
    /// neighbor leaf is split along the axis of @p direction and the moved session is dropped on the
    /// side the move came from. The moved pane stays active. When the active leaf and its neighbor are
    /// the two children of one split, the move degenerates to an in-place session swap (there is no
    /// distinct destination), preserving the tree.
    /// @param direction   The direction to move the active pane.
    /// @param newSplitId  The id the neighbor node adopts when it promotes into a split (only used
    ///                    when a genuine re-parent happens; ignored on the swap-degenerate path).
    /// @return true if the pane moved (topology or session changed), false if there was no neighbor.
    bool moveActivePane(FocusDirection direction, PaneId newSplitId);

    // {{{ Title

    /// The resolved title, in precedence order:
    ///   1. the runtime title override (rename), if set;
    ///   2. "Multiple panes" if the tab has more than one pane;
    ///   3. the active leaf session's own title, via @p resolver.
    [[nodiscard]] std::string title(SessionTitleResolver const& resolver) const;

    [[nodiscard]] std::optional<std::string> const& runtimeTitle() const noexcept { return _runtimeTitle; }

    /// Sets (or, with nullopt, clears) the runtime title override.
    void setRuntimeTitle(std::optional<std::string> title) { _runtimeTitle = std::move(title); }

    // }}}
    // {{{ Color

    /// The tab's effective color: the one assigned by the highest-precedence source that has one (see
    /// TabColorSource). No color from any source means the host paints its default tab color.
    /// @return The effective color, or nullopt if the tab is uncolored.
    [[nodiscard]] std::optional<vtbackend::RGBColor> color() const noexcept
    {
        for (auto const& color: _colors | std::views::reverse)
            if (color.has_value())
                return color;
        return std::nullopt;
    }

    /// The color assigned by @p source alone, ignoring precedence.
    /// @param source The assigning source.
    /// @return That source's color, or nullopt if it has assigned none.
    [[nodiscard]] std::optional<vtbackend::RGBColor> const& color(TabColorSource source) const noexcept
    {
        return _colors[std::to_underlying(source)];
    }

    /// Assigns @p color on behalf of @p source, leaving every other source's color untouched.
    /// @param source The assigning source.
    /// @param color The color to assign.
    void setColor(TabColorSource source, vtbackend::RGBColor color)
    {
        _colors[std::to_underlying(source)] = color;
    }

    /// Clears @p source's color, so the tab falls back to the next-highest source that has one (and to
    /// the host default if none does). This is what makes "set the tab color back to default" restore an
    /// application's DECAC color rather than erase it.
    /// @param source The source whose color to clear.
    void resetColor(TabColorSource source) { _colors[std::to_underlying(source)].reset(); }

    // }}}

  private:
    TabId _id;
    std::unique_ptr<Pane> _root;
    Pane* _activeLeaf;
    std::vector<PaneId> _mru; //!< Most-recently-used leaf ids, front = most recent.
    std::optional<std::string> _runtimeTitle;
    std::array<std::optional<vtbackend::RGBColor>, TabColorSourceCount> _colors {};

    void touchMru(PaneId id);
    void forgetFromMru(PaneId id);
};

} // namespace vtmux
