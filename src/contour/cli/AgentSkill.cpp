// SPDX-License-Identifier: Apache-2.0
#include <contour/cli/AgentSkill.hpp>
#include <contour/cli/ControlVerbs.hpp>

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
        }
        return {};
    }

    /// @param target An agent.
    /// @param env The environment.
    /// @param home The home directory.
    /// @return The agent's configuration directory.
    [[nodiscard]] std::filesystem::path configDirOf(AgentTarget const& target,
                                                    core::Environment const& env,
                                                    std::filesystem::path const& home)
    {
        if (!target.configDirEnv.empty())
            if (auto const relocated = env.get(target.configDirEnv); relocated && !relocated->empty())
                return core::pathFromUtf8(*relocated).value_or(home / target.defaultDir);
        return home / target.defaultDir;
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
    line("- `$CONTOUR_WINDOW`: your window.");
    line("- `$CONTOUR_DAEMON`: the daemon's socket.");
    line();
    line("## Targets");
    line();
    line("Wherever a verb takes `--pane P` (or a tab or window), `P` is resolved in this order, the first "
         "kind "
         "of match winning: a full id; a label (case-insensitive, see `contour label`); a 1-based position "
         "(tabs and windows); an id suffix of at least four hex digits. Several matches are an error that "
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
    line("Put `--` before the text a verb types or runs: without it, text that starts with an option's name "
         "is read as that option (`contour send --enter wait` waits, and types nothing).");
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

std::expected<std::vector<SkillOutcome>, std::string> setupAgentSkills(core::platform::FileSystem& fileSystem,
                                                                       core::Environment const& env,
                                                                       std::filesystem::path const& home,
                                                                       std::span<std::string const> agents,
                                                                       SetupMode mode,
                                                                       ForeignSkill foreign,
                                                                       std::string_view skillText)
{
    for (auto const& name: agents)
        if (!std::ranges::contains(AgentTargets, name, &AgentTarget::name))
        {
            auto known = std::string {};
            for (auto const& target: AgentTargets)
                known += (known.empty() ? "" : ", ") + std::string { target.name };
            return std::unexpected(std::format("unknown agent '{}'; known agents: {}", name, known));
        }

    auto outcomes = std::vector<SkillOutcome> {};
    for (auto const& target: AgentTargets)
    {
        if (!agents.empty() && !std::ranges::contains(agents, target.name))
            continue;
        auto const path = configDirOf(target, env, home) / target.skillPath;
        auto outcome = SkillOutcome { .agent = target.name, .path = path, .state = SkillState::Created };
        if (fileSystem.exists(path))
        {
            auto const existing = fileSystem.readFile(path);
            if (!existing)
                return std::unexpected(
                    std::format("cannot read {}: {}", core::pathToUtf8(path), existing.error()));
            if (!existing->contains(AgentSkillMarker))
                outcome.state = foreign == ForeignSkill::Replace ? SkillState::Updated : SkillState::Foreign;
            else
                outcome.state = *existing == skillText ? SkillState::Unchanged : SkillState::Updated;
        }
        auto const writes = outcome.state == SkillState::Created || outcome.state == SkillState::Updated;
        if (writes && mode == SetupMode::Write)
        {
            auto written = fileSystem.createDirectories(path.parent_path()).and_then([&] {
                return fileSystem.writeFile(path, skillText);
            });
            if (!written)
                return std::unexpected(
                    std::format("cannot write {}: {}", core::pathToUtf8(path), written.error()));
        }
        outcomes.emplace_back(std::move(outcome));
    }
    return outcomes;
}

std::string formatSkillOutcomes(std::span<SkillOutcome const> outcomes, SetupMode mode, OutcomeFormat format)
{
    if (format == OutcomeFormat::Json)
    {
        auto rows = std::vector<std::string> {};
        for (auto const& outcome: outcomes)
            rows.emplace_back(std::format(R"({{"agent":{},"path":{},"state":{}}})",
                                          vtcontrol::quoteJsonString(outcome.agent),
                                          vtcontrol::quoteJsonString(core::pathToUtf8(outcome.path)),
                                          vtcontrol::quoteJsonString(jsonNameOf(outcome.state))));
        return std::format("[{}]\n", core::joinHumanReadable(rows, ","));
    }

    auto labelOf = [](std::string_view agent) {
        auto const row = std::ranges::find(AgentTargets, agent, &AgentTarget::name);
        return row != AgentTargets.end() ? row->label : agent;
    };
    auto width = std::size_t { 0 };
    for (auto const& outcome: outcomes)
        width = std::max(width, labelOf(outcome.agent).size());
    auto text = std::string {};
    for (auto const& outcome: outcomes)
        text += std::format("{:<{}}  skill  {}{}  {}\n",
                            labelOf(outcome.agent),
                            width,
                            nameOf(outcome.state),
                            mode == SetupMode::DryRun ? " (dry run)" : "",
                            core::pathToUtf8(outcome.path));
    return text;
}

} // namespace contour::cli
