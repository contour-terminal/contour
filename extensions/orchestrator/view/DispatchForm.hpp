// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/tui/Component.hpp>
#include <core/tui/InputEvent.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <orchestrator/core/Engine.hpp>
#include <orchestrator/core/Model.hpp>

namespace orchestrator::view
{

/// What the user confirmed in the dispatch form: the guest steps it as the engine's Dispatch, stamped with
/// the time it arrived.
struct DispatchAction
{
    std::string repo = {};                ///< The repository's configured name.
    ItemRef item = {};                    ///< The item to work on; a topic's slug is the engine's to make.
    std::string url = {};                 ///< The item's web page; empty for a topic.
    std::string topic = {};               ///< A topic's text; empty for any other item.
    std::string task = {};                ///< The task's id.
    std::string model = {};               ///< The model.
    std::string permissions = {};         ///< The permission profile.
    std::optional<std::string> note = {}; ///< The user's note; absent when none was typed.
    std::string session = {};             ///< The agent's fresh session UUID.
    std::string nearTab = {};             ///< The dashboard tab the form was confirmed in.
    [[nodiscard]] bool operator==(DispatchAction const&) const = default;
};

/// The form's field that ←/→ and typed text go to.
enum class FormField : uint8_t
{
    Task = 0, ///< ←/→ cycle the tasks that apply to the item.
    Model,    ///< ←/→ cycle the configured models.
    Note,     ///< Typed text extends the note, or a topic form's topic; Backspace shortens it.
    Repo,     ///< A topic form's: ←/→ cycle the configured repositories.
    Permissions, ///< ←/→ cycle the permission profiles.
};

/// How far the user got past the warning that Claude Code's status hooks are missing.
enum class HooksWarning : uint8_t
{
    None = 0,  ///< No warning was shown: the hooks were installed as the form opened.
    Shown,     ///< The warning is shown; the first Enter confirms it.
    Confirmed, ///< Confirmed: an Enter in a later input dispatches.
};

/// An open dispatch form: what it dispatches, and where the user is in it.
struct DispatchFormState
{
    std::string repo = {};    ///< The repository's configured name.
    ItemRef item = {};        ///< The item.
    std::string url = {};     ///< The item's web page, as last listed.
    std::string title = {};   ///< The item's title, as last listed, sanitised.
    std::size_t task = 0;     ///< The task: an index into Config::tasks(), one that applies to the item.
    std::size_t model = 0;    ///< The model: an index into Config::models().
    std::size_t permissions = 0; ///< The permission profile: an index into Config::profiles().
    std::string note = {};    ///< What the user typed: the note, or a topic form's topic; empty: none.
    std::string session = {}; ///< The agent's session UUID, minted as the form opens.
    FormField focus = FormField::Task;       ///< Where ←/→ and typed text go.
    HooksWarning hooks = HooksWarning::None; ///< How far the user got past the missing-hooks warning.
    [[nodiscard]] bool operator==(DispatchFormState const&) const = default;
};

/// The warning the form shows while Claude Code's status hooks are missing: what a run loses without them.
inline constexpr std::string_view HooksMissingWarning =
    "Claude Code's status hooks are not installed: agents may not show as blocked or waiting for permission "
    "(contour agent setup)";

/// @param refusal Why a dispatch was refused.
/// @return What the user reads: one line, saying what to do where there is something to do.
[[nodiscard]] std::string_view refusalText(DispatchRefusal refusal) noexcept;

/// @param run A run as a preview makes it.
/// @return The agent's command line for @p run as a preview shows it: the template substituted, with the long
///         arguments (the board's instruction, the settings) left as their placeholders.
[[nodiscard]] std::optional<std::vector<std::string>> commandShown(Run const& run);

/// @param engine The engine.
/// @return Whether the agents' setup lacks something a run needs: the status hooks, or a plugin. A dispatch
/// then
///         asks for a second Enter.
[[nodiscard]] bool isSetupIncomplete(Engine const& engine) noexcept;

/// @param text Text that names a previewed run's id, such as its worktree.
/// @param run The previewed run.
/// @return @p text with the run's stamp, known only once the dispatch is confirmed, shown as `…`.
[[nodiscard]] std::string withoutStamp(std::string_view text, Run const& run);

/// @param run A previewed run.
/// @return How the dispatch checks the item out into the run's worktree.
[[nodiscard]] std::string checkoutText(Run const& run);

/// @param engine The engine.
/// @return What the agents' setup lacks, one line each: the status hooks, and each missing plugin.
[[nodiscard]] std::vector<std::string> setupWarnings(Engine const& engine);

/// Opens a form on an item the engine lists.
/// @param engine The engine: its configuration, its lists and whether the hooks are installed.
/// @param repo The repository's configured name.
/// @param item The item.
/// @param task The id of the task the form starts on; it must apply to the item.
/// @param session The agent's session UUID.
/// @return The form, on the task's own model; std::nullopt when the engine is not configured, does not list
///         the item, or the task does not apply to it.
[[nodiscard]] std::optional<DispatchFormState> openDispatchForm(Engine const& engine,
                                                                std::string_view repo,
                                                                ItemRef const& item,
                                                                std::string_view task,
                                                                std::string session);

/// Opens a form on a topic, focused on its text: the dashboard's `n`, and `d` on a finished topic run.
/// @param engine The engine: its configuration and whether the hooks are installed.
/// @param repo The repository's configured name; one the configuration lacks starts on its first.
/// @param topic The topic's text to start with; empty for a new one.
/// @param session The agent's session UUID.
/// @return The form, on the first task that applies to topics and its model; std::nullopt when the engine is
///         not configured or configures no repository.
[[nodiscard]] std::optional<DispatchFormState> openTopicForm(Engine const& engine,
                                                             std::string_view repo,
                                                             std::string topic,
                                                             std::string session);

/// @param form An open form.
/// @param engine The engine it was opened with.
/// @param nearTab The dashboard tab it is shown in.
/// @return What confirming it now would send.
[[nodiscard]] DispatchAction actionOf(DispatchFormState const& form,
                                      Engine const& engine,
                                      std::string_view nearTab);

/// @param form An open form.
/// @param engine The engine it was opened with.
/// @return What confirming it now would start, or why the engine would refuse it.
[[nodiscard]] std::expected<DispatchPreview, DispatchRefusal> previewOf(DispatchFormState const& form,
                                                                        Engine const& engine);

/// Applies one key to an open form. Esc closes it, and so does a key with Alt (an Esc and the key after it,
/// read together); a Ctrl chord does nothing; Tab and ↑/↓ move between the fields; ←/→ change the task or the
/// model; text and Backspace edit the note; Enter confirms. Enter does nothing while the note starts with `-`
/// or the engine would refuse the dispatch; while the hooks are missing, the first Enter only confirms the
/// warning, and an Enter dispatches only when the confirmation came in an earlier input.
/// @param key A key the terminal sent.
/// @param form The open form; closed (reset) when the key cancels or confirms it.
/// @param engine The engine it was opened with.
/// @param hooksAtInput The form's HooksWarning as the input that holds @p key began: two Enters in one read,
///        such as a paste, confirm the warning and nothing more.
/// @param nearTab The dashboard tab it is shown in.
/// @return The dispatch, when the key confirmed the form.
[[nodiscard]] std::optional<DispatchAction> applyFormKey(core::tui::KeyEvent const& key,
                                                         std::optional<DispatchFormState>& form,
                                                         Engine const& engine,
                                                         HooksWarning hooksAtInput,
                                                         std::string_view nearTab);

} // namespace orchestrator::view
