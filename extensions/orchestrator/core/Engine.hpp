// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <orchestrator/core/Commands.hpp>
#include <orchestrator/core/Config.hpp>
#include <orchestrator/core/Finished.hpp>
#include <orchestrator/core/Hooks.hpp>
#include <orchestrator/core/Model.hpp>
#include <orchestrator/core/Reconcile.hpp>
#include <orchestrator/core/Run.hpp>

namespace orchestrator
{

/// The most commands the engine has the host run at once; the rest wait in a queue.
inline constexpr std::size_t MaxInFlight = 4;
/// The longest the engine sleeps while a run is unfinished, so that a lost hint is made up for.
inline constexpr auto ReconcileEvery = std::chrono::milliseconds { std::chrono::seconds { 30 } };
/// How long past its timeout class a command's reply may be late before the engine abandons the call.
inline constexpr auto CallDeadlineMargin = std::chrono::milliseconds { std::chrono::seconds { 30 } };
/// The longest a command of the short timeout class runs.
inline constexpr auto ShortTimeout = std::chrono::milliseconds { std::chrono::seconds { 30 } };
/// The longest a command of the long timeout class runs.
inline constexpr auto LongTimeout = std::chrono::milliseconds { std::chrono::minutes { 10 } };
/// The longest the host takes to open a tab.
inline constexpr auto PaneOpenTimeout = std::chrono::milliseconds { std::chrono::seconds { 30 } };
/// The longest a failing poll's interval grows to.
inline constexpr auto MaxPollBackoff = std::chrono::milliseconds { std::chrono::minutes { 30 } };
/// The worktree listings of a repository that fail in a row before its main checkout counts as not to be
/// known: a run waiting for it then keeps its worktree unchecked rather than wait on. Each failure costs one
/// poll interval, and that interval backs off up to MaxPollBackoff while the forge fails too, so three bound
/// the wait before an honest "not known" to about an hour at worst: one failure could be a passing hiccup,
/// and many more would leave a run hanging in its ending for hours.
inline constexpr uint32_t MaxWorktreeListFailures = 3;

/// Whether a process started.
enum class StartResult : uint8_t
{
    Started,    ///< It ran.
    CwdMissing, ///< Its working directory does not exist.
    Refused,    ///< The host refused it.
};

/// How a started process ended.
enum class Completion : uint8_t
{
    Exited,   ///< It exited by itself.
    TimedOut, ///< Its timeout killed it.
};

/// What became of a command the engine asked the host to run.
struct ProcessOutcome
{
    StartResult start = StartResult::Started;   ///< Whether it started.
    Completion completion = Completion::Exited; ///< How it ended.
    std::optional<int32_t> exitCode;            ///< Its exit code; std::nullopt when it had none.
    OutputExtent extent = OutputExtent::Whole;  ///< Whether its output is whole.
    std::string stdoutText;                     ///< Its standard output.
    std::string stderrText;                     ///< Its standard error.
};

/// One entry of the extension's store.
struct StoredEntry
{
    std::string key;   ///< Its key, such as `run/<id>`.
    std::string value; ///< Its value.
};

/// A command that finished while the guest was away.
struct FinishedResult
{
    std::string tag;        ///< The tag it was started with.
    ProcessOutcome outcome; ///< What became of it.
};

/// Everything the engine needs, in one value: there is no Recovered input and no call order to remember.
struct StartContext
{
    uint64_t monoMs = 0;                            ///< Now, in monotonic milliseconds; greater than 0.
    uint64_t wallMs = 0;                            ///< Now, in wall-clock milliseconds.
    HostInfo host;                                  ///< What the host resolved.
    std::string configJson;                         ///< The `config` subtree, as JSON.
    std::vector<StoredEntry> stored;                ///< The store's entries.
    std::vector<std::string> inFlight;              ///< The tags of the commands the host still runs.
    std::vector<FinishedResult> finished;           ///< The commands that finished while the guest was away.
    std::vector<PaneFact> panes;                    ///< The listed panes tagged with a run, listed now.
    PaneListing paneListing = PaneListing::Unknown; ///< Where the tags of @ref panes come from.
};

// {{{ inputs

/// Time passed: the engine polls, reads status boards, and asks for its next wake.
struct Wake
{
    uint64_t monoMs = 0; ///< Now, in monotonic milliseconds.
    uint64_t wallMs = 0; ///< Now, in wall-clock milliseconds.
};

/// A command the engine started finished.
struct ProcessFinished
{
    std::string tag;        ///< The tag it was started with.
    ProcessOutcome outcome; ///< What became of it.
    uint64_t monoMs = 0;    ///< When it arrived, in monotonic milliseconds, read by the driver; 0: unknown.
};

/// A file the engine asked for was read.
struct FileRead
{
    std::string tag;                                   ///< The tag it was asked for with.
    std::expected<std::string, FileReadError> content; ///< Its bytes, or why they could not be read.
    uint64_t monoMs = 0; ///< When it arrived, in monotonic milliseconds, read by the driver; 0: unknown.
};

/// The panes were listed.
struct PanesListed
{
    std::vector<PaneFact> panes;  ///< Every listed pane tagged with a run.
    uint64_t requestedMonoMs = 0; ///< When the listing was asked for, in monotonic milliseconds; never 0.
    PaneListing listing = PaneListing::Unknown; ///< Where the tags of @ref panes come from.
};

/// A run's agent tab opened.
struct PaneOpenedFor
{
    std::string run;     ///< The run's id.
    std::string pane;    ///< The tab's id.
    uint64_t monoMs = 0; ///< When it arrived, in monotonic milliseconds, read by the driver; 0: unknown.
};

/// A run's agent tab could not be opened.
struct PaneOpenFailedFor
{
    std::string run;     ///< The run's id.
    std::string why;     ///< Untrusted; sanitised before it is kept.
    uint64_t monoMs = 0; ///< When it arrived, in monotonic milliseconds, read by the driver; 0: unknown.
};

/// The user confirmed the dispatch form.
struct Dispatch
{
    std::string repo;                ///< The repository's configured name.
    ItemRef item;                    ///< The item to work on.
    std::string url;                 ///< The item's web page.
    std::string task;                ///< The task's id.
    std::string model;               ///< The model; empty: the task's.
    std::optional<std::string> note; ///< The user's note, if any.
    std::string session;             ///< A fresh session UUID for the agent.
    std::optional<std::string> near; ///< The dashboard tab the form was confirmed in, if known.
    uint64_t wallMs = 0; ///< Now, in wall-clock milliseconds: what the run's names are stamped with.
    uint64_t monoMs = 0; ///< When it arrived, in monotonic milliseconds, read by the driver; 0: unknown.
};

/// The user asked to check a kept worktree again.
struct RetryRemovalOf
{
    std::string run;     ///< The run's id.
    uint64_t monoMs = 0; ///< When it arrived, in monotonic milliseconds, read by the driver; 0: unknown.
};

/// The user ended a run.
struct EndRun
{
    std::string run;     ///< The run's id.
    uint64_t monoMs = 0; ///< When it arrived, in monotonic milliseconds, read by the driver; 0: unknown.
};

/// The user dismissed a finished run.
struct DismissRun
{
    std::string run;     ///< The run's id.
    uint64_t monoMs = 0; ///< When it arrived, in monotonic milliseconds, read by the driver; 0: unknown.
};

/// The user opened a finished run.
struct MarkRunSeen
{
    std::string run;     ///< The run's id.
    uint64_t monoMs = 0; ///< When it arrived, in monotonic milliseconds, read by the driver; 0: unknown.
};

/// The user asked for the lists to be read again now.
struct RefreshNow
{
    uint64_t monoMs = 0; ///< When it arrived, in monotonic milliseconds, read by the driver; 0: unknown.
};

/// What the engine is fed.
using Input = std::variant<Wake,
                           ProcessFinished,
                           FileRead,
                           PanesListed,
                           PaneOpenedFor,
                           PaneOpenFailedFor,
                           Dispatch,
                           RetryRemovalOf,
                           EndRun,
                           DismissRun,
                           MarkRunSeen,
                           RefreshNow>;

// }}}
// {{{ effects

/// Run a declared command.
struct RunCommand
{
    std::string tag;                 ///< What its ProcessFinished carries.
    std::string_view command;        ///< The declared command's name.
    std::vector<Argument> arguments; ///< Its arguments, by placeholder.
    std::string cwd;                 ///< Its working directory, inside the command's root.
};

/// Read a file inside the worktree root.
struct ReadFile
{
    std::string tag;  ///< What its FileRead carries.
    std::string path; ///< The file.
};

/// Open a run's agent tab, tagged with the run's id.
struct OpenPane
{
    std::string run;                 ///< The run's id: the tab's tag.
    std::string_view paneTemplate;   ///< The declared pane template's name.
    std::vector<Argument> arguments; ///< Its arguments, by placeholder.
    std::string cwd;                 ///< Its working directory: the run's worktree.
    std::string title;               ///< The tab's title.
    std::optional<std::string> near; ///< The dashboard tab it opens beside (Run::near), if any.
};

/// Store a value.
struct StoreSet
{
    std::string key;   ///< The key.
    std::string value; ///< The value, at most MaxStoredRunBytes.
};

/// Remove a stored value.
struct StoreRemove
{
    std::string key; ///< The key.
};

/// Publish the extension's summary.
struct PublishSummary
{
    std::string shortText; ///< One line for the status line.
    std::string detail;    ///< The run list.
};

/// Wake the engine at a time.
struct WakeAt
{
    uint64_t monoMs = 0; ///< When, in monotonic milliseconds.
};

/// What the dashboard shows changed.
struct Redraw
{
};

/// Why a dispatch was refused.
enum class DispatchRefusal : uint8_t
{
    NotConfigured,  ///< The configuration is unusable.
    AgentLimit,     ///< `max_agents` runs are active.
    RepoLimit,      ///< `max_agents_per_repo` runs of the repository are active.
    UnknownRepo,    ///< No repository has that name.
    UnknownTask,    ///< No task has that id, or it does not apply to the item.
    UnknownModel,   ///< The model is not one of `models`.
    AlreadyRunning, ///< The item has an active run.
    BaseUnknown,    ///< The base branch is not known yet: the change request is not listed, or the default
                    ///< branch was not read and no `base` is configured.
    BadPrompt,      ///< The prompt, or another value the dispatch carries, cannot be passed on or stored.
};

/// A dispatch was refused.
struct DispatchRefused
{
    std::string repo;       ///< The repository it named.
    DispatchRefusal reason; ///< Why.
};

/// What the engine asks its driver to do, in order. A StoreSet of a run precedes every command, file read
/// and tab its new state awaits.
using Effect = std::variant<RunCommand,
                            ReadFile,
                            OpenPane,
                            ClosePane,
                            StoreSet,
                            StoreRemove,
                            PublishSummary,
                            WakeAt,
                            Redraw,
                            DispatchRefused>;

// }}}

/// What a dispatch would start, worked out as Engine::dispatch() works it out: what the form shows.
struct DispatchPreview
{
    Run run;                            ///< The run it would store, stamped with the dispatch's wall time.
    std::vector<std::string> agentArgv; ///< The agent's argv, as the pane template substitutes the run's
                                        ///< arguments (agentPaneArguments()): what `pane-open` is given.
    std::size_t active = 0;             ///< The active runs now (Engine::activeRuns()).
    std::size_t maxAgents = 0;          ///< `max_agents`.
};

/// The arguments the agent's pane template is opened with for @p run: the one place OpenPane's arguments
/// and a preview's argv come from.
/// @param run A run.
/// @return Its model, session and prompt, by placeholder.
[[nodiscard]] std::vector<Argument> agentPaneArguments(Run const& run);

/// The tag of the host call a run awaits: its stepTag() and its attempt.
/// @param run The run.
/// @return Such as "<id>/status#3"; std::nullopt when it awaits no host call.
[[nodiscard]] std::optional<std::string> callTag(Run const& run);

/// The store key a run is kept under.
/// @param runId The run's id.
/// @return `run/<runId>`.
[[nodiscard]] std::string runKey(std::string_view runId);

/// Whether a run belongs to the current configuration.
enum class RunScope : uint8_t
{
    Inside = 0, ///< Its repository is configured at the path it stored, and its worktree is where a dispatch
                ///< under this configuration puts it.
    Outside,    ///< A configuration is loaded, and the run's repository is not in it, moved, or its worktree
                ///< lies elsewhere.
    Unknown,    ///< No configuration is loaded (it was refused): nothing says the run is outside, and it is
                ///< shown and offered as one inside; the engine runs nothing until one is.
};

/// The orchestrator: every input stepped into the effects it asks for. It does no I/O and reads no clock;
/// every time is an input.
class Engine
{
  public:
    struct Started;

    /// Starts the engine from everything the host knows. Stale work is resolved here: a run whose call the
    /// host still runs waits for it, one whose call finished takes its result, any other is told its call
    /// was lost; a run waiting for its tab adopts a listed one. Stored values that cannot be read are kept
    /// and reported.
    /// @param context Everything the engine needs.
    /// @return The engine and its first effects; never fails: a bad configuration is a problem line.
    [[nodiscard]] static Started start(StartContext context);

    /// Steps one input.
    ///
    /// The driver carries the effects out in order. Driver contract:
    /// - After carrying out a Wake's effects, list the panes and feed PanesListed, whose request time is read
    ///   before the listing is asked for.
    /// - Every input carries the monotonic time the driver read when it arrived: a run it changes is stamped
    ///   with that time, which decides whether a listing asked for earlier may speak about the run.
    /// - Every RunCommand gets exactly one ProcessFinished. A command the host refuses to start (a limit, a
    ///   bad argument, an extension gone) is fed back as ProcessFinished with StartResult::Refused. Every
    ///   ReadFile gets exactly one FileRead.
    /// - Should a reply never come, the engine abandons the call once its timeout class plus
    ///   CallDeadlineMargin has passed, as if the host had refused it, and drops a reply that arrives later.
    /// - Every OpenPane gets exactly one PaneOpenedFor or PaneOpenFailedFor. Should neither come within
    ///   PaneOpenTimeout plus CallDeadlineMargin, the engine takes the opening as failed: the run closes any
    ///   tab tagged with it, then goes through the removal checks, and frees its slot.
    /// @param input What happened.
    /// @return What to do, in order.
    [[nodiscard]] std::vector<Effect> step(Input const& input);

    /// @return The runs, active and Finished, the ones outside the current configuration last.
    ///         A run outside it is left alone until the user ends it.
    [[nodiscard]] std::span<Run const> runs() const noexcept { return _runs; }
    /// @param repo A repository's name.
    /// @return The user's open change requests last listed there.
    [[nodiscard]] std::span<ChangeRequest const> changeRequests(std::string_view repo) const noexcept;
    /// @param repo A repository's name.
    /// @return Its open issues no listed change request claims.
    [[nodiscard]] std::span<Issue const> unclaimedIssues(std::string_view repo) const noexcept;
    /// @param repo A repository's name.
    /// @return Its worktrees no run accounts for.
    [[nodiscard]] std::span<WorktreeEntry const> orphans(std::string_view repo) const noexcept;
    /// @return What the user should know: configuration, store, `gh` and list problems, incomplete-list
    ///         notices, runs outside the current configuration, missing hooks and orphans, one line each.
    [[nodiscard]] std::span<std::string const> problems() const noexcept { return _problems; }
    /// @param repo A repository's name.
    /// @return Its main checkout, as its worktree listing names it; std::nullopt until it was listed.
    [[nodiscard]] std::optional<std::string_view> mainCheckoutOf(std::string_view repo) const noexcept;
    /// Why a run's removal checks could not compare its worktree against its repository's main checkout now.
    /// @param run A run.
    /// @return std::nullopt when it is known: its repository was listed, or the run's own directory stands
    /// for
    ///         it; else whether it may still become known.
    [[nodiscard]] std::optional<CheckoutUnknown> unknownCheckout(Run const& run) const noexcept;
    /// @return Whether Claude Code's status hooks are installed.
    [[nodiscard]] HooksState hooks() const noexcept { return _hooks; }
    /// @return The summary last published, as its PublishSummary carried it; empty before the first.
    [[nodiscard]] Summary const& published() const noexcept { return _published; }
    /// @return The configured repositories' names, in the configured order; empty when unconfigured.
    [[nodiscard]] std::span<std::string const> repositories() const noexcept { return _repositoryNames; }
    /// @return The configuration the engine runs with; nullptr when it was refused (a problem line says why).
    [[nodiscard]] Config const* config() const noexcept { return _config ? &*_config : nullptr; }
    /// @param run One of runs().
    /// @return Whether @p run belongs to the current configuration. One outside it stays endable, its cleanup
    ///         working from what the run stored, but its removal is never tried again.
    [[nodiscard]] RunScope scopeOf(Run const& run) const noexcept;

    /// What a dispatch would start, refused as dispatch() refuses it; it changes nothing.
    /// @param request The dispatch, as the form would send it.
    /// @return The run, the agent's argv and the limits; or why the dispatch would be refused.
    [[nodiscard]] std::expected<DispatchPreview, DispatchRefusal> preview(Dispatch const& request) const;

    /// @return How many runs count as active (countsAsActive()): what `max_agents` is checked against.
    [[nodiscard]] std::size_t activeRuns() const noexcept;

  private:
    /// Who a queued command is for: a run's go ahead of the polls.
    enum class Priority : uint8_t
    {
        Poll, ///< A list, the login check or the hooks check.
        Run,  ///< A run's step.
    };

    /// A command the host runs.
    struct InFlightCall
    {
        std::string tag;         ///< Its tag.
        uint64_t deadlineMs = 0; ///< When it is abandoned if no reply came, in monotonic milliseconds.
    };

    /// Whether a poll command was handed to the host.
    enum class Issued : uint8_t
    {
        No,  ///< It names no declared command.
        Yes, ///< It runs or waits for a slot.
    };

    /// A command waiting for a free slot.
    struct Queued
    {
        Priority priority = Priority::Poll; ///< Who it is for.
        RunCommand command;                 ///< The command.
    };

    /// Whether `gh` is logged in, as last checked.
    enum class AuthState : uint8_t
    {
        Unknown,   ///< Not checked yet.
        LoggedOut, ///< The check failed.
        LoggedIn,  ///< The check passed.
    };

    /// Whether a poll needs the login checked first.
    enum class AuthCheck : uint8_t
    {
        Skip,   ///< The last poll's lists were read.
        Needed, ///< A list failed, or the login was never confirmed.
    };

    /// How a poll went.
    enum class PollResult : uint8_t
    {
        Clean,  ///< Every command succeeded.
        Failed, ///< One failed: the next poll waits longer.
    };

    /// What the polls learnt about one repository.
    struct RepoState
    {
        std::string name;                              ///< Its configured name.
        std::vector<ChangeRequest> changeRequests;     ///< The user's open change requests.
        std::vector<std::vector<Issue>> issuesByQuery; ///< The open issues, one list per query.
        std::vector<Issue> unclaimed;                  ///< Those no change request claims.
        std::optional<std::string> defaultBranch;      ///< The forge's default branch, once read.
        std::vector<WorktreeEntry> worktrees;          ///< Its worktrees, as last listed.
        std::vector<WorktreeEntry> orphans;            ///< Those no run accounts for.
        CheckoutUnknown unknownCheckout =
            CheckoutUnknown::Pending;      ///< Why @ref worktrees names no main checkout, if they do not.
        uint32_t worktreeListFailures = 0; ///< The worktree listings in a row that failed.
    };

    Engine(std::optional<Config> config, HostInfo host, uint64_t monoMs, uint64_t wallMs);

    // Defined in Engine.cpp: runs, the queue and the step's end.
    [[nodiscard]] Run* findRun(std::string_view id) noexcept;
    [[nodiscard]] StepContext contextFor(Run const& run) const noexcept;
    void loadStore(std::span<StoredEntry const> stored);
    void resolveStaleWork(std::span<std::string const> awaited,
                          StartContext const& context,
                          std::vector<Effect>& out);
    void reconcileWith(std::span<PaneFact const> panes,
                       uint64_t requestedMonoMs,
                       PaneListing listing,
                       std::vector<Effect>& out);
    void feed(std::string_view runId, RunInput const& input, std::vector<Effect>& out);
    void apply(std::string_view runId, std::vector<RunEffect> effects, std::vector<Effect>& out);
    void onRunResult(std::string_view runId, ProcessOutcome const& outcome, std::vector<Effect>& out);
    void onProcessFinished(ProcessFinished const& finished, std::vector<Effect>& out);
    void onFileRead(FileRead const& read, std::vector<Effect>& out);
    void issue(RunCommand command, Priority priority, std::vector<Effect>& out);
    void drainQueue(std::vector<Effect>& out);
    void emit(RunCommand command, std::vector<Effect>& out);
    void abandonOverdueCalls(std::string_view answered, std::vector<Effect>& out);
    void abandonOverduePaneOpenings(std::string_view answeredRun, std::vector<Effect>& out);
    void forgetSettledPaneOpenings();
    [[nodiscard]] static bool isEndedByYou(Run const& run) noexcept;
    [[nodiscard]] bool isInFlight(std::string_view tag) const noexcept;
    [[nodiscard]] bool isPending(std::string_view tag) const noexcept;
    [[nodiscard]] bool isTracked(Run const& run) const noexcept;
    void endStep(std::vector<Effect>& out, uint64_t revision);
    [[nodiscard]] std::optional<uint64_t> nextWake() const;
    void rebuildProblems();
    void forget(std::string_view runId);

    // Defined in EngineDispatch.cpp.
    void dispatch(Dispatch const& request, std::vector<Effect>& out);
    [[nodiscard]] std::expected<Run, DispatchRefusal> prepare(Dispatch const& request) const;

    // Defined in EnginePoll.cpp: the polls, the lists and the status boards.
    void startOrResumePoll(StartContext const& context, std::vector<Effect>& out);
    void startPoll(std::vector<Effect>& out);
    void issueLists(std::vector<Effect>& out);
    void issueWorktreeLists(std::vector<Effect>& out);
    [[nodiscard]] bool isResumable(std::string_view tag) const noexcept;
    [[nodiscard]] bool isForgePoll(std::string_view tag) const noexcept;
    [[nodiscard]] Issued issuePoll(std::string tag,
                                   std::string_view command,
                                   std::vector<Argument> arguments,
                                   std::string cwd,
                                   std::vector<Effect>& out);
    void onPollResult(std::string_view tag, ProcessOutcome const& outcome, std::vector<Effect>& out);
    void onHooksResult(ProcessOutcome const& outcome);
    void onAuthResult(ProcessOutcome const& outcome, std::vector<Effect>& out);
    void onListAnswered(std::vector<Effect>& out);
    void checkHooks(std::vector<Effect>& out);
    void recheckFinished(std::vector<Effect>& out);
    void resumeCheckoutWaits(std::vector<Effect>& out);
    void readBoards(std::vector<Effect>& out);
    [[nodiscard]] std::optional<uint64_t> boardDueAt(Run const& run) const;
    void onBoardRead(std::string_view runId, FileRead const& read);
    void refreshOrphans();
    [[nodiscard]] RepoState* findRepoState(std::string_view name) noexcept;
    [[nodiscard]] RepoState const* findRepoState(std::string_view name) const noexcept;
    [[nodiscard]] uint32_t failingChangeRequests() const noexcept;

    std::optional<Config> _config;
    std::vector<std::string> _repositoryNames; ///< The configured repositories' names, in order.
    HostInfo _host;
    std::vector<Run> _runs;       ///< The runs inside the configuration first, then the ones outside it.
    std::size_t _insideCount = 0; ///< The runs the engine works on: those of this configuration, and the ones
                                  ///< outside it the user ended.
    std::vector<RepoState> _repos;
    std::vector<PaneFact> _panes; ///< As last listed.
    PaneListing _paneListing =
        PaneListing::Unknown; ///< How _panes was listed; Unknown until a listing was known.
    std::set<std::string, std::less<>> _boardsFound; ///< The runs whose STATUS.md this instance has read.
    std::vector<InFlightCall> _inFlight;
    std::vector<InFlightCall> _paneOpenings; ///< The tabs asked for and not yet answered for, by run id.
    std::deque<Queued> _queue;
    std::vector<std::string> _answeredAtStart; ///< During start(): the poll tags whose results it takes in.
    std::vector<std::string> _startProblems;   ///< The configuration's, the store's and the outside runs'.
    std::map<std::string, std::vector<std::string>, std::less<>> _pollProblems; ///< By poll tag.
    std::vector<std::string> _problems;
    HooksState _hooks = HooksState::Unknown;
    AuthState _auth = AuthState::Unknown;
    AuthCheck _authCheck = AuthCheck::Needed;
    PollResult _pollResult = PollResult::Clean;
    std::size_t _pollOutstanding = 0; ///< The poll commands not answered yet; 0: no poll runs.
    uint32_t _pollFailures = 0;       ///< The polls in a row that failed.
    uint64_t _nextPollMs = 0;
    uint64_t _monoMs = 0;
    uint64_t _wallMs = 0;
    std::optional<uint64_t> _wakeAt; ///< The last WakeAt asked for.
    Summary _published;              ///< The last summary published.
    uint64_t _revision = 0;          ///< Counts the changes the dashboard shows.
};

/// What Engine::start returns.
struct Engine::Started
{
    Engine engine;               ///< The engine.
    std::vector<Effect> effects; ///< Its first effects.
};

} // namespace orchestrator
