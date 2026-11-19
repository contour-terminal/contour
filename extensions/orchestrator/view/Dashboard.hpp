// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/Clock.hpp>
#include <core/Defines.hpp>
#include <core/tui/Buffer.hpp>
#include <core/tui/Rect.hpp>
#include <core/tui/Theme.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <orchestrator/core/Engine.hpp>
#include <orchestrator/core/Uuid.hpp>
#include <orchestrator/view/DispatchForm.hpp>
#include <orchestrator/view/Keys.hpp>

namespace orchestrator::view
{

/// What ends every frame: CUP to the top-left, where the hidden cursor rests (@see Dashboard::frame).
inline constexpr auto RestingCursor = std::string_view { "\033[H" };

/// What every frame sets before RestingCursor: bracketed paste (DECSET 2004), so that a paste reaches the
/// dashboard as one PasteEvent rather than as keys.
inline constexpr auto BracketedPasteOn = std::string_view { "\033[?2004h" };

/// The dashboard tab: the engine's lists drawn with core::tui, laid out by breakpoint (narrowest first, the
/// last one the tab's size meets wins: BandBreakpoints), and the keys typed into it.
///
/// It performs no I/O. Bytes the terminal sent go in through input(); frame() returns the bytes that bring
/// the tab up to date, which the guest writes to it.
class Dashboard
{
  public:
    /// @param engine The engine whose lists are drawn; it must outlive the dashboard.
    /// @param theme How the dashboard is drawn, such as dashboardTheme().
    /// @param clock What core::tui measures hover and spinner time on, and the agents' ages are measured
    ///        against, in the engine's monotonic milliseconds; the guest's own (GuestClock).
    /// @param size The tab's size.
    /// @param ids Where the session ids of the agents it dispatches come from; it must outlive the dashboard.
    /// @param tab The id of the tab it is served in: where the agents it dispatches open beside.
    Dashboard(Engine const& engine,
              core::tui::Theme theme,
              core::IClock const& clock CORE_LIFETIMEBOUND,
              core::tui::Size size,
              IUuidSource& ids CORE_LIFETIMEBOUND,
              std::string tab);
    /// A clock is kept, not copied: a temporary would dangle.
    Dashboard(Engine const& engine,
              core::tui::Theme theme,
              core::IClock const&& clock,
              core::tui::Size size,
              IUuidSource& ids,
              std::string tab) = delete;
    ~Dashboard();

    Dashboard(Dashboard const&) = delete;
    Dashboard& operator=(Dashboard const&) = delete;
    Dashboard(Dashboard&&) = delete;
    Dashboard& operator=(Dashboard&&) = delete;

    /// Applies what the user typed. A key whose encoding is split across two calls waits for its rest; an ESC
    /// that ends @p bytes waits up to 50 ms for one (see pendingEscapeDeadline()), and is the Escape key
    /// after.
    /// @param bytes What the terminal sent, decoded by core::tui::VtParser.
    /// @return What the engine is asked, in order.
    [[nodiscard]] std::vector<UiAction> input(std::string_view bytes);

    /// The tab was resized: the next frame repaints at the new size, in the band it chooses.
    /// @param size The tab's size.
    void resize(core::tui::Size size);

    /// The engine's lists changed: the next frame draws them again.
    void engineChanged();

    /// Selects the row of a run, dropping the filter when it hides that row and an end waiting for its
    /// confirmation, and returning from a board or a transcript to the dashboard; does nothing when no row
    /// stands for the run.
    /// @param run The run's id.
    /// @return What the engine is asked: HideBoardAction when a board was left; else nothing.
    [[nodiscard]] std::vector<UiAction> select(std::string_view run);

    /// Opens the dispatch form on an item the engine lists, its session id taken from the dashboard's source:
    /// what `d` does on a finished run, a board or a transcript, through the same openFormFor(). The form
    /// previews and refuses as every dispatch does; nothing starts before it is confirmed. When the engine
    /// does not list the item, or no task applies to it, no form opens and the summary line says why.
    /// @param repo The repository's configured name.
    /// @param item The item.
    /// @param task The id of the task the form starts on, such as a run's; when it no longer applies to the
    ///        item, the first task that does.
    void openForm(std::string_view repo, ItemRef const& item, std::string_view task);

    /// @return The dispatch form while it is open.
    [[nodiscard]] std::optional<DispatchFormState> const& form() const noexcept;

    /// The engine refused a dispatch confirmed here: the summary line says why, until the next key.
    /// @param refusal Why.
    void dispatchRefused(DispatchRefusal refusal);

    /// @return The run the selected row stands for; empty when it stands for none, or nothing is selected.
    [[nodiscard]] std::string_view selectedRun() const noexcept;

    /// @return When a lone ESC that ended the last input becomes the Escape key, unless more bytes come
    ///         first; std::nullopt when none waits. The guest wakes by then and calls frame(), which resolves
    ///         it.
    [[nodiscard]] std::optional<core::SteadyTimePoint> pendingEscapeDeadline() const noexcept;

    /// @return When an agent's row changes next with time alone (see nextRowChange()): the guest wakes by
    /// then
    ///         and calls frame(), which draws it; std::nullopt when none does. Valid after a frame or an
    ///         input.
    [[nodiscard]] std::optional<core::SteadyTimePoint> nextRepaintDue() const noexcept;

    /// @return How often the rows were built from the engine: once per input(), and in frame() only after
    ///         engineChanged() or when a row changed with time. For tests.
    [[nodiscard]] std::size_t rowBuilds() const noexcept;

    /// What the dashboard asks of the engine with no key typed; the guest calls it before frame(), so that it
    /// reaches the engine first. An ESC whose deadline passed (see pendingEscapeDeadline()) is the Escape key
    /// (Esc on a board hides it); and the transcript shown is asked for again when the engine keeps no page
    /// of it for this tab, so that the screen never waits for a page nobody reads.
    /// @return What the engine is asked, in order; empty when nothing is due.
    [[nodiscard]] std::vector<UiAction> dueActions();

    /// Resolves an ESC whose deadline passed (see pendingEscapeDeadline()), then draws. What that Escape key
    /// asks is dropped: dueActions() first takes it.
    /// @return The bytes that bring the tab up to date; empty when nothing changed since the last frame.
    [[nodiscard]] std::string frame();

    /// @return What the last frame drew: for golden tests.
    [[nodiscard]] core::tui::Buffer const& rendered() const noexcept;

    /// @return Where the user is: what is selected, open and filtered.
    [[nodiscard]] Interaction const& interaction() const noexcept;

    /// @return The id of the tab it is served in.
    [[nodiscard]] std::string_view tab() const noexcept;

    /// @return Where the session ids of the agents it dispatches come from.
    [[nodiscard]] IUuidSource const& ids() const noexcept;

    /// @return The index into BandBreakpoints the last frame was laid out by; std::nullopt before it.
    [[nodiscard]] std::optional<std::size_t> breakpoint() const noexcept;

    struct State;

  private:
    std::unique_ptr<State> _state;
};

} // namespace orchestrator::view
