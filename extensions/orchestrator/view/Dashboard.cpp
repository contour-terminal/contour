// SPDX-License-Identifier: Apache-2.0
#include <orchestrator/view/Dashboard.hpp>

#include <core/tui/Canvas.hpp>
#include <core/tui/Component.hpp>
#include <core/tui/HeadlessSurface.hpp>
#include <core/tui/HyperlinkEmitter.hpp>
#include <core/tui/Responsive.hpp>
#include <core/tui/Screen.hpp>
#include <core/tui/Spinner.hpp>
#include <core/tui/VtParser.hpp>
#include <core/tui/layout/Style.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iterator>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <orchestrator/view/Actions.hpp>
#include <orchestrator/view/Attention.hpp>
#include <orchestrator/view/Band.hpp>
#include <orchestrator/view/Fleet.hpp>
#include <orchestrator/view/Focus.hpp>
#include <orchestrator/view/HitMap.hpp>
#include <orchestrator/view/Palette.hpp>
#include <orchestrator/view/Sections.hpp>
#include <orchestrator/view/StatusBoard.hpp>
#include <orchestrator/view/Theme.hpp>
#include <orchestrator/view/Transcript.hpp>

using namespace std::string_view_literals;

namespace orchestrator::view
{

namespace
{
    using core::tui::Canvas;
    using core::tui::Component;
    using core::tui::FlexBox;
    using core::tui::Rect;
    using core::tui::Size;
    namespace layout = core::tui::layout;

    /// How long a lone ESC that ended an input waits for the rest of a sequence before it is the Escape key.
    constexpr auto EscapeTimeout = std::chrono::milliseconds { 50 };
    /// How far an agent's age moves before the rows are built again for it.
    constexpr auto AgeTickMs = uint64_t { 1000 };
    /// How often the transcript of a live run the Focus shows is read again.
    constexpr auto TranscriptFollowMs = uint64_t { 5000 };

    /// The widest the launcher is in the Wide band.
    constexpr auto LauncherMaxColumns = 80;
    /// The widest the palette is in the Wide band.
    constexpr auto PaletteMaxColumns = 64;
    /// How much of the tab's width a popup takes in the Medium band, in percent.
    constexpr auto PopupMediumPercent = 90;

    /// The shares of the body the Fleet and the Focus take, side by side (Wide) or one above the other
    /// (Medium, where the Focus takes 60 % of the height).
    constexpr auto FleetShare = 2;
    constexpr auto FocusShare = 3;

    /// The cells the `▰▱` meters take.
    constexpr auto MeterCells = 6;

    /// What follows the name of a run with news since the user last looked at it.
    constexpr auto UnseenMark = " •"sv;

    /// @return The cells a popup is wide in @p band, in a tab @p columns wide: in the Wide band at most
    ///         @p maxColumns, in the Medium band most of the tab, in the Narrow band all of it.
    [[nodiscard]] int popupWidthIn(Band band, int columns, int maxColumns) noexcept
    {
        switch (band)
        {
            case Band::Wide: return std::min(maxColumns, columns);
            case Band::Medium: return columns * PopupMediumPercent / 100;
            case Band::Narrow: break;
        }
        return columns;
    }

    /// @return The index into BandBreakpoints that @p size chooses: the last one whose minimum it meets.
    [[nodiscard]] std::size_t breakpointFor(Size size) noexcept
    {
        auto chosen = std::size_t { 0 };
        for (auto const index: std::views::iota(std::size_t { 0 }, BandBreakpoints.size()))
            if (size.width >= BandBreakpoints[index].minColumns)
                chosen = index;
        return chosen;
    }

    /// Whether a row is the selected one.
    enum class Selection : uint8_t
    {
        Unselected, ///< Drawn in its own roles.
        Selected,   ///< Drawn in the selection's.
    };

    /// A pane or bar that is shown or hidden as the user moves between the places.
    enum class PanelRole : uint8_t
    {
        TopBar, ///< The top bar, which gives way in a small tab.
        Fleet,  ///< The Fleet.
        Focus,  ///< The Focus.
    };

    /// How wide the Focus is drawn while it has the keys.
    enum class FocusWidth : uint8_t
    {
        Shared = 0, ///< Beside the Fleet, in the Wide band.
        Whole,      ///< The whole tab: a worktree's details hold a command line, which wants every cell.
    };

    /// @return Whether a panel of @p role is shown while the user is at @p place, in @p band, with @p given
    ///         furniture given way and the Focus as wide as @p width.
    [[nodiscard]] bool isShown(
        PanelRole role, Place place, Band band, Furniture given, FocusWidth width) noexcept
    {
        switch (role)
        {
            case PanelRole::TopBar: return !holds(given, Furniture::TopBar);
            case PanelRole::Fleet:
                return place == Place::Fleet || (band == Band::Wide && width == FocusWidth::Shared);
            case PanelRole::Focus: break;
        }
        return band != Band::Narrow || place == Place::Focus;
    }

    /// @return How wide the Focus of @p row is drawn while it has the keys.
    [[nodiscard]] FocusWidth focusWidthOf(Row const* row) noexcept
    {
        return row != nullptr && row->kind == RowKind::Orphan ? FocusWidth::Whole : FocusWidth::Shared;
    }

    /// How long after a press of the left button a second one on the same cell is a double click.
    constexpr auto DoubleClickMs = uint64_t { 400 };
    /// How many lines one step of the wheel scrolls the Focus's tab.
    constexpr auto WheelLines = std::ptrdiff_t { 3 };

    /// Whether a press of the left button is the second of a double click.
    enum class Clicks : uint8_t
    {
        Single = 0, ///< One press.
        Double,     ///< The second press on the same cell, within DoubleClickMs.
    };

    /// The last press of the left button.
    struct LastClick
    {
        int x = 0;         ///< Its column, 0-based.
        int y = 0;         ///< Its line, 0-based.
        uint64_t atMs = 0; ///< When, in the engine's monotonic milliseconds.
    };

    /// @return The identity of @p row across a rebuild: its run, its worktree, its group, or its kind.
    [[nodiscard]] std::string keyOf(Row const& row)
    {
        if (!row.run.empty())
            return row.run;
        if (row.kind == RowKind::Orphan)
            return std::format("orphan:{}", row.text);
        if (row.kind == RowKind::Heading)
            return std::format("group:{}", row.group);
        return std::format("kind:{}", std::to_underlying(row.kind));
    }
} // namespace

// {{{ the state

namespace
{
    class ProblemLines;
} // namespace

/// Everything the dashboard is: the screen it draws on, where the user is, and the rows it draws.
struct Dashboard::State
{
    State(Engine const& engine,
          core::tui::Theme theme,
          core::IClock const& clock,
          Size size,
          IUuidSource& ids,
          std::string tab);
    ~State();
    State(State const&) = delete;
    State& operator=(State const&) = delete;
    State(State&&) = delete;
    State& operator=(State&&) = delete;

    /// Builds the layout of @p breakpoint, an index into BandBreakpoints.
    [[nodiscard]] std::unique_ptr<Component> buildLayout(std::size_t breakpoint);
    /// Rebuilds the rows from the engine when they are stale: the engine changed, or an age moved on.
    void ensureRows();
    /// How refilter() keeps the selection.
    enum class Keep : uint8_t
    {
        Row, ///< On the row it was on, by its identity: across a rebuild, so that a run that changes groups
             ///< keeps it and a run that enters Needs you never takes it.
        Position, ///< At the position a key moved it to.
    };

    /// Applies the filter to the rows, and keeps the selection as @p keep says.
    void refilter(Keep keep);
    /// Applies keys the parser decoded: to the launcher while it is open, else by the key table.
    /// @param events The keys.
    /// @param hooksAtInput The form's HooksWarning as the input that holds them began.
    /// @return What the engine is asked, in order.
    [[nodiscard]] std::vector<UiAction> applyKeys(std::vector<core::tui::InputEvent> const& events,
                                                  HooksWarning hooksAtInput);
    /// Applies a mouse event to what the last frame drew under the pointer (MouseRows): a press of the left
    /// button clicks, the wheel scrolls; a popup takes them alone, and a click beside it closes it.
    /// @param mouse The event.
    /// @param actions What the engine is asked, appended to.
    void applyMouse(core::tui::MouseEvent const& mouse, std::vector<UiAction>& actions);
    /// Applies one key as if typed, as a click that stands for it does.
    /// @param key The key.
    /// @param actions What the engine is asked, appended to.
    void press(core::tui::KeyCode key, std::vector<UiAction>& actions);
    /// Runs a command as its key would, as a click on a tab or a chip does.
    /// @param command The command.
    /// @param actions What the engine is asked, appended to.
    void run(KeyCommand command, std::vector<UiAction>& actions);
    /// @return Whether a popup is open: the launcher, the palette or the timeline.
    [[nodiscard]] bool popupOpen() const noexcept;
    /// @return The form's HooksWarning now: what the next input begins with.
    [[nodiscard]] HooksWarning launcherHooks() const noexcept;
    /// Resolves a held ESC that waited past EscapeTimeout into the Escape key.
    /// @return What the engine is asked, in order.
    [[nodiscard]] std::vector<UiAction> resolveOverdueEscape();
    /// Shows the panes of the current place and band and hides the others; sets what the Focus draws; places
    /// the launcher, the palette and the timeline.
    void syncLayout();
    /// Gives a launcher just opened its agent's session id, from the dashboard's source.
    void mintSession();
    /// Shows the launcher where the band puts it, or hides it.
    void syncLauncher();
    /// Shows the palette where the band puts it, listing what applies to the selected row; or hides it.
    void syncPalette();
    /// Shows @p overlay centred in the tab at @p wanted, or moves it there.
    void showCentred(Component& overlay, Size wanted);
    /// Keeps the Focus's scroll within what its tab holds, once the last draw knows its end.
    void clampScroll() noexcept;
    /// @return What a key is applied to.
    [[nodiscard]] KeyContext keyContext() const noexcept;
    /// @return Now, in the engine's monotonic milliseconds.
    [[nodiscard]] uint64_t nowMonoMs() const noexcept;
    /// @return The selected row among the visible ones; nullptr when there is none.
    [[nodiscard]] Row const* selectedRow() const noexcept;
    /// @return The selected row's run; nullptr when it stands for none the engine knows.
    [[nodiscard]] Run const* selectedRun() const noexcept;
    /// Asks the engine to read the board of the run the Focus shows, and to stop reading the one it showed
    /// before: appended to @p actions.
    void syncBoard(std::vector<UiAction>& actions);
    /// Asks the engine to read the transcript page the Focus shows: on selection, again every
    /// TranscriptFollowMs while its run is live and it follows the agent, and at once when the log grew a
    /// page. Appended to @p actions.
    void syncTranscript(std::vector<UiAction>& actions);
    /// @return The transcript page the Focus asks for: the one turned to, else the last one the engine knows.
    [[nodiscard]] uint32_t wantedPage() const noexcept;
    /// @return When an agent answered with a quick reply is pointed at its tab if it still asks;
    ///         std::nullopt: none was answered.
    [[nodiscard]] std::optional<uint64_t> stillAsksDueMs() const noexcept;
    /// An agent still asking StillAsksAfterMs after its quick reply: the action bar says so.
    void remindStillAsking();
    /// A quick reply was not pressed: its chip empties, a message box it opened closes, the bar says why.
    void refuseReply(std::string notice);
    /// @return When the transcript the Focus follows is read again; std::nullopt: it is not.
    [[nodiscard]] std::optional<uint64_t> transcriptDueMs() const noexcept;
    /// @return The band the tab's size chooses.
    [[nodiscard]] Band band() const noexcept;
    /// @return How many runs need the user: what the top bar's pill counts.
    [[nodiscard]] std::size_t needYou() const noexcept;
    /// Notes the runs that entered Needs you since the rows were last built: the newest is named on the
    /// action bar. The first build only notes them: a run already waiting when the dashboard opens is not
    /// named.
    void noteNeeds();
    /// Notes what the user looks at, before a draw: the selected run and its shown tab are seen; every other
    /// run's row, and the selected run's other tabs, say whether they have news; a notice of a run that needs
    /// the user ends once it is selected or no longer needs them. Working rows draw the spinner's frame.
    void noteSeen();

    /// Whether the rows match the engine.
    enum class Rows : uint8_t
    {
        Current, ///< Built since the engine last changed.
        Stale,   ///< To be built again.
    };

    /// Whether the next frame draws.
    enum class Repaint : uint8_t
    {
        Clean,  ///< Nothing changed since the last frame.
        Needed, ///< Something did.
    };

    Engine const& engine;
    core::IClock const& clock;
    std::string shownBoard {};      ///< The run whose board the Focus asked the engine for; empty: none.
    std::string shownTranscript {}; ///< The run whose transcript the Focus asked for last; empty: none.
    uint64_t transcriptAskedMs = 0; ///< When it asked, in the engine's monotonic milliseconds.
    IUuidSource& ids;               ///< Where the session ids of the agents it dispatches come from.
    std::string tab;                ///< The tab it is served in.
    std::string frameBytes;         ///< What the surface was handed since the last frame.
    core::tui::HeadlessSurface surface;
    core::tui::Screen screen;
    core::tui::VtParser parser;
    Size size;
    Interaction interaction;
    std::string selectedKey;          ///< The identity (keyOf()) of the selected row, kept across rebuilds.
    std::string startedSession;       ///< The session of a run the launcher started, selected once it is
                                      ///< listed; empty: none.
    std::vector<Row> rows;            ///< Every row.
    std::vector<std::size_t> visible; ///< The indices of the rows the filter keeps.
    std::string visibleFilter;        ///< The filter @ref visible was made with.
    Rows rowsState = Rows::Stale;     ///< Whether @ref rows must be built again.
    uint64_t rowsBuiltAtMs = 0;       ///< When @ref rows was built, in the engine's monotonic milliseconds.
    std::size_t rowBuilds = 0;        ///< How often @ref rows was built.
    std::optional<uint64_t>
        rowChangeMs; ///< When a row changes next with time alone (nextRowChange()); std::nullopt: none does.
    std::optional<core::SteadyTimePoint> escapeHeldSince; ///< When a lone ESC that ended an input arrived.
    std::size_t scrollTop = 0;                            ///< The first visible row the Fleet shows.
    Component const* shownLayout = nullptr; ///< The layout @ref toggled and @ref problems belong to.
    std::vector<std::pair<PanelRole, Component*>> toggled; ///< Its panels shown by place.
    ProblemLines* problems = nullptr;                      ///< The current layout's problem lines.
    Repaint repaint = Repaint::Needed;
    HitMap hits;                                ///< What the last frame drew where, for the mouse.
    std::optional<LastClick> lastClick;         ///< The last press of the left button, for a double click.
    std::unique_ptr<LauncherView> launcherView; ///< Draws interaction.launcher.
    std::unique_ptr<PaletteView> paletteView;   ///< Draws interaction.palette.
    std::unique_ptr<TimelineView> timelineView; ///< Draws the last 24 hours while Overlay::Timeline.
    FocusFrame focus;                           ///< What the Focus draws this frame.
    FocusPanels focusPanels;                    ///< What the Focus's tabs learnt drawing themselves.
    core::tui::Spinner spinner; ///< Turns a frame per drawn frame (at most one per its interval): no timer.
    SeenLedger seen;            ///< What the user saw of each run.
    std::optional<std::vector<std::string>> needing; ///< The runs in Needs you as the rows were last built;
                                                     ///< std::nullopt before the first build.
    std::string newestNeed; ///< The run that entered Needs you last, named on the action bar until it is
                            ///< selected or leaves it; empty: none.
    std::unique_ptr<core::tui::Responsive> root;
};

// }}}
// {{{ the components

namespace
{

    /// The engine's problems, one line each, in the warning style.
    class ProblemLines final: public Component
    {
      public:
        explicit ProblemLines(Dashboard::State const& state):
            _state { state }, _count { state.engine.problems().size() }
        {
        }

        void render(Canvas& canvas) override
        {
            auto const& style = styleOf(canvas.theme(), Role::Warning);
            auto row = 0;
            for (auto const& line: _state.engine.problems() | std::views::take(std::max(0, canvas.height())))
                canvas.putString(row++, 1, cut(line, canvas.width() - 2), style);
        }

        [[nodiscard]] Size preferredSize() const override
        {
            return { .width = 0, .height = static_cast<int>(_state.engine.problems().size()) };
        }

        /// Lays the parent out again when the number of problems changed.
        void refresh()
        {
            if (std::exchange(_count, _state.engine.problems().size()) != _count)
                hintsChanged();
        }

      private:
        Dashboard::State const& _state;
        std::size_t _count;
    };

    /// The fleet gauge, `⬢ 3/4 ▰▰▰▱`, and the `● 2 need you` pill.
    struct Gauge
    {
        std::string fleet; ///< The gauge.
        std::string pill;  ///< The pill; empty while nothing needs the user.
    };

    [[nodiscard]] Gauge gaugeOf(Dashboard::State const& state)
    {
        auto const active = state.engine.activeRuns();
        auto const* const config = state.engine.config();
        auto gauge = Gauge {};
        if (config != nullptr)
            gauge.fleet = std::format(
                "⬢ {}/{} {}",
                active,
                config->maxAgents(),
                meterOf(BoardProgress { .done = active, .total = config->maxAgents(), .current = {} },
                        static_cast<int>(std::min<std::size_t>(config->maxAgents(), MeterCells))));
        else
            gauge.fleet = std::format("⬢ {}", active);
        if (auto const count = state.needYou(); count > 0)
            gauge.pill = std::format("● {} need you", count);
        return gauge;
    }

    /// Draws the gauge and the pill so that they end at @p end, on @p row.
    void drawGauge(Canvas& canvas, Gauge const& gauge, int row, int end)
    {
        auto const& theme = canvas.theme();
        auto x = end;
        if (!gauge.pill.empty())
        {
            x -= columnsOf(gauge.pill);
            canvas.putString(row, x, gauge.pill, styleOf(theme, Role::Warning));
            x -= 2;
        }
        x -= columnsOf(gauge.fleet);
        canvas.putString(row, x, gauge.fleet, styleOf(theme, Role::Working));
    }

    /// The top bar: the brand, the fleet gauge, the pill, and a setup warning while something is missing.
    class TopBar final: public Component
    {
      public:
        explicit TopBar(Dashboard::State const& state): _state { state } {}

        void render(Canvas& canvas) override
        {
            auto const& theme = canvas.theme();
            auto brand = styleOf(theme, Role::Accent);
            brand.bold = true;
            auto const x = 1 + canvas.putString(0, 1, "✦ Agents", brand);
            if (!_state.engine.missingPlugins().empty())
                canvas.putString(0,
                                 x + 2,
                                 cut("plugins missing · ^K install", (canvas.width() / 2) - x),
                                 styleOf(theme, Role::Warning));
            drawGauge(canvas, gaugeOf(_state), 0, canvas.width() - 1);
        }

        [[nodiscard]] Size preferredSize() const override { return { .width = 0, .height = 1 }; }

      private:
        Dashboard::State const& _state;
    };

    /// The rows of the Fleet, in a box titled with their count and the filter. In the Wide band a run's row
    /// takes two lines, its second what the agent is on; elsewhere one, but a Needs you run keeps its second
    /// line, so that its question shows without opening anything.
    class FleetPanel final: public Component
    {
      public:
        FleetPanel(Dashboard::State& state, Band band): _state { state }, _band { band } {}

        void render(Canvas& canvas) override
        {
            auto const& interaction = _state.interaction;
            auto const runs = static_cast<std::size_t>(
                std::ranges::count_if(_state.rows, [](Row const& row) { return !row.run.empty(); }));
            auto title = std::format("Agents {}", runs);
            if (interaction.filtering == Filtering::Typing || !interaction.filter.empty())
                title += std::format(
                    " · /{}{}", interaction.filter, interaction.filtering == Filtering::Typing ? "▏" : "");
            drawFrame(canvas, title, interaction.place == Place::Fleet ? Role::Focus : Role::Border);
            auto const hits = HitRecorder { _state.hits, { .x = screenBounds().x, .y = screenBounds().y } };
            hits.add(canvas.area(), Hit { .kind = HitKind::FleetPane });
            auto inside = insideFrame(canvas);
            auto const& rows = _state.visible;
            auto const height = std::max(0, inside.height());
            auto const selected = positionOfSelectable(_state.rows, rows, interaction.selected);
            auto heights =
                std::ranges::to<std::vector<int>>(rows | std::views::transform([&](std::size_t index) {
                                                      return heightOf(_state.rows[index]);
                                                  }));
            // `+ new agent` stands apart from the runs above it, where there are any.
            auto const apart = rows.size() > 1 && _state.rows[rows.back()].kind == RowKind::NewAgent;
            if (apart)
                ++heights.back();
            auto const linesBetween = [&](std::size_t from, std::size_t to) {
                auto lines = 0;
                for (auto const at: std::views::iota(from, to))
                    lines += heights[at];
                return lines;
            };
            auto& top = _state.scrollTop;
            top = std::min(top, rows.empty() ? 0 : rows.size() - 1);
            if (selected < rows.size())
            {
                top = std::min(top, selected);
                while (top < selected && linesBetween(top, selected + 1) > height)
                    ++top;
                // The heading right above the selection comes into view with it, where both fit.
                if (top == selected && top > 0 && _state.rows[rows[top - 1]].kind == RowKind::Heading
                    && linesBetween(top - 1, selected + 1) <= height)
                    --top;
            }
            // No blank space below the last row while rows above are hidden.
            while (top > 0 && linesBetween(top - 1, rows.size()) <= height)
                --top;
            auto y = 0;
            for (auto const at: std::views::iota(top, rows.size()))
            {
                if (y >= height)
                    break;
                auto const shift = apart && at + 1 == rows.size() ? 1 : 0;
                auto const& row = _state.rows[rows[at]];
                if (row.isSelectable())
                    hits.shifted(1, 1).add(
                        Rect { .x = 0, .y = y + shift, .width = inside.width(), .height = heightOf(row) },
                        Hit { .kind = HitKind::Row,
                              .value = selectableCount(_state.rows, std::span { rows }.first(at)) });
                drawRow(inside,
                        y + shift,
                        _state.rows[rows[at]],
                        at == selected ? Selection::Selected : Selection::Unselected);
                y += heights[at];
            }
        }

      private:
        /// @return Whether @p row draws its second line in this band.
        [[nodiscard]] bool hasSecondLine(Row const& row) const noexcept
        {
            if (row.context.empty() || row.kind == RowKind::Heading || row.kind == RowKind::Notice)
                return false;
            return _band == Band::Wide || row.attention == FleetGroup::NeedsYou;
        }

        /// @return The lines @p row takes, the blank above `+ new agent` aside.
        [[nodiscard]] int heightOf(Row const& row) const noexcept { return hasSecondLine(row) ? 2 : 1; }

        void drawRow(Canvas& canvas, int line, Row const& row, Selection selection) const
        {
            auto const& theme = canvas.theme();
            auto const width = canvas.width();
            auto const isSelected = selection == Selection::Selected;
            auto const pick = [&](Role role) {
                auto style = styleOf(theme, role);
                if (isSelected)
                    style.bg = styleOf(theme, Role::Selection).bg;
                return style;
            };
            switch (row.kind)
            {
                case RowKind::Notice:
                    canvas.putString(line, 1, cut(row.text, width - 2), styleOf(theme, Role::Text));
                    return;
                case RowKind::Heading: {
                    auto heading = pick(row.markRole);
                    heading.bold = true;
                    if (isSelected)
                        canvas.fill(
                            Rect { .x = 0, .y = line, .width = width, .height = 1 }, ' ', pick(Role::Text));
                    auto x = 1;
                    if (!row.mark.empty())
                        x += canvas.putString(line, x, row.mark, heading) + 1;
                    x += canvas.putString(line, x, cut(row.text, width - x - 1), heading) + 1;
                    canvas.putString(line, x, cut(row.status, width - x - 1), pick(Role::Dim));
                    return;
                }
                default: break;
            }
            auto const lines = hasSecondLine(row) ? 2 : 1;
            if (isSelected)
            {
                canvas.fill(
                    Rect { .x = 0, .y = line, .width = width, .height = lines }, ' ', pick(Role::Text));
                for (auto const at: std::views::iota(0, lines))
                    canvas.putString(line + at, 0, "▍", pick(Role::Accent));
            }
            canvas.putString(line, 1, cut(row.mark, 1), pick(row.markRole));
            auto column = 3;
            auto const end = width - 1;
            auto const status =
                cut(row.status, std::min(columnsOf(row.status), std::max(0, (end - column) / 2)));
            if (!row.prefix.empty())
                column += canvas.putString(
                              line, column, cut(row.prefix, std::max(0, (end - column) / 4)), pick(Role::Dim))
                          + 1;
            auto const textEnd = status.empty() ? end : end - columnsOf(status) - 1;
            // A run with news since the user last looked at it carries a `•` after its name.
            auto const newsColumns = row.news == News::Unseen ? columnsOf(UnseenMark) : 0;
            column += canvas.putString(
                line, column, cut(row.text, textEnd - column - newsColumns), pick(Role::Text));
            if (newsColumns != 0)
                column += canvas.putString(line, column, UnseenMark, pick(Role::Accent));
            // Folded to one line, the second line's words follow the name.
            if (lines == 1 && !row.context.empty() && textEnd - column > 4)
                canvas.putString(line,
                                 column + 1,
                                 cut(std::format("· {}", row.context), textEnd - column - 1),
                                 pick(row.contextRole));
            if (!status.empty())
                canvas.putString(line, end - columnsOf(status), status, pick(row.statusRole));
            if (lines == 1)
                return;
            auto contextEnd = end;
            if (row.progress)
            {
                auto const meter = meterOf(*row.progress, MeterCells);
                contextEnd -= MeterCells + 1;
                canvas.putString(line + 1, end - MeterCells, meter, pick(Role::Working));
            }
            canvas.putString(line + 1, 3, cut(row.context, contextEnd - 3), pick(row.contextRole));
        }

        Dashboard::State& _state;
        Band _band;
    };

    /// The action bar: a notice until the next key, the question an `e` asks, or the actions that apply to
    /// the selected row (barText()); and the gauge on its right while the top bar gave way.
    class ActionBar final: public Component
    {
      public:
        explicit ActionBar(Dashboard::State const& state): _state { state } {}

        void render(Canvas& canvas) override
        {
            // The dashboard shows no terminal cursor: the filter and the message box draw their own caret. A
            // frame's buffer keeps the cursor visible unless something hides it, and this line is in every
            // layout, so it is hidden here.
            canvas.hideCursor();
            auto const& theme = canvas.theme();
            auto end = canvas.width() - 1;
            if (holds(_state.focus.givenWay, Furniture::TopBar))
            {
                auto const gauge = gaugeOf(_state);
                drawGauge(canvas, gauge, 0, end);
                end -= columnsOf(gauge.fleet) + (gauge.pill.empty() ? 0 : columnsOf(gauge.pill) + 2) + 2;
            }
            // The run that entered Needs you last is named on the right, until it is selected.
            if (!_state.newestNeed.empty())
            {
                auto const need = std::format("{} needs you · home", nameOf(_state.newestNeed));
                if (auto const needColumns = columnsOf(need); needColumns < end / 2)
                {
                    canvas.putString(0, end - needColumns, need, styleOf(theme, Role::Warning));
                    end -= needColumns + 2;
                }
            }
            auto const columns = end - 1;
            auto const& interaction = _state.interaction;
            if (!interaction.notice.empty())
                canvas.putString(0, 1, cut(interaction.notice, columns), styleOf(theme, Role::Accent));
            else if (!interaction.pendingEnd.empty())
                canvas.putString(
                    0,
                    1,
                    cut(std::format("end {}? e again to end · esc keeps it", nameOf(interaction.pendingEnd)),
                        columns),
                    styleOf(theme, Role::Warning));
            else
            {
                auto const* const row = _state.selectedRow();
                auto const keys = interaction.place == Place::Focus && hasMessageBox(&_state.engine, row)
                                      ? MessageKeys::InBox
                                      : MessageKeys::Elsewhere;
                canvas.putString(
                    0, 1, barText(row, interaction.place, keys, columns), styleOf(theme, Role::Dim));
            }
        }

      private:
        /// @return The run's name, as its row says it.
        [[nodiscard]] std::string_view nameOf(std::string_view run) const noexcept
        {
            auto const row = std::ranges::find(_state.rows, run, &Row::run);
            return row != _state.rows.end() ? std::string_view { row->name } : run;
        }

        Dashboard::State const& _state;
    };

} // namespace

// }}}
// {{{ the layouts

Dashboard::State::State(Engine const& engine,
                        core::tui::Theme theme,
                        core::IClock const& clock,
                        Size size,
                        IUuidSource& ids,
                        std::string tab):
    engine { engine },
    clock { clock },
    ids { ids },
    tab { std::move(tab) },
    surface { size, [this](std::string_view bytes) { frameBytes.append(bytes); } },
    screen { surface, core::tui::ScreenConfig {}, clock },
    size { size },
    launcherView { std::make_unique<LauncherView>(engine, interaction.launcher, hits) },
    paletteView { std::make_unique<PaletteView>(interaction.palette, hits) },
    timelineView { std::make_unique<TimelineView>(engine, hits) },
    spinner { core::tui::SpinnerType::Dots, clock },
    root { std::make_unique<core::tui::Responsive>(std::ranges::to<std::vector<core::tui::Breakpoint>>(
        std::views::iota(std::size_t { 0 }, BandBreakpoints.size())
        | std::views::transform([this](std::size_t index) {
              return core::tui::Breakpoint { .minColumns = BandBreakpoints[index].minColumns,
                                             .minRows = 0,
                                             .make = [this, index] { return buildLayout(index); } };
          }))) }
{
    screen.setTheme(theme);
    screen.root().addChild(*root, { .flex = layout::Style { .grow = 1 } });
}

Dashboard::State::~State()
{
    for (Component* const overlay:
         std::array<Component*, 3> { launcherView.get(), paletteView.get(), timelineView.get() })
        if (screen.isOverlayVisible(*overlay))
            screen.hideOverlay(*overlay);
    screen.root().removeChild(*root);
}

std::unique_ptr<Component> Dashboard::State::buildLayout(std::size_t breakpoint)
{
    auto const band = BandBreakpoints[breakpoint].band;
    toggled.clear();
    // Shown or hidden once added: adding a child shows it.
    auto const toggle = [this, band](PanelRole role, Component& panel) {
        panel.setVisible(isShown(role, interaction.place, band, focus.givenWay, focusWidthOf(focus.row)));
        toggled.emplace_back(role, &panel);
    };

    auto column = std::make_unique<FlexBox>(layout::Style { .direction = layout::Direction::Column });
    toggle(PanelRole::TopBar, column->add(std::make_unique<TopBar>(*this), layout::Style { .shrink = 0 }));
    auto problemLines = std::make_unique<ProblemLines>(*this);
    problems = problemLines.get();
    column->add(std::move(problemLines), layout::Style { .shrink = 0 });
    auto body = std::make_unique<FlexBox>(
        layout::Style { .direction = band == Band::Wide ? layout::Direction::Row : layout::Direction::Column,
                        .gap = band == Band::Wide ? 1 : 0 });
    toggle(PanelRole::Fleet,
           body->add(std::make_unique<FleetPanel>(*this, band),
                     layout::Style { .grow = FleetShare, .basis = 0 }));
    toggle(PanelRole::Focus,
           body->add(std::make_unique<Focus>(engine, interaction, focus, tab, clock, focusPanels, hits),
                     layout::Style { .grow = FocusShare, .basis = 0 }));
    column->add(std::move(body), layout::Style { .grow = 1, .basis = 0 });
    column->add(std::make_unique<ActionBar>(*this), layout::Style { .shrink = 0 });
    shownLayout = column.get();
    return column;
}

// }}}
// {{{ the state's upkeep

uint64_t Dashboard::State::nowMonoMs() const noexcept
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(clock.now().time_since_epoch()).count());
}

void Dashboard::State::ensureRows()
{
    auto const now = nowMonoMs();
    if (rowsState == Rows::Current && now - rowsBuiltAtMs < AgeTickMs)
        return;
    rows = buildRows(engine, now);
    rowChangeMs = nextRowChange(engine, now);
    rowsState = Rows::Current;
    rowsBuiltAtMs = now;
    ++rowBuilds;
    visible.clear();
    refilter(Keep::Row);
    noteNeeds();
    // An answered agent that stopped asking is settled: its chips go with its asking box.
    if (auto const& answered = interaction.answered;
        answered && std::ranges::none_of(rows, [&](Row const& row) {
            return row.run == answered->run && row.rowState == RowState::Asking;
        }))
        interaction.answered.reset();
}

std::optional<uint64_t> Dashboard::State::stillAsksDueMs() const noexcept
{
    return interaction.answered.transform(
        [](AnsweredRequest const& answered) { return answered.atMs + StillAsksAfterMs; });
}

void Dashboard::State::remindStillAsking()
{
    if (auto const due = stillAsksDueMs(); !due || nowMonoMs() < *due)
        return;
    // Its chips are live again: the reply may not have taken, and the user may try another.
    auto const row = std::ranges::find(rows, interaction.answered->run, &Row::run);
    if (row != rows.end() && row->rowState == RowState::Asking)
        interaction.notice = std::format("{} still asks · r opens its tab", row->name);
    interaction.answered.reset();
}

void Dashboard::State::refilter(Keep keep)
{
    // A run the launcher started takes the selection once the engine lists it.
    if (!startedSession.empty())
        if (auto const run = std::ranges::find(engine.runs(), startedSession, &Run::session);
            run != engine.runs().end())
        {
            selectedKey = run->id;
            keep = Keep::Row;
            startedSession.clear();
        }
    if (visible.empty() || interaction.filter != visibleFilter)
    {
        visible = visibleRows(rows, interaction.filter);
        visibleFilter = interaction.filter;
    }
    auto const selectable = selectableCount(rows, visible);
    // The selection stays on its row across a rebuild: by its identity, not by its position.
    auto ordinal = std::size_t { 0 };
    auto found = std::optional<std::size_t> {};
    for (auto const index: visible)
    {
        if (!rows[index].isSelectable())
            continue;
        // A group just folded selects its heading, its runs gone; one just unfolded, its first run.
        if (!interaction.followGroup.empty() && rows[index].group == interaction.followGroup)
        {
            found = ordinal;
            break;
        }
        if (keep == Keep::Row && interaction.followGroup.empty() && !selectedKey.empty()
            && keyOf(rows[index]) == selectedKey)
            found = ordinal;
        ++ordinal;
    }
    interaction.followGroup.clear();
    if (found)
        interaction.selected = *found;
    interaction.selected = selectable == 0 ? 0 : std::min(interaction.selected, selectable - 1);
    auto const* const row = selectedRow();
    selectedKey = row != nullptr ? keyOf(*row) : std::string {};
}

Row const* Dashboard::State::selectedRow() const noexcept
{
    auto const position = positionOfSelectable(rows, visible, interaction.selected);
    return position < visible.size() ? &rows[visible[position]] : nullptr;
}

Run const* Dashboard::State::selectedRun() const noexcept
{
    auto const* const row = selectedRow();
    if (row == nullptr || row->run.empty())
        return nullptr;
    auto const runs = engine.runs();
    auto const found = std::ranges::find(runs, row->run, &Run::id);
    return found != runs.end() ? &*found : nullptr;
}

std::size_t Dashboard::State::needYou() const noexcept
{
    return static_cast<std::size_t>(std::ranges::count_if(
        engine.runs(), [](Run const& run) { return fleetGroupOf(run) == FleetGroup::NeedsYou; }));
}

void Dashboard::State::noteNeeds()
{
    auto now = needingRuns(rows);
    if (needing)
        if (auto const entries = needsYouEntries(*needing, now); !entries.empty())
            newestNeed = entries.back();
    needing = std::move(now);
}

void Dashboard::State::noteSeen()
{
    auto const* const selected = selectedRow();
    auto const selectedId = selected != nullptr ? std::string_view { selected->run } : std::string_view {};
    if (!newestNeed.empty()
        && (newestNeed == selectedId || !std::ranges::binary_search(*needing, newestNeed)))
        newestNeed.clear();
    focus.tabNews = {};
    auto const frame = std::string { spinner.currentFrame() };
    auto const runs = engine.runs();
    for (auto& row: rows)
    {
        if (row.rowState == RowState::Working)
            row.mark = frame;
        auto const run = std::ranges::find(runs, row.run, &Run::id);
        if (row.run.empty() || run == runs.end())
            continue;
        auto const marks = runMarksOf(*run);
        if (row.run != selectedId)
        {
            row.news = seen.newsOf(row.run, marks);
            continue;
        }
        auto const tabs = tabMarksOf(engine, tab, row.run);
        seen.look(row.run, marks, tabs, focus.tab);
        row.news = News::None;
        for (auto const shown: { FocusTab::Board, FocusTab::Transcript, FocusTab::Details })
            focus.tabNews[std::to_underlying(shown)] = seen.tabNewsOf(row.run, tabs, shown);
    }
}

Band Dashboard::State::band() const noexcept
{
    return BandBreakpoints[breakpointFor(size)].band;
}

KeyContext Dashboard::State::keyContext() const noexcept
{
    auto const* const row = selectedRow();
    auto const tab = activeTabOf(interaction, row);
    auto const [lines, shown] = focusPanels.extentOf(tab);
    auto const* const view = row != nullptr ? engine.transcript(this->tab, row->run) : nullptr;
    return KeyContext { .selectable = selectableCount(rows, visible),
                        .selected = row,
                        .engine = &engine,
                        .tab = tab,
                        .lines = lines,
                        .rows = shown,
                        .pages = view != nullptr && view->content ? view->content->pages : 0,
                        .nowMonoMs = nowMonoMs() };
}

std::vector<UiAction> Dashboard::State::applyKeys(std::vector<core::tui::InputEvent> const& events,
                                                  HooksWarning hooksAtInput)
{
    auto actions = std::vector<UiAction> {};
    auto const before = interaction;
    for (auto const& event: events)
    {
        // A launcher opened by a key of this input confirms nothing an earlier one of it, or of an earlier
        // input, did: the confirmation belongs to the launcher it was given in.
        if (!interaction.launcher)
            hooksAtInput = HooksWarning::None;
        if (auto const* const paste = std::get_if<core::tui::PasteEvent>(&event))
        {
            interaction.notice.clear();
            pasteInto(interaction, keyContext(), paste->text);
            continue;
        }
        if (auto const* const mouse = std::get_if<core::tui::MouseEvent>(&event))
        {
            applyMouse(*mouse, actions);
            continue;
        }
        auto const* const key = std::get_if<core::tui::KeyEvent>(&event);
        if (key == nullptr)
            continue;
        interaction.notice.clear();
        if (interaction.launcher)
        {
            if (auto dispatched = applyLauncherKey(*key, interaction.launcher, engine, hooksAtInput, tab))
            {
                // The run it starts is selected once it is listed: its session is the one the launcher
                // minted.
                startedSession = dispatched->session;
                actions.emplace_back(std::move(*dispatched));
            }
            continue;
        }
        if (auto action = applyKey(*key, interaction, keyContext()))
            actions.push_back(std::move(*action));
        // A launcher a key opened gets its agent's session id now.
        mintSession();
        refilter(Keep::Position);
    }
    if (interaction != before)
        repaint = Repaint::Needed;
    return actions;
}

// }}}
// {{{ the mouse

namespace
{
    using State = Dashboard::State;

    /// Which layer a hit is on: while a popup is open, only its own layer takes the mouse.
    enum class Layer : uint8_t
    {
        Dashboard = 0, ///< The panes beneath.
        Popup,         ///< A popup: the launcher, the palette or the timeline.
    };

    /// What a click on a hit of one kind does.
    using ClickHandler = void (*)(State& state,
                                  std::size_t value,
                                  Clicks clicks,
                                  std::vector<UiAction>& actions);
    /// What the wheel over a hit of one kind does; @p steps is negative upwards.
    using WheelHandler = void (*)(State& state, int steps, std::vector<UiAction>& actions);

    /// What the mouse does on one kind of hit.
    struct MouseRow
    {
        HitKind kind = HitKind::Popup;  ///< The kind.
        Layer layer = Layer::Dashboard; ///< Its layer.
        ClickHandler click = nullptr;   ///< A press of the left button; nullptr: it falls through.
        WheelHandler wheel = nullptr;   ///< The wheel; nullptr: it falls through.
    };

    /// One row per HitKind, indexed by its value.
    constexpr auto MouseRows = std::array {
        // A popup covers the dashboard: a click on it that hits nothing of it does nothing, and the wheel
        // moves its highlight.
        MouseRow { .kind = HitKind::Popup,
                   .layer = Layer::Popup,
                   .click = [](State&, std::size_t, Clicks, std::vector<UiAction>&) {},
                   .wheel =
                       [](State& state, int steps, std::vector<UiAction>& actions) {
                           state.press(steps < 0 ? core::tui::KeyCode::Up : core::tui::KeyCode::Down,
                                       actions);
                       } },
        MouseRow { .kind = HitKind::FleetPane,
                   .layer = Layer::Dashboard,
                   .click = nullptr,
                   .wheel =
                       [](State& state, int steps, std::vector<UiAction>& /*actions*/) {
                           auto const at = static_cast<std::ptrdiff_t>(state.interaction.selected) + steps;
                           selectRow(state.interaction,
                                     state.keyContext(),
                                     static_cast<std::size_t>(std::max<std::ptrdiff_t>(0, at)));
                       } },
        MouseRow { .kind = HitKind::FocusPane,
                   .layer = Layer::Dashboard,
                   .click = nullptr,
                   .wheel =
                       [](State& state, int steps, std::vector<UiAction>& /*actions*/) {
                           scrollFocus(state.interaction, state.keyContext(), steps * WheelLines);
                       } },
        // A click selects a row; a double click, or one on `+ new agent`, runs its primary action.
        MouseRow { .kind = HitKind::Row,
                   .layer = Layer::Dashboard,
                   .click =
                       [](State& state, std::size_t value, Clicks clicks, std::vector<UiAction>& actions) {
                           selectRow(state.interaction, state.keyContext(), value);
                           state.refilter(State::Keep::Position);
                           auto const* const row = state.selectedRow();
                           if (clicks == Clicks::Double || (row != nullptr && row->kind == RowKind::NewAgent))
                               state.run(KeyCommand::Enter, actions);
                       },
                   .wheel = nullptr },
        // A tab or a chip runs what its key would.
        MouseRow { .kind = HitKind::Command,
                   .layer = Layer::Dashboard,
                   .click =
                       [](State& state, std::size_t value, Clicks, std::vector<UiAction>& actions) {
                           state.run(static_cast<KeyCommand>(value), actions);
                       },
                   .wheel = nullptr },
        MouseRow { .kind = HitKind::MessageBox,
                   .layer = Layer::Dashboard,
                   .click =
                       [](State& state, std::size_t, Clicks, std::vector<UiAction>& actions) {
                           state.run(KeyCommand::Open, actions);
                       },
                   .wheel = nullptr },
        // A palette entry runs at once, as the palette is a menu.
        MouseRow { .kind = HitKind::PaletteEntry,
                   .layer = Layer::Popup,
                   .click =
                       [](State& state, std::size_t value, Clicks, std::vector<UiAction>& actions) {
                           state.interaction.palette->highlighted = value;
                           state.press(core::tui::KeyCode::Enter, actions);
                       },
                   .wheel = nullptr },
        // A launcher candidate is highlighted, its settings shown; a double click starts it.
        MouseRow { .kind = HitKind::LauncherCandidate,
                   .layer = Layer::Popup,
                   .click =
                       [](State& state, std::size_t value, Clicks clicks, std::vector<UiAction>& actions) {
                           highlightCandidate(*state.interaction.launcher, state.engine, value);
                           if (clicks == Clicks::Double)
                               state.press(core::tui::KeyCode::Enter, actions);
                       },
                   .wheel = nullptr },
    };

    static_assert(std::ranges::all_of(std::views::iota(std::size_t { 0 }, MouseRows.size()),
                                      [](std::size_t index) {
                                          return std::to_underlying(MouseRows.at(index).kind) == index;
                                      }),
                  "MouseRows is indexed by the HitKind's value");

    /// @return What the mouse does on @p hit.
    [[nodiscard]] MouseRow const& mouseRowOf(Hit const& hit) noexcept
    {
        return MouseRows.at(std::to_underlying(hit.kind));
    }

    /// The most cells of a link the action bar names when it opens.
    constexpr auto LinkLabelColumns = 48;
} // namespace

bool Dashboard::State::popupOpen() const noexcept
{
    return interaction.launcher || interaction.palette || interaction.overlay != Overlay::None;
}

void Dashboard::State::press(core::tui::KeyCode key, std::vector<UiAction>& actions)
{
    std::ranges::move(applyKeys({ core::tui::KeyEvent { .key = key } }, launcherHooks()),
                      std::back_inserter(actions));
}

void Dashboard::State::run(KeyCommand command, std::vector<UiAction>& actions)
{
    if (auto action = dispatch(command, interaction, keyContext()))
        actions.push_back(std::move(*action));
    mintSession();
    refilter(Keep::Position);
}

void Dashboard::State::applyMouse(core::tui::MouseEvent const& mouse, std::vector<UiAction>& actions)
{
    using Type = core::tui::MouseEvent::Type;
    auto const x = mouse.x - 1;
    auto const y = mouse.y - 1;
    auto const under = hits.at(x, y);
    auto const takes = [&](Hit const& hit) {
        return !popupOpen() || mouseRowOf(hit).layer == Layer::Popup;
    };
    if (mouse.type == Type::ScrollUp || mouse.type == Type::ScrollDown)
    {
        // The pane under the pointer scrolls: the topmost hit that the wheel moves.
        auto const pane =
            std::ranges::find_if(under, [](Hit const& hit) { return mouseRowOf(hit).wheel != nullptr; });
        if (pane != under.end() && takes(*pane))
            mouseRowOf(*pane).wheel(*this, mouse.type == Type::ScrollUp ? -1 : 1, actions);
        return;
    }
    if (mouse.type != Type::Press || mouse.button != 0)
        return;
    auto const now = nowMonoMs();
    auto const clicks =
        lastClick && lastClick->x == x && lastClick->y == y && now - lastClick->atMs <= DoubleClickMs
            ? Clicks::Double
            : Clicks::Single;
    lastClick =
        clicks == Clicks::Double ? std::nullopt : std::optional { LastClick { .x = x, .y = y, .atMs = now } };
    interaction.notice.clear();
    auto const target =
        std::ranges::find_if(under, [](Hit const& hit) { return mouseRowOf(hit).click != nullptr; });
    // A click beside an open popup closes it, as esc does.
    if (popupOpen() && (under.empty() || !takes(under.front())))
    {
        press(core::tui::KeyCode::Escape, actions);
        return;
    }
    // A link opens on the machine the user sits at.
    if (!popupOpen())
        if (auto const uri = core::tui::HyperlinkIndex { screen.renderedBuffer() }.uriAt(y, x); !uri.empty())
        {
            actions.emplace_back(
                OpenUrlAction { .url = std::string { uri }, .label = cut(uri, LinkLabelColumns) });
            return;
        }
    if (target != under.end())
        mouseRowOf(*target).click(*this, target->value, clicks, actions);
}

// }}}
// {{{ the engine's reads

void Dashboard::State::syncBoard(std::vector<UiAction>& actions)
{
    auto const* const row = selectedRow();
    auto wanted = row != nullptr && std::ranges::contains(row->offers, KeyCommand::BoardTab) ? row->run
                                                                                             : std::string {};
    if (wanted == shownBoard)
        return;
    if (!wanted.empty())
        actions.emplace_back(ShowBoardAction { .run = wanted });
    else
        actions.emplace_back(HideBoardAction {});
    shownBoard = std::move(wanted);
}

uint32_t Dashboard::State::wantedPage() const noexcept
{
    if (interaction.page != 0)
        return interaction.page;
    auto const* const row = selectedRow();
    auto const* const view = row != nullptr ? engine.transcript(tab, row->run) : nullptr;
    return view != nullptr && view->content ? std::max(view->content->pages, uint32_t { 1 }) : 1;
}

std::optional<uint64_t> Dashboard::State::transcriptDueMs() const noexcept
{
    auto const* const run = selectedRun();
    if (run == nullptr || !countsAsActive(*run) || interaction.follow != Follow::Live
        || shownTranscript != run->id)
        return std::nullopt;
    return transcriptAskedMs + TranscriptFollowMs;
}

void Dashboard::State::syncTranscript(std::vector<UiAction>& actions)
{
    auto const* const run = selectedRun();
    // The engine reads no transcript while its configuration is refused.
    if (run == nullptr || engine.config() == nullptr)
        return;
    auto const page = wantedPage();
    auto const* const view = engine.transcript(tab, run->id);
    auto const now = nowMonoMs();
    auto const due = transcriptDueMs();
    // Asked for anew when the Focus shows another run, when the engine dropped the page (from its cache) or
    // keeps another, and while it follows a live agent, every TranscriptFollowMs; never while a read is in
    // flight, which answers first.
    auto const stale = shownTranscript != run->id || view == nullptr || view->page != page;
    auto const followed = !stale && due && now >= *due && view->state != FetchState::Loading;
    if (!stale && !followed)
        return;
    actions.emplace_back(ReadTranscriptAction { .run = run->id, .page = page });
    shownTranscript = run->id;
    transcriptAskedMs = now;
}

std::vector<UiAction> Dashboard::State::resolveOverdueEscape()
{
    if (!escapeHeldSince || clock.now() - *escapeHeldSince < EscapeTimeout)
        return {};
    escapeHeldSince.reset();
    ensureRows();
    return applyKeys(parser.timeout(), launcherHooks());
}

HooksWarning Dashboard::State::launcherHooks() const noexcept
{
    return interaction.launcher ? interaction.launcher->hooks : HooksWarning::None;
}

void Dashboard::State::syncLayout()
{
    // The panels are known only for the layout shown: one Responsive destroyed is forgotten.
    if (shownLayout != root->current())
    {
        shownLayout = nullptr;
        toggled.clear();
        problems = nullptr;
    }
    auto const* const row = selectedRow();
    auto const currentBand = band();
    focus.row = row;
    focus.tab = activeTabOf(interaction, row);
    focus.place = interaction.place;
    focus.box = bottomBoxOf(engine, row, interaction.place);
    auto const hasRuns = std::ranges::any_of(rows, [](Row const& candidate) {
        return candidate.isSelectable() && candidate.kind != RowKind::NewAgent;
    });
    focus.content = FleetContent::Unconfigured;
    if (engine.config() != nullptr)
        focus.content = hasRuns ? FleetContent::Runs : FleetContent::Empty;
    auto const* const view = row != nullptr ? engine.transcript(tab, row->run) : nullptr;
    focus.subject = FocusSubject { .run = row != nullptr ? row->run : std::string {},
                                   .page = view != nullptr ? view->page : wantedPage(),
                                   .scroll = interaction.scroll,
                                   .follow = interaction.follow };
    // The top bar gives way, in its turn, when the Focus would keep too few rows for its tab.
    focus.givenWay = Furniture::None;
    if (isShown(PanelRole::Focus, interaction.place, currentBand, Furniture::None, FocusWidth::Shared)
        && row != nullptr && (!row->run.empty() || row->kind == RowKind::Orphan))
    {
        auto const shared = currentBand == Band::Medium && interaction.place == Place::Fleet;
        auto const focusOf = [&](int body) {
            return shared ? body * FocusShare / (FleetShare + FocusShare) : body;
        };
        auto const body = size.height - static_cast<int>(engine.problems().size()) - 2;
        auto const draft = interaction.drafts.find(row->run);
        auto const given = furnitureGivingWay(FocusRows {
            .height = focusOf(body),
            .topBarRows = focusOf(body + 1) - focusOf(body),
            .bottomRows =
                bottomRowsOf(focus.box,
                             row,
                             draft != interaction.drafts.end() ? std::string_view { draft->second } : ""sv,
                             size.width,
                             focusOf(body)),
            .tab = focus.tab });
        if (holds(given, Furniture::TopBar))
            focus.givenWay = Furniture::TopBar;
    }
    for (auto const& [role, panel]: toggled)
        if (auto const shown =
                isShown(role, interaction.place, currentBand, focus.givenWay, focusWidthOf(row));
            panel->visible() != shown)
            panel->setVisible(shown);
    if (problems != nullptr)
        problems->refresh();
    syncLauncher();
    syncPalette();
    if (interaction.overlay == Overlay::Timeline)
    {
        auto const wanted = timelineView->preferredSize();
        showCentred(*timelineView,
                    Size { .width = std::min(wanted.width, size.width),
                           .height = std::min(wanted.height, size.height) });
    }
    else if (screen.isOverlayVisible(*timelineView))
        screen.hideOverlay(*timelineView);
}

void Dashboard::State::showCentred(Component& overlay, Size wanted)
{
    auto const at =
        core::tui::Point { .x = (size.width - wanted.width) / 2, .y = (size.height - wanted.height) / 2 };
    if (screen.isOverlayVisible(overlay))
        screen.positionOverlay(overlay, at);
    else
        screen.showOverlay(overlay, at);
}

void Dashboard::State::syncPalette()
{
    if (!interaction.palette)
    {
        if (screen.isOverlayVisible(*paletteView))
            screen.hideOverlay(*paletteView);
        return;
    }
    auto entries = paletteEntries(selectedRow(), &engine);
    auto const wanted = Size { .width = popupWidthIn(band(), size.width, PaletteMaxColumns),
                               .height = std::min(paletteView->heightFor(entries), size.height) };
    paletteView->place(wanted, std::move(entries));
    showCentred(*paletteView, wanted);
}

void Dashboard::State::clampScroll() noexcept
{
    // The keys scroll past an end not yet wrapped; the draw that wrapped it knows where it is.
    auto const [lines, shown] = focusPanels.extentOf(focus.tab);
    if (interaction.follow == Follow::Away || focus.tab != FocusTab::Transcript)
        interaction.scroll = std::min(interaction.scroll, lines > shown ? lines - shown : 0);
}

void Dashboard::State::mintSession()
{
    if (!interaction.launcher || !interaction.launcher->session.empty())
        return;
    interaction.launcher->session = formatUuidV4(ids.next());
    if (interaction.launcher->form)
        interaction.launcher->form->session = interaction.launcher->session;
}

void Dashboard::State::syncLauncher()
{
    if (!interaction.launcher)
    {
        if (screen.isOverlayVisible(*launcherView))
            screen.hideOverlay(*launcherView);
        return;
    }
    // Centred; the Narrow band gives it the whole tab.
    auto const band = this->band();
    auto const width = popupWidthIn(band, size.width, LauncherMaxColumns);
    auto const height =
        band == Band::Narrow ? size.height : std::min(launcherView->heightAt(width), size.height);
    auto const wanted = Size { .width = width, .height = height };
    launcherView->place(wanted);
    showCentred(*launcherView, wanted);
}

// }}}
// {{{ the dashboard

Dashboard::Dashboard(Engine const& engine,
                     core::tui::Theme theme,
                     core::IClock const& clock,
                     core::tui::Size size,
                     IUuidSource& ids,
                     std::string tab):
    _state { std::make_unique<State>(engine, theme, clock, size, ids, std::move(tab)) }
{
}

Dashboard::~Dashboard() = default;

std::vector<UiAction> Dashboard::input(std::string_view bytes)
{
    auto& state = *_state;
    // An ESC that ended the last input and waited past the timeout was the Escape key; a younger one is
    // continued by these bytes, as the rest of a split sequence or the Alt of a key.
    auto actions = state.resolveOverdueEscape();
    state.escapeHeldSince.reset();
    state.ensureRows();
    // A form's warning confirmed by a key of this input is not confirmed for the rest of it.
    std::ranges::move(state.applyKeys(state.parser.feed(bytes), state.launcherHooks()),
                      std::back_inserter(actions));
    if (bytes.ends_with('\x1b'))
        state.escapeHeldSince = state.clock.now();
    return actions;
}

std::optional<core::SteadyTimePoint> Dashboard::pendingEscapeDeadline() const noexcept
{
    auto const& held = _state->escapeHeldSince;
    return held ? std::optional { *held + EscapeTimeout } : std::nullopt;
}

std::optional<core::SteadyTimePoint> Dashboard::nextRepaintDue() const noexcept
{
    auto const& state = *_state;
    // An agent's age moves on in its row; a board's in the Board tab; a followed transcript is read again.
    auto earliest = state.rowChangeMs;
    auto const consider = [&](std::optional<uint64_t> due) {
        if (due)
            earliest = earliest ? std::min(*earliest, *due) : *due;
    };
    if (state.focus.tab == FocusTab::Board)
        consider(state.focusPanels.board.ageChangeMs);
    consider(state.transcriptDueMs());
    consider(state.stillAsksDueMs());
    return earliest ? std::optional { core::SteadyTimePoint { std::chrono::milliseconds { *earliest } } }
                    : std::nullopt;
}

std::size_t Dashboard::rowBuilds() const noexcept
{
    return _state->rowBuilds;
}

void Dashboard::resize(core::tui::Size size)
{
    auto& state = *_state;
    if (size.width == state.size.width && size.height == state.size.height)
        return;
    state.size = size;
    state.surface.resize(size);
    state.repaint = State::Repaint::Needed;
}

void Dashboard::engineChanged()
{
    _state->rowsState = State::Rows::Stale;
    _state->repaint = State::Repaint::Needed;
}

std::vector<UiAction> Dashboard::select(std::string_view run)
{
    auto& state = *_state;
    state.ensureRows();
    auto const selectableIndexOf = [&]() -> std::optional<std::size_t> {
        auto seen = std::size_t { 0 };
        for (auto const index: state.visible)
        {
            if (!state.rows[index].isSelectable())
                continue;
            if (state.rows[index].run == run)
                return seen;
            ++seen;
        }
        return std::nullopt;
    };
    auto found = selectableIndexOf();
    if (!found && !state.interaction.filter.empty())
    {
        state.interaction.filter.clear();
        state.interaction.filtering = Filtering::Off;
        state.refilter(State::Keep::Position);
        found = selectableIndexOf();
    }
    if (!found)
        return {};
    if (state.interaction.selected != *found)
    {
        state.interaction.scroll = 0;
        state.interaction.follow = Follow::Live;
        state.interaction.page = 0;
    }
    state.interaction.selected = *found;
    state.selectedKey = std::string { run };
    // An end that waited for its confirmation was for the row selected before.
    state.interaction.pendingEnd.clear();
    backToFleet(state.interaction);
    state.repaint = State::Repaint::Needed;
    return {};
}

void Dashboard::openLauncherOn(std::string_view repo, ItemRef const& item, std::string_view task)
{
    auto& state = *_state;
    openLauncherFor(state.interaction, state.engine, repo, item, task, {});
    state.mintSession();
    state.repaint = State::Repaint::Needed;
}

std::optional<LauncherState> const& Dashboard::launcher() const noexcept
{
    return _state->interaction.launcher;
}

void Dashboard::messageRefused(std::string_view why)
{
    auto& interaction = _state->interaction;
    // The draft comes back, and the keys to the box, so that nothing written is lost.
    if (auto sent = std::exchange(interaction.sent, std::nullopt))
    {
        auto& draft = interaction.drafts[sent->run];
        draft = draft.empty() ? std::move(sent->text) : std::format("{}\n{}", sent->text, draft);
        if (_state->selectedKey == sent->run)
            interaction.place = Place::Focus;
    }
    interaction.notice = std::format("message not sent: {}", why);
    _state->repaint = State::Repaint::Needed;
}

void Dashboard::State::refuseReply(std::string notice)
{
    if (auto const answered = std::exchange(interaction.answered, std::nullopt);
        answered && selectedKey == answered->run)
        backToFleet(interaction);
    interaction.notice = std::move(notice);
    repaint = Repaint::Needed;
}

void Dashboard::replyRefused(std::string_view why)
{
    _state->refuseReply(std::format("reply not sent: {}", why));
}

void Dashboard::replyOutdated(std::string_view run)
{
    auto const& rows = _state->rows;
    auto const row = std::ranges::find(rows, run, &Row::run);
    _state->refuseReply(
        std::format("{} is no longer asking", row != rows.end() ? std::string_view { row->name } : run));
}

void Dashboard::urlOpened(std::string_view label)
{
    _state->interaction.notice = std::format("opening {} on your machine", label);
    _state->repaint = State::Repaint::Needed;
}

void Dashboard::urlRefused(std::string_view why)
{
    _state->interaction.notice = std::format("not opened: {}", why);
    _state->repaint = State::Repaint::Needed;
}

void Dashboard::dispatchRefused(DispatchRefusal refusal)
{
    _state->interaction.notice = std::format("dispatch refused: {}", refusalText(refusal));
    _state->repaint = State::Repaint::Needed;
}

std::string_view Dashboard::selectedRun() const noexcept
{
    auto const* const row = _state->selectedRow();
    return row != nullptr ? std::string_view { row->run } : std::string_view {};
}

std::vector<UiAction> Dashboard::dueActions()
{
    auto& state = *_state;
    auto actions = state.resolveOverdueEscape();
    // The rows as they stand say which run the Focus shows; building them here would take a row change that
    // fell due from the frame that draws it.
    state.syncBoard(actions);
    state.syncTranscript(actions);
    return actions;
}

std::string Dashboard::frame()
{
    auto& state = *_state;
    // The guest took what an overdue Escape key asks with dueActions(); one resolved here only closes.
    static_cast<void>(state.resolveOverdueEscape());
    // A row changed with time alone (an age moved on, a trust-prompt hint fell due): the rows are built and
    // drawn again, though the engine did not change.
    if (state.rowChangeMs && state.nowMonoMs() >= *state.rowChangeMs)
    {
        state.rowsState = State::Rows::Stale;
        state.repaint = State::Repaint::Needed;
    }
    // An answered agent may still ask: the frame says so.
    if (auto const due = state.stillAsksDueMs(); due && state.nowMonoMs() >= *due)
        state.repaint = State::Repaint::Needed;
    // A board's age moved on.
    if (auto const due = state.focusPanels.board.ageChangeMs;
        state.focus.tab == FocusTab::Board && due && state.nowMonoMs() >= *due)
        state.repaint = State::Repaint::Needed;
    if (state.repaint == State::Repaint::Clean)
        return {};
    state.ensureRows();
    state.remindStillAsking();
    state.refilter(State::Keep::Row);
    state.syncLayout();
    // The spinner turns on the redraws that happen anyway: it asks for no wake of its own.
    static_cast<void>(state.spinner.tick());
    state.noteSeen();
    // Each component records what it covers as it draws.
    state.hits.clear();
    state.screen.draw();
    state.clampScroll();
    // The cursor rests at the top-left, hidden: every frame addresses its cells absolutely, and a shorter tab
    // then cuts its bottom rows rather than pushing its top ones into a history -- in the terminal and in
    // every window that mirrors it.
    // Bracketed paste on, in every frame, so that a tab the host drew in the meantime gets it back: a paste
    // arrives as text, never as the keys it spells (an Enter in it would confirm the form).
    auto bytes = std::exchange(state.frameBytes, {});
    bytes += BracketedPasteOn;
    bytes += MouseReportingOn;
    bytes += RestingCursor;
    state.repaint = State::Repaint::Clean;
    return bytes;
}

core::tui::Buffer const& Dashboard::rendered() const noexcept
{
    return _state->screen.renderedBuffer();
}

HitMap const& Dashboard::hits() const noexcept
{
    return _state->hits;
}

std::string_view Dashboard::tab() const noexcept
{
    return _state->tab;
}

IUuidSource const& Dashboard::ids() const noexcept
{
    return _state->ids;
}

Interaction const& Dashboard::interaction() const noexcept
{
    return _state->interaction;
}

std::optional<std::size_t> Dashboard::breakpoint() const noexcept
{
    return _state->root->breakpoint();
}

// }}}

} // namespace orchestrator::view
