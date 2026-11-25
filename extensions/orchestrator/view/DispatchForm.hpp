// SPDX-License-Identifier: Apache-2.0
#pragma once

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
    std::string worker = {};              ///< The worker, this run only; empty: the task's.
    std::optional<std::string> note = {}; ///< The user's note; absent when none was typed.
    std::string session = {};             ///< The agent's fresh session UUID.
    std::string nearTab = {};             ///< The dashboard tab the form was confirmed in.
    [[nodiscard]] bool operator==(DispatchAction const&) const = default;
};

/// The form's field that ←/→ and typed text go to.
enum class FormField : uint8_t
{
    Task = 0,    ///< ←/→ cycle the tasks that apply to the item.
    Model,       ///< ←/→ cycle the configured models.
    Note,        ///< An item's note: a text field.
    Repo,        ///< A topic form's: ←/→ cycle the configured repositories.
    Permissions, ///< ←/→ cycle the permission profiles.
    Worker,      ///< ←/→ cycle the workers.
};

/// How far the user got past the warning that the agents' setup is incomplete (status hooks or a plugin).
enum class HooksWarning : uint8_t
{
    None = 0,  ///< No warning was shown: the hooks were installed as the form opened.
    Shown,     ///< The warning is shown; the first Enter confirms it.
    Confirmed, ///< Confirmed: an Enter in a later input dispatches.
};

/// An open dispatch form: what it dispatches, and where the user is in it.
struct DispatchFormState
{
    std::string repo = {};       ///< The repository's configured name.
    ItemRef item = {};           ///< The item.
    std::string url = {};        ///< The item's web page, as last listed.
    std::string title = {};      ///< The item's title, as last listed, sanitised.
    std::size_t task = 0;        ///< The task: an index into Config::tasks(), one that applies to the item.
    std::size_t model = 0;       ///< The model: an index into Config::models().
    std::size_t permissions = 0; ///< The permission profile: an index into Config::profiles().
    std::size_t worker = 0;      ///< The worker this run starts on: an index into Config::workers().
    std::string note = {};       ///< An item's note, added to its prompt; empty: none.
    std::string topic = {};      ///< A topic's text; empty for an item.
    std::string session = {};    ///< The agent's session UUID, minted as the form opens.
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

/// @param form An open form.
/// @param engine The engine.
/// @return The worker the form runs on (DispatchFormState::worker); nullptr while the engine has no
///         configuration.
[[nodiscard]] WorkerDef const* workerOf(DispatchFormState const& form, Engine const& engine) noexcept;

/// @param engine The engine.
/// @param worker The worker a dispatch runs on.
/// @return Whether @p worker's setup lacks something a run needs: the status hooks of its set, or a plugin.
///         A dispatch then asks for a second Enter. An agent that reads no Claude Code state lacks nothing.
[[nodiscard]] bool isSetupIncomplete(Engine const& engine, WorkerDef const& worker) noexcept;

/// @param form A dispatch's choices.
/// @param engine The engine they would dispatch through.
/// @return The warning a form opened on @p form shows: Shown while its worker's setup is incomplete
///         (isSetupIncomplete()), else None.
[[nodiscard]] HooksWarning setupWarningOf(DispatchFormState const& form, Engine const& engine) noexcept;

/// Whether an Enter gets past the warning that the worker's setup is incomplete.
enum class SetupGate : uint8_t
{
    Held = 0, ///< It only confirmed the warning, or belongs to the input that did: nothing dispatches.
    Passed,   ///< The setup is complete, or the warning was confirmed by an earlier input.
};

/// Applies an Enter to the incomplete-setup warning: the first Enter confirms it, and only an Enter of a
/// later input passes it, so that a paste or a held key cannot click through it.
/// @param form The choices confirmed.
/// @param engine The engine they would dispatch through.
/// @param hooks The form's warning; set Confirmed by the first Enter.
/// @param hooksAtInput @p hooks as the input that holds the Enter began.
/// @return Whether the Enter dispatches.
[[nodiscard]] SetupGate passSetupWarning(DispatchFormState const& form,
                                         Engine const& engine,
                                         HooksWarning& hooks,
                                         HooksWarning hooksAtInput) noexcept;

/// @param form The choices.
/// @param engine The engine they would dispatch through.
/// @param hooks The form's warning.
/// @return What Enter does now, as a hint: `enter start`, or past an incomplete setup `enter confirms the
///         warning` and then `enter starts it anyway`.
[[nodiscard]] std::string_view enterHint(DispatchFormState const& form,
                                         Engine const& engine,
                                         HooksWarning hooks) noexcept;

/// @param text Text that names a previewed run's id, such as its worktree.
/// @param run The previewed run.
/// @return @p text with the run's stamp, known only once the dispatch is confirmed, shown as `…`.
[[nodiscard]] std::string withoutStamp(std::string_view text, Run const& run);

/// @param run A previewed run.
/// @return How the dispatch checks the item out into the run's worktree.
[[nodiscard]] std::string checkoutText(Run const& run);

/// @param engine The engine.
/// @param worker The worker a dispatch runs on.
/// @return What @p worker's setup lacks, one line each: its set's status hooks, and each plugin its set
///         lacks; none for an agent that reads no Claude Code state.
[[nodiscard]] std::vector<std::string> setupWarnings(Engine const& engine, WorkerDef const& worker);

/// Opens a form on an item as a caller found it listed, such as a Work board row.
/// @param engine The engine: its configuration and its runs.
/// @param repo The repository's configured name.
/// @param item The item.
/// @param url The item's web page, as listed.
/// @param title The item's title, as listed, sanitised.
/// @param task The id of the task the form starts on; it must apply to the item.
/// @param session The agent's session UUID.
/// @return The form, on the task's own model and its default worker (defaultWorker()); std::nullopt when the
///         engine is not configured or the task does not apply to the item.
[[nodiscard]] std::optional<DispatchFormState> openItemForm(Engine const& engine,
                                                            std::string_view repo,
                                                            ItemRef const& item,
                                                            std::string url,
                                                            std::string title,
                                                            std::string_view task,
                                                            std::string session);

/// The choices for a topic: what the `n` box starts.
/// @param engine The engine: its configuration and whether the hooks are installed.
/// @param repo The repository's configured name; one the configuration lacks starts on its first.
/// @param topic The topic's text to start with; empty for a new one.
/// @param session The agent's session UUID.
/// @return The form, on the first task that applies to topics, its model and its default worker
///         (defaultWorker()); std::nullopt when the engine is not configured or configures no repository.
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

/// @param form A dispatch's choices.
/// @param engine The engine they would dispatch through.
/// @return Whether confirming them now dispatches: not while a note or a topic starts with `-`, nor while the
///         engine would refuse.
[[nodiscard]] bool canConfirm(DispatchFormState const& form, Engine const& engine);

/// Changes one of a dispatch's settings by a step, wrapping around: the task (among those that apply to the
/// item, bringing its own model, profile and default worker), the model, a topic's repository, the profile,
/// or the worker. The note is text, and is not stepped.
/// @param form The choices.
/// @param field Which setting.
/// @param step 1 for the next value, -1 for the one before.
/// @param engine The engine whose configuration lists the values.
void changeSetting(DispatchFormState& form, FormField field, int step, Engine const& engine);

} // namespace orchestrator::view
