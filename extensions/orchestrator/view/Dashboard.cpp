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
#include <orchestrator/view/Theme.hpp>

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

    /// The widest the dispatch form is in the Wide band.
    constexpr auto FormMaxColumns = 80;
    /// How much of the tab's width the dispatch form takes in the Medium band, in percent.
    constexpr auto FormMediumPercent = 90;

    /// @return The cells the dispatch form is wide in @p band, in a tab @p columns wide: in the Wide band a
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

    /// Where a panel of a band that cannot show the list and the details at once is shown.
    enum class PanelRole : uint8_t
    {
        List,         ///< The list.
        DetailsBelow, ///< The details below the list (Medium).
        DetailsPage,  ///< The details in the list's place.
    };

    /// The pane each role is shown in, indexed by PanelRole.
    constexpr auto ShownInPane = std::array { Pane::List, Pane::List, Pane::Details };
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
    /// Applies keys the parser decoded: to the dispatch form while it is open, else by the key table.
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
    /// Shows the form where the band puts it, or hides it.
    void syncForm();
    /// @return What a key is applied to.
    [[nodiscard]] KeyContext keyContext() const noexcept;
    /// @return Now, in the engine's monotonic milliseconds.
    [[nodiscard]] uint64_t nowMonoMs() const noexcept;
    /// @return The selected row among the visible ones; nullptr when there is none.
    [[nodiscard]] Row const* selectedRow() const noexcept;

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
    IUuidSource& ids;       ///< Where the session ids of the agents it dispatches come from.
    std::string tab;        ///< The tab it is served in.
    std::string frameBytes; ///< What the surface was handed since the last frame.
    core::tui::HeadlessSurface surface;
    core::tui::Screen screen;
    core::tui::VtParser parser;
    Size size;
    Interaction interaction;
    std::vector<Row> rows;            ///< Every row.
    std::vector<std::size_t> visible; ///< The indices of the rows the filter and the layout keep.
    std::string visibleFilter;        ///< The filter @ref visible was made with.
    Furniture visibleFurniture = Furniture::Shown; ///< The furniture @ref visible was made with.
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
    std::unique_ptr<DispatchFormView> formView; ///< Draws interaction.form.
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
            if (selected < rows.size() && selected == top && top > 0 && height > 1
                && _state.rows[rows[top - 1]].kind == RowKind::Heading)
                --top;
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

    /// @return The details word-wrapped to @p columns cells, continuation lines indented.
    [[nodiscard]] std::vector<DetailText> detailLinesOf(std::span<DetailLine const> details, int columns)
    {
        auto lines = std::vector<DetailText> {};
        for (auto const& detail: details)
        {
            auto wrapped = wrap(detail.value, columns, columns - ContinuationIndent);
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

    /// The keys that act now in a band, by their hints.
    class HintLine final: public Component
    {
      public:
        explicit HintLine(Band band): _band { band } {}

        void render(Canvas& canvas) override
        {
            auto hinted = std::vector<std::string_view> {};
            for (auto const& row: keyRows())
                if (isHintedIn(row, _band))
                    hinted.push_back(row.hint);
            canvas.putString(0, 1, joinHints(hinted, canvas.width() - 2), styleOf(canvas.theme(), Role::Dim));
        }

      private:
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
            if (auto const& notice = _state.interaction.notice; !notice.empty())
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

    /// @return The hints of the keys that have one, in the table's order.
    [[nodiscard]] auto hints()
    {
        return keyRows() | std::views::transform(&KeyRow::hint)
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
    formView { std::make_unique<DispatchFormView>(engine, interaction.form) },
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
    if (screen.isOverlayVisible(*formView))
        screen.hideOverlay(*formView);
    screen.root().removeChild(*root);
}

std::unique_ptr<Component> Dashboard::State::buildLayout(std::size_t breakpoint)
{
    auto const& chosen = BandBreakpoints[breakpoint];
    toggled.clear();
    // Shown or hidden once added: adding a child shows it.
    auto const toggle = [this](PanelRole role, Component& panel) {
        panel.setVisible(ShownInPane[std::to_underlying(role)] == interaction.pane);
        toggled.emplace_back(role, &panel);
    };
    auto const filling = layout::Style { .grow = 1, .basis = 0 };

    auto column = std::make_unique<FlexBox>(layout::Style { .direction = layout::Direction::Column });
    auto problemLines = std::make_unique<ProblemLines>(*this);
    problems = problemLines.get();
    column->add(std::move(problemLines), layout::Style { .shrink = 0 });
    switch (chosen.band)
    {
        case Band::Wide: {
            auto body =
                std::make_unique<FlexBox>(layout::Style { .direction = layout::Direction::Row, .gap = 1 });
            body->add(std::make_unique<ListPanel>(*this), layout::Style { .grow = 3, .basis = 0 });
            body->add(std::make_unique<DetailsPanel>(*this), layout::Style { .grow = 2, .basis = 0 });
            column->add(std::move(body), filling);
            break;
        }
        case Band::Medium: {
            auto body = std::make_unique<FlexBox>(layout::Style { .direction = layout::Direction::Column });
            toggle(PanelRole::List, body->add(std::make_unique<ListPanel>(*this), filling));
            toggle(PanelRole::DetailsBelow,
                   body->add(std::make_unique<DetailsPanel>(*this),
                             layout::Style { .shrink = 0, .basis = DetailsBelowRows }));
            toggle(PanelRole::DetailsPage, body->add(std::make_unique<DetailsPanel>(*this), filling));
            column->add(std::move(body), filling);
            break;
        }
        case Band::Narrow: {
            auto body = std::make_unique<FlexBox>(layout::Style { .direction = layout::Direction::Column });
            toggle(PanelRole::List, body->add(std::make_unique<ListPanel>(*this), filling));
            toggle(PanelRole::DetailsPage, body->add(std::make_unique<DetailsPanel>(*this), filling));
            column->add(std::move(body), filling);
            break;
        }
    }
    if (chosen.furniture == Furniture::Shown)
        column->add(std::make_unique<HintLine>(chosen.band), layout::Style { .shrink = 0 });
    column->add(std::make_unique<SummaryLine>(*this), layout::Style { .shrink = 0 });
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
    auto const furniture = BandBreakpoints[breakpointFor(size)].furniture;
    if (!visible.empty() && interaction.filter == visibleFilter && furniture == visibleFurniture)
        return;
    visible = visibleRows(rows, interaction.filter, furniture);
    visibleFilter = interaction.filter;
    visibleFurniture = furniture;
    auto const selectable = selectableCount(rows, visible);
    interaction.selected = selectable == 0 ? 0 : std::min(interaction.selected, selectable - 1);
}

Row const* Dashboard::State::selectedRow() const noexcept
{
    auto const position = positionOfSelectable(rows, visible, interaction.selected);
    return position < visible.size() ? &rows[visible[position]] : nullptr;
}

KeyContext Dashboard::State::keyContext() const noexcept
{
    return KeyContext { .band = BandBreakpoints[breakpointFor(size)].band,
                        .selectable = selectableCount(rows, visible),
                        .selected = selectedRow(),
                        .engine = &engine,
                        .task = std::nullopt };
}

std::vector<UiAction> Dashboard::State::applyKeys(std::vector<core::tui::InputEvent> const& events,
                                                  HooksWarning hooksAtInput)
{
    auto actions = std::vector<UiAction> {};
    auto const before = interaction;
    for (auto const& event: events)
    {
        // A form opened by a key of this input confirms nothing an earlier form of it, or of an earlier
        // input, did: the confirmation belongs to the form it was given in.
        if (!interaction.form)
            hooksAtInput = HooksWarning::None;
        if (auto const* const paste = std::get_if<core::tui::PasteEvent>(&event))
        {
            interaction.notice.clear();
            pasteInto(interaction, paste->text);
            continue;
        }
        auto const* const key = std::get_if<core::tui::KeyEvent>(&event);
        if (key == nullptr)
            continue;
        interaction.notice.clear();
        if (interaction.form)
        {
            if (auto dispatched = applyFormKey(*key, interaction.form, engine, hooksAtInput, tab))
                actions.emplace_back(std::move(*dispatched));
            continue;
        }
        if (auto action = applyKey(*key, interaction, keyContext()))
            actions.push_back(std::move(*action));
        // A form a key opened gets its agent's session id now.
        if (interaction.form && interaction.form->session.empty())
            interaction.form->session = formatUuidV4(ids.next());
        refilter();
    }
    if (interaction != before)
        repaint = Repaint::Needed;
    return actions;
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
    return interaction.form ? interaction.form->hooks : HooksWarning::None;
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
        if (auto const shown = ShownInPane[std::to_underlying(role)] == interaction.pane;
            panel->visible() != shown)
            panel->setVisible(shown);
    if (problems != nullptr)
        problems->refresh();
    syncForm();
    if (interaction.overlay == Overlay::Help && !interaction.form)
    {
        auto const wanted = help->preferredSize();
        screen.showOverlay(*help,
                           { .x = std::max(0, (size.width - wanted.width) / 2),
                             .y = std::max(0, (size.height - wanted.height) / 2) });
    }
    else if (screen.isOverlayVisible(*help))
        screen.hideOverlay(*help);
}

void Dashboard::State::syncForm()
{
    if (!interaction.form)
    {
        if (screen.isOverlayVisible(*formView))
            screen.hideOverlay(*formView);
        return;
    }
    // Centred; the Narrow band gives it the whole tab.
    auto const band = BandBreakpoints[breakpointFor(size)].band;
    auto const width = formWidthIn(band, size.width);
    auto const height = band == Band::Narrow ? size.height : std::min(formView->heightAt(width), size.height);
    formView->place(Size { .width = width, .height = height });
    auto const at = core::tui::Point { .x = (size.width - width) / 2, .y = (size.height - height) / 2 };
    if (screen.isOverlayVisible(*formView))
        screen.positionOverlay(*formView, at);
    else
        screen.showOverlay(*formView, at);
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
    auto const& due = _state->rowChangeMs;
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

void Dashboard::select(std::string_view run)
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
        return;
    state.interaction.selected = *found;
    // An end that waited for its confirmation was for the row selected before.
    state.interaction.pendingEnd.clear();
    state.repaint = State::Repaint::Needed;
}

void Dashboard::openForm(std::string_view repo, ItemRef const& item, std::string_view task)
{
    auto& state = *_state;
    auto form = openDispatchForm(state.engine, repo, item, task, {});
    if (!form)
        return;
    form->session = formatUuidV4(state.ids.next());
    state.interaction.form = std::move(form);
    state.interaction.overlay = Overlay::None;
    state.repaint = State::Repaint::Needed;
}

std::optional<DispatchFormState> const& Dashboard::form() const noexcept
{
    return _state->interaction.form;
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

std::string Dashboard::frame()
{
    auto& state = *_state;
    // An Escape key resolved here asks the engine nothing: it only closes what is open.
    static_cast<void>(state.resolveOverdueEscape());
    // A row changed with time alone (an age moved on, a trust-prompt hint fell due): the rows are built and
    // drawn again, though the engine did not change.
    if (state.rowChangeMs && state.nowMonoMs() >= *state.rowChangeMs)
    {
        state.rowsState = State::Rows::Stale;
        state.repaint = State::Repaint::Needed;
    }
    if (state.repaint == State::Repaint::Clean)
        return {};
    state.ensureRows();
    state.refilter();
    state.syncLayout();
    state.screen.draw();
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
