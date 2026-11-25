// SPDX-License-Identifier: Apache-2.0
//
// The `n` box: a custom task. The user types what the agent should do, and ⏎ starts it at once as a topic, on
// the defaults: the repository chip's repository (else the selected row's, else the first), the topic task's
// worker by the busy rule, and its model. Tab reaches those three settings.
#pragma once

#include <core/tui/Canvas.hpp>
#include <core/tui/Component.hpp>
#include <core/tui/InputEvent.hpp>

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <orchestrator/core/Engine.hpp>
#include <orchestrator/view/DispatchForm.hpp>
#include <orchestrator/view/HitMap.hpp>
#include <orchestrator/view/StartDialog.hpp>

namespace orchestrator::view
{

/// Where the `n` box's keys go.
enum class TaskBoxField : uint8_t
{
    Text = 0, ///< "What should the agent do?": ←/→ change nothing.
    Repo,     ///< ←/→ step the repository.
    Worker,   ///< ←/→ step the worker.
    Model,    ///< ←/→ step the model.
};

/// An open `n` box.
struct TaskBoxState
{
    std::string text = {};                   ///< "What should the agent do?"
    TaskBoxField focus = TaskBoxField::Text; ///< What ←/→ change.
    DispatchFormState form; ///< A topic form: repository, worker, model; its topic is @ref text.
    HooksWarning hooks = HooksWarning::None; ///< As the dialog has it: a second ⏎ past an incomplete setup.
    [[nodiscard]] bool operator==(TaskBoxState const&) const = default;
};

/// @param engine The engine: its configuration and its runs.
/// @param chipRepo The repository the Work view's chip names; empty: every one.
/// @param selectedRepo The selected row's repository; empty: none.
/// @param text The text it opens with: a finished topic run's, or empty.
/// @param session The agent's session UUID; empty for the caller to mint.
/// @return The box: repository = @p chipRepo while configured, else @p selectedRepo while configured, else
///         the first; worker = defaultWorker() of the topic task; model = the topic task's; @p text typed.
///         StartRefusal::NotConfigured when the engine has no configuration or no repository.
[[nodiscard]] std::expected<TaskBoxState, StartRefusal> openTaskBox(Engine const& engine,
                                                                    std::string_view chipRepo,
                                                                    std::string_view selectedRepo,
                                                                    std::string text,
                                                                    std::string session);

/// @param field A place the box's keys go.
/// @return The form's setting ←/→ step there; std::nullopt for the text, which has none.
[[nodiscard]] std::optional<FormField> taskBoxSettingOf(TaskBoxField field) noexcept;

/// @param box An open box.
/// @return What it dispatches: its form, its topic the text typed.
[[nodiscard]] DispatchFormState formOf(TaskBoxState const& box);

/// Applies one key: typing and the line-editing keys edit the text wherever the keys are; Tab moves Text →
/// Repo → Worker → Model → Text (Shift+Tab back); ←/→ step the setting; Esc, or a key with Alt, closes; ⏎
/// dispatches the text as a topic at once (nothing to choose), not while the text is blank or canConfirm() is
/// false, and past an incomplete setup only with an Enter of a later input than the one that confirmed the
/// warning.
/// @param key A key the terminal sent.
/// @param box The open box; closed (reset) when the key cancels or confirms it.
/// @param engine The engine it was opened with.
/// @param hooksAtInput The box's HooksWarning as the input that holds @p key began.
/// @param nearTab The dashboard tab it is shown in.
/// @return The dispatch, when the key confirmed it.
[[nodiscard]] std::optional<DispatchAction> applyTaskBoxKey(core::tui::KeyEvent const& key,
                                                            std::optional<TaskBoxState>& box,
                                                            Engine const& engine,
                                                            HooksWarning hooksAtInput,
                                                            std::string_view nearTab);

/// Types a paste into the box's text, as one line: a paste is text, never keys.
/// @param box The open box; nothing happens when none is.
/// @param text What was pasted, already folded to one line (pastedText()).
void pasteIntoTaskBox(std::optional<TaskBoxState>& box, std::string_view text);

struct TaskBoxLayout;

/// The box, in a rounded box titled `+ New task`: "What should the agent do?" and the text typed, then Repo,
/// Worker (with its active runs, `(setup)` when isSetupIncomplete) and Model, the branch and worktree a
/// dispatch would make or the engine's refusal, the setup warnings, and the hint line. Shown as an overlay
/// over the dashboard, at the size the dashboard places it at.
class TaskBoxView final: public core::tui::Component
{
  public:
    /// @param engine The engine it previews against.
    /// @param box The dashboard's box; nullopt while none is open, when nothing is drawn.
    /// @param hits Where it records what a click on it lands on.
    TaskBoxView(Engine const& engine, std::optional<TaskBoxState> const& box, HitMap& hits);
    ~TaskBoxView() override;
    TaskBoxView(TaskBoxView const&) = delete;
    TaskBoxView& operator=(TaskBoxView const&) = delete;
    TaskBoxView(TaskBoxView&&) = delete;
    TaskBoxView& operator=(TaskBoxView&&) = delete;
    void render(core::tui::Canvas& canvas) override;
    /// @return The size place() gave it.
    [[nodiscard]] core::tui::Size preferredSize() const override { return _size; }
    /// Sizes it for a tab: the tab's geometry is the window's to decide.
    /// @param size The cells it takes.
    void place(core::tui::Size size) noexcept { _size = size; }
    /// @param width The cells it is drawn in.
    /// @return The rows it needs at that width, its box included.
    [[nodiscard]] int heightAt(int width) const;

  private:
    /// @param width The cells its lines take.
    /// @return Its lines at @p width: built again only once the box, the engine or the width changed.
    [[nodiscard]] TaskBoxLayout const& layoutAt(int width) const;

    struct LayoutCache;

    Engine const& _engine;
    std::optional<TaskBoxState> const& _box;
    mutable std::unique_ptr<LayoutCache> _cache; ///< The last layout, and what it was built from.
    HitMap& _hits;
    core::tui::Size _size {};
};

/// The widest the `n` box is drawn.
inline constexpr int TaskBoxMaxColumns = 80;

} // namespace orchestrator::view
