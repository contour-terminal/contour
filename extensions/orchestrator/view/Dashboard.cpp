// SPDX-License-Identifier: Apache-2.0
#include <orchestrator/view/Dashboard.hpp>

#include <core/tui/Canvas.hpp>
#include <core/tui/Component.hpp>
#include <core/tui/HeadlessSurface.hpp>
#include <core/tui/Responsive.hpp>
#include <core/tui/Screen.hpp>
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

#include <orchestrator/view/Band.hpp>
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

    /// The rows the details panel's box takes below the list in the Medium band.
    constexpr auto DetailsBelowRows = 8;
    /// The cells a detail's label takes, its padding included.
    constexpr auto LabelColumns = 10;
    /// How long a lone ESC that ended an input waits for the rest of a sequence before it is the Escape key.
    constexpr auto EscapeTimeout = std::chrono::milliseconds { 50 };
    /// How far an agent's age moves before the rows are built again for it.
    constexpr auto AgeTickMs = uint64_t { 1000 };

    /// The widest the launcher is in the Wide band.
    constexpr auto FormMaxColumns = 80;
    /// How much of the tab's width the launcher takes in the Medium band, in percent.
    constexpr auto FormMediumPercent = 90;

    /// @return The cells the launcher is wide in @p band, in a tab @p columns wide: in the Wide band a
    /// popup
    ///         of at most FormMaxColumns, in the Medium band most of the tab, in the Narrow band all of it.
    [[nodiscard]] int formWidthIn(Band band, int columns) noexcept
    {
        switch (band)
        {
            case Band::Wide: return std::min(FormMaxColumns, columns);
            case Band::Medium: return columns * FormMediumPercent / 100;
            case Band::Narrow: break;
        }
        return columns;
    }

    /// @return The index into BandBreakpoints that @p size chooses: the last one whose minima it meets.
    [[nodiscard]] std::size_t breakpointFor(Size size) noexcept
    {
        auto chosen = std::size_t { 0 };
        for (auto const index: std::views::iota(std::size_t { 0 }, BandBreakpoints.size()))
            if (size.width >= BandBreakpoints[index].minColumns
                && size.height >= BandBreakpoints[index].minRows)
                chosen = index;
        return chosen;
    }

    /// Whether a row is the selected one.
    enum class Selection : uint8_t
    {
        Unselected, ///< Drawn in its own roles.
        Selected,   ///< Drawn in the selection's.
    };

    /// Which pane or screen a panel is shown in.
    enum class PanelRole : uint8_t
    {
        List,           ///< The list.
        DetailsBelow,   ///< The details below the list (Medium).
        DetailsPage,    ///< The details in the list's place, the whole width.
        ListAndDetails, ///< The list and the details side by side (Wide).
        Body,           ///< What holds the list and the details (Medium, Narrow): the dashboard's alone.
        Board,          ///< A run's status board in the body's place.
        Transcript,     ///< A run's transcript in the body's place.
        Chrome, ///< The problem lines and the summary line, which a board or a transcript in the Narrow band
                ///< covers.
    };

    /// The pane each of the dashboard's roles is shown in, indexed by PanelRole up to Board.
    constexpr auto ShownInPane = std::array { Pane::List, Pane::List, Pane::Details, Pane::List };

    /// @return Whether a panel of @p role is shown while the user is at @p interaction, in @p band.
    [[nodiscard]] bool isShown(PanelRole role, Interaction const& interaction, Band band) noexcept
    {
        switch (role)
        {
            case PanelRole::Body: return interaction.screen == Screen::Dashboard;
            case PanelRole::Board: return interaction.screen == Screen::Board;
            case PanelRole::Transcript: return interaction.screen == Screen::Transcript;
            // The Narrow band gives a board or a transcript the whole tab but for its keys.
            case PanelRole::Chrome: return interaction.screen == Screen::Dashboard || band != Band::Narrow;
            default: break;
        }
        return interaction.screen == Screen::Dashboard
               && ShownInPane[std::to_underlying(role)] == interaction.pane;
    }
} // namespace

// {{{ the state

namespace
{
    class ProblemLines;
    class HelpOverlay;
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
    /// Applies the filter and the layout's furniture to the rows, and keeps the selection among them.
    void refilter();
    /// Applies keys the parser decoded: to the launcher while it is open, else by the key table.
    /// @param events The keys.
    /// @param hooksAtInput The form's HooksWarning as the input that holds them began.
    /// @return What the engine is asked, in order.
    [[nodiscard]] std::vector<UiAction> applyKeys(std::vector<core::tui::InputEvent> const& events,
                                                  HooksWarning hooksAtInput);
    /// @return The form's HooksWarning now: what the next input begins with.
    [[nodiscard]] HooksWarning formHooks() const noexcept;
    /// Resolves a held ESC that waited past EscapeTimeout into the Escape key.
    /// @return What the engine is asked, in order.
    [[nodiscard]] std::vector<UiAction> resolveOverdueEscape();
    /// Shows the panels of the current pane and hides the others; places the help and the form.
    void syncLayout();
    /// Gives a launcher just opened its agent's session id, from the dashboard's source.
    void mintSession();
    /// Shows the launcher where the band puts it, or hides it.
    void syncForm();
    /// Keeps a board's or a transcript's scroll within what it holds, once the last draw knows its end: the
    /// keys scroll past an end not yet wrapped, and a draw shows such a scroll from the end.
    void clampScroll() noexcept;
    /// @return What a key is applied to.
    [[nodiscard]] KeyContext keyContext() const noexcept;
    /// @return Now, in the engine's monotonic milliseconds.
    [[nodiscard]] uint64_t nowMonoMs() const noexcept;
    /// @return The selected row among the visible ones; nullptr when there is none.
    [[nodiscard]] Row const* selectedRow() const noexcept;
    /// Asks the engine to read the board of the run the dashboard selects, which its details show, and to
    /// stop reading the one it showed before: appended to @p actions. A board or a transcript screen asks for
    /// its own; the details ask again once the dashboard is back.
    void syncDetailsBoard(std::vector<UiAction>& actions);
    /// @return The band the tab's size chooses.
    [[nodiscard]] Band band() const noexcept;

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
    std::string detailsBoard {}; ///< The run whose board the details asked the engine for; empty: none.
    IUuidSource& ids;            ///< Where the session ids of the agents it dispatches come from.
    std::string tab;             ///< The tab it is served in.
    std::string frameBytes;      ///< What the surface was handed since the last frame.
    core::tui::HeadlessSurface surface;
    core::tui::Screen screen;
    core::tui::VtParser parser;
    Size size;
    Interaction interaction;
    std::vector<Row> rows;            ///< Every row.
    std::vector<std::size_t> visible; ///< The indices of the rows the filter and the layout keep.
    std::string visibleFilter;        ///< The filter @ref visible was made with.
    Furniture visibleFurniture = Furniture::Shown; ///< The furniture @ref visible was made with.
    ChartRows visibleCharts = ChartRows::Shown;    ///< Whether @ref visible kept the chart rows.
    Rows rowsState = Rows::Stale;                  ///< Whether @ref rows must be built again.
    uint64_t rowsBuiltAtMs = 0; ///< When @ref rows was built, in the engine's monotonic milliseconds.
    std::size_t rowBuilds = 0;  ///< How often @ref rows was built.
    std::optional<uint64_t>
        rowChangeMs; ///< When a row changes next with time alone (nextRowChange()); std::nullopt: none does.
    std::optional<core::SteadyTimePoint> escapeHeldSince; ///< When a lone ESC that ended an input arrived.
    std::size_t scrollTop = 0;                            ///< The first visible row the list shows.
    Component const* shownLayout = nullptr; ///< The layout @ref toggled and @ref problems belong to.
    std::vector<std::pair<PanelRole, Component*>> toggled; ///< Its panels shown by pane.
    ProblemLines* problems = nullptr;                      ///< The current layout's problem lines.
    Repaint repaint = Repaint::Needed;
    std::unique_ptr<HelpOverlay> help;
    std::unique_ptr<LauncherView> launcherView; ///< Draws interaction.launcher.
    BoardPanelState boardPanel;                 ///< What the board panel learnt drawing itself.
    TranscriptPanelState transcriptPanel;       ///< What the transcript panel learnt drawing itself.
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
                canvas.putString(row++, 0, cut(line, canvas.width()), style);
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

    /// The rows, in a box titled with the filter.
    class ListPanel final: public Component
    {
      public:
        explicit ListPanel(Dashboard::State& state): _state { state } {}

        void render(Canvas& canvas) override
        {
            auto const& interaction = _state.interaction;
            auto title = std::string { "Agents" };
            if (interaction.filtering == Filtering::Typing || !interaction.filter.empty())
                title += std::format(
                    " · /{}{}", interaction.filter, interaction.filtering == Filtering::Typing ? "▏" : "");
            drawFrame(canvas, title);
            auto inside = insideFrame(canvas);
            auto const& rows = _state.visible;
            auto const height = static_cast<std::size_t>(std::max(0, inside.height()));
            auto const selected = positionOfSelectable(_state.rows, rows, interaction.selected);
            auto& top = _state.scrollTop;
            if (selected < rows.size())
            {
                if (selected < top)
                    top = selected;
                else if (height > 0 && selected >= top + height)
                    top = selected - height + 1;
            }
            // The selected row is never the first one shown with its section's heading hidden right above
            // it, where both fit: not after scrolling up to it, nor when rows arrive above it (a list
            // answered after the rows the selection first scrolled to).
            // An open group's header counts as part of the heading above it: both come into view.
            if (selected < rows.size() && selected >= top)
            {
                auto const isHeader = [&](std::size_t at) {
                    auto const& row = _state.rows[rows[at]];
                    return row.kind == RowKind::Heading
                           || (row.kind == RowKind::Group && !row.isSelectable());
                };
                // Only headers stand between the top and the selection: the one above them comes into view
                // too.
                auto const onlyHeadersAbove = [&] {
                    return std::ranges::all_of(std::views::iota(top, selected), isHeader);
                };
                while (top > 0 && selected - top + 1 < height && isHeader(top - 1) && onlyHeadersAbove())
                    --top;
            }
            top = std::min(top, rows.size() > height ? rows.size() - height : 0);
            for (auto const line: std::views::iota(std::size_t { 0 }, std::min(height, rows.size() - top)))
                drawRow(inside,
                        static_cast<int>(line),
                        _state.rows[rows[top + line]],
                        top + line == selected ? Selection::Selected : Selection::Unselected);
        }

      private:
        static void drawRow(Canvas& canvas, int line, Row const& row, Selection selection)
        {
            auto const& theme = canvas.theme();
            auto const width = canvas.width();
            switch (row.kind)
            {
                case RowKind::Spacer: return;
                case RowKind::Heading:
                case RowKind::Placeholder:
                    canvas.putString(line, 1, cut(row.text, width - 2), styleOf(theme, Role::Dim));
                    return;
                case RowKind::Notice:
                    canvas.putString(line, 1, cut(row.text, width - 2), styleOf(theme, Role::Text));
                    return;
                case RowKind::Chart: {
                    // The label, the bars from its column on, and what they add up to on the right when it
                    // fits.
                    auto const bars = cut(row.text, std::max(0, width - 2 - ChartLabelColumns));
                    auto const used = 1 + ChartLabelColumns + columnsOf(bars);
                    auto const status = cut(row.status, std::max(0, width - 1 - used - 2));
                    canvas.putString(
                        line, 1, cut(row.prefix, ChartLabelColumns - 1), styleOf(theme, Role::Dim));
                    canvas.putString(line, 1 + ChartLabelColumns, bars, styleOf(theme, Role::Text));
                    canvas.putString(line, width - 1 - columnsOf(status), status, styleOf(theme, Role::Dim));
                    return;
                }
                default: break;
            }
            auto const pick = [&](Role role) -> core::tui::Style const& {
                return styleOf(theme, selection == Selection::Selected ? Role::Selection : role);
            };
            if (selection == Selection::Selected)
                canvas.fill(Rect { .x = 0, .y = line, .width = width, .height = 1 }, ' ', pick(Role::Text));
            canvas.putString(line, 1, cut(row.mark, 1), pick(row.markRole));
            auto column = 3;
            auto const end = width - 1;
            auto const statusColumns = std::min(columnsOf(row.status), std::max(0, (end - column) / 2));
            auto const status = cut(row.status, statusColumns);
            if (!row.prefix.empty())
            {
                auto const prefix = cut(row.prefix, std::max(0, (end - column) / 4));
                column += canvas.putString(line, column, prefix, pick(Role::Dim)) + 1;
            }
            auto const textEnd = status.empty() ? end : end - columnsOf(status) - 1;
            canvas.putString(line, column, cut(row.text, textEnd - column), pick(Role::Text));
            if (!status.empty())
                canvas.putString(line, end - columnsOf(status), status, pick(row.statusRole));
        }

        Dashboard::State& _state;
    };

    /// How far a detail's continuation lines are indented past its value.
    constexpr auto ContinuationIndent = 2;

    /// One drawn line of the details: a detail's first line with its label, or one of its continuations.
    struct DetailText
    {
        std::string_view label = {}; ///< The detail's label on its first line; empty on a continuation.
        std::string text = {};       ///< Sanitised and wrapped to its cells.
        Role role = Role::Text;      ///< How it is drawn.
        int indent = 0;              ///< The cells it starts past the value column.
    };

    /// @return The details fitted to @p columns cells as each says (fitted()), continuation lines indented.
    [[nodiscard]] std::vector<DetailText> detailLinesOf(std::span<DetailLine const> details, int columns)
    {
        auto lines = std::vector<DetailText> {};
        for (auto const& detail: details)
        {
            auto wrapped = fitted(detail.value, detail.fit, columns, columns - ContinuationIndent);
            if (wrapped.empty())
                wrapped.emplace_back();
            for (auto const index: std::views::iota(std::size_t { 0 }, wrapped.size()))
                lines.push_back(DetailText { .label = index == 0 ? std::string_view { detail.label } : ""sv,
                                             .text = std::move(wrapped[index]),
                                             .role = detail.role,
                                             .indent = index == 0 ? 0 : ContinuationIndent });
        }
        return lines;
    }

    /// @return @p text with a `…` that says more follows, cut to fit @p columns.
    [[nodiscard]] std::string endingInEllipsis(std::string_view text, int columns)
    {
        auto withEllipsis = std::format("{}…", text);
        return columnsOf(withEllipsis) <= columns ? withEllipsis : cut(withEllipsis, columns);
    }

    /// The selected row's details, in a box.
    class DetailsPanel final: public Component
    {
      public:
        explicit DetailsPanel(Dashboard::State const& state): _state { state } {}

        void render(Canvas& canvas) override
        {
            drawFrame(canvas, "Details");
            auto inside = insideFrame(canvas);
            auto const& theme = canvas.theme();
            auto const width = inside.width() - 2;
            auto const* const row = _state.selectedRow();
            if (row == nullptr)
            {
                inside.putString(0, 1, cut("Nothing selected.", width), styleOf(theme, Role::Dim));
                return;
            }
            inside.putString(0, 1, cut(row->title, width), styleOf(theme, Role::Text));
            auto const lines = detailLinesOf(row->details, width - LabelColumns);
            auto const room = static_cast<std::size_t>(std::max(0, inside.height() - 2));
            for (auto const index: std::views::iota(std::size_t { 0 }, std::min(room, lines.size())))
            {
                auto const& line = lines[index];
                auto const y = 2 + static_cast<int>(index);
                auto const columns = width - LabelColumns - line.indent;
                // The last line that fits says that more follows.
                auto const text = index + 1 == room && lines.size() > room
                                      ? endingInEllipsis(line.text, columns)
                                      : std::string { line.text };
                inside.putString(y, 1, cut(line.label, LabelColumns - 1), styleOf(theme, Role::Dim));
                inside.putString(y, 1 + LabelColumns + line.indent, text, styleOf(theme, line.role));
            }
        }

      private:
        Dashboard::State const& _state;
    };

    /// The keys that act now in a band and on the screen shown, by their hints.
    class HintLine final: public Component
    {
      public:
        HintLine(Dashboard::State const& state, Band band): _state { state }, _band { band } {}

        void render(Canvas& canvas) override
        {
            auto const& interaction = _state.interaction;
            auto hinted = std::vector<std::string_view> {};
            for (auto const& row: keyRows())
                if (isHintedIn(row, _band, interaction.screen) && isOffered(row))
                    hinted.push_back(row.hint);
            canvas.putString(0, 1, joinHints(hinted, canvas.width() - 2), styleOf(canvas.theme(), Role::Dim));
        }

      private:
        /// @return Whether the row acts now. On the dashboard the hints are the keys of every row; on a
        ///         board or a transcript Enter needs a run whose agent has a tab, and `b` a run whose
        ///         worktree is there.
        [[nodiscard]] bool isOffered(KeyRow const& row) const noexcept
        {
            auto const& interaction = _state.interaction;
            if (interaction.screen == Screen::Dashboard)
                return true;
            if (row.command == KeyCommand::Enter)
                return hasAgentTab(&_state.engine, interaction.subject);
            if (row.command == KeyCommand::Board)
                return hasBoardToShow(&_state.engine, interaction.subject);
            return true;
        }

        Dashboard::State const& _state;
        Band _band;
    };

    /// The extension's summary, as the status line shows it; or the question an `e` asks (D14).
    class SummaryLine final: public Component
    {
      public:
        explicit SummaryLine(Dashboard::State const& state): _state { state } {}

        void render(Canvas& canvas) override
        {
            // The dashboard shows no terminal cursor: the filter draws its own caret. A frame's buffer keeps
            // the cursor visible unless something hides it, and this line is in every layout, so it is hidden
            // here.
            canvas.hideCursor();
            auto const& pending = _state.interaction.pendingEnd;
            if (auto const& message = _state.interaction.message)
                canvas.putString(
                    0,
                    1,
                    cut(std::format("to {}: {}▏ · enter sends · esc drops", message->run, message->text),
                        canvas.width() - 2),
                    styleOf(canvas.theme(), Role::Accent));
            else if (auto const& notice = _state.interaction.notice; !notice.empty())
                canvas.putString(
                    0, 1, cut(notice, canvas.width() - 2), styleOf(canvas.theme(), Role::Failing));
            else if (!pending.empty())
                canvas.putString(
                    0,
                    1,
                    cut(std::format("end {}? e to confirm · esc to keep", pending), canvas.width() - 2),
                    styleOf(canvas.theme(), Role::Warning));
            else
                canvas.putString(0,
                                 1,
                                 cut(_state.engine.published().shortText, canvas.width() - 2),
                                 styleOf(canvas.theme(), Role::Text));
        }

      private:
        Dashboard::State const& _state;
    };

    /// @return The hints of the dashboard's keys that have one, in the table's order.
    [[nodiscard]] auto hints()
    {
        return keyRows()
               | std::views::filter([](KeyRow const& row) { return holds(row.on, Screen::Dashboard); })
               | std::views::transform(&KeyRow::hint)
               | std::views::filter([](std::string_view hint) { return !hint.empty(); });
    }

    /// The keys and what they do, over the dashboard.
    /// The keys and what they do, over the dashboard: the table's hints, then the configured tasks' keys.
    class HelpOverlay final: public Component
    {
      public:
        explicit HelpOverlay(Engine const& engine): _engine { engine } {}

        void render(Canvas& canvas) override
        {
            canvas.clear(styleOf(canvas.theme(), Role::Text));
            drawFrame(canvas, "Keys");
            auto inside = insideFrame(canvas);
            auto line = 0;
            for (auto const& [text, role]: lines() | std::views::take(std::max(0, inside.height())))
                inside.putString(line++, 1, cut(text, inside.width() - 2), styleOf(canvas.theme(), role));
        }

        [[nodiscard]] Size preferredSize() const override
        {
            auto const all = lines();
            auto widest = 0;
            for (auto const& line: all)
                widest = std::max(widest, columnsOf(line.first));
            return { .width = widest + 4, .height = static_cast<int>(all.size()) + 2 };
        }

      private:
        /// @return Each line and its role: the hints, then a `tasks` heading and one `<key> <label>` per
        /// task.
        [[nodiscard]] std::vector<std::pair<std::string, Role>> lines() const
        {
            auto all = std::vector<std::pair<std::string, Role>> {};
            for (auto const hint: hints())
                all.emplace_back(std::string { hint }, Role::Text);
            auto const* const config = _engine.config();
            if (config == nullptr || config->tasks().empty())
                return all;
            all.emplace_back(std::string {}, Role::Text);
            all.emplace_back("tasks", Role::Dim);
            for (auto const& task: config->tasks())
                all.emplace_back(std::format("{} {}", task.key, task.label), Role::Text);
            return all;
        }

        Engine const& _engine;
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
    help { std::make_unique<HelpOverlay>(engine) },
    launcherView { std::make_unique<LauncherView>(engine, interaction.launcher) },
    root { std::make_unique<core::tui::Responsive>(std::ranges::to<std::vector<core::tui::Breakpoint>>(
        std::views::iota(std::size_t { 0 }, BandBreakpoints.size())
        | std::views::transform([this](std::size_t index) {
              return core::tui::Breakpoint { .minColumns = BandBreakpoints[index].minColumns,
                                             .minRows = BandBreakpoints[index].minRows,
                                             .make = [this, index] { return buildLayout(index); } };
          }))) }
{
    screen.setTheme(theme);
    screen.root().addChild(*root, { .flex = layout::Style { .grow = 1 } });
}

Dashboard::State::~State()
{
    if (screen.isOverlayVisible(*help))
        screen.hideOverlay(*help);
    if (screen.isOverlayVisible(*launcherView))
        screen.hideOverlay(*launcherView);
    screen.root().removeChild(*root);
}

std::unique_ptr<Component> Dashboard::State::buildLayout(std::size_t breakpoint)
{
    auto const& chosen = BandBreakpoints[breakpoint];
    toggled.clear();
    // Shown or hidden once added: adding a child shows it.
    auto const toggle = [this, &chosen](PanelRole role, Component& panel) {
        panel.setVisible(isShown(role, interaction, chosen.band));
        toggled.emplace_back(role, &panel);
    };
    auto const filling = layout::Style { .grow = 1, .basis = 0 };

    auto column = std::make_unique<FlexBox>(layout::Style { .direction = layout::Direction::Column });
    auto problemLines = std::make_unique<ProblemLines>(*this);
    problems = problemLines.get();
    toggle(PanelRole::Chrome, column->add(std::move(problemLines), layout::Style { .shrink = 0 }));
    switch (chosen.band)
    {
        case Band::Wide: {
            auto body =
                std::make_unique<FlexBox>(layout::Style { .direction = layout::Direction::Row, .gap = 1 });
            body->add(std::make_unique<ListPanel>(*this), layout::Style { .grow = 3, .basis = 0 });
            body->add(std::make_unique<DetailsPanel>(*this), layout::Style { .grow = 2, .basis = 0 });
            toggle(PanelRole::ListAndDetails, column->add(std::move(body), filling));
            toggle(PanelRole::DetailsPage, column->add(std::make_unique<DetailsPanel>(*this), filling));
            break;
        }
        case Band::Medium: {
            auto body = std::make_unique<FlexBox>(layout::Style { .direction = layout::Direction::Column });
            toggle(PanelRole::List, body->add(std::make_unique<ListPanel>(*this), filling));
            toggle(PanelRole::DetailsBelow,
                   body->add(std::make_unique<DetailsPanel>(*this),
                             layout::Style { .shrink = 0, .basis = DetailsBelowRows }));
            toggle(PanelRole::DetailsPage, body->add(std::make_unique<DetailsPanel>(*this), filling));
            toggle(PanelRole::Body, column->add(std::move(body), filling));
            break;
        }
        case Band::Narrow: {
            auto body = std::make_unique<FlexBox>(layout::Style { .direction = layout::Direction::Column });
            toggle(PanelRole::List, body->add(std::make_unique<ListPanel>(*this), filling));
            toggle(PanelRole::DetailsPage, body->add(std::make_unique<DetailsPanel>(*this), filling));
            toggle(PanelRole::Body, column->add(std::move(body), filling));
            break;
        }
    }
    toggle(PanelRole::Board,
           column->add(std::make_unique<StatusBoardPanel>(engine, interaction, clock, boardPanel), filling));
    toggle(
        PanelRole::Transcript,
        column->add(std::make_unique<TranscriptPanel>(engine, interaction, tab, transcriptPanel), filling));
    if (chosen.furniture == Furniture::Shown)
        column->add(std::make_unique<HintLine>(*this, chosen.band), layout::Style { .shrink = 0 });
    toggle(PanelRole::Chrome,
           column->add(std::make_unique<SummaryLine>(*this), layout::Style { .shrink = 0 }));
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
    refilter();
}

void Dashboard::State::refilter()
{
    auto const& breakpoint = BandBreakpoints[breakpointFor(size)];
    auto const furniture = breakpoint.furniture;
    // A chart needs the Wide band's width: its hours would be cut to nothing in a narrower list.
    auto const charts = breakpoint.band == Band::Wide ? ChartRows::Shown : ChartRows::Hidden;
    if (!visible.empty() && interaction.filter == visibleFilter && furniture == visibleFurniture
        && charts == visibleCharts)
        return;
    visible = visibleRows(rows, interaction.filter, furniture, charts);
    visibleFilter = interaction.filter;
    visibleFurniture = furniture;
    visibleCharts = charts;
    auto const selectable = selectableCount(rows, visible);
    interaction.selected = selectable == 0 ? 0 : std::min(interaction.selected, selectable - 1);
    // A group just folded selects its header, its items gone; one just unfolded, its first item.
    if (!interaction.followGroup.empty())
    {
        auto ordinal = std::size_t { 0 };
        for (auto const index: visible)
        {
            if (!rows[index].isSelectable())
                continue;
            // Whichever of the header and the first item is selectable now: folded, the header; else the
            // item.
            if (rows[index].group == interaction.followGroup)
            {
                interaction.selected = ordinal;
                break;
            }
            ++ordinal;
        }
        interaction.followGroup.clear();
    }
}

Row const* Dashboard::State::selectedRow() const noexcept
{
    auto const position = positionOfSelectable(rows, visible, interaction.selected);
    return position < visible.size() ? &rows[visible[position]] : nullptr;
}

Band Dashboard::State::band() const noexcept
{
    return BandBreakpoints[breakpointFor(size)].band;
}

KeyContext Dashboard::State::keyContext() const noexcept
{
    // What the keys scroll: the board or the transcript shown.
    auto const isTranscript = interaction.screen == Screen::Transcript;
    return KeyContext { .band = band(),
                        .selectable = selectableCount(rows, visible),
                        .selected = selectedRow(),
                        .engine = &engine,
                        .task = std::nullopt,
                        .lines = isTranscript ? transcriptPanel.lines : boardPanel.lines,
                        .rows = isTranscript ? transcriptPanel.rows : boardPanel.rows,
                        .tab = tab };
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
            pasteInto(interaction, engine, paste->text);
            continue;
        }
        auto const* const key = std::get_if<core::tui::KeyEvent>(&event);
        if (key == nullptr)
            continue;
        interaction.notice.clear();
        if (interaction.launcher)
        {
            if (auto dispatched = applyLauncherKey(*key, interaction.launcher, engine, hooksAtInput, tab))
                actions.emplace_back(std::move(*dispatched));
            continue;
        }
        auto const wasOnBoard = interaction.screen == Screen::Board;
        auto action = applyKey(*key, interaction, keyContext());
        // A board the key left, for the dashboard or a transcript, is no longer read.
        if (wasOnBoard && interaction.screen != Screen::Board)
        {
            actions.emplace_back(HideBoardAction {});
            detailsBoard.clear();
        }
        if (action)
            actions.push_back(std::move(*action));
        // A launcher a key opened gets its agent's session id now.
        mintSession();
        refilter();
    }
    if (interaction != before)
        repaint = Repaint::Needed;
    return actions;
}

void Dashboard::State::syncDetailsBoard(std::vector<UiAction>& actions)
{
    // A board or a transcript screen asks for what it shows; the one it replaced is the engine's to forget.
    if (interaction.screen != Screen::Dashboard)
    {
        detailsBoard.clear();
        return;
    }
    auto const* const row = selectedRow();
    auto wanted =
        row != nullptr && std::ranges::contains(row->offers, KeyCommand::Board) ? row->run : std::string {};
    if (wanted == detailsBoard)
        return;
    if (!wanted.empty())
        actions.emplace_back(ShowBoardAction { .run = wanted });
    else
        actions.emplace_back(HideBoardAction {});
    detailsBoard = std::move(wanted);
}

std::vector<UiAction> Dashboard::State::resolveOverdueEscape()
{
    if (!escapeHeldSince || clock.now() - *escapeHeldSince < EscapeTimeout)
        return {};
    escapeHeldSince.reset();
    ensureRows();
    return applyKeys(parser.timeout(), formHooks());
}

HooksWarning Dashboard::State::formHooks() const noexcept
{
    return interaction.launcher && interaction.launcher->form ? interaction.launcher->form->hooks
                                                              : HooksWarning::None;
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
    for (auto const& [role, panel]: toggled)
        if (auto const shown = isShown(role, interaction, band()); panel->visible() != shown)
            panel->setVisible(shown);
    if (problems != nullptr)
        problems->refresh();
    syncForm();
    if (interaction.overlay == Overlay::Help && !interaction.launcher)
    {
        auto const wanted = help->preferredSize();
        screen.showOverlay(*help,
                           { .x = std::max(0, (size.width - wanted.width) / 2),
                             .y = std::max(0, (size.height - wanted.height) / 2) });
    }
    else if (screen.isOverlayVisible(*help))
        screen.hideOverlay(*help);
}

void Dashboard::State::clampScroll() noexcept
{
    auto const& [lines, rows] = interaction.screen == Screen::Transcript
                                    ? std::pair { transcriptPanel.lines, transcriptPanel.rows }
                                    : std::pair { boardPanel.lines, boardPanel.rows };
    if (interaction.screen != Screen::Dashboard)
        interaction.scroll = std::min(interaction.scroll, lines > rows ? lines - rows : 0);
}

void Dashboard::State::mintSession()
{
    if (!interaction.launcher || !interaction.launcher->session.empty())
        return;
    interaction.launcher->session = formatUuidV4(ids.next());
    if (interaction.launcher->form)
        interaction.launcher->form->session = interaction.launcher->session;
}

void Dashboard::State::syncForm()
{
    if (!interaction.launcher)
    {
        if (screen.isOverlayVisible(*launcherView))
            screen.hideOverlay(*launcherView);
        return;
    }
    // Centred; the Narrow band gives it the whole tab.
    auto const band = BandBreakpoints[breakpointFor(size)].band;
    auto const width = formWidthIn(band, size.width);
    auto const height =
        band == Band::Narrow ? size.height : std::min(launcherView->heightAt(width), size.height);
    launcherView->place(Size { .width = width, .height = height });
    auto const at = core::tui::Point { .x = (size.width - width) / 2, .y = (size.height - height) / 2 };
    if (screen.isOverlayVisible(*launcherView))
        screen.positionOverlay(*launcherView, at);
    else
        screen.showOverlay(*launcherView, at);
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
    std::ranges::move(state.applyKeys(state.parser.feed(bytes), state.formHooks()),
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
    // A board shows no rows: only its age moves on with time. A transcript shows nothing that does.
    auto const screen = _state->interaction.screen;
    if (screen == Screen::Transcript)
        return std::nullopt;
    auto const& due = screen == Screen::Board ? _state->boardPanel.ageChangeMs : _state->rowChangeMs;
    return due ? std::optional { core::SteadyTimePoint { std::chrono::milliseconds { *due } } }
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
        state.refilter();
        found = selectableIndexOf();
    }
    if (!found)
        return {};
    state.interaction.selected = *found;
    // An end that waited for its confirmation was for the row selected before.
    state.interaction.pendingEnd.clear();
    state.repaint = State::Repaint::Needed;
    // The row is shown on the dashboard: a board or a transcript shown in its place is left, and a board no
    // longer read.
    auto const wasOnBoard = state.interaction.screen == Screen::Board;
    leaveSubject(state.interaction);
    auto actions = std::vector<UiAction> {};
    if (wasOnBoard)
    {
        actions.emplace_back(HideBoardAction {});
        state.detailsBoard.clear();
    }
    return actions;
}

void Dashboard::openLauncherOn(std::string_view repo, ItemRef const& item, std::string_view task)
{
    auto& state = *_state;
    openFormFor(state.interaction, state.engine, repo, item, task, {});
    state.mintSession();
    state.repaint = State::Repaint::Needed;
}

std::optional<LauncherState> const& Dashboard::launcher() const noexcept
{
    return _state->interaction.launcher;
}

void Dashboard::messageRefused(std::string_view why)
{
    _state->interaction.notice = std::format("message not sent: {}", why);
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
    // A transcript whose page the engine keeps no view of for this tab -- dropped from its cache -- is asked
    // for again; while its run is gone or the configuration is refused, the screen says so instead.
    auto const& interaction = state.interaction;
    if (interaction.screen == Screen::Transcript && state.engine.config() != nullptr
        && std::ranges::contains(state.engine.runs(), interaction.subject, &Run::id))
        if (auto const* const view = state.engine.transcript(state.tab, interaction.subject);
            view == nullptr || view->page != interaction.page)
            actions.emplace_back(
                ReadTranscriptAction { .run = interaction.subject, .page = interaction.page });
    // The selected agent's details show its board: the engine reads it for this tab while it is selected. The
    // rows as they stand say which agent that is; building them here would take a row change that fell due
    // from the frame that draws it.
    state.syncDetailsBoard(actions);
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
        if (state.interaction.screen == Screen::Dashboard)
            state.repaint = State::Repaint::Needed;
    }
    // A board's age moved on.
    if (auto const due = state.boardPanel.ageChangeMs;
        state.interaction.screen == Screen::Board && due && state.nowMonoMs() >= *due)
        state.repaint = State::Repaint::Needed;
    if (state.repaint == State::Repaint::Clean)
        return {};
    state.ensureRows();
    state.refilter();
    state.syncLayout();
    state.screen.draw();
    state.clampScroll();
    // The cursor rests at the top-left, hidden: every frame addresses its cells absolutely, and a shorter tab
    // then cuts its bottom rows rather than pushing its top ones into a history -- in the terminal and in
    // every window that mirrors it.
    // Bracketed paste on, in every frame, so that a tab the host drew in the meantime gets it back: a paste
    // arrives as text, never as the keys it spells (an Enter in it would confirm the form).
    state.frameBytes += BracketedPasteOn;
    state.frameBytes += RestingCursor;
    state.repaint = State::Repaint::Clean;
    return std::exchange(state.frameBytes, {});
}

core::tui::Buffer const& Dashboard::rendered() const noexcept
{
    return _state->screen.renderedBuffer();
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
