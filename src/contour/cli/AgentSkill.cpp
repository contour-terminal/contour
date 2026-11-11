// SPDX-License-Identifier: Apache-2.0
#include <contour/cli/AgentSkill.hpp>
#include <contour/cli/ControlVerbs.hpp>
#include <contour/config/AtomicFileWrite.hpp>

#include <core/Utf8Path.hpp>
#include <core/Utils.hpp>

#include <algorithm>
#include <format>
#include <ranges>

#include <vtcontrol/Describe.hpp>
#include <vtcontrol/Json.hpp>

using namespace std::string_literals;

namespace contour::cli
{

namespace
{
    /// @param state A skill state.
    /// @return Its name for reports.
    [[nodiscard]] std::string_view nameOf(SkillState state) noexcept
    {
        switch (state)
        {
            case SkillState::Created: return "created";
            case SkillState::Updated: return "updated";
            case SkillState::Unchanged: return "unchanged";
            case SkillState::Foreign: return "foreign (kept; use --force to replace)";
            case SkillState::Untouched: return "untouched";
        }
        return {};
    }

    /// @param state A skill state.
    /// @return Its name in JSON.
    [[nodiscard]] std::string_view jsonNameOf(SkillState state) noexcept
    {
        switch (state)
        {
            case SkillState::Created: return "created";
            case SkillState::Updated: return "updated";
            case SkillState::Unchanged: return "unchanged";
            case SkillState::Foreign: return "foreign";
            case SkillState::Untouched: return "untouched";
        }
        return {};
    }

    /// @param result What a hook edit did.
    /// @return Its name for reports.
    [[nodiscard]] std::string_view nameOf(HookMergeResult result) noexcept
    {
        switch (result)
        {
            case HookMergeResult::Unchanged: return "unchanged";
            case HookMergeResult::Changed: return "changed";
            case HookMergeResult::Malformed: return "malformed (kept; fix the file first)";
            case HookMergeResult::DirectoryMissing: return "directory missing (not created)";
        }
        return {};
    }

    /// @param result What a hook edit did.
    /// @return Its name in JSON.
    [[nodiscard]] std::string_view jsonNameOf(HookMergeResult result) noexcept
    {
        switch (result)
        {
            case HookMergeResult::Unchanged: return "unchanged";
            case HookMergeResult::Changed: return "changed";
            case HookMergeResult::Malformed: return "malformed";
            case HookMergeResult::DirectoryMissing: return "directory-missing";
        }
        return {};
    }

    /// @param selection Why an agent was set up, or not.
    /// @return Its name in JSON.
    [[nodiscard]] std::string_view jsonNameOf(AgentSelection selection) noexcept
    {
        switch (selection)
        {
            case AgentSelection::Skipped: return "skipped";
            case AgentSelection::Detected: return "detected";
            case AgentSelection::Named: return "named";
        }
        return {};
    }

    /// @param integration The agent whose hooks they are.
    /// @param action Install or uninstall.
    /// @param text The agent's hook file, empty when there is none.
    /// @param exe The `contour` binary.
    /// @return The edited text.
    [[nodiscard]] HookMerge editHooks(AgentIntegration const& integration,
                                      HookAction action,
                                      std::string_view text,
                                      std::string_view exe)
    {
        return action == HookAction::Install ? mergeHooks(integration, text, exe)
                                             : removeHooks(integration, text);
    }

    /// Whether a variable moved a directory away from its default.
    enum class Relocated : uint8_t
    {
        No = 0,
        Yes,
    };

    /// @param path A file setup reads and replaces.
    /// @return What to read and replace: the file a symlink at @p path points to, so the link survives the
    ///         replacement; else @p path.
    [[nodiscard]] std::filesystem::path replacedFileOf(core::platform::FileSystem const& fileSystem,
                                                       std::filesystem::path const& path)
    {
        return fileSystem.isSymlink(path) ? fileSystem.weaklyCanonical(path) : path;
    }

    /// Reads a file setup may replace.
    /// @return Its text, empty when there is none; or why it cannot be read.
    [[nodiscard]] std::expected<std::string, std::string> readReplaced(
        core::platform::FileSystem const& fileSystem, std::filesystem::path const& path)
    {
        auto const file = replacedFileOf(fileSystem, path);
        if (!fileSystem.exists(file))
            return std::string {};
        return fileSystem.readFile(file).transform_error([&](std::string const& error) {
            return std::format("cannot read {}: {}", core::pathToUtf8(path), error);
        });
    }

    /// Replaces a file durably: a crash or a full disk leaves the old text or the new, never a torn file.
    /// @return Nothing, or why it cannot be written.
    [[nodiscard]] std::expected<void, std::string> replace(core::platform::FileSystem const& fileSystem,
                                                           std::filesystem::path const& path,
                                                           std::string_view text)
    {
        return config::writeFileDurably(fileSystem, replacedFileOf(fileSystem, path), text)
            .transform_error([&](std::string const& error) {
                return std::format("cannot write {}: {}", core::pathToUtf8(path), error);
            });
    }

    /// Sets up one agent's hooks.
    /// @return What happened, or why the file cannot be read or written.
    [[nodiscard]] std::expected<HookFileOutcome, std::string> setupHooks(
        core::platform::FileSystem& fileSystem,
        AgentTarget const& target,
        std::filesystem::path const& directory,
        Relocated relocated,
        HookSetup const& hookSetup,
        SetupMode mode)
    {
        auto const path = directory / target.hookFile;
        if (relocated == Relocated::Yes && target.relocatedHookDirectory == RelocatedHookDirectory::MustExist
            && !fileSystem.isDirectory(directory))
            return HookFileOutcome { .path = path,
                                     .result = HookMergeResult::DirectoryMissing,
                                     .detail = std::format("${} names a directory that does not exist: {}",
                                                           target.configDirEnv,
                                                           core::pathToUtf8(directory)) };
        return readReplaced(fileSystem, path)
            .and_then([&](std::string const& existing) -> std::expected<HookFileOutcome, std::string> {
                auto const edit = editHooks(
                    integrationOf(*target.hookAgent), hookSetup.action, existing, hookSetup.contourExe);
                if (edit.result == HookMergeResult::Changed && mode == SetupMode::Write)
                    if (auto written = replace(fileSystem, path, edit.text); !written)
                        return std::unexpected(std::move(written.error()));
                return HookFileOutcome { .path = path, .result = edit.result, .detail = edit.detail };
            });
    }

    /// Sets up one agent's skill.
    /// @return What happened, or why the file cannot be read or written.
    [[nodiscard]] std::expected<SkillFileOutcome, std::string> setupSkill(
        core::platform::FileSystem& fileSystem,
        std::filesystem::path const& path,
        SetupMode mode,
        ForeignSkill foreign,
        std::string_view skillText)
    {
        auto const existing = readReplaced(fileSystem, path);
        if (!existing)
            return std::unexpected(existing.error());
        auto state = SkillState::Created;
        if (fileSystem.exists(replacedFileOf(fileSystem, path)))
        {
            if (!existing->contains(AgentSkillMarker))
                state = foreign == ForeignSkill::Replace ? SkillState::Updated : SkillState::Foreign;
            else
                state = *existing == skillText ? SkillState::Unchanged : SkillState::Updated;
        }
        auto const writes = state == SkillState::Created || state == SkillState::Updated;
        if (writes && mode == SetupMode::Write)
            if (auto written = replace(fileSystem, path, skillText); !written)
                return std::unexpected(std::move(written.error()));
        return SkillFileOutcome { .path = path, .state = state };
    }

    /// @param relocatingEnv The variable that relocates a directory; empty for none.
    /// @param env The environment.
    /// @return Yes when the variable is set to something.
    [[nodiscard]] Relocated relocationOf(std::string_view relocatingEnv, core::Environment const& env)
    {
        if (relocatingEnv.empty())
            return Relocated::No;
        auto const relocated = env.get(relocatingEnv);
        return relocated && !relocated->empty() ? Relocated::Yes : Relocated::No;
    }

    /// @param relocatingEnv The variable that relocates the directory; empty for none.
    /// @param defaultDir The directory beneath the home directory.
    /// @param env The environment.
    /// @param home The home directory.
    /// @return The directory: the variable's value when it is set, else the default beneath home.
    [[nodiscard]] std::filesystem::path configDirOf(std::string_view relocatingEnv,
                                                    std::string_view defaultDir,
                                                    core::Environment const& env,
                                                    std::filesystem::path const& home)
    {
        if (relocationOf(relocatingEnv, env) == Relocated::Yes)
            return core::pathFromUtf8(*env.get(relocatingEnv)).value_or(home / defaultDir);
        return home / defaultDir;
    }

    /// @return Whether, and why, @p target is set up.
    [[nodiscard]] AgentSelection selectionOf(AgentTarget const& target,
                                             std::span<std::string const> named,
                                             core::platform::FileSystem const& fileSystem,
                                             std::filesystem::path const& configDir)
    {
        if (std::ranges::contains(named, target.name))
            return AgentSelection::Named;
        if (!named.empty())
            return AgentSelection::Skipped;
        if (target.detection == AgentDetection::Always || fileSystem.isDirectory(configDir))
            return AgentSelection::Detected;
        return AgentSelection::Skipped;
    }

} // namespace

std::string renderAgentSkill(std::string_view version)
{
    auto text = std::string {};
    auto const line = [&](std::string_view content = {}) {
        text += content;
        text += '\n';
    };

    line("---");
    line("name: contour");
    line("description: Use when $CONTOUR_PANE is set, or when asked to operate Contour terminal panes: open "
         "tabs and splits, type into panes, read their output and command results, and wait for commands.");
    line("---");
    line(AgentSkillMarker);
    line();
    line("# Operating Contour panes");
    line();
    line(std::format("Generated for Contour {}. The `contour` command talks to the daemon that hosts your "
                     "terminal.",
                     version));
    line();
    line("## Where you are");
    line();
    line("A shell hosted by the Contour daemon has these environment variables:");
    line();
    line("- `$CONTOUR_PANE`: your own pane, `pane:<uuid>`. The default target of every verb.");
    line("- `$CONTOUR_TAB`: your tab.");
    line("- `$CONTOUR_WORKSPACE`: your workspace.");
    line("- `$CONTOUR_DAEMON`: the daemon's socket.");
    line();
    line("## Targets");
    line();
    line("Wherever a verb takes `--pane P` (or a tab or workspace), `P` is resolved in this order, the first "
         "kind "
         "of match winning: a full id; a label (case-insensitive, see `contour label`); a 1-based position "
         "(tabs and workspaces); an id suffix of at least four hex digits. Several matches are an error that "
         "lists the candidates. Without `--pane`, the verb acts on `$CONTOUR_PANE`, and a pane that has "
         "closed is an error (exit 3), never another pane.");
    line();
    line("## Verbs");
    line();
    line("Every verb takes `--json` to print the reply as JSON, and `--daemon`, `--token-file` and "
         "`--tls-ca` to reach another daemon.");
    line();
    for (auto const& verb: controlVerbs())
        line(std::format("- `contour {}`: {}", verb.usage, verb.summary));
    line();
    line(
        "Put `--` before the text a verb types or runs: without it, text that starts with an option's name "
        "is read as that option (`contour send --enter wait` waits, and types nothing), and text that starts "
        "with any other dash word is refused (`contour send -x` types nothing).");
    line();
    line("## MCP");
    line();
    line("An agent that speaks the Model Context Protocol can take these verbs as tools instead: register "
         "the server with `claude mcp add contour -- contour mcp`. It serves your own tab unless started "
         "with `--allow-all-tabs`, and `--read-only` leaves out the tools that type or change the "
         "workspace.");
    line();
    line("## Exit statuses");
    line();
    for (auto const status: ExitStatuses)
        line(std::format("- {}: {}", exitCode(status), meaningOf(status)));
    line("- `wait` and `send --wait` exit with the command's own status (0 to 255) instead.");
    line();
    line("## Workflow: run the tests beside you");
    line();
    line("```sh");
    line("pane=$(contour split --below --no-focus --json | jq -r .pane)");
    line("contour send --pane \"$pane\" --enter --wait -- 'make test'");
    line("contour block read --pane \"$pane\" --output");
    line("```");
    line();
    line("The second command exits with the status of `make test`; the third prints what it printed.");
    line();
    line("## Everything else");
    line();
    line("`contour api list` lists every method and event, `contour api describe NAME` shows one with its "
         "schemas and examples, and `contour api call NAME [JSON]` calls any of them. The methods are:");
    line();
    auto const api = vtcontrol::listApi();
    for (auto const& method: api.methods)
        line(std::format("- `{}`: {}", method.name, method.summary));
    line();
    line("The events, for `contour events --kind`:");
    line();
    for (auto const& event: api.events)
        line(std::format("- `{}`: {}", event.name, event.summary));
    return text;
}

std::vector<std::string> parseAgentList(std::string_view commaList)
{
    return std::ranges::to<std::vector>(
        std::views::split(commaList, ',')
        | std::views::transform([](auto part) { return std::string_view { part }; })
        | std::views::filter([](auto part) { return !part.empty(); })
        | std::views::transform([](auto part) { return std::string { part }; }));
}

std::expected<std::filesystem::path, std::string> hookExecutable(
    core::Environment const& env,
    std::function<std::expected<std::filesystem::path, std::string>()> const& runningBinary)
{
    if (auto const appImage = env.get("APPIMAGE"); appImage && !appImage->empty())
        if (auto path = core::pathFromUtf8(*appImage))
            return *std::move(path);
    return runningBinary();
}

std::expected<std::vector<AgentOutcome>, std::string> setupAgentSkills(core::platform::FileSystem& fileSystem,
                                                                       core::Environment const& env,
                                                                       std::filesystem::path const& home,
                                                                       std::span<std::string const> agents,
                                                                       SetupMode mode,
                                                                       ForeignSkill foreign,
                                                                       std::string_view skillText,
                                                                       HookSetup const& hookSetup)
{
    for (auto const& name: agents)
        if (!std::ranges::contains(AgentTargets, name, &AgentTarget::name))
        {
            auto known = std::string {};
            for (auto const& target: AgentTargets)
                known += (known.empty() ? "" : ", ") + std::string { target.name };
            return std::unexpected(std::format("unknown agent '{}'; known agents: {}", name, known));
        }

    auto outcomes = std::vector<AgentOutcome> {};
    for (auto const& target: AgentTargets)
    {
        auto const configDir = configDirOf(target.configDirEnv, target.defaultDir, env, home);
        auto outcome = AgentOutcome { .agent = target.name,
                                      .selection = selectionOf(target, agents, fileSystem, configDir),
                                      .configDir = configDir,
                                      .skill = std::nullopt,
                                      .hooks = std::nullopt };
        // An agent a named list leaves out is not reported; one found missing is, with how to set it up.
        if (outcome.selection == AgentSelection::Skipped && !agents.empty())
            continue;
        if (outcome.selection != AgentSelection::Skipped)
        {
            if (target.hookAgent && hookSetup.action != HookAction::Skip)
            {
                auto hooks = setupHooks(
                    fileSystem, target, configDir, relocationOf(target.configDirEnv, env), hookSetup, mode);
                if (!hooks)
                    return std::unexpected(std::move(hooks.error()));
                outcome.hooks = *std::move(hooks);
            }
            if (!target.skillPath.empty())
            {
                if (hookSetup.action == HookAction::Uninstall)
                    outcome.skill = SkillFileOutcome { .path = configDir / target.skillPath,
                                                       .state = SkillState::Untouched };
                else
                {
                    auto skill =
                        setupSkill(fileSystem, configDir / target.skillPath, mode, foreign, skillText);
                    if (!skill)
                        return std::unexpected(std::move(skill.error()));
                    outcome.skill = *std::move(skill);
                }
            }
        }
        outcomes.emplace_back(std::move(outcome));
    }
    return outcomes;
}

std::string formatSkillOutcomes(std::span<AgentOutcome const> outcomes,
                                SetupMode mode,
                                HookAction action,
                                OutcomeFormat format)
{
    if (format == OutcomeFormat::Json)
    {
        auto rows = std::vector<std::string> {};
        for (auto const& outcome: outcomes)
        {
            auto const skill =
                outcome.skill ? std::format(R"(,"skill":{{"path":{},"state":{}}})",
                                            vtcontrol::quoteJsonString(core::pathToUtf8(outcome.skill->path)),
                                            vtcontrol::quoteJsonString(jsonNameOf(outcome.skill->state)))
                              : std::string {};
            auto const hooks =
                outcome.hooks ? std::format(R"(,"hooks":{{"path":{},"state":{},"detail":{}}})",
                                            vtcontrol::quoteJsonString(core::pathToUtf8(outcome.hooks->path)),
                                            vtcontrol::quoteJsonString(jsonNameOf(outcome.hooks->result)),
                                            vtcontrol::quoteJsonString(outcome.hooks->detail))
                              : std::string {};
            rows.emplace_back(std::format(R"({{"agent":{},"selection":{}{}{}}})",
                                          vtcontrol::quoteJsonString(outcome.agent),
                                          vtcontrol::quoteJsonString(jsonNameOf(outcome.selection)),
                                          skill,
                                          hooks));
        }
        return std::format("[{}]\n", core::joinHumanReadable(rows, ","));
    }

    auto labelOf = [](std::string_view agent) {
        auto const row = std::ranges::find(AgentTargets, agent, &AgentTarget::name);
        return row != AgentTargets.end() ? row->label : agent;
    };
    auto width = std::size_t { 0 };
    for (auto const& outcome: outcomes)
        width = std::max(width, labelOf(outcome.agent).size());
    auto const dryRun = mode == SetupMode::DryRun ? " (dry run)" : "";
    auto text = std::string {};
    for (auto const& outcome: outcomes)
    {
        if (outcome.selection == AgentSelection::Skipped)
            text += std::format("{:<{}}  skipped  {} not found; {}\n",
                                labelOf(outcome.agent),
                                width,
                                core::pathToUtf8(outcome.configDir),
                                action == HookAction::Uninstall
                                    ? std::string { "nothing to remove" }
                                    : std::format("name it with --agent {} to set it up", outcome.agent));
        if (outcome.skill)
            text += std::format("{:<{}}  skill  {}{}  {}\n",
                                labelOf(outcome.agent),
                                width,
                                nameOf(outcome.skill->state),
                                dryRun,
                                core::pathToUtf8(outcome.skill->path));
        if (outcome.hooks)
            text += std::format("{:<{}}  hooks  {}{}  {}{}\n",
                                labelOf(outcome.agent),
                                width,
                                nameOf(outcome.hooks->result),
                                dryRun,
                                core::pathToUtf8(outcome.hooks->path),
                                outcome.hooks->detail.empty() ? ""s : ": " + outcome.hooks->detail);
    }
    return text;
}

} // namespace contour::cli
