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
#include <orchestrator/core/NavKeys.hpp>
#include <orchestrator/core/WorkItems.hpp>
#include <orchestrator/view/DispatchForm.hpp>
#include <orchestrator/view/HitMap.hpp>

namespace orchestrator::view
{

/// Whether a box's text field has the keys.
enum class FieldMode : uint8_t
{
    Navigating = 0, ///< The keys move between the box's settings: letters may act.
    Typing,         ///< The text field has the keys: letters type.
};

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
    FieldMode mode = FieldMode::Navigating;       ///< Typing: the note has the keys.
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

/// Applies one key (Decision 9's "Start dialog / `n` box" column). @p nav is what the key resolved to in the
/// dialog's context (navContextOf()), or what the mouse asked (Dashboard::State::press(NavAction)).
/// - `nav == Close` closes it, whatever the mode.
/// - Navigating: move_up/move_down step between Task, Worker, Model, Perms and Note, stopping at the ends;
///   first/last go to Task/Note; previous_value/next_value step the setting; enter_field types the note, but
///   `⏎` off the Note line confirms; Ctrl+P toggles the preview; no printable or line-editing key acts;
///   `Esc`, and a key with Alt, do what `esc` does there, and close the dialog where `esc` is bound to
///   nothing there.
/// - Typing (on Note): the line-editing and printable keys edit the note, `⏎` confirms, leave_field (or a key
///   with Alt, as `esc`) turns to Navigating on Note, Ctrl+P toggles the preview; a plain special key acts as
///   it would navigating (passedKeyAction()), and no chord acts.
/// - Tab/Shift+Tab, in either mode, move to the next/previous setting, wrapping (Perms is passed over for a
///   worker that takes no profile): arriving on Note types, arriving elsewhere navigates.
/// A move by a navigation key that leaves Note stops typing; one that arrives on Note only selects it.
/// ⏎ dispatches unless canConfirm() is false (the engine's refusal is shown) or the worker's setup is
/// incomplete and the warning was not confirmed by an earlier input (the first ⏎ only confirms).
/// A task's configured key selects no task here: a navigation key always acts as navigation.
/// @param key A key the terminal sent.
/// @param nav What @p key resolved to; std::nullopt: nothing.
/// @param dialog The open dialog; closed (reset) when the key cancels or confirms it.
/// @param engine The engine it was opened with.
/// @param hooksAtInput The dialog's HooksWarning as the input that holds @p key began.
/// @param nearTab The dashboard tab it is shown in.
/// @return The dispatch, when the key confirmed it.
[[nodiscard]] std::optional<DispatchAction> applyStartDialogKey(core::tui::KeyEvent const& key,
                                                                std::optional<NavAction> nav,
                                                                std::optional<StartDialogState>& dialog,
                                                                Engine const& engine,
                                                                HooksWarning hooksAtInput,
                                                                std::string_view nearTab);

/// Picks a setting as a click on it does: on Note the note is typed, elsewhere the dialog navigates.
/// @param dialog The open dialog.
/// @param setting The setting picked.
void pickStartDialogSetting(StartDialogState& dialog, FormField setting) noexcept;

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
