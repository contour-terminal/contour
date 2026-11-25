// SPDX-License-Identifier: Apache-2.0
//
// The start dialog: `⏎` on a Work board row opens it over the board, on the task the row's state suggests and
// the worker the busy rule picks, and starts an agent on the item once confirmed.
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
#include <orchestrator/core/WorkItems.hpp>
#include <orchestrator/view/DispatchForm.hpp>
#include <orchestrator/view/HitMap.hpp>

namespace orchestrator::view
{

/// How much of what a dispatch runs the dialog shows.
enum class DialogPreview : uint8_t
{
    Short = 0, ///< The checkout and the worktree.
    Full,      ///< Also the prompt and the command line (Ctrl+P).
};

/// An open start dialog.
struct StartDialogState
{
    DispatchFormState form;                       ///< What ⏎ dispatches.
    std::string reason;                           ///< Why its task was suggested (TaskSuggestion::reason).
    FormField setting = FormField::Task;          ///< What ←/→ change.
    HooksWarning hooks = HooksWarning::None;      ///< How far the user got past the incomplete-setup warning.
    DialogPreview preview = DialogPreview::Short; ///< How much it shows.
    [[nodiscard]] bool operator==(StartDialogState const&) const = default;
};

/// Why the dialog did not open.
enum class StartRefusal : uint8_t
{
    NotConfigured = 0, ///< The orchestrator is not configured.
    NotListed,         ///< The engine does not list the item (any more).
    NoTask,            ///< No task applies to the item.
};

/// @param refusal Why the dialog did not open.
/// @param label The item's label, such as `#2375`.
/// @return What the action bar says.
[[nodiscard]] std::string startRefusalNotice(StartRefusal refusal, std::string_view label);

/// @param engine The engine: its configuration, its lists and its runs.
/// @param item A Work board row.
/// @param task A task to start on while it applies to the item, such as a finished run's for "run again";
///        empty: the one suggestTask() suggests.
/// @param session The agent's session UUID; empty for the caller to mint.
/// @return The dialog on @p item: the suggested task (or @p task while it applies), its model,
///         defaultWorker(), and the reason; or why it cannot open.
[[nodiscard]] std::expected<StartDialogState, StartRefusal> openStartDialog(Engine const& engine,
                                                                            WorkItem const& item,
                                                                            std::string_view task,
                                                                            std::string session);

/// Applies one key: Tab moves Task → Worker → Model → Perms → Note → Task (Shift+Tab back; Perms is passed
/// over for a worker that takes no profile); ←/→ step the setting; typing edits the note; Ctrl+P toggles the
/// preview; Esc, or a key with Alt, closes; ⏎ dispatches unless canConfirm() is false (the engine's refusal
/// is shown) or the worker's setup is incomplete and the warning was not confirmed by an earlier input (the
/// first ⏎ only confirms).
/// @param key A key the terminal sent.
/// @param dialog The open dialog; closed (reset) when the key cancels or confirms it.
/// @param engine The engine it was opened with.
/// @param hooksAtInput The dialog's HooksWarning as the input that holds @p key began.
/// @param nearTab The dashboard tab it is shown in.
/// @return The dispatch, when the key confirmed it.
[[nodiscard]] std::optional<DispatchAction> applyStartDialogKey(core::tui::KeyEvent const& key,
                                                                std::optional<StartDialogState>& dialog,
                                                                Engine const& engine,
                                                                HooksWarning hooksAtInput,
                                                                std::string_view nearTab);

/// Types a paste into the dialog's note, as one line: a paste is text, never keys.
/// @param dialog The open dialog; nothing happens when none is.
/// @param text What was pasted, already folded to one line (pastedText()).
void pasteIntoStartDialog(std::optional<StartDialogState>& dialog, std::string_view text);

struct StartDialogLayout;

/// The dialog, in a rounded box titled `▶ Start agent`: the item, Task (with `‹ ›` and the reason), Worker
/// (each worker as a chip `name n` with its active runs, `(setup)` when isSetupIncomplete, `(no profile)`
/// when takesNoProfileFor(task, worker)), Model, Perms, Note, the checkout and worktree, the setup warnings,
/// `⬢ <worker> already works on this` when an active run is on the item, and with Full the prompt and command
/// line. Shown as an overlay over the dashboard, at the size the dashboard places it at.
class StartDialogView final: public core::tui::Component
{
  public:
    /// @param engine The engine it previews against.
    /// @param dialog The dashboard's dialog; nullopt while none is open, when nothing is drawn.
    /// @param hits Where it records what a click on it lands on.
    StartDialogView(Engine const& engine, std::optional<StartDialogState> const& dialog, HitMap& hits);
    ~StartDialogView() override;
    StartDialogView(StartDialogView const&) = delete;
    StartDialogView& operator=(StartDialogView const&) = delete;
    StartDialogView(StartDialogView&&) = delete;
    StartDialogView& operator=(StartDialogView&&) = delete;
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
    /// @return Its lines at @p width: built again only once the dialog, the engine or the width changed.
    [[nodiscard]] StartDialogLayout const& layoutAt(int width) const;

    struct LayoutCache;

    Engine const& _engine;
    std::optional<StartDialogState> const& _dialog;
    mutable std::unique_ptr<LayoutCache> _cache; ///< The last layout, and what it was built from.
    HitMap& _hits;
    core::tui::Size _size {};
};

/// The widest the start dialog is drawn.
inline constexpr int StartDialogMaxColumns = 100;

} // namespace orchestrator::view
