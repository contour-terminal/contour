// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/screen/ProgramStatusSet.hpp>

#include <catch2/catch_test_macros.hpp>

#include <format>
#include <ranges>
#include <string>
#include <vector>

using namespace vtbackend;

namespace
{

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

    auto const root = set.apply(recordAt("", ProgramState::Working));
    CHECK(root.updated == std::vector<std::string> { "" });
    CHECK(root.removed.empty());
    CHECK(root.reason == ProgramStatusReason::Report);
    (void) set.apply(recordAt("build", ProgramState::Done));
    (void) set.apply(recordAt("build/test", ProgramState::Blocked));
    (void) set.apply(recordAt("builder", ProgramState::Idle));
    CHECK(set.records().size() == 4);

    // A report replaces its id's record whole and makes it the most recently updated.
    (void) set.apply(recordAt("", ProgramState::Done));
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
        (void) set.apply(recordAt(std::format("r{}", index), ProgramState::Working));
    CHECK(set.records().size() == MaxProgramStatusRecords);
    auto const evicting = set.apply(recordAt("r64", ProgramState::Working));
    CHECK(evicting.updated == std::vector<std::string> { "r64" });
    CHECK(evicting.removed == std::vector<std::string> { "r0" });
    CHECK(set.records().size() == MaxProgramStatusRecords);
    CHECK(set.find("r0") == nullptr);

    // Replacing a record already held evicts nothing, though the set is full.
    CHECK(set.apply(recordAt("r1", ProgramState::Done)).removed.empty());
}

TEST_CASE("ProgramStatusSet: a prompt drops working and blocked only", "[vtbackend][programstatus]")
{
    auto set = ProgramStatusSet {};
    (void) set.apply(recordAt("idle", ProgramState::Idle));
    (void) set.apply(recordAt("working", ProgramState::Working));
    (void) set.apply(recordAt("done", ProgramState::Done));
    (void) set.apply(recordAt("blocked", ProgramState::Blocked));
    (void) set.apply(recordAt("error", ProgramState::Error));

    auto const prompt = set.promptStarted();
    CHECK(prompt.reason == ProgramStatusReason::Prompt);
    CHECK(prompt.removed == std::vector<std::string> { "working", "blocked" });
    CHECK(idsOf(set) == std::vector<std::string> { "idle", "done", "error" });

    CHECK(set.promptStarted().empty());

    (void) set.apply(recordAt("working", ProgramState::Working));
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
    (void) set.apply(recordAt("", ProgramState::Working, "deploy"));
    (void) set.apply(recordAt("eu", ProgramState::Working, "terraform"));
    (void) set.apply(recordAt("eu/west/1", ProgramState::Working));
    (void) set.apply(recordAt("us", ProgramState::Working));

    CHECK(set.appOf(*set.find("eu/west/1")) == "terraform"); // "eu/west" is absent; "eu" names one
    CHECK(set.appOf(*set.find("us")) == "deploy");
    CHECK(set.appOf(*set.find("eu")) == "terraform");

    (void) set.clear("");
    (void) set.apply(recordAt("a/b", ProgramState::Idle));
    CHECK(set.appOf(*set.find("a/b")).empty());
}

TEST_CASE("ProgramStatusSet: a mirror replaces the whole set", "[vtbackend][programstatus]")
{
    auto set = ProgramStatusSet { 2 };
    (void) set.apply(recordAt("same", ProgramState::Done));
    (void) set.apply(recordAt("gone", ProgramState::Working));

    auto const change = set.replaceAll({ recordAt("old", ProgramState::Idle),
                                         recordAt("same", ProgramState::Done),
                                         recordAt("new", ProgramState::Error) });
    CHECK(change.reason == ProgramStatusReason::Mirror);
    // Held to the cap, keeping the newest; "same" did not change, so only "new" is news.
    CHECK(idsOf(set) == std::vector<std::string> { "same", "new" });
    CHECK(change.updated == std::vector<std::string> { "new" });
    CHECK(change.removed == std::vector<std::string> { "gone" });

    CHECK(set.replaceAll({ recordAt("same", ProgramState::Done), recordAt("new", ProgramState::Error) })
              .empty());
}
