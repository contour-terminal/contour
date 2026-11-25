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
#include <orchestrator/view/RowMenu.hpp>
#include <orchestrator/view/Sections.hpp>
#include <orchestrator/view/StartDialog.hpp>
#include <orchestrator/view/StatusBoard.hpp>
#include <orchestrator/view/TaskBox.hpp>
#include <orchestrator/view/Theme.hpp>
#include <orchestrator/view/Transcript.hpp>
#include <orchestrator/view/WorkBoard.hpp>

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
    /// How often the transcript of a live run the Focus shows is read again.
    constexpr auto TranscriptFollowMs = uint64_t { 5000 };

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
        Work,   ///< The Work board, which replaces the Fleet and the Focus while it is shown.
    };

    /// How wide the Focus is drawn while it has the keys.
    enum class FocusWidth : uint8_t
    {
        Shared = 0, ///< Beside the Fleet, in the Wide band.
        Whole,      ///< The whole tab: a worktree's details hold a command line, which wants every cell.
    };

    /// Where the user is, as far as which panels are shown goes.
    struct Showing
    {
        View view = View::Agents;              ///< The view shown.
        Place place = Place::Fleet;            ///< Which pane of the Agents view has the keys.
        Band band = Band::Wide;                ///< The tab's band.
        Furniture given = Furniture::None;     ///< What gave way to the Focus.
        FocusWidth width = FocusWidth::Shared; ///< How wide the Focus is drawn while it has the keys.
    };

    /// @return Whether a panel of @p role is shown as @p showing says.
    [[nodiscard]] bool isShown(PanelRole role, Showing const& showing) noexcept
    {
        auto const agents = showing.view == View::Agents;
        switch (role)
        {
            case PanelRole::TopBar: return !holds(showing.given, Furniture::TopBar);
            case PanelRole::Work: return !agents;
            case PanelRole::Fleet:
                return agents
                       && (showing.place == Place::Fleet
                           || (showing.band == Band::Wide && showing.width == FocusWidth::Shared));
            case PanelRole::Focus: break;
        }
        return agents && (showing.band != Band::Narrow || showing.place == Place::Focus);
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
    /// Rebuilds the rows of the view shown from the engine when they are stale: the Fleet's when the engine
    /// changed or an age moved on, the Work view's items when the engine's revision moved.
    void ensureRows();
    /// Builds the Work view's lines again from its items and the interaction, and keeps its selection.
    void syncWork();
    /// @return Where the user is, as far as which panels are shown goes.
    [[nodiscard]] Showing showing(Furniture given, FocusWidth width) const noexcept;
    /// How refilter() keeps the selection.
    enum class Keep : uint8_t
    {
        Row, ///< On the row it was on, by its identity: across a rebuild, so that a run that changes groups
             ///< keeps it and a run that enters Needs you never takes it.
        Position, ///< At the position a key moved it to.
    };

    /// Applies the filter to the rows, and keeps the selection as @p keep says.
    void refilter(Keep keep);
    /// Applies keys the parser decoded: to the start dialog or the `n` box while one is open, else by the key
    /// table.
    /// @param events The keys.
    /// @param hooksAtInput The form's HooksWarning as the input that holds them began.
    /// @return What the engine is asked, in order.
    [[nodiscard]] std::vector<UiAction> applyKeys(std::vector<core::tui::InputEvent> const& events,
                                                  HooksWarning hooksAtInput);
    /// Applies a mouse event to what the last frame drew under the pointer (MouseRows): a press of the left
    /// button clicks, one of the right button right-clicks, the wheel scrolls, a move hovers or drags; a
    /// popup takes them alone, and a click beside it closes it.
    /// @param mouse The event.
    /// @param actions What the engine is asked, appended to.
    void applyMouse(core::tui::MouseEvent const& mouse, std::vector<UiAction>& actions);
    /// Applies a press of the right button: it steps a dialog's setting back, opens a Work row's menu, folds
    /// the other groups from a heading, steps a chip back; beside an open popup it closes it.
    /// @param under The regions under the pointer, the topmost first.
    /// @param at The cell pressed, 0-based.
    /// @param actions What the engine is asked, appended to.
    void rightClick(std::span<HitRegion const> under, core::tui::Point at, std::vector<UiAction>& actions);
    /// Applies a move of the pointer: with no button held over the Work view, the item line under it is
    /// hovered; with the left button held after a press on the scrollbar, the list follows it.
    /// @param mouse The event.
    /// @param under The regions under the pointer, the topmost first.
    void move(core::tui::MouseEvent const& mouse, std::span<HitRegion const> under);
    /// Runs a row menu command on the item the menu was about.
    /// @param command The command.
    /// @param item The item.
    /// @param actions What the engine is asked, appended to.
    void runMenuCommand(RowMenuCommand command, WorkItem const& item, std::vector<UiAction>& actions);
    /// Places a row menu a key or a hint opened under the selected row, as the last frame drew it.
    /// @param before The menu before the command.
    void anchorMenu(std::optional<RowMenuState> const& before);
    /// Applies one key as if typed, as a click that stands for it does.
    /// @param key The key.
    /// @param actions What the engine is asked, appended to.
    void press(core::tui::KeyCode key, std::vector<UiAction>& actions);
    /// Runs a command as its key would, as a click on a tab or a chip does.
    /// @param command The command.
    /// @param actions What the engine is asked, appended to.
    void run(KeyCommand command, std::vector<UiAction>& actions);
    /// What follows every key and click: a start dialog or an `n` box opened gets its session id, a view
    /// switched to builds its rows, and the selection is kept on the rows of the view shown.
    /// @param before The view shown before the command.
    void afterCommand(View before);
    /// @return Whether a popup is open: the start dialog, the `n` box, the palette, the timeline or the row
    ///         menu.
    [[nodiscard]] bool popupOpen() const noexcept;
    /// @return The HooksWarning of the start dialog or the `n` box now: what the next input begins with.
    [[nodiscard]] HooksWarning dialogHooks() const noexcept;
    /// Resolves a held ESC that waited past EscapeTimeout into the Escape key.
    /// @return What the engine is asked, in order.
    [[nodiscard]] std::vector<UiAction> resolveOverdueEscape();
    /// Shows the panes of the current place and band and hides the others; sets what the Focus draws; places
    /// the start dialog, the `n` box, the palette and the timeline.
    void syncLayout();
    /// Gives a start dialog or an `n` box just opened its agent's session id, from the dashboard's source.
    void mintSession();
    /// Shows the start dialog centred, or hides it.
    void syncStartDialog();
    /// Shows the `n` box centred, or hides it.
    void syncTaskBox();
    /// Shows the row menu where it was opened, moved into the tab, or hides it.
    void syncRowMenu();
    /// Shows the palette where the band puts it, listing what applies to the selected row; or hides it.
    void syncPalette();
    /// Shows @p overlay centred in the tab at @p wanted, or moves it there.
    void showCentred(Component& overlay, Size wanted);
    /// Shows @p overlay with its top-left at @p at, or moves it there.
    void showAt(Component& overlay, core::tui::Point at);
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
    std::string startedSession;       ///< The session of a run the start dialog or the `n` box started,
                                      ///< selected once it is listed; empty: none.
    std::vector<Row> rows;            ///< Every row.
    std::vector<std::size_t> visible; ///< The indices of the rows the filter keeps.
    std::string visibleFilter;        ///< The filter @ref visible was made with.
    Rows rowsState = Rows::Stale;     ///< Whether @ref rows must be built again.
    std::size_t rowBuilds = 0;        ///< How often @ref rows was built.
    std::optional<uint64_t>
        rowChangeMs; ///< When a row changes next with time alone (nextRowChange()); std::nullopt: none does.
    std::optional<core::SteadyTimePoint> escapeHeldSince; ///< When a lone ESC that ended an input arrived.
    std::size_t scrollTop = 0;                            ///< The first visible row the Fleet shows.
    Component const* shownLayout = nullptr; ///< The layout @ref toggled and @ref problems belong to.
    std::vector<std::pair<PanelRole, Component*>> toggled; ///< Its panels shown by place.
    ProblemLines* problems = nullptr;                      ///< The current layout's problem lines.
    Repaint repaint = Repaint::Needed;
    HitMap hits;                        ///< What the last frame drew where, for the mouse.
    std::optional<LastClick> lastClick; ///< The last press of the left button, for a double click.
    /// The Work view's scrollbar while the left button, pressed on it, is held: the list follows the pointer.
    std::optional<Rect> scrollbarDrag;
    std::unique_ptr<StartDialogView> startDialogView; ///< Draws interaction.startDialog.
    std::unique_ptr<TaskBoxView> taskBoxView;         ///< Draws interaction.taskBox.
    std::unique_ptr<PaletteView> paletteView;         ///< Draws interaction.palette.
    std::unique_ptr<TimelineView> timelineView;       ///< Draws the last 24 hours while Overlay::Timeline.
    std::unique_ptr<RowMenuView> rowMenuView;         ///< Draws interaction.work.menu.
    /// Where each shown overlay was last put: moving one repaints the whole tab, so an overlay is moved only
    /// when it is put elsewhere, not on every frame it stays.
    std::vector<std::pair<Component const*, core::tui::Point>> overlayAt;
    FocusFrame focus;           ///< What the Focus draws this frame.
    FocusPanels focusPanels;    ///< What the Focus's tabs learnt drawing themselves.
    core::tui::Spinner spinner; ///< Turns a frame per drawn frame (at most one per its interval): no timer.
    SeenLedger seen;            ///< What the user saw of each run.
    std::optional<std::vector<std::string>> needing; ///< The runs in Needs you as the rows were last built;
                                                     ///< std::nullopt before the first build.
    std::string newestNeed; ///< The run that entered Needs you last, named on the action bar until it is
                            ///< selected or leaves it; empty: none.
    WorkBoardData work;     ///< What the Work view draws from.
    View framedView = View::Agents; ///< The view the last frame showed.
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

    /// A view the top bar names.
    struct ViewName
    {
        View view = View::Agents;   ///< The view.
        std::string_view name = {}; ///< What the top bar calls it.
    };

    /// The views, in the order the top bar names them.
    constexpr auto ViewNames = std::array {
        ViewName { .view = View::Agents, .name = "Agents"sv },
        ViewName { .view = View::Work, .name = "Work"sv },
    };

    /// The top bar: the brand and the views' names (the shown one in the focus colour, a click on the other
    /// switches to it), the fleet gauge, the pill, and a setup warning while something is missing.
    class TopBar final: public Component
    {
      public:
        explicit TopBar(Dashboard::State& state): _state { state } {}

        void render(Canvas& canvas) override
        {
            auto const& theme = canvas.theme();
            auto brand = styleOf(theme, Role::Accent);
            brand.bold = true;
            auto x = 1 + canvas.putString(0, 1, "✦", brand) + 1;
            auto const hits = HitRecorder { _state.hits, { .x = screenBounds().x, .y = screenBounds().y } };
            for (auto const& [index, shown]: std::views::enumerate(ViewNames))
            {
                if (index > 0)
                    x += 2;
                auto const isShown = shown.view == _state.interaction.view;
                auto style = styleOf(theme, isShown ? Role::Focus : Role::Dim);
                style.bold = isShown;
                auto const columns = canvas.putString(0, x, shown.name, style);
                hits.add(Rect { .x = x, .y = 0, .width = columns, .height = 1 },
                         Hit { .kind = HitKind::ViewName, .value = std::to_underlying(shown.view) });
                x += columns;
            }
            if (!_state.engine.missingPlugins().empty())
                canvas.putString(0,
                                 x + 2,
                                 cut("plugins missing · ^K install", (canvas.width() / 2) - x),
                                 styleOf(theme, Role::Warning));
            drawGauge(canvas, gaugeOf(_state), 0, canvas.width() - 1);
        }

        [[nodiscard]] Size preferredSize() const override { return { .width = 0, .height = 1 }; }

      private:
        Dashboard::State& _state;
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
            // `+ new task` stands apart from the runs above it, where there are any.
            auto const apart = rows.size() > 1 && _state.rows[rows.back()].kind == RowKind::NewTask;
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
                drawRow(inside, y + shift, row, at == selected ? Selection::Selected : Selection::Unselected);
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

        /// @return The lines @p row takes, the blank above `+ new task` aside.
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
    /// the selected row (barText()); and the gauge on its right while the top bar gave way. In the Work view
    /// each hint records what a click on it runs.
    class ActionBar final: public Component
    {
      public:
        explicit ActionBar(Dashboard::State& state): _state { state } {}

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
            auto const agents = _state.interaction.view == View::Agents;
            if (agents && !_state.newestNeed.empty())
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
            else if (!agents)
            {
                auto const& work = interaction.work;
                auto const& lines = _state.work.lines;
                auto const selected =
                    work.selected < lines.size() ? std::optional { lines[work.selected].kind } : std::nullopt;
                auto const hits =
                    HitRecorder { _state.hits, { .x = screenBounds().x, .y = screenBounds().y } };
                auto x = 1;
                for (auto const& hint: workBarHints(selected, columns))
                {
                    if (x > 1)
                        x += canvas.putString(0, x, HintSeparator, styleOf(theme, Role::Dim));
                    auto const width = canvas.putString(0, x, hint.text, styleOf(theme, Role::Dim));
                    hits.add(Rect { .x = x, .y = 0, .width = width, .height = 1 },
                             Hit { .kind = HitKind::Command, .value = std::to_underlying(hint.command) });
                    x += width;
                }
            }
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

        Dashboard::State& _state;
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
    startDialogView { std::make_unique<StartDialogView>(engine, interaction.startDialog, hits) },
    taskBoxView { std::make_unique<TaskBoxView>(engine, interaction.taskBox, hits) },
    paletteView { std::make_unique<PaletteView>(interaction.palette, hits) },
    timelineView { std::make_unique<TimelineView>(engine, hits) },
    rowMenuView { std::make_unique<RowMenuView>(interaction.work.menu, work.items, hits) },
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
    for (Component* const overlay: std::array<Component*, 5> { startDialogView.get(),
                                                               taskBoxView.get(),
                                                               paletteView.get(),
                                                               timelineView.get(),
                                                               rowMenuView.get() })
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
        auto shown = showing(focus.givenWay, focusWidthOf(focus.row));
        shown.band = band;
        panel.setVisible(isShown(role, shown));
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
    toggle(PanelRole::Work,
           body->add(std::make_unique<WorkBoardView>(engine, interaction, work, hits, clock),
                     layout::Style { .grow = 1, .basis = 0 }));
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
    // The Work view builds its items again only when the engine changed what they are made of: its revision
    // counts every such change, the folds and the sort among them. Its lines follow from them cheaply.
    if (interaction.view == View::Work)
    {
        if (work.revision != engine.revision())
        {
            work.items = engine.workItems();
            work.order = orderOf(work.items, engine.repositories(), sortOf(engine));
            work.revision = engine.revision();
            ++rowBuilds;
        }
        syncWork();
        return;
    }
    auto const now = nowMonoMs();
    // Built again once the engine changed, or once a row says otherwise with time alone (an age, a
    // countdown): a key after a pause finds the rows as they were.
    if (rowsState == Rows::Current && (!rowChangeMs || now < *rowChangeMs))
        return;
    rows = buildRows(engine, now);
    rowChangeMs = nextRowChange(engine, now);
    rowsState = Rows::Current;
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

void Dashboard::State::syncWork()
{
    auto& state = interaction.work;
    work.lines = workLines(work.items, work.order, engine, state);
    keepWorkSelection(state, work.lines, work.items, work.listRows);
    // A menu whose item a poll dropped closes; a hover past the lines is forgotten until the pointer moves.
    if (state.menu && workItemOf(work.items, state.menu->itemKey) == nullptr)
        state.menu.reset();
    if (state.hovered
        && (*state.hovered >= work.lines.size() || work.lines[*state.hovered].kind != WorkLineKind::Item))
        state.hovered.reset();
}

Showing Dashboard::State::showing(Furniture given, FocusWidth width) const noexcept
{
    return Showing {
        .view = interaction.view, .place = interaction.place, .band = band(), .given = given, .width = width
    };
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
    // A run the start dialog or the `n` box started takes the selection once the engine lists it.
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
    // In the Work view the user looks at no run.
    auto const* const selected = keyContext().selected;
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
    // The Work view selects no Fleet row: the palette's row commands do not apply there.
    auto const* const row = interaction.view == View::Agents ? selectedRow() : nullptr;
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
        // A dialog opened by a key of this input confirms nothing an earlier one of it, or of an earlier
        // input, did: the confirmation belongs to the dialog it was given in.
        if (!interaction.startDialog && !interaction.taskBox)
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
        if (interaction.startDialog)
        {
            if (auto dispatched =
                    applyStartDialogKey(*key, interaction.startDialog, engine, hooksAtInput, tab))
            {
                // The run it starts is selected once it is listed: its session is the one the dialog minted.
                startedSession = dispatched->session;
                actions.emplace_back(std::move(*dispatched));
            }
            continue;
        }
        if (interaction.taskBox)
        {
            if (auto dispatched = applyTaskBoxKey(*key, interaction.taskBox, engine, hooksAtInput, tab))
            {
                // The run it starts is selected once it is listed: its session is the one the box minted.
                startedSession = dispatched->session;
                actions.emplace_back(std::move(*dispatched));
            }
            continue;
        }
        auto const view = interaction.view;
        // The row menu takes every key while it is open.
        if (auto& menu = interaction.work.menu)
        {
            auto const* const item = workItemOf(work.items, menu->itemKey);
            if (auto const command = applyRowMenuKey(*key, menu, item); command && item != nullptr)
                runMenuCommand(*command, *item, actions);
            afterCommand(view);
            continue;
        }
        // The Work view takes its own keys while nothing is over it.
        if (view == View::Work && !interaction.palette && interaction.overlay == Overlay::None)
        {
            std::ranges::move(applyWorkKey(*key, interaction, work.lines, work.items, engine, work.listRows),
                              std::back_inserter(actions));
            anchorMenu(std::nullopt);
        }
        // In the Work view a palette entry the Work view has a command for runs there, as its key would.
        else if (auto const command = view == View::Work && interaction.palette
                                              && key->key == core::tui::KeyCode::Enter
                                              && key->modifiers == core::tui::Modifier::None
                                          ? highlightedCommand(*interaction.palette, keyContext())
                                          : std::nullopt;
                 command && isWorkCommand(*command))
        {
            interaction.palette.reset();
            run(*command, actions);
            continue;
        }
        else if (auto action = applyKey(*key, interaction, keyContext()))
            actions.push_back(std::move(*action));
        afterCommand(view);
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
        Popup,         ///< A popup: the start dialog, the `n` box, the palette, the timeline or the row menu.
    };

    /// Where the pointer is, and the region of the hit it is on.
    struct Pointer
    {
        core::tui::Point at {}; ///< The cell, 0-based.
        Rect area {};           ///< The hit's region.
    };

    /// What a press of the left button on a hit of one kind does.
    using ClickHandler = void (*)(State& state,
                                  std::size_t value,
                                  Clicks clicks,
                                  Pointer const& pointer,
                                  std::vector<UiAction>& actions);
    /// What a press of the right button on a hit of one kind does.
    using RightClickHandler = void (*)(State& state,
                                       std::size_t value,
                                       Pointer const& pointer,
                                       std::vector<UiAction>& actions);
    /// What the wheel over a hit of one kind does; @p steps is negative upwards.
    using WheelHandler = void (*)(State& state, std::size_t value, int steps, std::vector<UiAction>& actions);

    /// What the mouse does on one kind of hit.
    struct MouseRow
    {
        HitKind kind = HitKind::Popup;          ///< The kind.
        Layer layer = Layer::Dashboard;         ///< Its layer.
        ClickHandler click = nullptr;           ///< A press of the left button; nullptr: it falls through.
        RightClickHandler rightClick = nullptr; ///< A press of the right button; nullptr: it falls through.
        WheelHandler wheel = nullptr;           ///< The wheel; nullptr: it falls through.
    };

    // {{{ what the rows do

    /// Gives the Work view's list the keys and selects one of its lines, as a click on it does.
    void clickWorkLine(State& state, std::size_t line)
    {
        auto& work = state.interaction.work;
        work.focus = WorkFocus::List;
        work.filtering = Filtering::Off;
        selectWorkLine(work, state.work, line);
    }

    /// @return The item a Work line stands for; nullptr for a heading, or a line no longer drawn.
    [[nodiscard]] WorkItem const* itemOfLine(State const& state, std::size_t line) noexcept
    {
        auto const& lines = state.work.lines;
        if (line >= lines.size() || lines[line].kind != WorkLineKind::Item)
            return nullptr;
        return &state.work.items[lines[line].item];
    }

    /// Scrolls the Work view's list by WheelLines a step.
    void scrollWork(State& state, std::size_t /*value*/, int steps, std::vector<UiAction>& /*actions*/)
    {
        scrollWorkList(state.interaction.work, state.work, steps * WheelLines);
    }

    /// Selects a Work line and opens the row menu on its item at the pointer.
    void openMenuAt(State& state,
                    std::size_t line,
                    Pointer const& pointer,
                    std::vector<UiAction>& /*actions*/)
    {
        auto const* const item = itemOfLine(state, line);
        if (item == nullptr)
            return;
        clickWorkLine(state, line);
        state.interaction.work.menu =
            RowMenuState { .itemKey = workItemKey(*item), .highlighted = 0, .at = pointer.at };
    }

    /// Picks a setting of the open start dialog or `n` box.
    /// @param field A FormField of the dialog, or a TaskBoxField of the box.
    void pickDialogSetting(State& state, std::size_t field)
    {
        if (auto& dialog = state.interaction.startDialog)
            dialog->setting = static_cast<FormField>(field);
        else if (auto& box = state.interaction.taskBox)
            box->focus = static_cast<TaskBoxField>(field);
    }

    /// Picks a setting of the open start dialog or `n` box and steps it, as ←/→ do.
    /// @param field A FormField of the dialog, or a TaskBoxField of the box.
    /// @param step 1 on, -1 back.
    void stepDialogSetting(State& state, std::size_t field, int step)
    {
        pickDialogSetting(state, field);
        if (auto& dialog = state.interaction.startDialog)
            changeSetting(dialog->form, dialog->setting, step, state.engine);
        else if (auto& box = state.interaction.taskBox)
            if (auto const setting = taskBoxSettingOf(box->focus))
                changeSetting(box->form, *setting, step, state.engine);
    }

    /// @return The field a DialogStep hit steps.
    [[nodiscard]] constexpr std::size_t stepField(std::size_t value) noexcept
    {
        return value / 2;
    }

    /// @return The way a DialogStep hit steps its field: 1 on, -1 back.
    [[nodiscard]] constexpr int stepWay(std::size_t value) noexcept
    {
        return value % 2 == 1 ? 1 : -1;
    }

    /// Steps a chip of the Work view's filter bar, giving it the keys: the repository chip on or back, the
    /// sort chip to the other sort; the filter is given the keys, as `/` does, and does not step.
    void stepChip(State& state, WorkFocus chip, int step, std::vector<UiAction>& actions)
    {
        auto& work = state.interaction.work;
        switch (chip)
        {
            case WorkFocus::Filter: state.run(KeyCommand::Filter, actions); return;
            case WorkFocus::RepoChip:
                work.focus = chip;
                work.filtering = Filtering::Off;
                state.run(step < 0 ? KeyCommand::RepoPrevious : KeyCommand::RepoNext, actions);
                return;
            case WorkFocus::SortChip:
                work.focus = chip;
                work.filtering = Filtering::Off;
                state.run(KeyCommand::Sort, actions);
                return;
            case WorkFocus::List: break;
        }
    }

    // }}}

    /// One row per HitKind, indexed by its value.
    constexpr auto MouseRows = std::array {
        // A popup covers the dashboard: a click on it that hits nothing of it does nothing, and the wheel
        // moves its highlight.
        MouseRow { .kind = HitKind::Popup,
                   .layer = Layer::Popup,
                   .click = [](State&, std::size_t, Clicks, Pointer const&, std::vector<UiAction>&) {},
                   .rightClick = [](State&, std::size_t, Pointer const&, std::vector<UiAction>&) {},
                   .wheel =
                       [](State& state, std::size_t, int steps, std::vector<UiAction>& actions) {
                           state.press(steps < 0 ? core::tui::KeyCode::Up : core::tui::KeyCode::Down,
                                       actions);
                       } },
        MouseRow { .kind = HitKind::FleetPane,
                   .layer = Layer::Dashboard,
                   .click = nullptr,
                   .wheel =
                       [](State& state, std::size_t, int steps, std::vector<UiAction>& /*actions*/) {
                           auto const at = static_cast<std::ptrdiff_t>(state.interaction.selected) + steps;
                           selectRow(state.interaction,
                                     state.keyContext(),
                                     static_cast<std::size_t>(std::max<std::ptrdiff_t>(0, at)));
                       } },
        MouseRow { .kind = HitKind::FocusPane,
                   .layer = Layer::Dashboard,
                   .click = nullptr,
                   .wheel =
                       [](State& state, std::size_t, int steps, std::vector<UiAction>& /*actions*/) {
                           scrollFocus(state.interaction, state.keyContext(), steps * WheelLines);
                       } },
        // A click selects a row; a double click, or one on `+ new task`, runs its primary action.
        MouseRow { .kind = HitKind::Row,
                   .layer = Layer::Dashboard,
                   .click =
                       [](State& state,
                          std::size_t value,
                          Clicks clicks,
                          Pointer const&,
                          std::vector<UiAction>& actions) {
                           selectRow(state.interaction, state.keyContext(), value);
                           state.refilter(State::Keep::Position);
                           auto const* const row = state.selectedRow();
                           if (clicks == Clicks::Double || (row != nullptr && row->kind == RowKind::NewTask))
                               state.run(KeyCommand::Enter, actions);
                       },
                   .wheel = nullptr },
        // A tab, a chip or a hint runs what its key would.
        MouseRow {
            .kind = HitKind::Command,
            .layer = Layer::Dashboard,
            .click =
                [](State& state, std::size_t value, Clicks, Pointer const&, std::vector<UiAction>& actions) {
                    state.run(static_cast<KeyCommand>(value), actions);
                },
            .wheel = nullptr },
        MouseRow { .kind = HitKind::MessageBox,
                   .layer = Layer::Dashboard,
                   .click =
                       [](State& state, std::size_t, Clicks, Pointer const&, std::vector<UiAction>& actions) {
                           state.run(KeyCommand::Open, actions);
                       },
                   .wheel = nullptr },
        // A palette entry runs at once, as the palette is a menu.
        MouseRow {
            .kind = HitKind::PaletteEntry,
            .layer = Layer::Popup,
            .click =
                [](State& state, std::size_t value, Clicks, Pointer const&, std::vector<UiAction>& actions) {
                    state.interaction.palette->highlighted = value;
                    state.press(core::tui::KeyCode::Enter, actions);
                },
            .wheel = nullptr },
        // A view's name in the top bar switches to it.
        MouseRow {
            .kind = HitKind::ViewName,
            .layer = Layer::Dashboard,
            .click =
                [](State& state, std::size_t value, Clicks, Pointer const&, std::vector<UiAction>& actions) {
                    if (value != std::to_underlying(state.interaction.view))
                        state.run(KeyCommand::SwitchView, actions);
                },
            .wheel = nullptr },
        // A Work row: a click selects it, a double click opens the start dialog on it, the right button its
        // menu; the wheel scrolls the list.
        MouseRow { .kind = HitKind::WorkRow,
                   .layer = Layer::Dashboard,
                   .click =
                       [](State& state,
                          std::size_t value,
                          Clicks clicks,
                          Pointer const&,
                          std::vector<UiAction>& actions) {
                           clickWorkLine(state, value);
                           if (clicks == Clicks::Double)
                               state.run(KeyCommand::Enter, actions);
                       },
                   .rightClick = openMenuAt,
                   .wheel = scrollWork },
        // An MR number: a click selects its row, a double click opens it in the browser.
        MouseRow { .kind = HitKind::WorkMrLink,
                   .layer = Layer::Dashboard,
                   .click =
                       [](State& state,
                          std::size_t value,
                          Clicks clicks,
                          Pointer const&,
                          std::vector<UiAction>& actions) {
                           clickWorkLine(state, value);
                           if (auto const* const item = itemOfLine(state, value);
                               item != nullptr && clicks == Clicks::Double && item->changeRequest)
                               actions.emplace_back(
                                   OpenUrlAction { .url = item->changeRequest->url,
                                                   .label = workMrLabel(state.engine, *item) });
                       },
                   .rightClick = openMenuAt,
                   .wheel = scrollWork },
        // A ticket key: a click selects its row, a double click opens the ticket in the browser.
        MouseRow { .kind = HitKind::WorkTicketLink,
                   .layer = Layer::Dashboard,
                   .click =
                       [](State& state,
                          std::size_t value,
                          Clicks clicks,
                          Pointer const&,
                          std::vector<UiAction>& actions) {
                           clickWorkLine(state, value);
                           if (auto const* const item = itemOfLine(state, value);
                               item != nullptr && clicks == Clicks::Double
                               && menuApplies(RowMenuCommand::OpenTicket, *item))
                               actions.emplace_back(
                                   OpenUrlAction { .url = item->ticket->url, .label = item->ticketKey });
                       },
                   .rightClick = openMenuAt,
                   .wheel = scrollWork },
        // A group's heading: a click folds or unfolds it (the second press of a double click adds nothing),
        // the right button folds every other group shown open.
        MouseRow { .kind = HitKind::WorkHeading,
                   .layer = Layer::Dashboard,
                   .click =
                       [](State& state,
                          std::size_t value,
                          Clicks clicks,
                          Pointer const&,
                          std::vector<UiAction>& actions) {
                           if (clicks == Clicks::Double)
                               return;
                           state.interaction.work.focus = WorkFocus::List;
                           state.interaction.work.filtering = Filtering::Off;
                           actions.push_back(
                               foldWorkGroup(state.interaction.work, static_cast<WorkGroup>(value)));
                       },
                   .rightClick =
                       [](State& state, std::size_t value, Pointer const&, std::vector<UiAction>& actions) {
                           std::ranges::move(foldOtherGroups(state.interaction.work,
                                                             state.work.lines,
                                                             state.engine,
                                                             static_cast<WorkGroup>(value)),
                                             std::back_inserter(actions));
                       },
                   .wheel = scrollWork },
        // The list's right edge: a press jumps there, and the list follows the pointer while it is held.
        MouseRow { .kind = HitKind::WorkScrollbar,
                   .layer = Layer::Dashboard,
                   .click =
                       [](State& state, std::size_t, Clicks, Pointer const& pointer, std::vector<UiAction>&) {
                           state.scrollbarDrag = pointer.area;
                           jumpWorkList(state.interaction.work,
                                        state.work,
                                        static_cast<std::size_t>(std::max(0, pointer.at.y - pointer.area.y)));
                       },
                   .wheel = scrollWork },
        // The filter bar: a click gives the filter the keys or steps a chip, the right button steps it back,
        // the wheel steps it.
        MouseRow {
            .kind = HitKind::WorkChip,
            .layer = Layer::Dashboard,
            .click =
                [](State& state, std::size_t value, Clicks, Pointer const&, std::vector<UiAction>& actions) {
                    stepChip(state, static_cast<WorkFocus>(value), 1, actions);
                },
            .rightClick =
                [](State& state, std::size_t value, Pointer const&, std::vector<UiAction>& actions) {
                    if (auto const chip = static_cast<WorkFocus>(value); chip != WorkFocus::Filter)
                        stepChip(state, chip, -1, actions);
                },
            .wheel =
                [](State& state, std::size_t value, int steps, std::vector<UiAction>& actions) {
                    if (auto const chip = static_cast<WorkFocus>(value); chip != WorkFocus::Filter)
                        stepChip(state, chip, steps, actions);
                } },
        // A dialog's setting: a click picks it, a double click starts, the right button and the wheel step
        // it.
        MouseRow { .kind = HitKind::DialogSetting,
                   .layer = Layer::Popup,
                   .click =
                       [](State& state,
                          std::size_t value,
                          Clicks clicks,
                          Pointer const&,
                          std::vector<UiAction>& actions) {
                           pickDialogSetting(state, value);
                           if (clicks == Clicks::Double)
                               state.press(core::tui::KeyCode::Enter, actions);
                       },
                   .rightClick =
                       [](State& state, std::size_t value, Pointer const&, std::vector<UiAction>&) {
                           stepDialogSetting(state, value, -1);
                       },
                   .wheel =
                       [](State& state, std::size_t value, int steps, std::vector<UiAction>&) {
                           stepDialogSetting(state, value, steps);
                       } },
        // A setting's value or arrows: each press steps it, a double click's second too.
        MouseRow { .kind = HitKind::DialogStep,
                   .layer = Layer::Popup,
                   .click =
                       [](State& state, std::size_t value, Clicks, Pointer const&, std::vector<UiAction>&) {
                           // The second press of a double click steps again: a stepper never starts.
                           stepDialogSetting(state, stepField(value), stepWay(value));
                       },
                   .rightClick =
                       [](State& state, std::size_t value, Pointer const&, std::vector<UiAction>&) {
                           stepDialogSetting(state, stepField(value), -1);
                       },
                   .wheel =
                       [](State& state, std::size_t value, int steps, std::vector<UiAction>&) {
                           stepDialogSetting(state, stepField(value), steps);
                       } },
        // A worker chip: a click picks the worker, a double click starts.
        MouseRow { .kind = HitKind::WorkerChip,
                   .layer = Layer::Popup,
                   .click =
                       [](State& state,
                          std::size_t value,
                          Clicks clicks,
                          Pointer const&,
                          std::vector<UiAction>& actions) {
                           if (auto& dialog = state.interaction.startDialog)
                           {
                               dialog->setting = FormField::Worker;
                               dialog->form.worker = value;
                           }
                           if (clicks == Clicks::Double)
                               state.press(core::tui::KeyCode::Enter, actions);
                       },
                   .rightClick =
                       [](State& state, std::size_t, Pointer const&, std::vector<UiAction>&) {
                           stepDialogSetting(state, std::to_underlying(FormField::Worker), -1);
                       },
                   .wheel =
                       [](State& state, std::size_t, int steps, std::vector<UiAction>&) {
                           stepDialogSetting(state, std::to_underlying(FormField::Worker), steps);
                       } },
        // A row menu entry runs at once where it applies; one that does not stays highlighted.
        MouseRow {
            .kind = HitKind::MenuEntry,
            .layer = Layer::Popup,
            .click =
                [](State& state, std::size_t value, Clicks, Pointer const&, std::vector<UiAction>& actions) {
                    // A hit the last frame recorded may outlive the menu: a second press, or a poll, closed
                    // it.
                    auto& menu = state.interaction.work.menu;
                    if (!menu)
                        return;
                    menu->highlighted = value;
                    state.press(core::tui::KeyCode::Enter, actions);
                },
            .wheel = nullptr },
    };

    static_assert(std::ranges::all_of(std::views::iota(std::size_t { 0 }, MouseRows.size()),
                                      [](std::size_t index) {
                                          return std::to_underlying(MouseRows[index].kind) == index;
                                      }),
                  "MouseRows is indexed by the HitKind's value");
    static_assert(MouseRows.size() == std::to_underlying(HitKind::MenuEntry) + 1U, "every HitKind has a row");

    /// What the mouse does on a hit.
    /// @param hit The hit.
    /// @return Its row; a kind out of range, which a valid HitKind never is, gets the popup's, which a click
    ///         leaves alone.
    [[nodiscard]] MouseRow const& mouseRowOf(Hit const& hit) noexcept
    {
        auto const index = std::to_underlying(hit.kind);
        return index < MouseRows.size() ? MouseRows[index] : MouseRows.front();
    }

    /// The most cells of a link the action bar names when it opens.
    constexpr auto LinkLabelColumns = 48;

    /// What a row menu command does to the item the menu was about.
    using MenuHandler = void (*)(State& state, WorkItem const& item, std::vector<UiAction>& actions);

    /// One row menu command and what it does.
    struct MenuRow
    {
        RowMenuCommand command = RowMenuCommand::StartAgent; ///< The command.
        MenuHandler handler = nullptr;                       ///< What it does.
    };

    /// One row per RowMenuCommand, indexed by its value.
    constexpr auto MenuRows = std::array {
        MenuRow { .command = RowMenuCommand::StartAgent,
                  .handler =
                      [](State& state, WorkItem const& item, std::vector<UiAction>&) {
                          openStartDialogFor(state.interaction, state.engine, item.repo, item.item, {});
                      } },
        MenuRow { .command = RowMenuCommand::OpenMr,
                  .handler =
                      [](State& state, WorkItem const& item, std::vector<UiAction>& actions) {
                          actions.emplace_back(OpenUrlAction { .url = item.changeRequest->url,
                                                               .label = workMrLabel(state.engine, item) });
                      } },
        MenuRow { .command = RowMenuCommand::OpenTicket,
                  .handler =
                      [](State&, WorkItem const& item, std::vector<UiAction>& actions) {
                          actions.emplace_back(
                              OpenUrlAction { .url = item.ticket->url, .label = item.ticketKey });
                      } },
        MenuRow { .command = RowMenuCommand::FoldGroup,
                  .handler =
                      [](State& state, WorkItem const& item, std::vector<UiAction>& actions) {
                          actions.push_back(foldWorkGroup(state.interaction.work, item.group));
                      } },
    };
    static_assert(std::ranges::all_of(std::views::iota(std::size_t { 0 }, MenuRows.size()),
                                      [](std::size_t index) {
                                          return std::to_underlying(MenuRows[index].command) == index;
                                      }),
                  "MenuRows is indexed by the RowMenuCommand's value");
    static_assert(MenuRows.size() == RowMenuEntries.size(), "every menu entry has a row");
} // namespace

bool Dashboard::State::popupOpen() const noexcept
{
    return interaction.startDialog || interaction.taskBox || interaction.palette
           || interaction.overlay != Overlay::None || interaction.work.menu;
}

void Dashboard::State::press(core::tui::KeyCode key, std::vector<UiAction>& actions)
{
    std::ranges::move(applyKeys({ core::tui::KeyEvent { .key = key } }, dialogHooks()),
                      std::back_inserter(actions));
}

void Dashboard::State::run(KeyCommand command, std::vector<UiAction>& actions)
{
    auto const view = interaction.view;
    auto const menu = interaction.work.menu;
    if (view == View::Work)
        std::ranges::move(
            applyWorkCommand(command, interaction, work.lines, work.items, engine, work.listRows),
            std::back_inserter(actions));
    else if (auto action = dispatch(command, interaction, keyContext()))
        actions.push_back(std::move(*action));
    anchorMenu(menu);
    afterCommand(view);
}

void Dashboard::State::afterCommand(View before)
{
    // A start dialog or an `n` box a command opened gets its agent's session id now.
    mintSession();
    // A view switched to builds what it shows.
    if (interaction.view != before)
        ensureRows();
    if (interaction.view == View::Work)
        syncWork();
    else
        refilter(Keep::Position);
}

void Dashboard::State::runMenuCommand(RowMenuCommand command,
                                      WorkItem const& item,
                                      std::vector<UiAction>& actions)
{
    MenuRows[std::to_underlying(command)].handler(*this, item, actions);
}

void Dashboard::State::anchorMenu(std::optional<RowMenuState> const& before)
{
    auto& menu = interaction.work.menu;
    if (before || !menu)
        return;
    // Under the selected row, a little in from its left edge, as the last frame drew it.
    auto const regions = hits.regions();
    auto const row = std::ranges::find(
        regions, Hit { .kind = HitKind::WorkRow, .value = interaction.work.selected }, &HitRegion::hit);
    if (row != regions.end())
        menu->at = core::tui::Point { .x = row->area.x + 2, .y = row->area.y + 1 };
}

void Dashboard::State::rightClick(std::span<HitRegion const> under,
                                  core::tui::Point at,
                                  std::vector<UiAction>& actions)
{
    // A click beside an open popup closes it, as esc does.
    if (popupOpen() && (under.empty() || mouseRowOf(under.front().hit).layer != Layer::Popup))
    {
        press(core::tui::KeyCode::Escape, actions);
        return;
    }
    auto const target = std::ranges::find_if(
        under, [](HitRegion const& region) { return mouseRowOf(region.hit).rightClick != nullptr; });
    if (target != under.end())
        mouseRowOf(target->hit)
            .rightClick(*this, target->hit.value, Pointer { .at = at, .area = target->area }, actions);
}

void Dashboard::State::move(core::tui::MouseEvent const& mouse, std::span<HitRegion const> under)
{
    // The button that pressed the scrollbar is held: the list follows the pointer, past the bar's ends too.
    if (mouse.button == 0 && scrollbarDrag)
    {
        jumpWorkList(
            interaction.work, work, static_cast<std::size_t>(std::max(0, mouse.y - 1 - scrollbarDrag->y)));
        return;
    }
    // A move with no button held (3): a drag whose release went elsewhere is over.
    if (mouse.button != 3)
        return;
    scrollbarDrag.reset();
    if (interaction.view != View::Work || popupOpen())
        return;
    // The item line under the pointer is hovered; applyKeys() repaints only when that changed.
    auto const row = std::ranges::find_if(under, [](HitRegion const& region) {
        auto const kind = region.hit.kind;
        return kind == HitKind::WorkRow || kind == HitKind::WorkMrLink || kind == HitKind::WorkTicketLink;
    });
    interaction.work.hovered = row != under.end() ? std::optional { row->hit.value } : std::nullopt;
}

void Dashboard::State::applyMouse(core::tui::MouseEvent const& mouse, std::vector<UiAction>& actions)
{
    using Type = core::tui::MouseEvent::Type;
    auto const at = core::tui::Point { .x = mouse.x - 1, .y = mouse.y - 1 };
    auto const under = hits.regionsAt(at.x, at.y);
    auto const takes = [&](Hit const& hit) {
        return !popupOpen() || mouseRowOf(hit).layer == Layer::Popup;
    };
    switch (mouse.type)
    {
        case Type::Move: move(mouse, under); return;
        case Type::Release: scrollbarDrag.reset(); return;
        case Type::ScrollUp:
        case Type::ScrollDown: {
            // The pane under the pointer scrolls: the topmost hit that the wheel moves.
            auto const pane = std::ranges::find_if(
                under, [](HitRegion const& region) { return mouseRowOf(region.hit).wheel != nullptr; });
            if (pane != under.end() && takes(pane->hit))
                mouseRowOf(pane->hit).wheel(
                    *this, pane->hit.value, mouse.type == Type::ScrollUp ? -1 : 1, actions);
            return;
        }
        case Type::Press: break;
    }
    if (mouse.button == 2)
    {
        interaction.notice.clear();
        rightClick(under, at, actions);
        return;
    }
    if (mouse.button != 0)
        return;
    auto const now = nowMonoMs();
    auto const clicks =
        lastClick && lastClick->x == at.x && lastClick->y == at.y && now - lastClick->atMs <= DoubleClickMs
            ? Clicks::Double
            : Clicks::Single;
    lastClick = clicks == Clicks::Double ? std::nullopt
                                         : std::optional { LastClick { .x = at.x, .y = at.y, .atMs = now } };
    interaction.notice.clear();
    auto const target = std::ranges::find_if(
        under, [](HitRegion const& region) { return mouseRowOf(region.hit).click != nullptr; });
    // A click beside an open popup closes it, as esc does.
    if (popupOpen() && (under.empty() || !takes(under.front().hit)))
    {
        press(core::tui::KeyCode::Escape, actions);
        return;
    }
    // A link opens on the machine the user sits at.
    if (!popupOpen())
        if (auto const uri = core::tui::HyperlinkIndex { screen.renderedBuffer() }.uriAt(at.y, at.x);
            !uri.empty())
        {
            actions.emplace_back(
                OpenUrlAction { .url = std::string { uri }, .label = cut(uri, LinkLabelColumns) });
            return;
        }
    if (target != under.end())
        mouseRowOf(target->hit)
            .click(*this, target->hit.value, clicks, Pointer { .at = at, .area = target->area }, actions);
}

// }}}
// {{{ the engine's reads

void Dashboard::State::syncBoard(std::vector<UiAction>& actions)
{
    // The Work view hides the Focus: no board is read for it.
    auto const* const row = keyContext().selected;
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
    if (interaction.view != View::Agents || run == nullptr || !countsAsActive(*run)
        || interaction.follow != Follow::Live || shownTranscript != run->id)
        return std::nullopt;
    return transcriptAskedMs + TranscriptFollowMs;
}

void Dashboard::State::syncTranscript(std::vector<UiAction>& actions)
{
    auto const* const run = selectedRun();
    // The engine reads no transcript while its configuration is refused, nor while the Work view hides the
    // Focus.
    if (interaction.view != View::Agents || run == nullptr || engine.config() == nullptr)
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
    return applyKeys(parser.timeout(), dialogHooks());
}

HooksWarning Dashboard::State::dialogHooks() const noexcept
{
    if (interaction.startDialog)
        return interaction.startDialog->hooks;
    return interaction.taskBox ? interaction.taskBox->hooks : HooksWarning::None;
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
        return candidate.isSelectable() && candidate.kind != RowKind::NewTask;
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
    if (isShown(PanelRole::Focus, showing(Furniture::None, FocusWidth::Shared)) && row != nullptr
        && (!row->run.empty() || row->kind == RowKind::Orphan))
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
        if (auto const shown = isShown(role, showing(focus.givenWay, focusWidthOf(row)));
            panel->visible() != shown)
            panel->setVisible(shown);
    if (problems != nullptr)
        problems->refresh();
    syncStartDialog();
    syncTaskBox();
    syncPalette();
    syncRowMenu();
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
    showAt(overlay,
           core::tui::Point { .x = (size.width - wanted.width) / 2, .y = (size.height - wanted.height) / 2 });
}

void Dashboard::State::syncRowMenu()
{
    auto const& menu = interaction.work.menu;
    if (!menu || interaction.view != View::Work)
    {
        if (screen.isOverlayVisible(*rowMenuView))
            screen.hideOverlay(*rowMenuView);
        return;
    }
    // Where it was opened, moved left or up as far as it must to fit in the tab.
    auto const wanted = rowMenuView->preferredSize();
    showAt(*rowMenuView,
           core::tui::Point { .x = std::clamp(menu->at.x, 0, std::max(0, size.width - wanted.width)),
                              .y = std::clamp(menu->at.y, 0, std::max(0, size.height - wanted.height)) });
}

void Dashboard::State::showAt(Component& overlay, core::tui::Point at)
{
    auto const last =
        std::ranges::find(overlayAt, &overlay, &std::pair<Component const*, core::tui::Point>::first);
    if (screen.isOverlayVisible(overlay))
    {
        // Its contents are drawn afresh every frame and sent as a diff; only a move needs the whole tab
        // again.
        if (last != overlayAt.end() && last->second == at)
            return;
        screen.positionOverlay(overlay, at);
    }
    else
        screen.showOverlay(overlay, at);
    if (last != overlayAt.end())
        last->second = at;
    else
        overlayAt.emplace_back(&overlay, at);
}

void Dashboard::State::syncPalette()
{
    if (!interaction.palette)
    {
        if (screen.isOverlayVisible(*paletteView))
            screen.hideOverlay(*paletteView);
        return;
    }
    auto entries = paletteEntries(keyContext().selected, &engine);
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
    if (auto& dialog = interaction.startDialog; dialog && dialog->form.session.empty())
        dialog->form.session = formatUuidV4(ids.next());
    if (auto& box = interaction.taskBox; box && box->form.session.empty())
        box->form.session = formatUuidV4(ids.next());
}

void Dashboard::State::syncStartDialog()
{
    if (!interaction.startDialog)
    {
        if (screen.isOverlayVisible(*startDialogView))
            screen.hideOverlay(*startDialogView);
        return;
    }
    auto const width = popupWidthIn(band(), size.width, StartDialogMaxColumns);
    auto const wanted =
        Size { .width = width, .height = std::min(startDialogView->heightAt(width), size.height) };
    startDialogView->place(wanted);
    showCentred(*startDialogView, wanted);
}

void Dashboard::State::syncTaskBox()
{
    if (!interaction.taskBox)
    {
        if (screen.isOverlayVisible(*taskBoxView))
            screen.hideOverlay(*taskBoxView);
        return;
    }
    auto const width = popupWidthIn(band(), size.width, TaskBoxMaxColumns);
    auto const wanted =
        Size { .width = width, .height = std::min(taskBoxView->heightAt(width), size.height) };
    taskBoxView->place(wanted);
    showCentred(*taskBoxView, wanted);
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
    std::ranges::move(state.applyKeys(state.parser.feed(bytes), state.dialogHooks()),
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
    // An agent's age moves on in its row; a board's in the Board tab; a followed transcript is read again;
    // in the Work view, an item's age. Only the view shown asks for a wake.
    auto earliest = std::optional<uint64_t> {};
    auto const consider = [&](std::optional<uint64_t> due) {
        if (due)
            earliest = earliest ? std::min(*earliest, *due) : *due;
    };
    if (state.interaction.view == View::Work)
        consider(state.work.ageChangeMs);
    else
    {
        consider(state.rowChangeMs);
        if (state.focus.tab == FocusTab::Board)
            consider(state.focusPanels.board.ageChangeMs);
        consider(state.transcriptDueMs());
    }
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

void Dashboard::openStartDialogOn(std::string_view repo, ItemRef const& item, std::string_view task)
{
    auto& state = *_state;
    openStartDialogFor(state.interaction, state.engine, repo, item, task);
    state.mintSession();
    state.repaint = State::Repaint::Needed;
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
    // Only the view shown changes with time: the Fleet's rows and the Board's age, or the Work view's ages.
    auto const agents = state.interaction.view == View::Agents;
    if (agents && state.rowChangeMs && state.nowMonoMs() >= *state.rowChangeMs)
    {
        state.rowsState = State::Rows::Stale;
        state.repaint = State::Repaint::Needed;
    }
    if (!agents && state.work.ageChangeMs && state.nowMonoMs() >= *state.work.ageChangeMs)
        state.repaint = State::Repaint::Needed;
    // An answered agent may still ask: the frame says so.
    if (auto const due = state.stillAsksDueMs(); due && state.nowMonoMs() >= *due)
        state.repaint = State::Repaint::Needed;
    // A board's age moved on.
    if (auto const due = state.focusPanels.board.ageChangeMs;
        agents && state.focus.tab == FocusTab::Board && due && state.nowMonoMs() >= *due)
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
    // The Work view asks for the pointer's every move, for hovering and dragging; the view after it, no more.
    if (state.interaction.view == View::Work)
        bytes += AnyMotionOn;
    else if (state.framedView == View::Work)
        bytes += AnyMotionOff;
    state.framedView = state.interaction.view;
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
