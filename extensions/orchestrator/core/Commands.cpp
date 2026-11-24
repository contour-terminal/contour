// SPDX-License-Identifier: Apache-2.0
#include <orchestrator/core/Commands.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <expected>
#include <initializer_list>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace std::string_view_literals;

namespace orchestrator
{

namespace
{
    // {{{ arguments

    constexpr auto ArgumentTable = std::array {
        ArgumentDecl { .name = "number", .kind = ArgumentKind::Number, .root = std::nullopt },
        ArgumentDecl { .name = "session", .kind = ArgumentKind::Uuid, .root = std::nullopt },
        ArgumentDecl { .name = "remote", .kind = ArgumentKind::Name, .root = std::nullopt },
        ArgumentDecl { .name = "model", .kind = ArgumentKind::Name, .root = std::nullopt },
        ArgumentDecl { .name = "branch", .kind = ArgumentKind::Ref, .root = std::nullopt },
        ArgumentDecl { .name = "base", .kind = ArgumentKind::Ref, .root = std::nullopt },
        ArgumentDecl { .name = "path", .kind = ArgumentKind::Path, .root = Root::Worktrees },
        ArgumentDecl { .name = "query", .kind = ArgumentKind::Text, .root = std::nullopt },
        ArgumentDecl { .name = "prompt", .kind = ArgumentKind::Text, .root = std::nullopt },
        ArgumentDecl { .name = "board", .kind = ArgumentKind::Text, .root = std::nullopt },
        // A JSON document starts with `{`, so it is never read as an option.
        ArgumentDecl { .name = "settings", .kind = ArgumentKind::Text, .root = std::nullopt },
        ArgumentDecl { .name = "plugin", .kind = ArgumentKind::Text, .root = std::nullopt },
        ArgumentDecl { .name = "source", .kind = ArgumentKind::Text, .root = std::nullopt },
        ArgumentDecl { .name = "page", .kind = ArgumentKind::Number, .root = std::nullopt },
        ArgumentDecl { .name = "format", .kind = ArgumentKind::Number, .root = std::nullopt },
        ArgumentDecl { .name = "record", .kind = ArgumentKind::Text, .root = std::nullopt },
        // A Jira query: Text, so one that starts with '-' is refused before it could read as an option.
        ArgumentDecl { .name = "jql", .kind = ArgumentKind::Text, .root = std::nullopt },
        ArgumentDecl { .name = EnvSetArgument, .kind = ArgumentKind::Name, .root = std::nullopt },
    };

    // }}}
    // {{{ roots and the other single lines

    struct RootDecl
    {
        Root root;
        std::string_view pointer; ///< Where in the extension's configuration the directories come from.
        RootCreation creation;    ///< Whether the host creates a missing directory.
    };

    /// The worktree root is created on demand, so a fresh configuration works from its first dispatch; the
    /// repositories are the user's own and must exist.
    constexpr auto RootTable = std::array {
        RootDecl { .root = Root::Worktrees,
                   .pointer = "config:worktree_root",
                   .creation = RootCreation::CreateIfMissing },
        RootDecl {
            .root = Root::Repos, .pointer = "config:repos/*/path", .creation = RootCreation::Existing },
    };

    /// Only the worktrees are readable: nothing under the user's repositories is.
    constexpr auto ReadableRoots = std::array { Root::Worktrees };

    constexpr auto ActionTable = std::array {
        ActionDecl { .kind = ActionKind::Open, .id = "open", .title = "Open Agents" },
        ActionDecl { .kind = ActionKind::Refresh, .id = "refresh", .title = "Agents: Refresh now" },
        ActionDecl {
            .kind = ActionKind::ShowBlocked, .id = "show-blocked", .title = "Agents: Show blocked agent" },
    };
    static_assert(ActionTable.size() == std::to_underlying(ActionKind::ShowBlocked) + 1U);
    static_assert(std::ranges::all_of(std::views::iota(std::size_t { 0 }, ActionTable.size()),
                                      [](std::size_t index) {
                                          return std::to_underlying(ActionTable[index].kind) == index;
                                      }));

    // }}}
    // {{{ environments

    /// The environment tables joined in order: @p parts' variables one after another.
    template <std::size_t... Sizes>
    [[nodiscard]] constexpr auto concatenated(std::array<EnvironmentVariable, Sizes> const&... parts)
    {
        auto result = std::array<EnvironmentVariable, (Sizes + ...)> {};
        auto next = result.begin();
        ((next = std::ranges::copy(parts, next).out), ...);
        return result;
    }

    /// The configuration every declared command forces: no fsmonitor program and no hooks. A
    /// GIT_CONFIG_COUNT that counts these pairs must come with them.
    constexpr auto ForcedConfiguration = std::array {
        EnvironmentVariable { .name = "GIT_CONFIG_KEY_0", .value = "core.fsmonitor" },
        EnvironmentVariable { .name = "GIT_CONFIG_VALUE_0", .value = "false" },
        EnvironmentVariable { .name = "GIT_CONFIG_KEY_1", .value = "core.hooksPath" },
        EnvironmentVariable { .name = "GIT_CONFIG_VALUE_1", .value = "/dev/null" },
    };

    constexpr auto CommandEnvironmentTable = concatenated(
        std::array {
            EnvironmentVariable { .name = "GIT_TERMINAL_PROMPT", .value = "0" },
            EnvironmentVariable { .name = "GIT_ALLOW_PROTOCOL", .value = "https:ssh:git:file" },
            EnvironmentVariable { .name = "GIT_CONFIG_COUNT", .value = "2" },
        },
        ForcedConfiguration);

    /// The empty tree's object id: attributes read from it name no filter.
    constexpr auto EmptyTree = "4b825dc642cb6eb9a060e54bf8d69288fbee4904"sv;

    /// The cleanup commands' git configuration: the forced pairs again, because a command's own
    /// GIT_CONFIG_COUNT replaces the global one, plus no attributes file, so neither `core.attributesFile`
    /// nor the user's `$XDG_CONFIG_HOME/git/attributes` can select a filter (measured: harness/r11.sh). It
    /// must not reach `git worktree add` or `gh pr checkout`, where the user's attributes may select LFS.
    constexpr auto CleanupConfiguration =
        concatenated(std::array { EnvironmentVariable { .name = "GIT_CONFIG_COUNT", .value = "3" } },
                     ForcedConfiguration,
                     std::array {
                         EnvironmentVariable { .name = "GIT_CONFIG_KEY_2", .value = "core.attributesFile" },
                         EnvironmentVariable { .name = "GIT_CONFIG_VALUE_2", .value = "/dev/null" },
                     });

    /// For the commands that run git inside an agent-controlled worktree: attributes come from the empty
    /// tree, so a .gitattributes the agent wrote cannot make git run a clean filter it names. It must not
    /// reach `git worktree add` or `gh pr checkout`, where it would skip smudge filters and break LFS
    /// checkouts. No optional lock either: a kept worktree is checked again on every poll, and a status that
    /// refreshed the index would take `index.lock` while the user works in that worktree.
    constexpr auto CleanupEnvironment =
        concatenated(CleanupConfiguration,
                     std::array {
                         EnvironmentVariable { .name = "GIT_ATTR_SOURCE", .value = EmptyTree },
                         EnvironmentVariable { .name = "GIT_OPTIONAL_LOCKS", .value = "0" },
                     });

    /// The cleanup environment, plus the C locale: the caller reads git's untranslated lines.
    constexpr auto OperationStateEnvironment = concatenated(
        std::array { EnvironmentVariable { .name = "LC_ALL", .value = "C" } }, CleanupEnvironment);

    /// An issue's branch gets no upstream when it is created (`--no-track`), so it never tracks the default
    /// branch; the agent's first push sets the upstream to the branch of the same name.
    constexpr auto AgentPaneEnvironment = std::array {
        EnvironmentVariable { .name = "GIT_CONFIG_COUNT", .value = "2" },
        EnvironmentVariable { .name = "GIT_CONFIG_KEY_0", .value = "push.default" },
        EnvironmentVariable { .name = "GIT_CONFIG_VALUE_0", .value = "upstream" },
        EnvironmentVariable { .name = "GIT_CONFIG_KEY_1", .value = "push.autoSetupRemote" },
        EnvironmentVariable { .name = "GIT_CONFIG_VALUE_1", .value = "true" },
    };

    // }}}
    // {{{ variable sets

    /// What a Claude worker's set may hold: where its state and its network come from.
    constexpr auto ClaudeSetVariables = std::array {
        "CLAUDE_CONFIG_DIR"sv, "ANTHROPIC_BASE_URL"sv, "HTTPS_PROXY"sv, "HTTP_PROXY"sv, "NO_PROXY"sv,
    };
    /// What a Copilot worker's set may hold: its state directory and the network.
    constexpr auto CopilotSetVariables = std::array {
        "COPILOT_HOME"sv,
        "HTTPS_PROXY"sv,
        "HTTP_PROXY"sv,
        "NO_PROXY"sv,
    };

    // }}}
    // {{{ commands

    constexpr auto GhAuthArgv = std::array { "gh"sv, "auth"sv, "status"sv };
    constexpr auto GhDefaultBranchArgv =
        std::array { "gh"sv, "repo"sv, "view"sv, "--json"sv, "defaultBranchRef"sv };
    /// The reply is projected at the source into positional rows (GitHub.cpp reads them): `gh` prints a
    /// check run's URLs, times and workflow too, which made 100 pull requests about 820 KiB and their parse
    /// overrun the guest's 50 ms deadline (measured: harness/ParseHarness.cpp). The filter is one word: no
    /// space, no brace and no quote, as the manifest grammar asks. A key an entry lacks is projected as
    /// `null`.
    constexpr auto GhPrsArgv = std::array {
        "gh"sv,
        "pr"sv,
        "list"sv,
        "--author"sv,
        "@me"sv,
        "--state"sv,
        "open"sv,
        "--limit"sv,
        "100"sv,
        "--json"sv,
        "number,title,url,headRefName,baseRefName,isDraft,isCrossRepository,reviewDecision,statusCheckRollup,"
        "closingIssuesReferences"sv,
        "--jq"sv,
        "[.[]|[.number,.title,.url,.headRefName,.baseRefName,.isDraft,.isCrossRepository,.reviewDecision,"
        "[.closingIssuesReferences[]|[.number,.url]],"
        "[.statusCheckRollup[]|[.__typename,.name,.context,.status,.conclusion,.state]]]]"sv,
    };
    constexpr auto GhIssuesArgv = std::array {
        "gh"sv,       "issue"sv,   "list"sv,   "--state"sv,          "open"sv, "--limit"sv, "100"sv,
        "--search"sv, "{query}"sv, "--json"sv, "number,title,url"sv,
    };
    constexpr auto GitFetchArgv = std::array { "git"sv, "fetch"sv, "--quiet"sv, "{remote}"sv };
    constexpr auto GitWorktreeAddArgv =
        std::array { "git"sv, "worktree"sv, "add"sv, "--detach"sv, "{path}"sv, "{base}"sv };
    constexpr auto GitWorktreeAddBranchArgv = std::array {
        "git"sv, "worktree"sv, "add"sv, "--no-track"sv, "-b"sv, "{branch}"sv, "{path}"sv, "{base}"sv,
    };
    constexpr auto GitWorktreeListArgv =
        std::array { "git"sv, "worktree"sv, "list"sv, "--porcelain"sv, "-z"sv };
    constexpr auto GhPrCheckoutArgv =
        std::array { "gh"sv, "pr"sv, "checkout"sv, "{number}"sv, "--branch"sv, "{branch}"sv };
    /// Untracked files are listed and submodules compared whatever the configuration says: an agent can set
    /// `status.showUntrackedFiles=no` in the shared repository config, and the unforced remove reads the same
    /// key, so it would then delete untracked work (measured).
    constexpr auto GitStatusArgv = std::array {
        "git"sv, "status"sv, "--porcelain"sv, "--untracked-files=normal"sv, "--ignore-submodules=none"sv,
    };
    /// Only refs that outlive the worktree count: its own `refs/worktree/*` go with it (measured). One ref is
    /// enough to answer, so it stops there instead of printing every branch and tag that contains HEAD.
    constexpr auto GitHeadReachableArgv = std::array {
        "git"sv,  "for-each-ref"sv, "--count=1"sv, "--contains"sv,
        "HEAD"sv, "refs/heads"sv,   "refs/tags"sv, "refs/remotes"sv,
    };
    /// The repository directory git actually uses from inside the worktree, resolved. A worktree whose
    /// `commondir` link was redirected names another directory here, whose configuration and
    /// `info/attributes` git would then read (measured: harness/r11.sh). It reads no attributes and no index.
    constexpr auto GitCommonDirArgv =
        std::array { "git"sv, "rev-parse"sv, "--path-format=absolute"sv, "--git-common-dir"sv };
    /// Each index entry with its tag and mode in front. The tag is lowercase for a file marked
    /// assume-unchanged and `S` for one marked skip-worktree: `git status` does not see a change to such a
    /// file, and the unforced remove deletes it. Mode 160000 is a submodule, whose own repository
    /// `git status` would enter. It reads no attributes (measured: harness/r11.sh), so it runs before
    /// anything that does.
    constexpr auto GitIndexFlagsArgv = std::array { "git"sv, "ls-files"sv, "-v"sv, "--stage"sv };
    /// The long status names a rebase, merge, cherry-pick, revert or bisect in progress on a line of its own;
    /// without the comment prefix and colour those lines start in column 0, and it exits 0 in every state.
    constexpr auto GitOperationStateArgv = std::array {
        "git"sv,    "-c"sv,     "status.displayCommentPrefix=false"sv, "-c"sv, "color.status=false"sv,
        "status"sv, "--long"sv,
    };
    /// The one forced command, pinned to the one file the extension itself writes.
    constexpr auto GitCleanArgv = std::array { "git"sv, "clean"sv, "-f"sv, "--"sv, "STATUS.md"sv };
    constexpr auto GitWorktreeRemoveArgv = std::array { "git"sv, "worktree"sv, "remove"sv, "{path}"sv };
    constexpr auto AgentSetupCheckArgv =
        std::array { "contour"sv, "agent"sv, "setup"sv, "--dry-run"sv, "--json"sv };
    /// The `claude` the agents run, asked which plugins it has: the skills a task's prompt names come in one.
    constexpr auto ClaudePluginsArgv = std::array { "claude"sv, "plugin"sv, "list"sv, "--json"sv };
    /// Run only once the user confirmed it on the dashboard (`I`), each into the user's own Claude Code.
    constexpr auto ClaudeMarketplaceAddArgv =
        std::array { "claude"sv, "plugin"sv, "marketplace"sv, "add"sv, "{source}"sv };
    constexpr auto ClaudePluginInstallArgv = std::array { "claude"sv, "plugin"sv, "install"sv, "{plugin}"sv };
    /// The run's worktree is passed as `--cwd`, not used as the working directory: it may be gone, and the
    /// verb only compares it with the directories the session's log names.
    constexpr auto AgentTranscriptArgv = std::array {
        "contour"sv, "agent"sv,  "transcript"sv, "--session"sv, "{session}"sv, "--cwd"sv,
        "{path}"sv,  "--page"sv, "{page}"sv,     "--format"sv,  "{format}"sv,  "--json"sv,
    };
    /// The run's line is the orchestrator's own (contract/agent-runs/); contour only checks and appends it.
    constexpr auto AgentRecordArgv = std::array { "contour"sv,  "agent"sv,    "record"sv,  "--line"sv,
                                                  "{record}"sv, "--format"sv, "{format}"sv };
    /// As the transcript's: the worktree is compared, not entered, and may be gone.
    constexpr auto AgentStatsArgv = std::array {
        "contour"sv, "agent"sv,  "stats"sv,    "--session"sv, "{session}"sv,
        "--cwd"sv,   "{path}"sv, "--format"sv, "{format}"sv,  "--json"sv,
    };

    // GitLab, through `glab`: recorded from glab 1.119.0 (core/testdata/gitlab/). It prints the REST API's
    // objects unprojected and has no `--jq`. JSON is asked for with `--output json`, never `-F json`: `-F` is
    // `--output` on `glab mr list` and `glab repo view` but `--output-format` on `glab issue list`, where
    // `-F json` prints a sentence instead.
    // Jira, through Atlassian's `acli`: recorded from acli 1.3.41 (core/testdata/jira/). The search prints
    // one indented JSON array, `[]` when nothing matches; a JQL it refuses exits 1 with the reason on stderr.
    constexpr auto GlabAuthArgv = std::array { "glab"sv, "auth"sv, "status"sv };
    constexpr auto GlabDefaultBranchArgv =
        std::array { "glab"sv, "repo"sv, "view"sv, "--output"sv, "json"sv };
    constexpr auto GlabMrsArgv = std::array {
        "glab"sv, "mr"sv, "list"sv, "--author=@me"sv, "--per-page"sv, "100"sv, "--output"sv, "json"sv,
    };
    constexpr auto GlabIssuesArgv = std::array {
        "glab"sv, "issue"sv,    "list"sv,    "--assignee=@me"sv, "--per-page"sv,
        "100"sv,  "--search"sv, "{query}"sv, "--output"sv,       "json"sv,
    };
    constexpr auto AcliAuthArgv = std::array { "acli"sv, "jira"sv, "auth"sv, "status"sv };
    constexpr auto AcliIssuesArgv = std::array {
        "acli"sv, "jira"sv, "workitem"sv, "search"sv, "--jql"sv, "{jql}"sv, "--limit"sv, "100"sv, "--json"sv,
    };
    constexpr auto GlabMrCheckoutArgv =
        std::array { "glab"sv, "mr"sv, "checkout"sv, "{number}"sv, "--branch"sv, "{branch}"sv };

    constexpr auto CommandTable = std::array {
        // An auth check's exit status is all that counts: what it prints may hold a token.
        CommandDecl { .name = "gh-auth",
                      .timeout = Timeout::Short,
                      .root = Root::Repos,
                      .argv = GhAuthArgv,
                      .environment = {},
                      .verification = Verification::Recorded,
                      .output = OutputUse::ExitStatusOnly },
        CommandDecl { .name = "gh-default-branch",
                      .timeout = Timeout::Short,
                      .root = Root::Repos,
                      .argv = GhDefaultBranchArgv,
                      .environment = {} },
        CommandDecl { .name = "gh-prs",
                      .timeout = Timeout::Long,
                      .root = Root::Repos,
                      .argv = GhPrsArgv,
                      .environment = {} },
        CommandDecl { .name = "gh-issues",
                      .timeout = Timeout::Long,
                      .root = Root::Repos,
                      .argv = GhIssuesArgv,
                      .environment = {} },
        CommandDecl { .name = "git-fetch",
                      .timeout = Timeout::Long,
                      .root = Root::Repos,
                      .argv = GitFetchArgv,
                      .environment = {} },
        CommandDecl { .name = "git-worktree-add",
                      .timeout = Timeout::Long,
                      .root = Root::Repos,
                      .argv = GitWorktreeAddArgv,
                      .environment = {} },
        CommandDecl { .name = "git-worktree-add-branch",
                      .timeout = Timeout::Long,
                      .root = Root::Repos,
                      .argv = GitWorktreeAddBranchArgv,
                      .environment = {} },
        CommandDecl { .name = "git-worktree-list",
                      .timeout = Timeout::Short,
                      .root = Root::Repos,
                      .argv = GitWorktreeListArgv,
                      .environment = {} },
        CommandDecl { .name = "gh-pr-checkout",
                      .timeout = Timeout::Long,
                      .root = Root::Worktrees,
                      .argv = GhPrCheckoutArgv,
                      .environment = {} },
        CommandDecl { .name = "git-common-dir",
                      .timeout = Timeout::Short,
                      .root = Root::Worktrees,
                      .argv = GitCommonDirArgv,
                      .environment = CleanupEnvironment },
        CommandDecl { .name = "git-status",
                      .timeout = Timeout::Short,
                      .root = Root::Worktrees,
                      .argv = GitStatusArgv,
                      .environment = CleanupEnvironment },
        CommandDecl { .name = "git-head-reachable",
                      .timeout = Timeout::Short,
                      .root = Root::Worktrees,
                      .argv = GitHeadReachableArgv,
                      .environment = CleanupEnvironment },
        CommandDecl { .name = "git-operation-state",
                      .timeout = Timeout::Short,
                      .root = Root::Worktrees,
                      .argv = GitOperationStateArgv,
                      .environment = OperationStateEnvironment },
        CommandDecl { .name = "git-index-flags",
                      .timeout = Timeout::Short,
                      .root = Root::Worktrees,
                      .argv = GitIndexFlagsArgv,
                      .environment = CleanupEnvironment },
        CommandDecl { .name = "git-clean",
                      .timeout = Timeout::Short,
                      .root = Root::Worktrees,
                      .argv = GitCleanArgv,
                      .environment = CleanupEnvironment },
        // Run from inside the worktree being removed, not from its repository: git then checks that the
        // worktree's link points back to it, and refuses a worktree whose link was redirected elsewhere.
        CommandDecl { .name = "git-worktree-remove",
                      .timeout = Timeout::Long,
                      .root = Root::Worktrees,
                      .argv = GitWorktreeRemoveArgv,
                      .environment = CleanupEnvironment },
        // Run in the worktree root itself, so it never depends on a run's worktree still existing.
        CommandDecl { .name = "agent-setup-check",
                      .timeout = Timeout::Short,
                      .root = Root::Worktrees,
                      .argv = AgentSetupCheckArgv,
                      .environment = {},
                      .setVariables = ClaudeSetVariables },
        // In the worktree root, as the hooks check.
        CommandDecl { .name = "claude-plugins",
                      .timeout = Timeout::Short,
                      .root = Root::Worktrees,
                      .argv = ClaudePluginsArgv,
                      .environment = {},
                      .setVariables = ClaudeSetVariables },
        // Their exit status is all that is read: what they print is Claude Code's to word.
        CommandDecl { .name = "claude-marketplace-add",
                      .timeout = Timeout::Long,
                      .root = Root::Worktrees,
                      .argv = ClaudeMarketplaceAddArgv,
                      .environment = {},
                      .verification = Verification::Recorded,
                      .output = OutputUse::ExitStatusOnly,
                      .setVariables = ClaudeSetVariables },
        CommandDecl { .name = "claude-plugin-install",
                      .timeout = Timeout::Long,
                      .root = Root::Worktrees,
                      .argv = ClaudePluginInstallArgv,
                      .environment = {},
                      .verification = Verification::Recorded,
                      .output = OutputUse::ExitStatusOnly,
                      .setVariables = ClaudeSetVariables },
        // In the worktree root too: a Finished run's worktree is usually gone, its transcript is not.
        CommandDecl { .name = "agent-transcript",
                      .timeout = Timeout::Short,
                      .root = Root::Worktrees,
                      .argv = AgentTranscriptArgv,
                      .environment = {},
                      .setVariables = ClaudeSetVariables },
        // Appends to a file in contour's state directory; it runs in the worktree root, which always exists.
        CommandDecl { .name = "agent-record",
                      .timeout = Timeout::Short,
                      .root = Root::Worktrees,
                      .argv = AgentRecordArgv,
                      .environment = {} },
        // In the worktree root for the same reason: a run's statistics are read once it finished.
        CommandDecl { .name = "agent-stats",
                      .timeout = Timeout::Short,
                      .root = Root::Worktrees,
                      .argv = AgentStatsArgv,
                      .environment = {},
                      .setVariables = ClaudeSetVariables },
        CommandDecl { .name = "glab-auth",
                      .timeout = Timeout::Short,
                      .root = Root::Repos,
                      .argv = GlabAuthArgv,
                      .environment = {},
                      .verification = Verification::Recorded,
                      .output = OutputUse::ExitStatusOnly },
        CommandDecl { .name = "glab-default-branch",
                      .timeout = Timeout::Short,
                      .root = Root::Repos,
                      .argv = GlabDefaultBranchArgv,
                      .environment = {},
                      .verification = Verification::Recorded },
        CommandDecl { .name = "glab-mrs",
                      .timeout = Timeout::Long,
                      .root = Root::Repos,
                      .argv = GlabMrsArgv,
                      .environment = {},
                      .verification = Verification::Recorded },
        CommandDecl { .name = "glab-issues",
                      .timeout = Timeout::Long,
                      .root = Root::Repos,
                      .argv = GlabIssuesArgv,
                      .environment = {},
                      .verification = Verification::Recorded },
        CommandDecl { .name = "glab-mr-checkout",
                      .timeout = Timeout::Long,
                      .root = Root::Worktrees,
                      .argv = GlabMrCheckoutArgv,
                      .environment = {},
                      .verification = Verification::Recorded },
        CommandDecl { .name = "acli-auth",
                      .timeout = Timeout::Short,
                      .root = Root::Repos,
                      .argv = AcliAuthArgv,
                      .environment = {},
                      .verification = Verification::Recorded,
                      .output = OutputUse::ExitStatusOnly },
        CommandDecl { .name = "acli-issues",
                      .timeout = Timeout::Long,
                      .root = Root::Repos,
                      .argv = AcliIssuesArgv,
                      .environment = {},
                      .verification = Verification::Recorded },
    };

    // }}}
    // {{{ panes

    /// `--settings` carries the run's permission profile (`permissions`), on top of the user's own Claude
    /// Code settings. The system prompt asks for the status board the dashboard shows (`status_board`).
    constexpr auto AgentPaneArgv = std::array {
        "claude"sv,    "--model"sv,    "{model}"sv,    "--session-id"sv,
        "{session}"sv, "--settings"sv, "{settings}"sv, "--append-system-prompt"sv,
        "{board}"sv,   "{prompt}"sv,
    };
    /// The same without the board's instruction: `status_board: off`.
    constexpr auto AgentPlainPaneArgv = std::array {
        "claude"sv,    "--model"sv,    "{model}"sv,    "--session-id"sv,
        "{session}"sv, "--settings"sv, "{settings}"sv, "{prompt}"sv,
    };

    /// The Copilot CLI, asked for the model and the prompt, in its interactive mode.
    constexpr auto CopilotPaneArgv =
        std::array { "copilot"sv, "--model"sv, "{model}"sv, "-i"sv, "{prompt}"sv };

    constexpr auto PaneTable = std::array {
        PaneDecl { .name = AgentPaneTemplate,
                   .root = Root::Worktrees,
                   .argv = AgentPaneArgv,
                   .environment = AgentPaneEnvironment,
                   .setVariables = ClaudeSetVariables },
        PaneDecl { .name = AgentPlainPaneTemplate,
                   .root = Root::Worktrees,
                   .argv = AgentPlainPaneArgv,
                   .environment = AgentPaneEnvironment,
                   .setVariables = ClaudeSetVariables },
        PaneDecl { .name = CopilotPaneTemplate,
                   .root = Root::Worktrees,
                   .argv = CopilotPaneArgv,
                   .environment = AgentPaneEnvironment,
                   .setVariables = CopilotSetVariables },
    };

    // }}}

    /// The words of @p argv joined by single spaces, as a manifest line spells them.
    [[nodiscard]] std::string joined(std::span<std::string_view const> argv)
    {
        auto text = std::string {};
        for (auto const word: argv)
        {
            if (!text.empty())
                text += ' ';
            text += word;
        }
        return text;
    }

    /// @return Whether @p decl reaches the manifest.
    [[nodiscard]] constexpr bool isDeclared(CommandDecl const& decl) noexcept
    {
        return decl.verification == Verification::Recorded;
    }

    /// @return Whether a declared command or a pane binds a variable set, which puts the env-set argument in
    ///         the manifest.
    [[nodiscard]] bool bindsSet() noexcept
    {
        return std::ranges::any_of(
                   CommandTable,
                   [](CommandDecl const& decl) { return isDeclared(decl) && !decl.setVariables.empty(); })
               || std::ranges::any_of(PaneTable,
                                      [](PaneDecl const& decl) { return !decl.setVariables.empty(); });
    }

    /// @return Whether a declared command or a pane names the argument @p name: an argument nothing in the
    ///         manifest uses is left out of it. A binding uses the set argument.
    [[nodiscard]] bool isNamedInManifest(std::string_view name) noexcept
    {
        if (name == EnvSetArgument && bindsSet())
            return true;
        auto const names = [&](std::span<std::string_view const> argv) {
            return std::ranges::any_of(argv,
                                       [&](std::string_view word) { return placeholderName(word) == name; });
        };
        return std::ranges::any_of(
                   CommandTable,
                   [&](CommandDecl const& decl) { return isDeclared(decl) && names(decl.argv); })
               || std::ranges::any_of(PaneTable, [&](PaneDecl const& decl) { return names(decl.argv); });
    }

    /// Concatenates @p parts.
    [[nodiscard]] std::string concat(std::initializer_list<std::string_view> parts)
    {
        auto text = std::string {};
        for (auto const part: parts)
            text += part;
        return text;
    }

    /// A `pane-env-set` or `command-env-set` line.
    /// @param keyword The line's keyword, with its trailing space.
    /// @param target The pane or command the binding is for.
    /// @param variables What the target may take from the set.
    /// @return The line, without its line end.
    [[nodiscard]] std::string setLine(std::string_view keyword,
                                      std::string_view target,
                                      std::span<std::string_view const> variables)
    {
        auto const words = joined(variables);
        return concat({ keyword, target, " {", EnvSetArgument, "} = ", words });
    }

    /// The manifest's spelling of @p decl's kind: `path:<root>` for a Path.
    [[nodiscard]] std::string kindLabel(ArgumentDecl const& decl)
    {
        if (decl.kind == ArgumentKind::Path && decl.root)
            return concat({ manifestLabel(decl.kind), ":", manifestLabel(*decl.root) });
        return std::string(manifestLabel(decl.kind));
    }

} // namespace

std::span<ArgumentDecl const> argumentDecls() noexcept
{
    return ArgumentTable;
}

std::span<EnvironmentVariable const> commandEnvironment() noexcept
{
    return CommandEnvironmentTable;
}

std::span<CommandDecl const> commands() noexcept
{
    return CommandTable;
}

std::span<PaneDecl const> paneTemplates() noexcept
{
    return PaneTable;
}

ArgumentDecl const* findArgumentDecl(std::string_view name) noexcept
{
    auto const found = std::ranges::find(ArgumentTable, name, &ArgumentDecl::name);
    return found == ArgumentTable.end() ? nullptr : &*found;
}

CommandDecl const* findCommand(std::string_view name) noexcept
{
    auto const found = std::ranges::find(CommandTable, name, &CommandDecl::name);
    return found == CommandTable.end() ? nullptr : &*found;
}

PaneDecl const* findPaneTemplate(std::string_view name) noexcept
{
    auto const found = std::ranges::find(PaneTable, name, &PaneDecl::name);
    return found == PaneTable.end() ? nullptr : &*found;
}

std::span<ActionDecl const> actionDecls() noexcept
{
    return ActionTable;
}

ActionDecl const* findAction(std::string_view id) noexcept
{
    auto const found = std::ranges::find(ActionTable, id, &ActionDecl::id);
    return found == ActionTable.end() ? nullptr : &*found;
}

std::expected<std::vector<std::string>, SubstituteError> substitute(std::span<std::string_view const> argv,
                                                                    std::span<Argument const> arguments)
{
    auto result = std::vector<std::string> {};
    result.reserve(argv.size());
    for (auto const word: argv)
    {
        auto const name = placeholderName(word);
        if (!name)
        {
            // A brace outside a whole-word placeholder names nothing a call could fill in.
            if (word.contains('{') || word.contains('}'))
                return std::unexpected { SubstituteError::UnknownPlaceholder };
            result.emplace_back(word);
            continue;
        }
        auto const* const decl = findArgumentDecl(*name);
        if (!decl)
            return std::unexpected { SubstituteError::UnknownPlaceholder };
        auto const argument = std::ranges::find(arguments, *name, &Argument::name);
        if (argument == arguments.end())
            return std::unexpected { SubstituteError::MissingArgument };
        if (!argumentAccepts(decl->kind, argument->value))
            return std::unexpected { SubstituteError::Rejected };
        result.push_back(argument->value);
    }
    return result;
}

std::string shellQuoted(std::span<std::string const> argv)
{
    // What a shell reads as part of a plain word, wherever in the word it stands.
    constexpr auto Plain = std::string_view { "_-./:@%+=," };
    auto const isPlain = [&](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
               || Plain.contains(c);
    };
    auto line = std::string {};
    for (auto const& word: argv)
    {
        if (!line.empty())
            line += ' ';
        if (!word.empty() && std::ranges::all_of(word, isPlain))
        {
            line += word;
            continue;
        }
        line += '\'';
        for (auto const c: word)
        {
            if (c == '\'')
                line += "'\\''";
            else
                line += c;
        }
        line += '\'';
    }
    return line;
}

std::vector<std::string> manifestLines()
{
    auto lines = std::vector<std::string> {};
    for (auto const& decl: RootTable)
        lines.push_back(concat({ "root ",
                                 manifestLabel(decl.root),
                                 decl.creation == RootCreation::CreateIfMissing ? " create" : "",
                                 " = ",
                                 decl.pointer }));
    for (auto const root: ReadableRoots)
        lines.push_back(concat({ "read ", manifestLabel(root) }));
    for (auto const& decl: ArgumentTable | std::views::filter([](ArgumentDecl const& argument) {
                               return isNamedInManifest(argument.name);
                           }))
        lines.push_back(concat({ "arg ", decl.name, " = ", kindLabel(decl) }));
    for (auto const& variable: CommandEnvironmentTable)
        lines.push_back(concat({ "command-env ", variable.name, " = ", variable.value }));
    auto declared = CommandTable | std::views::filter(isDeclared);
    for (auto const& decl: declared)
        lines.push_back(concat({ "command ",
                                 decl.name,
                                 " ",
                                 manifestLabel(decl.timeout),
                                 " @",
                                 manifestLabel(decl.root),
                                 " = ",
                                 joined(decl.argv) }));
    for (auto const& decl: declared)
        for (auto const& variable: decl.environment)
            lines.push_back(
                concat({ "command-env-for ", decl.name, " ", variable.name, " = ", variable.value }));
    for (auto const& decl: declared)
        if (!decl.setVariables.empty())
            lines.push_back(setLine("command-env-set ", decl.name, decl.setVariables));
    for (auto const& decl: PaneTable)
        lines.push_back(
            concat({ "pane ", decl.name, " @", manifestLabel(decl.root), " = ", joined(decl.argv) }));
    for (auto const& decl: PaneTable)
        for (auto const& variable: decl.environment)
            lines.push_back(concat({ "pane-env ", decl.name, " ", variable.name, " = ", variable.value }));
    for (auto const& decl: PaneTable)
        if (!decl.setVariables.empty())
            lines.push_back(setLine("pane-env-set ", decl.name, decl.setVariables));
    // The `m` line types what the user wrote into an agent's tab: nothing else is ever typed there.
    lines.emplace_back("pane-input");
    lines.push_back(concat({ "serve ", ServedScheme }));
    // `o` and a click on a link ask the frontend the user is at to open the run's link.
    lines.emplace_back("url-open");
    for (auto const& decl: ActionTable)
        lines.push_back(concat({ "action ", decl.id, " = ", decl.title }));
    return lines;
}

std::string manifestText()
{
    auto text = std::string("name = orchestrator\nversion = 2\ntitle = Agents\nplacement daemon\n");
    for (auto const& line: manifestLines())
        (text += line) += '\n';
    return text;
}

} // namespace orchestrator
