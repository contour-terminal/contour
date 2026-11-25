// SPDX-License-Identifier: Apache-2.0
//
// The command palette (`^K`, `?`): every action, those that apply to the selected row first, typed to filter;
// and the popup it opens on the last 24 hours.
#pragma once

#include <core/tui/Canvas.hpp>
#include <core/tui/Component.hpp>
#include <core/tui/InputEvent.hpp>

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <orchestrator/core/Engine.hpp>
#include <orchestrator/view/HitMap.hpp>
#include <orchestrator/view/Keys.hpp>
#include <orchestrator/view/Sections.hpp>

namespace orchestrator::view
{

/// One command the palette lists.
struct PaletteEntry
{
    KeyCommand command = KeyCommand::Open; ///< What it runs: what its key would.
    std::string key = {};                  ///< Its key, `⏎` for the primary action; empty: palette only.
    std::string_view label = {};           ///< What it is called, for the selected row.
    std::string reason = {};               ///< Why it does not apply now, drawn dimmed; empty: it applies.
    [[nodiscard]] bool operator==(PaletteEntry const&) const = default;
};

/// @param row The selected row; nullptr: none.
/// @param engine The engine; nullptr: none.
/// @return Every command, in order: the row's actions that apply, in rank order; the global commands that
///         apply; then every other command, with the reason it does not apply.
[[nodiscard]] std::vector<PaletteEntry> paletteEntries(Row const* row, Engine const* engine);

/// One entry the query matches.
struct PaletteMatch
{
    std::size_t entry = 0;                 ///< Into the entries.
    std::vector<std::size_t> positions {}; ///< The grapheme clusters of its label the query matched.
    [[nodiscard]] bool operator==(PaletteMatch const&) const = default;
};

/// @param entries The palette's entries.
/// @param query What was typed; empty: every entry matches.
/// @return The entries whose label @p query matches fuzzily (smart case), in their order while it is empty;
///         else those that apply first, each part best match first.
[[nodiscard]] std::vector<PaletteMatch> paletteMatches(std::span<PaletteEntry const> entries,
                                                       std::string_view query);

/// @param palette The open palette.
/// @param context The selected row and the engine: what the entries are.
/// @return The command of the highlighted entry; std::nullopt when the query matches none.
[[nodiscard]] std::optional<KeyCommand> highlightedCommand(PaletteState const& palette,
                                                           KeyContext const& context);

/// Applies one key to the open palette. Printable keys and Backspace edit the query; ↑/↓ move the highlight;
/// ⏎ closes it and runs the highlighted command as its key would (dispatch()), which says why where it does
/// not apply; esc, or a key with Alt (an esc and the key after it, read together), closes it keeping the
/// query for the next time it opens.
/// @param key A key the terminal sent.
/// @param interaction Where the user is: its palette open.
/// @param context The selected row and the engine.
/// @return What the command it ran asks of the engine, if anything.
[[nodiscard]] std::optional<UiAction> applyPaletteKey(core::tui::KeyEvent const& key,
                                                      Interaction& interaction,
                                                      KeyContext const& context);

/// The palette, drawn in a rounded box: the query, the matching entries with their keys (`palette only` for
/// those without), the matched letters underlined, those that do not apply dimmed with the reason, and its
/// keys. Shown as an overlay at the size the dashboard places it at.
class PaletteView final: public core::tui::Component
{
  public:
    /// @param palette The dashboard's palette; nullopt while none is open, when nothing is drawn.
    /// @param hits Where it records what a click on it lands on.
    PaletteView(std::optional<PaletteState> const& palette, HitMap& hits):
        _palette { palette }, _hits { hits }
    {
    }
    void render(core::tui::Canvas& canvas) override;
    /// @return The size place() gave it.
    [[nodiscard]] core::tui::Size preferredSize() const override { return _size; }
    /// Sizes it, and sets what it lists: the selected row and the engine decide that, and the dashboard
    /// knows both.
    /// @param size The cells it takes.
    /// @param entries Its entries (paletteEntries()).
    void place(core::tui::Size size, std::vector<PaletteEntry> entries);
    /// @param entries The entries it would list.
    /// @return The rows it needs, its box included, with up to MaxPaletteRows of the list.
    [[nodiscard]] int heightFor(std::span<PaletteEntry const> entries) const;

  private:
    std::optional<PaletteState> const& _palette;
    HitMap& _hits;
    core::tui::Size _size {};
    std::vector<PaletteEntry> _entries;
};

/// The most entries the palette shows at once.
inline constexpr int MaxPaletteRows = 12;

/// The last 24 hours, an hour a bar, in a rounded box: the agents active, the tokens they wrote, the runs
/// started and ended. Opened from the palette; esc closes it.
class TimelineView final: public core::tui::Component
{
  public:
    /// @param engine The engine whose runs it charts.
    /// @param hits Where it records the area it covers.
    TimelineView(Engine const& engine, HitMap& hits): _engine { engine }, _hits { hits } {}
    void render(core::tui::Canvas& canvas) override;
    /// @return The size it needs: its lines and its box.
    [[nodiscard]] core::tui::Size preferredSize() const override;

  private:
    Engine const& _engine;
    HitMap& _hits;
};

} // namespace orchestrator::view
