// SPDX-License-Identifier: Apache-2.0
#include <vthost/WorkspaceGit.hpp>

#include <core/async/DetachedTask.hpp>
#include <core/async/ResumeOn.hpp>

#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <string_view>
#include <unordered_map>

using vtbackend::GitHeadKind;

namespace vthost
{

std::optional<std::pair<uint32_t, uint32_t>> parseLeftRightCount(std::string_view standardOutput)
{
    while (!standardOutput.empty() && (standardOutput.back() == '\n' || standardOutput.back() == '\r'))
        standardOutput.remove_suffix(1);
    auto const separator = standardOutput.find_first_of(" \t");
    if (separator == std::string_view::npos)
        return std::nullopt;
    auto const number = [](std::string_view text) -> std::optional<uint32_t> {
        auto value = uint32_t {};
        auto const [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (text.empty() || error != std::errc {} || end != text.data() + text.size())
            return std::nullopt;
        return value;
    };
    auto const behind = number(standardOutput.substr(0, separator));
    auto const ahead = number(standardOutput.substr(separator + 1));
    if (!behind || !ahead)
        return std::nullopt;
    return std::pair { *behind, *ahead };
}

std::vector<std::string> gitProbeEnvironment(core::Environment const& environment)
{
    // What git reads to find its configuration and the programs it starts, and nothing else of the daemon's.
    static constexpr auto Inherited = std::to_array<std::string_view>(
        { "PATH", "HOME", "USER", "XDG_CONFIG_HOME", "GIT_CONFIG_GLOBAL", "GIT_CONFIG_SYSTEM" });
    auto result = std::vector<std::string> {};
    for (auto const name: Inherited)
        if (auto const value = environment.get(name))
            result.push_back(std::string { name } + '=' + *value);
    result.emplace_back("GIT_OPTIONAL_LOCKS=0");
    // No prompt can be answered: a credential helper or askpass that waits for a user would hang the probe.
    result.emplace_back("GIT_TERMINAL_PROMPT=0");
    result.emplace_back("GIT_ASKPASS=");
    return result;
}

namespace
{
    /// How long `git rev-list` may take: a hung repository or mount must not stall every workspace's git
    /// state.
    constexpr auto GitCommandDeadline = std::chrono::seconds { 5 };

    class CommandGitStatusProbe final: public IGitStatusProbe
    {
      public:
        CommandGitStatusProbe(vtbackend::GitFileSystem const& fileSystem, CommandRunner& commands):
            _fileSystem { fileSystem }, _commands { commands }
        {
        }

        [[nodiscard]] std::optional<GitInfo> probe(std::filesystem::path const& cwd) const override
        {
            auto const head = vtbackend::readGitHead(_fileSystem, cwd);
            if (!head)
                return std::nullopt;
            auto info = GitInfo { .kind = head->kind, .branch = head->name };
            if (head->kind != GitHeadKind::Branch)
                return info;
            auto const argv =
                std::vector<std::string> { "git",          "-C",      cwd.string(),        "rev-list",
                                           "--left-right", "--count", "@{upstream}...HEAD" };
            // Exit 128 is "no upstream configured": a branch alone, which is no failure.
            auto const outcome = _commands.runWithin(argv, GitCommandDeadline);
            if (outcome && outcome->exitCode == 0)
                if (auto const counts = parseLeftRightCount(outcome->standardOutput))
                {
                    info.behind = counts->first;
                    info.ahead = counts->second;
                }
            return info;
        }

      private:
        vtbackend::GitFileSystem const& _fileSystem;
        CommandRunner& _commands;
    };

    /// Whether a probe is running for a workspace.
    enum class ProbePhase : uint8_t
    {
        Idle = 0,  ///< None.
        Running,   ///< One is queued or running.
        Superseded ///< One is, and was asked again meanwhile: its answer is stale.
    };

    struct Entry
    {
        uint64_t ticket = 0; ///< Names this entry's life: a result for a forgotten one is not its own.
        std::filesystem::path cwd {};
        ProbePhase phase = ProbePhase::Idle;
        std::optional<GitInfo> info {};
    };
} // namespace

std::unique_ptr<IGitStatusProbe> makeGitStatusProbe(vtbackend::GitFileSystem const& fileSystem,
                                                    CommandRunner& commands)
{
    return std::make_unique<CommandGitStatusProbe>(fileSystem, commands);
}

struct WorkspaceGitTracker::State
{
    State(IGitStatusProbe const& probe, core::async::IExecutor& executor, Post post, Changed onChanged):
        probe { probe }, executor { executor }, post { std::move(post) }, onChanged { std::move(onChanged) }
    {
    }

    /// Starts the probe of @p entry's directory on the executor.
    static void start(std::shared_ptr<State> const& state, uint64_t workspace, Entry& entry)
    {
        entry.phase = ProbePhase::Running;
        (void) probeOnExecutor(state, workspace, entry.ticket, entry.cwd);
    }

    /// A static member with by-value parameters: the frame outlives the call, and maybe the tracker.
    static core::async::DetachedTask probeOnExecutor(std::shared_ptr<State> state,
                                                     uint64_t workspace,
                                                     uint64_t ticket,
                                                     std::filesystem::path cwd)
    {
        co_await core::async::ResumeOn { state->executor };
        if (state->stopped.load(std::memory_order_acquire))
            co_return;
        auto answer = state->probe.probe(cwd);
        state->post([state, workspace, ticket, answer = std::move(answer)] {
            State::settle(state, workspace, ticket, answer);
        });
    }

    /// Takes a probe's answer, on the loop thread.
    static void settle(std::shared_ptr<State> const& state,
                       uint64_t workspace,
                       uint64_t ticket,
                       std::optional<GitInfo> const& answer)
    {
        if (state->stopped.load(std::memory_order_acquire))
            return;
        auto const found = state->entries.find(workspace);
        if (found == state->entries.end() || found->second.ticket != ticket)
            return;
        auto& entry = found->second;
        auto const stale = entry.phase == ProbePhase::Superseded;
        entry.phase = ProbePhase::Idle;
        if (stale)
        {
            start(state, workspace, entry); // The newest directory's answer is the one worth telling.
            return;
        }
        if (entry.info == answer)
            return;
        entry.info = answer;
        auto const told = entry.info; // The callback may forget the workspace.
        state->onChanged(vtworkspace::WorkspaceId { workspace }, told);
    }

    IGitStatusProbe const& probe;
    core::async::IExecutor& executor;
    Post post;
    Changed onChanged;
    std::atomic<bool> stopped = false;
    std::unordered_map<uint64_t, Entry> entries;
    uint64_t nextTicket = 1;
};

WorkspaceGitTracker::WorkspaceGitTracker(IGitStatusProbe const& probe,
                                         core::async::IExecutor& executor,
                                         Post post,
                                         Changed onChanged):
    _state { std::make_shared<State>(probe, executor, std::move(post), std::move(onChanged)) }
{
}

WorkspaceGitTracker::~WorkspaceGitTracker()
{
    _state->stopped.store(true, std::memory_order_release);
}

void WorkspaceGitTracker::refresh(vtworkspace::WorkspaceId workspace, std::filesystem::path cwd)
{
    auto [found, created] = _state->entries.try_emplace(workspace.value);
    auto& entry = found->second;
    if (created)
        entry.ticket = _state->nextTicket++;
    entry.cwd = std::move(cwd);
    if (entry.phase == ProbePhase::Idle)
        State::start(_state, workspace.value, entry);
    else
        entry.phase = ProbePhase::Superseded;
}

void WorkspaceGitTracker::forget(vtworkspace::WorkspaceId workspace)
{
    _state->entries.erase(workspace.value);
}

std::optional<GitInfo> WorkspaceGitTracker::infoOf(vtworkspace::WorkspaceId workspace) const
{
    auto const found = _state->entries.find(workspace.value);
    return found == _state->entries.end() ? std::nullopt : found->second.info;
}

} // namespace vthost
