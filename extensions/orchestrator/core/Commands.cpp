// SPDX-License-Identifier: Apache-2.0
#include <orchestrator/core/Commands.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <expected>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
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

    /// The scheme of the tabs the extension serves.
    constexpr auto ServedScheme = "orchestrator"sv;

    /// A palette row: the id the guest receives and the title the user sees.
    struct ActionDecl
    {
        std::string_view id;
        std::string_view title;
    };

    constexpr auto ActionTable = std::array {
        ActionDecl { .id = "open", .title = "Open Agents" },
        ActionDecl { .id = "refresh", .title = "Agents: Refresh now" },
        ActionDecl { .id = "show-blocked", .title = "Agents: Show blocked agent" },
    };

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

    constexpr auto CommandTable = std::array {
        CommandDecl { .name = "gh-auth",
                      .timeout = Timeout::Short,
                      .root = Root::Repos,
                      .argv = GhAuthArgv,
                      .environment = {} },
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
                      .environment = {} },
    };

    // }}}
    // {{{ panes

    /// No permission flag and no `--settings`: the agent runs with the user's own Claude Code settings.
    constexpr auto AgentPaneArgv = std::array {
        "claude"sv, "--model"sv, "{model}"sv, "--session-id"sv, "{session}"sv, "{prompt}"sv,
    };

    constexpr auto PaneTable = std::array {
        PaneDecl { .name = AgentPaneTemplate,
                   .root = Root::Worktrees,
                   .argv = AgentPaneArgv,
                   .environment = AgentPaneEnvironment },
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

    /// Concatenates @p parts.
    [[nodiscard]] std::string concat(std::initializer_list<std::string_view> parts)
    {
        auto text = std::string {};
        for (auto const part: parts)
            text += part;
        return text;
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
    for (auto const& decl: ArgumentTable)
        lines.push_back(concat({ "arg ", decl.name, " = ", kindLabel(decl) }));
    for (auto const& variable: CommandEnvironmentTable)
        lines.push_back(concat({ "command-env ", variable.name, " = ", variable.value }));
    for (auto const& decl: CommandTable)
        lines.push_back(concat({ "command ",
                                 decl.name,
                                 " ",
                                 manifestLabel(decl.timeout),
                                 " @",
                                 manifestLabel(decl.root),
                                 " = ",
                                 joined(decl.argv) }));
    for (auto const& decl: CommandTable)
        for (auto const& variable: decl.environment)
            lines.push_back(
                concat({ "command-env-for ", decl.name, " ", variable.name, " = ", variable.value }));
    for (auto const& decl: PaneTable)
        lines.push_back(
            concat({ "pane ", decl.name, " @", manifestLabel(decl.root), " = ", joined(decl.argv) }));
    for (auto const& decl: PaneTable)
        for (auto const& variable: decl.environment)
            lines.push_back(concat({ "pane-env ", decl.name, " ", variable.name, " = ", variable.value }));
    lines.push_back(concat({ "serve ", ServedScheme }));
    for (auto const& decl: ActionTable)
        lines.push_back(concat({ "action ", decl.id, " = ", decl.title }));
    return lines;
}

std::string manifestText()
{
    auto text = std::string("name = orchestrator\nversion = 2\nplacement daemon\n");
    for (auto const& line: manifestLines())
        (text += line) += '\n';
    return text;
}

} // namespace orchestrator
