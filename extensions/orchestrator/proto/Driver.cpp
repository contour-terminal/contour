// SPDX-License-Identifier: Apache-2.0
#include <orchestrator/proto/Driver.hpp>

#include <algorithm>
#include <array>
#include <expected>
#include <filesystem>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <orchestrator/core/Overloaded.hpp>
#include <orchestrator/proto/Environment.hpp>
#include <orchestrator/proto/Host.hpp>
#include <orchestrator/proto/Panes.hpp>

using namespace std::string_view_literals;

namespace orchestrator::proto
{

namespace
{
    /// How long the loop sleeps at most while a command closed its output but has not exited yet.
    constexpr auto ReapPollMs = 20;

    /// A dispatch refusal as the user reads it.
    struct RefusalText
    {
        DispatchRefusal reason;
        std::string_view text;
    };

    constexpr auto RefusalTexts = std::array {
        RefusalText { .reason = DispatchRefusal::NotConfigured, .text = "the configuration is unusable"sv },
        RefusalText { .reason = DispatchRefusal::AgentLimit, .text = "max_agents runs are active"sv },
        RefusalText { .reason = DispatchRefusal::RepoLimit, .text = "max_agents_per_repo runs are active"sv },
        RefusalText { .reason = DispatchRefusal::UnknownRepo, .text = "no repository has that name"sv },
        RefusalText { .reason = DispatchRefusal::UnknownTask, .text = "no such task for this item"sv },
        RefusalText { .reason = DispatchRefusal::UnknownModel, .text = "the model is not one of models"sv },
        RefusalText { .reason = DispatchRefusal::AlreadyRunning, .text = "the item has an active run"sv },
        RefusalText { .reason = DispatchRefusal::BaseUnknown, .text = "the base branch is not known yet"sv },
        RefusalText { .reason = DispatchRefusal::BadPrompt, .text = "the prompt cannot be passed on"sv },
    };

    /// @return How long a command of @p timeout may run.
    [[nodiscard]] std::chrono::milliseconds durationOf(Timeout timeout) noexcept
    {
        return timeout == Timeout::Short ? ShortTimeout : LongTimeout;
    }

    /// @return The panes the runs recorded opening: the driver's own record, Contour's listing having no tag.
    [[nodiscard]] std::vector<OwnedPane> ownedPanes(std::span<Run const> runs)
    {
        auto owned = std::vector<OwnedPane> {};
        for (auto const& run: runs)
            if (run.pane)
                owned.push_back(OwnedPane { .pane = *run.pane, .tag = run.id });
        return owned;
    }

} // namespace

Driver::Driver(Engine engine, DriverSetup setup):
    _engine { std::move(engine) },
    _setup { std::move(setup) },
    _config { Config::parse(_setup.configJson, _setup.host)
                  .transform([](Config config) { return std::optional<Config> { std::move(config) }; })
                  .value_or(std::nullopt) }
{
}

std::expected<Driver, DriverHealth> Driver::start(DriverSetup setup)
{
    auto stored = setup.store.load();
    auto runs = std::vector<Run> {};
    for (auto const& entry: stored)
        if (auto run = runFromJson(entry.value))
            runs.push_back(std::move(*run));
    auto const owned = ownedPanes(runs);

    // The listing's request time is the start's own time, read before the listing is asked for.
    auto const monoMs = monotonicNowMs();
    auto panes = std::vector<PaneFact> {};
    auto listing = setup.control.listWorkspaces().and_then([&](std::string const& json) {
        return parseWorkspaceList(json, owned).transform_error([](ParseError) {
            return ControlFailure { .why = "the listing could not be read" };
        });
    });
    if (listing)
        panes = std::move(*listing);
    else
    {
        // Unread, the panes are taken to be there, which tells no run that its tab is gone.
        std::println(stderr,
                     "warning: panes not listed ({}); every recorded tab is taken to be open",
                     listing.error().why);
        for (auto const& pane: owned)
            panes.push_back(PaneFact { .pane = pane.pane,
                                       .tag = pane.tag,
                                       .status = std::nullopt,
                                       .program = ProgramLiveness::Alive });
    }

    auto started = Engine::start(StartContext { .monoMs = monoMs,
                                                .wallMs = wallNowMs(),
                                                .host = setup.host,
                                                .configJson = setup.configJson,
                                                .stored = std::move(stored),
                                                .inFlight = {},
                                                .finished = {},
                                                .panes = std::move(panes),
                                                .paneListing = PaneListing::OwnRecordsOnly });
    auto driver = Driver { std::move(started.engine), std::move(setup) };
    if (auto const health = driver.carryOut(started.effects); health != DriverHealth::Healthy)
        return std::unexpected { health };
    if (auto const health = driver.drain(); health != DriverHealth::Healthy)
        return std::unexpected { health };
    return driver;
}

DriverHealth Driver::feed(Input input)
{
    _queue.push(std::move(input));
    return drain();
}

DriverHealth Driver::drain()
{
    while (true)
    {
        switch (_queue.next())
        {
            case NextStep::Idle: return DriverHealth::Healthy;
            case NextStep::Feed: {
                auto const input = _queue.take();
                if (auto const health = carryOut(_engine.step(input)); health != DriverHealth::Healthy)
                    return health;
                _queue.carriedOut(input);
                break;
            }
            case NextStep::ListPanes:
                // Every queued input is fed by now, so no run changes after this listing's request time
                // because of something that happened before it.
                _queue.listingTaken();
                if (auto listed = listPanes())
                    _queue.push(std::move(*listed));
                break;
        }
    }
}

DriverHealth Driver::carryOut(std::vector<Effect> const& effects)
{
    for (auto const& effect: effects)
    {
        auto health = DriverHealth::Healthy;
        std::visit(
            Overloaded {
                [&](RunCommand const& command) { runCommand(command); },
                [&](ReadFile const& read) {
                    _queue.push(FileRead {
                        .tag = read.tag, .content = readRegularFile(read.path), .monoMs = monotonicNowMs() });
                },
                [&](OpenPane const& open) { openPane(open); },
                [&](ClosePane const& close) {
                    if (auto const closed = _setup.control.closePane(controlPaneId(close.pane)); !closed)
                        std::println("close {}: {}", close.pane, closed.error().why);
                },
                [&](StoreSet const& set) {
                    if (!_setup.store.set(set.key, set.value))
                        health = DriverHealth::StoreFailed;
                },
                [&](StoreRemove const& remove) {
                    if (!_setup.store.remove(remove.key))
                        health = DriverHealth::StoreFailed;
                },
                [&](PostInbox const& post) {
                    std::println(
                        "inbox {} for pane {}: not delivered by the prototype", post.message, post.pane);
                },
                [&](PublishSummary const& summary) {
                    if (summary.shortText != _publishedShort)
                        std::println("{}", summary.shortText);
                    _publishedShort = summary.shortText;
                },
                [&](WakeAt const& wake) { _wakeAtMs = wake.monoMs; },
                [&](Redraw const&) {},
                [&](DispatchRefused const& refused) {
                    auto const text = std::ranges::find(RefusalTexts, refused.reason, &RefusalText::reason);
                    std::println("dispatch refused ({}): {}",
                                 refused.repo,
                                 text != RefusalTexts.end() ? text->text : "?"sv);
                },
            },
            effect);
        if (health != DriverHealth::Healthy)
        {
            std::println(
                stderr, "error: the store at {} could not be written; stopping", _setup.store.directory());
            return health;
        }
    }
    return DriverHealth::Healthy;
}

std::expected<Child, StartResult> Driver::startCommand(RunCommand const& command) const
{
    auto const* const decl = findCommand(command.command);
    auto argv = decl != nullptr ? substitute(decl->argv, command.arguments)
                                : std::unexpected { SubstituteError::UnknownPlaceholder };
    if (!argv)
        return std::unexpected { StartResult::Refused };
    auto error = std::error_code {};
    if (decl->root == Root::Worktrees)
        std::filesystem::create_directories(_setup.host.worktreeRoot, error);
    if (!std::filesystem::is_directory(command.cwd, error))
        return std::unexpected { StartResult::CwdMissing };
    auto const layers = std::array { commandEnvironment(), decl->environment };
    return Child::spawn(SpawnRequest { .argv = std::move(*argv),
                                       .environment = withVariables(_setup.environment, layers),
                                       .cwd = command.cwd,
                                       .timeout = durationOf(decl->timeout) },
                        monotonicNowMs());
}

void Driver::runCommand(RunCommand const& command)
{
    auto child = startCommand(command);
    if (child)
        _children.emplace_back(command.tag, std::move(*child));
    else // Every RunCommand gets exactly one ProcessFinished: one that cannot start gets it now.
        _queue.push(ProcessFinished {
            .tag = command.tag, .outcome = notStartedOutcome(child.error()), .monoMs = monotonicNowMs() });
}

std::expected<std::string, std::string> Driver::openTabFor(OpenPane const& open) const
{
    auto const* const decl = findPaneTemplate(open.paneTemplate);
    auto argv = decl != nullptr ? substitute(decl->argv, open.arguments)
                                : std::unexpected { SubstituteError::UnknownPlaceholder };
    if (!argv)
        return std::unexpected { std::string("the pane template refused its arguments") };
    // `env NAME=value … <argv>` stands in for the host's pane environment.
    auto command = std::vector<std::string> { "env" };
    for (auto const& variable: decl->environment)
        command.push_back(std::format("{}={}", variable.name, variable.value));
    std::ranges::move(*argv, std::back_inserter(command));
    auto const opened = _setup.control.openTab(open.cwd, open.title, std::move(command));
    if (!opened)
        return std::unexpected { opened.error().why };
    auto const pane = enginePaneId(*opened);
    if (!pane)
        return std::unexpected { std::string("tab.create replied with an unknown pane id") };
    return std::string(*pane);
}

void Driver::openPane(OpenPane const& open)
{
    auto pane = openTabFor(open);
    if (pane)
        _queue.push(PaneOpenedFor { .run = open.run, .pane = std::move(*pane), .monoMs = monotonicNowMs() });
    else
        _queue.push(PaneOpenFailedFor {
            .run = open.run, .why = std::move(pane.error()), .monoMs = monotonicNowMs() });
}

std::optional<PanesListed> Driver::listPanes() const
{
    // Read before the listing is asked for: a run changed after it is not spoken for by the listing.
    auto const requestedMonoMs = monotonicNowMs();
    auto const owned = ownedPanes(_engine.runs());
    auto const listing = _setup.control.listWorkspaces();
    if (!listing)
    {
        std::println("panes not listed: {}", listing.error().why);
        return std::nullopt;
    }
    auto facts = parseWorkspaceList(*listing, owned);
    if (!facts)
        return std::nullopt; // An incomplete listing is never fed.
    return PanesListed { .panes = std::move(*facts),
                         .requestedMonoMs = requestedMonoMs,
                         .listing = PaneListing::OwnRecordsOnly };
}

void Driver::addPollFds(std::vector<pollfd>& fds) const
{
    for (auto const& [tag, child]: _children)
        child.addPollFds(fds);
}

int Driver::pollTimeoutMs(uint64_t nowMs) const
{
    auto until = _wakeAtMs;
    for (auto const& [tag, child]: _children)
    {
        until = std::min(until.value_or(child.nextCheckMs()), child.nextCheckMs());
        auto open = std::vector<pollfd> {};
        child.addPollFds(open);
        if (open.empty())
            until = std::min(*until, nowMs + ReapPollMs);
    }
    if (!until)
        return -1;
    return static_cast<int>(*until > nowMs ? *until - nowMs : 0);
}

DriverHealth Driver::service(std::span<pollfd const> fds)
{
    auto const nowMs = monotonicNowMs();
    for (auto& [tag, child]: _children)
    {
        child.service(fds, nowMs);
        child.enforceDeadline(nowMs);
        if (child.state() == ChildState::Ended)
            _queue.push(
                ProcessFinished { .tag = tag, .outcome = child.outcome(), .monoMs = monotonicNowMs() });
    }
    std::erase_if(_children, [](auto const& entry) { return entry.second.state() == ChildState::Ended; });
    if (_wakeAtMs && nowMs >= *_wakeAtMs)
    {
        _wakeAtMs.reset();
        _queue.push(Wake { .monoMs = nowMs, .wallMs = wallNowMs() });
    }
    return drain();
}

void Driver::stopCommands() noexcept
{
    for (auto& [tag, child]: _children)
        child.terminate();
    _children.clear();
}

} // namespace orchestrator::proto
