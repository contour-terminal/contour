// SPDX-License-Identifier: Apache-2.0
//
// The mouse's map of the dashboard: what each drawn row, tab, chip, box and entry covers, recorded by the
// components while they draw, so that a click is resolved against the frame the user sees and never against
// a second layout of it.
#pragma once

#include <core/tui/Rect.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace orchestrator::view
{

/// What a region of the screen stands for.
enum class HitKind : uint8_t
{
    Popup = 0,      ///< A popup's whole area (a dialog, the palette, the timeline): it covers the dashboard.
    FleetPane,      ///< The Fleet's box: the wheel moves the selection.
    FocusPane,      ///< The Focus's box: the wheel scrolls its tab.
    Row,            ///< A Fleet row; Hit::value is its position among the rows that can be selected.
    Command,        ///< A tab or a chip; Hit::value is the KeyCommand a click runs, as its key would.
    MessageBox,     ///< The message box: a click gives it the keys.
    PaletteEntry,   ///< A palette entry; Hit::value is its position among the entries the query matches.
    ViewName,       ///< A view's name in the top bar; Hit::value is the View.
    WorkRow,        ///< A Work line; Hit::value is its index into workLines().
    WorkMrLink,     ///< An item's MR number cell; Hit::value is its line.
    WorkTicketLink, ///< An item's ticket key cell; Hit::value is its line.
    WorkHeading,    ///< A group heading; Hit::value is the WorkGroup.
    WorkScrollbar,  ///< The list's right edge; Hit::value is unused (the pointer's line decides).
    WorkChip,       ///< The filter bar; Hit::value is the WorkFocus (Filter, RepoChip, SortChip).
    DialogSetting,  ///< A dialog's or the `n` box's setting label; Hit::value is the FormField or the
                    ///< TaskBoxField.
    DialogStep,     ///< Its value or `‹ ›`; Hit::value is 2 × the field + (0 back, 1 forward).
    WorkerChip,     ///< A worker chip; Hit::value is its index in Config::workers().
    MenuEntry,      ///< A row menu entry; Hit::value is its index in RowMenuEntries.
};

/// What a click lands on.
struct Hit
{
    HitKind kind = HitKind::Popup; ///< What it is.
    std::size_t value = 0;         ///< Which one, as HitKind says.
    [[nodiscard]] bool operator==(Hit const&) const = default;
};

/// One region a component recorded, in screen cells (0-based).
struct HitRegion
{
    core::tui::Rect area; ///< Where it is.
    Hit hit;              ///< What it stands for.
    [[nodiscard]] bool operator==(HitRegion const&) const = default;
};

/// The regions of one frame, in the order they were drawn: what is drawn later lies on top.
class HitMap
{
  public:
    /// Forgets every region: a frame is about to be drawn.
    void clear() noexcept { _regions.clear(); }

    /// Records a region; an empty one is ignored.
    /// @param area Where it is, in screen cells.
    /// @param hit What it stands for.
    void add(core::tui::Rect area, Hit hit);

    /// What lies at a cell. Not `at()`: the guest has no exceptions, and its portability check reads every
    /// `at(` as the throwing accessor.
    /// @param x A column, 0-based.
    /// @param y A line, 0-based.
    /// @return What lies at the cell, the topmost first: what was drawn last.
    [[nodiscard]] std::vector<Hit> hitsAt(int x, int y) const;

    /// @param x A column, 0-based.
    /// @param y A line, 0-based.
    /// @return The regions that cover the cell, the topmost first: what hitsAt() answers, with where each is.
    [[nodiscard]] std::vector<HitRegion> regionsAt(int x, int y) const;

    /// @return Every region, in the order it was recorded.
    [[nodiscard]] std::span<HitRegion const> regions() const noexcept { return _regions; }

  private:
    std::vector<HitRegion> _regions;
};

/// Records hits in the coordinates of a canvas: a component's origin on the screen, moved as it draws into
/// parts of itself.
class HitRecorder
{
  public:
    /// @param map Where the hits go; it must outlive the recorder.
    /// @param origin The canvas's top-left cell on the screen: Component::screenBounds() while it renders.
    HitRecorder(HitMap& map, core::tui::Point origin): _map { &map }, _origin { origin } {}

    /// @return A recorder for a part of the canvas that starts @p dx columns and @p dy lines into it.
    [[nodiscard]] HitRecorder shifted(int dx, int dy) const noexcept
    {
        return HitRecorder { *_map, _origin.offset(dx, dy) };
    }

    /// Records @p hit at @p area, given in the canvas's coordinates.
    void add(core::tui::Rect area, Hit hit) const { _map->add(area.offset(_origin.x, _origin.y), hit); }

  private:
    HitMap* _map;
    core::tui::Point _origin;
};

} // namespace orchestrator::view
