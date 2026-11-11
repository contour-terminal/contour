// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/screen/ProgramStatusSet.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <format>
#include <ranges>
#include <string>
#include <vector>

using namespace vtbackend;

namespace
{

/// A fixed instant for tests that do not look at `since`.
constexpr auto Epoch = std::chrono::system_clock::time_point {};

/// A record of @p state at @p id, naming @p app.
ProgramStatusRecord recordAt(std::string id, ProgramState state, std::string app = {})
{
    return ProgramStatusRecord { .id = std::move(id),
                                 .state = state,
                                 .kind = BlockedKind::None,
                                 .progress = std::nullopt,
                                 .app = std::move(app),
                                 .title = {},
                                 .message = {} };
}

/// The ids of @p set, least recently updated first.
std::vector<std::string> idsOf(ProgramStatusSet const& set)
{
    auto ids = std::vector<std::string> {};
    for (auto const& record: set.records())
        ids.push_back(record.id);
    return ids;
}

} // namespace

TEST_CASE("ProgramStatusSet replaces, clears subtrees and evicts at the cap", "[vtbackend][programstatus]")
{
    auto set = ProgramStatusSet {};

    auto const root = set.apply(recordAt("", ProgramState::Working), Epoch);
    CHECK(root.updated == std::vector<std::string> { "" });
    CHECK(root.removed.empty());
    CHECK(root.reason == ProgramStatusReason::Report);
    (void) set.apply(recordAt("build", ProgramState::Done), Epoch);
    (void) set.apply(recordAt("build/test", ProgramState::Blocked), Epoch);
    (void) set.apply(recordAt("builder", ProgramState::Idle), Epoch);
    CHECK(set.records().size() == 4);

    // A report replaces its id's record whole and makes it the most recently updated.
    (void) set.apply(recordAt("", ProgramState::Done), Epoch);
    CHECK(idsOf(set) == std::vector<std::string> { "build", "build/test", "builder", "" });
    CHECK(set.find("")->state == ProgramState::Done);

    // "builder" is not beneath "build": only whole segments nest.
    auto const cleared = set.clear("build");
    CHECK(cleared.removed == std::vector<std::string> { "build", "build/test" });
    CHECK(idsOf(set) == std::vector<std::string> { "builder", "" });

    CHECK(set.clear("nothing-here").empty());
    CHECK(set.clear("").removed.size() == 2);
    CHECK(set.records().empty());

    // 65 distinct ids: the oldest goes.
    for (auto const index: std::views::iota(0, 64))
        (void) set.apply(recordAt(std::format("r{}", index), ProgramState::Working), Epoch);
    CHECK(set.records().size() == MaxProgramStatusRecords);
    auto const evicting = set.apply(recordAt("r64", ProgramState::Working), Epoch);
    CHECK(evicting.updated == std::vector<std::string> { "r64" });
    CHECK(evicting.removed == std::vector<std::string> { "r0" });
    CHECK(set.records().size() == MaxProgramStatusRecords);
    CHECK(set.find("r0") == nullptr);

    // Replacing a record already held evicts nothing, though the set is full.
    CHECK(set.apply(recordAt("r1", ProgramState::Done), Epoch).removed.empty());
}

TEST_CASE("ProgramStatusSet: a prompt drops working and blocked only", "[vtbackend][programstatus]")
{
    auto set = ProgramStatusSet {};
    (void) set.apply(recordAt("idle", ProgramState::Idle), Epoch);
    (void) set.apply(recordAt("working", ProgramState::Working), Epoch);
    (void) set.apply(recordAt("done", ProgramState::Done), Epoch);
    (void) set.apply(recordAt("blocked", ProgramState::Blocked), Epoch);
    (void) set.apply(recordAt("error", ProgramState::Error), Epoch);

    auto const prompt = set.promptStarted();
    CHECK(prompt.reason == ProgramStatusReason::Prompt);
    CHECK(prompt.removed == std::vector<std::string> { "working", "blocked" });
    CHECK(idsOf(set) == std::vector<std::string> { "idle", "done", "error" });

    CHECK(set.promptStarted().empty());

    (void) set.apply(recordAt("working", ProgramState::Working), Epoch);
    auto const exited = set.processExited();
    CHECK(exited.reason == ProgramStatusReason::ProcessExit);
    CHECK(exited.removed == std::vector<std::string> { "working" });

    auto const reset = set.reset();
    CHECK(reset.reason == ProgramStatusReason::Reset);
    CHECK(reset.removed.size() == 3);
    CHECK(set.records().empty());
}

TEST_CASE("ProgramStatusSet: app is inherited from the nearest ancestor", "[vtbackend][programstatus]")
{
    auto set = ProgramStatusSet {};
    (void) set.apply(recordAt("", ProgramState::Working, "deploy"), Epoch);
    (void) set.apply(recordAt("eu", ProgramState::Working, "terraform"), Epoch);
    (void) set.apply(recordAt("eu/west/1", ProgramState::Working), Epoch);
    (void) set.apply(recordAt("us", ProgramState::Working), Epoch);

    CHECK(set.appOf(*set.find("eu/west/1")) == "terraform"); // "eu/west" is absent; "eu" names one
    CHECK(set.appOf(*set.find("us")) == "deploy");
    CHECK(set.appOf(*set.find("eu")) == "terraform");

    (void) set.clear("");
    (void) set.apply(recordAt("a/b", ProgramState::Idle), Epoch);
    CHECK(set.appOf(*set.find("a/b")).empty());
}

TEST_CASE("ProgramStatusSet: a mirror replaces the whole set", "[vtbackend][programstatus]")
{
    auto set = ProgramStatusSet { 2 };
    (void) set.apply(recordAt("same", ProgramState::Done), Epoch);
    (void) set.apply(recordAt("gone", ProgramState::Working), Epoch);

    auto const same = *set.find("same");
    auto const change =
        set.replaceAll({ recordAt("old", ProgramState::Idle), same, recordAt("new", ProgramState::Error) },
                       ProgramStatusTiming::Carried,
                       Epoch);
    CHECK(change.reason == ProgramStatusReason::Mirror);
    // Held to the cap, keeping the newest; "same" did not change, so only "new" is news.
    CHECK(idsOf(set) == std::vector<std::string> { "same", "new" });
    CHECK(change.updated == std::vector<std::string> { "new" });
    CHECK(change.removed == std::vector<std::string> { "gone" });

    CHECK(set.replaceAll({ same, *set.find("new") }, ProgramStatusTiming::Carried, Epoch).empty());
}

namespace
{

/// An Integration-origin record of @p state at @p id.
ProgramStatusRecord integrationAt(std::string id, ProgramState state, BlockedKind kind = BlockedKind::None)
{
    auto record = recordAt(std::move(id), state);
    record.origin = ProgramStatusOrigin::Integration;
    record.kind = kind;
    return record;
}

} // namespace

TEST_CASE("ProgramStatusSet.generation.bumpsOnStateChangeOnly", "[programstatus]")
{
    auto set = ProgramStatusSet {};
    auto const t0 = std::chrono::system_clock::time_point { std::chrono::seconds(100) };
    auto const t1 = t0 + std::chrono::seconds(5);
    (void) set.apply(recordAt("a", ProgramState::Working), t0);
    auto const g0 = set.find("a")->generation;
    CHECK(set.find("a")->since == t0);
    auto still = recordAt("a", ProgramState::Working);
    still.message = "still";
    (void) set.apply(still, t1);
    CHECK(set.find("a")->generation == g0); // same state: same generation, same since
    CHECK(set.find("a")->since == t0);
    (void) set.apply(recordAt("a", ProgramState::Done), t1);
    CHECK(set.find("a")->generation > g0);
    CHECK(set.find("a")->since == t1);
}

TEST_CASE("ProgramStatusSet.generation.blockedKindChangeBumps", "[programstatus]")
{
    auto set = ProgramStatusSet {};
    auto const t0 = std::chrono::system_clock::time_point { std::chrono::seconds(100) };
    auto const t1 = t0 + std::chrono::seconds(1);
    (void) set.apply(integrationAt("a", ProgramState::Blocked, BlockedKind::Permission), t0);
    auto const g0 = set.find("a")->generation;
    (void) set.apply(integrationAt("a", ProgramState::Blocked, BlockedKind::Question), t1);
    CHECK(set.find("a")->generation > g0);
    CHECK(set.find("a")->since == t1);
}

TEST_CASE("ProgramStatusSet.promptStarted.clearsIntegrationRecordsOfAnyState", "[programstatus]")
{
    auto set = ProgramStatusSet {};
    (void) set.apply(integrationAt("agent", ProgramState::Done), Epoch);
    (void) set.apply(recordAt("prog", ProgramState::Done), Epoch);
    auto const change = set.promptStarted();
    CHECK(change.removed == std::vector<std::string> { "agent" });
    CHECK(idsOf(set) == std::vector<std::string> { "prog" });
}

TEST_CASE("ProgramStatusSet.processExited.clearsIntegrationRecords", "[programstatus]")
{
    auto set = ProgramStatusSet {};
    (void) set.apply(integrationAt("agent", ProgramState::Error), Epoch);
    (void) set.apply(recordAt("prog", ProgramState::Error), Epoch);
    auto const change = set.processExited();
    CHECK(change.removed == std::vector<std::string> { "agent" });
    CHECK(idsOf(set) == std::vector<std::string> { "prog" });
}

TEST_CASE("ProgramStatusSet.noteUserInput.turnsIntegrationBlockedIntoWorking", "[programstatus]")
{
    auto set = ProgramStatusSet {};
    auto const t0 = std::chrono::system_clock::time_point { std::chrono::seconds(100) };
    auto const t1 = t0 + std::chrono::seconds(9);
    (void) set.apply(integrationAt("agent", ProgramState::Blocked, BlockedKind::Permission), t0);
    (void) set.apply(recordAt("prog", ProgramState::Blocked), t0);
    auto const g0 = set.find("agent")->generation;

    auto const change = set.noteUserInput(t1);
    CHECK(change.reason == ProgramStatusReason::UserInput);
    CHECK(change.updated == std::vector<std::string> { "agent" });
    auto const& agent = *set.find("agent");
    CHECK(agent.state == ProgramState::Working);
    CHECK(agent.kind == BlockedKind::None);
    CHECK(agent.generation > g0);
    CHECK(agent.since == t1);
    CHECK(set.find("prog")->state == ProgramState::Blocked);
    // Updated last, so it is the most recently updated record now.
    CHECK(idsOf(set) == std::vector<std::string> { "prog", "agent" });

    CHECK(set.noteUserInput(t1).empty());
}

TEST_CASE("ProgramStatusSet.clearIntegration.removesOnlyTheIntegrationsOwnRecord", "[programstatus]")
{
    auto set = ProgramStatusSet {};
    auto const t0 = std::chrono::system_clock::time_point { std::chrono::seconds(100) };
    (void) set.apply(integrationAt("agent", ProgramState::Working), t0);
    (void) set.apply(integrationAt("agent/sub", ProgramState::Working), t0);
    (void) set.apply(recordAt("prog", ProgramState::Working), t0);

    CHECK(set.clearIntegration("prog").empty()); // a program's record
    auto const change = set.clearIntegration("agent");
    CHECK(change.removed == std::vector<std::string> { "agent" });
    CHECK(idsOf(set) == std::vector<std::string> { "agent/sub", "prog" }); // not the subtree
    CHECK(set.clearIntegration("agent").empty());
}

TEST_CASE("ProgramStatusSet.replaceAll.keepsSinceAndGenerationVerbatim", "[programstatus]")
{
    auto set = ProgramStatusSet {};
    auto const t0 = std::chrono::system_clock::time_point { std::chrono::seconds(100) };
    auto record = integrationAt("a", ProgramState::Working);
    record.since = t0;
    record.generation = 7;
    (void) set.replaceAll({ record }, ProgramStatusTiming::Carried, Epoch);
    CHECK(set.find("a")->since == t0);
    CHECK(set.find("a")->generation == 7);
    CHECK(set.nextGeneration() > 7);
}

TEST_CASE("ProgramStatusSet.replaceAll.stampsRecordsThatCarryNoTiming", "[programstatus]")
{
    // An older host sends no generation: unstamped, every Done would be generation 0, which every view has
    // seen, so it would never show.
    auto set = ProgramStatusSet {};
    auto const t0 = std::chrono::system_clock::time_point { std::chrono::seconds(100) };
    auto const t1 = t0 + std::chrono::seconds(5);
    auto const fromHost = [](ProgramState state) {
        auto record = recordAt("agent", state);
        record.since = {};
        record.generation = 0;
        return record;
    };

    (void) set.replaceAll({ fromHost(ProgramState::Working) }, ProgramStatusTiming::Absent, t0);
    auto const working = *set.find("agent");
    CHECK(working.generation != 0);
    CHECK(working.since == t0);

    // The same state again is no change: it keeps its instant and generation, and is no news.
    CHECK(set.replaceAll({ fromHost(ProgramState::Working) }, ProgramStatusTiming::Absent, t1).empty());
    CHECK(set.find("agent")->generation == working.generation);
    CHECK(set.find("agent")->since == t0);

    // A new state is a new generation, newer than anything seen before it.
    auto const change = set.replaceAll({ fromHost(ProgramState::Done) }, ProgramStatusTiming::Absent, t1);
    CHECK(change.updated == std::vector<std::string> { "agent" });
    CHECK(set.find("agent")->generation > working.generation);
    CHECK(set.find("agent")->since == t1);
}
