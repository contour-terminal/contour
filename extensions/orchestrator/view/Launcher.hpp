// SPDX-License-Identifier: Apache-2.0
//
// The launcher (`n`): one popup that browses the pull requests and issues and starts an agent on one of them,
// or on a topic typed into it.
#pragma once

#include <core/tui/Canvas.hpp>
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
#include <orchestrator/view/DispatchForm.hpp>
#include <orchestrator/view/HitMap.hpp>

namespace orchestrator::view
{

/// Where a candidate is listed, in the order the groups are shown.
enum class CandidateGroup : uint8_t
{
    Topic = 0,      ///< A topic on what was typed.
    NeedsAttention, ///< One of your pull requests whose checks fail or whose reviewer asked for changes.
    MyPullRequests, ///< One of your other pull requests.
    Issues,         ///< An issue without a pull request, from the forge or Jira.
};

/// One row the launcher offers: an item the engine lists, or a topic on the query.
struct Candidate
{
    CandidateGroup group = CandidateGroup::Topic; ///< Where it is listed.
    std::string repo = {};                        ///< Its repository; empty for the topic row.
    ItemRef item = {};                            ///< The item; a TopicRef for the topic row.
    std::string url = {};                         ///< Its web page; empty for the topic row.
    std::string title = {};                       ///< Its title, sanitised; the topic row's: the query.
    CiState ci = CiState::None;                   ///< A pull request's checks.
    ReviewState review = ReviewState::None;       ///< A pull request's review.
    std::vector<std::string> failingChecks = {};  ///< The names of its failing checks.
    std::string activeRun = {};                   ///< The id of an active run on the item; empty: none.
    [[nodiscard]] bool operator==(Candidate const&) const = default;
};

/// What the launcher lists for @p query.
/// @param engine Its lists and runs.
/// @param query What was typed; blanks around it are ignored.
/// @return With an empty query, every item: those that need attention, then the other pull requests, then
///         the issues, each in the engine's order. Otherwise the items whose `#<number> <title>` (or Jira key
///         and title) holds the query, case folded: one the query names exactly (its number, `#number`, URL
///         or Jira key) first, then the topic row on the query, then the rest in group order.
[[nodiscard]] std::vector<Candidate> candidatesOf(Engine const& engine, std::string_view query);

/// Where the launcher's keys go.
enum class LauncherFocus : uint8_t
{
    List = 0, ///< Typing edits the query; ↑/↓ move the highlight.
    Settings, ///< ←/→ change the setting the form's focus names: task, model, repository or profile.
};

/// How much of what a dispatch runs the launcher shows.
enum class LauncherPreview : uint8_t
{
    Short = 0, ///< The checkout and the worktree.
    Full,      ///< Also the prompt and the command line (Ctrl+P).
};

/// An open launcher.
struct LauncherState
{
    std::string query = {};                    ///< What was typed.
    std::size_t highlighted = 0;               ///< Into candidatesOf(engine, query).
    LauncherFocus focus = LauncherFocus::List; ///< Where the keys go.
    FormField setting = FormField::Task;       ///< The setting ←/→ change while the focus is on the settings.
    /// What Enter would dispatch: the highlighted candidate with its settings; none when nothing is listed or
    /// no task applies to it.
    std::optional<DispatchFormState> form = {};
    std::string session = {};                         ///< The agent's session UUID, minted as it opened.
    HooksWarning hooks =
        HooksWarning::None; ///< How far the user got past the warning of an incomplete setup.
    LauncherPreview preview = LauncherPreview::Short; ///< How much it shows.
    [[nodiscard]] bool operator==(LauncherState const&) const = default;
};

/// Opens the launcher.
/// @param engine Its lists and configuration.
/// @param query What it starts with typed: empty to browse, or a finished topic run's text.
/// @param session The agent's session UUID.
/// @return The launcher, its first candidate highlighted; std::nullopt when the engine is not configured.
[[nodiscard]] std::optional<LauncherState> openLauncher(Engine const& engine,
                                                        std::string query,
                                                        std::string session);

/// Why the launcher did not open on an item.
enum class LauncherRefusal : uint8_t
{
    NotConfigured, ///< The orchestrator is not configured.
    NotListed,     ///< The engine does not list the item (any more).
    NoTask,        ///< No task applies to the item.
};

/// Opens the launcher on an item, as Enter or `d` on its row, or a task's key, ask.
/// @param engine Its lists and configuration.
/// @param repo The item's repository.
/// @param item The item.
/// @param task The task to start on while it applies to the item, such as a finished run's; else the first
///        task that does.
/// @param session The agent's session UUID.
/// @return The launcher with the item's label typed and the item highlighted, or why it cannot open.
[[nodiscard]] std::expected<LauncherState, LauncherRefusal> openLauncherOn(Engine const& engine,
                                                                           std::string_view repo,
                                                                           ItemRef const& item,
                                                                           std::string_view task,
                                                                           std::string session);

/// Types a paste into the launcher's query, as one line: a paste is text, never keys.
/// @param launcher The open launcher; nothing happens when none is.
/// @param engine The engine it was opened with.
/// @param text What was pasted, already folded to one line (pastedText()).
void pasteIntoLauncher(std::optional<LauncherState>& launcher, Engine const& engine, std::string_view text);

/// Applies one key to an open launcher. Esc closes it, and so does a key with Alt (an Esc and the key after
/// it, read together); Ctrl+P shows more or less of what a dispatch runs; any other Ctrl chord does nothing.
/// Printable keys and Backspace edit the query; ↑/↓ move the highlight; Tab moves through the settings and
/// back; ←/→ change one. Enter starts the agent, unless the engine would refuse it (the launcher shows why);
/// while the agents' setup is incomplete the first Enter only confirms the warning, as the dispatch form did.
/// @param key A key the terminal sent.
/// @param launcher The open launcher; closed (reset) when the key cancels or confirms it.
/// @param engine The engine it was opened with.
/// @param hooksAtInput The form's HooksWarning as the input that holds @p key began.
/// @param nearTab The dashboard tab it is shown in.
/// @return The dispatch, when the key confirmed it.
[[nodiscard]] std::optional<DispatchAction> applyLauncherKey(core::tui::KeyEvent const& key,
                                                             std::optional<LauncherState>& launcher,
                                                             Engine const& engine,
                                                             HooksWarning hooksAtInput,
                                                             std::string_view nearTab);

/// Highlights a candidate, as a click on it does: its settings are built afresh and the keys go to the list.
/// @param state The open launcher.
/// @param engine The engine it was opened with.
/// @param index The candidate's position in candidatesOf(engine, state.query); kept within them.
void highlightCandidate(LauncherState& state, Engine const& engine, std::size_t index);

/// The launcher, drawn in a rounded box titled `✦ New agent`: the query, the candidates in their groups with
/// their checks, review and agent columns, the settings, what the dispatch runs, the setup's warnings and the
/// keys. Shown as an overlay over the dashboard, at the size the dashboard places it at.
class LauncherView final: public core::tui::Component
{
  public:
    /// @param engine The engine it lists and previews against.
    /// @param launcher The dashboard's launcher; nullopt while none is open, when nothing is drawn.
    /// @param hits Where it records what a click on it lands on.
    LauncherView(Engine const& engine, std::optional<LauncherState> const& launcher, HitMap& hits):
        _engine { engine }, _launcher { launcher }, _hits { hits }
    {
    }
    void render(core::tui::Canvas& canvas) override;
    /// @return The size place() gave it.
    [[nodiscard]] core::tui::Size preferredSize() const override { return _size; }
    /// Sizes it for a tab: the tab's geometry is the window's to decide, so it is set again as the tab or the
    /// launcher changes rather than fixed at construction.
    /// @param size The cells it takes.
    void place(core::tui::Size size) noexcept { _size = size; }
    /// @param width The cells it is drawn in.
    /// @return The rows it needs at that width, its box included, with up to MaxLauncherListRows of the list.
    [[nodiscard]] int heightAt(int width) const;

  private:
    Engine const& _engine;
    std::optional<LauncherState> const& _launcher;
    HitMap& _hits;
    core::tui::Size _size {};
};

/// The most rows of candidates, their group headings included, the launcher shows at once.
inline constexpr int MaxLauncherListRows = 14;

} // namespace orchestrator::view
