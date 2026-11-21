// SPDX-License-Identifier: Apache-2.0
#include <contour/cli/AgentTranscript.hpp>
#include <contour/cli/ExitStatus.hpp>
#include <contour/cli/SessionLogTestSupport.hpp>

#include <core/platform/testing/InMemoryFileSystem.hpp>
#include <core/testing/Environment.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <format>
#include <fstream>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <glaze/glaze.hpp>

using namespace std::string_view_literals;
using namespace contour::cli;
using namespace contour::cli::testing;

namespace
{

/// One entry of a format-1 page, as the orchestrator reads it: what the tests read the verb's output into.
struct PrintedEntry
{
    std::string kind;
    std::string time;
    std::optional<int32_t> offset;
    std::string text;
};

/// A format-1 page, as the orchestrator reads it.
struct PrintedPage
{
    uint32_t format = 0;
    std::string session;
    std::string writtenBy;
    uint32_t page = 0;
    uint32_t pages = 0;
    std::size_t notUnderstood = 0;
    std::vector<PrintedEntry> entries;
};

} // namespace

namespace
{

/// The recorded fixture's Claude Code version: the directory it lives in.
constexpr auto RecordedVersion = "2.1.328"sv;

[[nodiscard]] std::filesystem::path testData()
{
    return std::filesystem::path { CONTOUR_AGENT_TRANSCRIPT_TESTDATA };
}

[[nodiscard]] std::string fixture(std::filesystem::path const& relative)
{
    auto in = std::ifstream { testData() / relative, std::ios::binary };
    REQUIRE(in.good());
    return std::string { std::istreambuf_iterator<char> { in }, std::istreambuf_iterator<char> {} };
}

struct Outcome
{
    int status = 0;
    std::string out;
    std::string err;
};

/// The time zone UTC: what the contract's pages are printed in.
[[nodiscard]] std::chrono::local_seconds utcLocalTime(std::chrono::system_clock::time_point when) noexcept
{
    return std::chrono::local_seconds { std::chrono::floor<std::chrono::seconds>(when).time_since_epoch() };
}

/// Central European time around the end of daylight saving in 2026: CEST (UTC+2) until 2026-10-25 01:00 UTC,
/// CET (UTC+1) from then on.
[[nodiscard]] std::chrono::local_seconds berlinLocalTime(std::chrono::system_clock::time_point when) noexcept
{
    using namespace std::chrono;
    constexpr auto SummerTimeEnds = sys_days { 2026y / October / 25 } + 1h;
    auto const offset = when < SummerTimeEnds ? 2h : 1h;
    return local_seconds { floor<seconds>(when).time_since_epoch() + offset };
}

/// @return The format-1 page @p json, its keys checked one by one.
[[nodiscard]] PrintedPage pageFrom(std::string const& json)
{
    auto document = glz::generic {};
    REQUIRE_FALSE(glz::read_json(document, json));
    auto const number = [](glz::generic const& value) {
        REQUIRE(value.is_number());
        return value.get_number();
    };
    auto page = PrintedPage { .format = static_cast<uint32_t>(number(document["format"])),
                              .session = document["session"].get_string(),
                              .writtenBy = document["writtenBy"].get_string(),
                              .page = static_cast<uint32_t>(number(document["page"])),
                              .pages = static_cast<uint32_t>(number(document["pages"])),
                              .notUnderstood = static_cast<std::size_t>(number(document["notUnderstood"])),
                              .entries = {} };
    for (auto const& entry: document["entries"].get_array())
    {
        // Every key is one format 1 names; offset alone may be left out.
        for (auto const& [key, value]: entry.get_object())
            CHECK(std::ranges::contains(std::array { "kind"sv, "time"sv, "offset"sv, "text"sv }, key));
        page.entries.push_back(
            PrintedEntry { .kind = entry["kind"].get_string(),
                           .time = entry["time"].get_string(),
                           .offset = entry.contains("offset")
                                         ? std::optional { static_cast<int32_t>(number(entry["offset"])) }
                                         : std::nullopt,
                           .text = entry["text"].get_string() });
    }
    return page;
}

/// A home with Claude Code's projects directory, and the logs in it.
struct World
{
    core::platform::testing::InMemoryFileSystem fs;
    MemoryOpener opener;
    std::optional<core::testing::FakeEnvironment> env { std::in_place,
                                                        std::map<std::string, std::string, std::less<>> {
                                                            { std::string { ClaudeHomeVariable },
                                                              nativePath(Home) } } };
    vtbackend::LocalTimeConverter localTime = &utcLocalTime; ///< The reader's time zone: UTC unless set.

    World() { fs.addDirectory(projectsDirectory()); }

    /// Adds a log of the session under `<projects>/<project>/`, its `cwd` values spelled for this platform.
    std::filesystem::path addLog(std::string_view project,
                                 std::string_view content,
                                 std::filesystem::path const& projects = projectsDirectory())
    {
        auto const directory = projects / project;
        fs.addDirectory(projects);
        fs.addDirectory(directory);
        auto const log = directory / std::format("{}.jsonl", Session);
        fs.addFile(log, "in the opener");
        opener.add(log, withNativeCwds(content));
        return log;
    }

    [[nodiscard]] Outcome run(TranscriptRequest const& request)
    {
        auto out = std::ostringstream {};
        auto err = std::ostringstream {};
        auto const status = runAgentTranscript(request, *env, fs, opener, localTime, out, err);
        return Outcome { .status = status, .out = out.str(), .err = err.str() };
    }

    /// @return What the verb prints for the run in @p cwd, as text.
    [[nodiscard]] Outcome runIn(std::string_view cwd) { return run(request(cwd)); }

    /// @return Page @p page as the verb prints it in format 1; fails the test unless it prints one.
    [[nodiscard]] PrintedPage printedPage(uint32_t page = 1)
    {
        auto asked = request(Worktree, page);
        asked.output = TranscriptOutput::Json;
        auto const outcome = run(asked);
        INFO(outcome.err);
        REQUIRE(outcome.status == 0);
        return pageFrom(outcome.out);
    }

    /// @param cwd The run's directory, spelled the POSIX way.
    [[nodiscard]] static TranscriptRequest request(std::string_view cwd = Worktree, uint32_t page = 1)
    {
        return TranscriptRequest { .session = std::string { Session },
                                   .cwd = nativePath(cwd),
                                   .page = page,
                                   .format = TranscriptFormat,
                                   .output = TranscriptOutput::Text };
    }
};

/// @return Page @p page of @p log, the run's only log, as the verb prints it in format 1.
[[nodiscard]] PrintedPage pageOf(std::string_view log, uint32_t page = 1)
{
    auto world = World {};
    world.addLog("-home-user-proj", log);
    return world.printedPage(page);
}

/// @return The kind of each of @p page's entries, as format 1 names it.
[[nodiscard]] std::vector<std::string> kindsOf(PrintedPage const& page)
{
    auto kinds = std::vector<std::string> {};
    for (auto const& entry: page.entries)
        kinds.push_back(entry.kind);
    return kinds;
}

/// @return Whether the verb refused, finding no log of the run.
[[nodiscard]] bool foundNoLog(Outcome const& outcome)
{
    return outcome.status == exitCode(ExitStatus::TargetNotFound) && outcome.out.empty()
           && outcome.err.starts_with("no session log for ");
}

/// A typed prompt, as Claude Code writes one.
[[nodiscard]] std::string userLine(std::string_view text, std::string_view cwd = Worktree)
{
    return std::format(R"({{"type":"user","timestamp":"2026-11-19T10:00:00.000Z","version":"2.1.328",)"
                       R"("cwd":"{}","sessionId":"{}","isSidechain":false,)"
                       R"("message":{{"role":"user","content":"{}"}}}})"
                       "\n",
                       cwd,
                       Session,
                       text);
}

/// An assistant reply made of one block.
[[nodiscard]] std::string assistantLine(std::string_view block)
{
    return std::format(R"({{"type":"assistant","timestamp":"2026-11-19T10:00:01.000Z","version":"2.1.328",)"
                       R"("cwd":"{}","sessionId":"{}","isSidechain":false,)"
                       R"("message":{{"role":"assistant","content":[{}]}}}})"
                       "\n",
                       Worktree,
                       Session,
                       block);
}

/// A generated log of @p count prompts, numbered from @p first.
[[nodiscard]] std::string generatedLog(int count, int first = 0)
{
    auto log = std::string {};
    for (auto const number: std::views::iota(first, first + count))
        log += userLine(std::format("prompt {}", number));
    return log;
}

[[nodiscard]] std::string metadataLine(std::string_view type, std::size_t padding = 0)
{
    return std::format(R"({{"type":"{}","sessionId":"{}","message":{{"role":"user","content":"{}"}}}})"
                       "\n",
                       type,
                       Session,
                       std::string(padding, 'x'));
}

} // namespace

// {{{ reading a page
TEST_CASE("the recorded log reads as user, tool-call, tool-result and assistant entries", "[cli][transcript]")
{
    auto const page = pageOf(fixture(std::filesystem::path { RecordedVersion } / "basic.jsonl"));

    CHECK(kindsOf(page)
          == std::vector<std::string> { "user",
                                        "tool-call",
                                        "tool-error",
                                        "tool-call",
                                        "tool-result",
                                        "tool-call",
                                        "tool-result",
                                        "assistant" });
    CHECK(page.page == 1);
    CHECK(page.pages == 1);
    CHECK(page.notUnderstood == 0);
    CHECK(page.writtenBy == RecordedVersion);
    REQUIRE(page.entries.size() == 8);
    CHECK(page.entries[0].text.starts_with("Create one.txt"));
    CHECK(page.entries[3].text.starts_with("Write {"));
    CHECK(page.entries[0].time.starts_with("2026-"));
}

TEST_CASE("every metadata type is ignored, not counted", "[cli][transcript]")
{
    constexpr auto MeasuredMetadata = std::array {
        "last-prompt"sv,
        "mode"sv,
        "permission-mode"sv,
        "session-owner"sv,
        "bridge-session"sv,
        "custom-title"sv,
        "agent-name"sv,
        "ai-title"sv,
        "queue-operation"sv,
        "file-history-snapshot"sv,
        "file-history-delta"sv,
        "api-request"sv,
        "api-request-shape"sv,
        "api-request-blob"sv,
        "atis-latch"sv,
        "tools-used"sv,
        "pr-link"sv,
        "cost-state"sv,
        "frame-link"sv,
        "artifact-comment-monitor"sv,
        "artifact-autoreact-ledger"sv,
        "history-suppression"sv,
        "attachment"sv,
    };
    auto log = std::string {};
    for (auto const type: MeasuredMetadata)
        log += metadataLine(type);
    for (auto const subtype: { "turn_duration"sv,
                               "stop_hook_summary"sv,
                               "local_command"sv,
                               "away_summary"sv,
                               "informational"sv,
                               "scheduled_task_fire"sv,
                               "bridge_status"sv })
        log += std::format(R"({{"type":"system","subtype":"{}","cwd":"{}"}})"
                           "\n",
                           subtype,
                           Worktree);

    auto const page = pageOf(log);

    CHECK(page.entries.empty());
    CHECK(page.notUnderstood == 0);
    CHECK(page.pages == 1);
}

TEST_CASE("an unknown entry type is counted and skipped", "[cli][transcript]")
{
    auto const page = pageOf(fixture("synthetic/unknown-entries.jsonl"));

    CHECK(kindsOf(page) == std::vector<std::string> { "user", "assistant" });
    // future-metadata, the non-JSON line, the hologram block and the future system subtype.
    CHECK(page.notUnderstood == 4);
    CHECK(page.entries.back().text == "Hello.");
}

TEST_CASE("a line that is not JSON is counted", "[cli][transcript]")
{
    auto const page = pageOf(userLine("one") + "{not json\n" + "[1,2]\n" + userLine("two"));

    CHECK(page.entries.size() == 2);
    CHECK(page.notUnderstood == 2);
}

TEST_CASE("an oversized line is skipped and counted", "[cli][transcript]")
{
    constexpr auto TwoMiB = std::size_t { 2 } * 1024 * 1024;
    auto const oversizedTurn = userLine(std::string(TwoMiB, 'x'));
    auto const oversizedBlob = metadataLine("api-request-blob", TwoMiB);

    auto const page = pageOf(userLine("before") + oversizedTurn + oversizedBlob + userLine("after"));

    CHECK(page.entries.size() == 2);
    CHECK(page.notUnderstood == 1);
}

TEST_CASE("a torn last line is left for the next read", "[cli][transcript]")
{
    auto log = fixture("synthetic/torn-tail.jsonl");
    REQUIRE_FALSE(log.ends_with('\n'));

    auto const torn = pageOf(log);
    CHECK(torn.entries.size() == 1);
    CHECK(torn.notUnderstood == 0);

    log += ".\"}]}}\n";
    auto const whole = pageOf(log);
    CHECK(whole.entries.size() == 2);
    CHECK(whole.entries.back().text == "Hello.");
    CHECK(whole.notUnderstood == 0);
}

TEST_CASE("api-request-blob is never a turn", "[cli][transcript]")
{
    auto const blob =
        std::format(R"({{"type":"api-request-blob","cwd":"{}","version":"9.9.9",)"
                    R"("message":{{"role":"user","content":[{{"type":"text","text":"not a turn"}}]}}}})"
                    "\n",
                    Worktree);

    auto const page = pageOf(userLine("one") + blob);

    CHECK(page.entries.size() == 1);
    CHECK(page.notUnderstood == 0);
    CHECK(page.writtenBy == "2.1.328");
}

TEST_CASE("thinking blocks and isMeta entries are not shown", "[cli][transcript]")
{
    auto const page = pageOf(fixture("synthetic/compacted.jsonl"));

    // The summary a compaction writes (isCompactSummary), the injected context (isMeta) and the thinking
    // block are all left out.
    CHECK(kindsOf(page) == std::vector<std::string> { "user", "note", "assistant" });
    CHECK(page.notUnderstood == 0);
    for (auto const& entry: page.entries)
    {
        CHECK_FALSE(entry.text.contains("Summary"));
        CHECK_FALSE(entry.text.contains("Injected"));
    }
}

TEST_CASE("a compaction is a note", "[cli][transcript]")
{
    auto const page = pageOf(fixture("synthetic/compacted.jsonl"));

    REQUIRE(page.entries.size() == 3);
    CHECK(page.entries[1].kind == "note");
    CHECK(page.entries[1].text == "conversation compacted");
    CHECK(page.entries[1].time == "2026-11-19T10:05:00.000Z");
}

TEST_CASE("a page before the last is stable while the log grows", "[cli][transcript]")
{
    auto log = generatedLog(65);
    auto world = World {};
    world.addLog("-home-user-proj", log);
    auto const before1 = world.run(World::request(Worktree, 1)).out;
    auto const before2 = world.run(World::request(Worktree, 2)).out;

    log += generatedLog(10, 65);
    world.addLog("-home-user-proj", log);

    // Only the count of pages in the header moves on.
    auto const header = [](std::string const& text) {
        return text.substr(text.find('\n'));
    };
    CHECK(header(world.run(World::request(Worktree, 1)).out) == header(before1));
    CHECK(header(world.run(World::request(Worktree, 2)).out) == header(before2));
    CHECK(pageOf(log, 3).entries.size() == 15);
}

TEST_CASE("pages are contiguous", "[cli][transcript]")
{
    auto const log = generatedLog(65);

    auto texts = std::vector<std::string> {};
    for (auto const number: std::views::iota(1U, 4U))
    {
        auto const page = pageOf(log, number);
        CHECK(page.page == number);
        CHECK(page.pages == 3);
        for (auto const& entry: page.entries)
            texts.push_back(entry.text);
    }

    REQUIRE(texts.size() == 65);
    for (auto const number: std::views::iota(0, 65))
        CHECK(texts[static_cast<std::size_t>(number)] == std::format("prompt {}", number));
}

TEST_CASE("page 0 and a page past the last are errors", "[cli][transcript]")
{
    auto world = World {};
    world.addLog("-home-user-proj", generatedLog(65));
    for (auto const page: { 0U, 4U })
    {
        INFO(page);
        auto const outcome = world.run(World::request(Worktree, page));
        CHECK(outcome.status == exitCode(ExitStatus::TargetNotFound));
        CHECK(outcome.out.empty());
        CHECK(outcome.err == std::format("page {} of 3 does not exist\n", page));
    }

    // An empty log is page 1 of 1.
    auto const empty = pageOf(fixture("synthetic/empty.jsonl"));
    CHECK(empty.entries.empty());
    CHECK(empty.pages == 1);
}

TEST_CASE("escapes in a tool result are dropped", "[cli][transcript]")
{
    auto const result = std::format(
        R"({{"type":"user","timestamp":"2026-11-19T10:00:00.000Z","version":"2.1.328",)"
        R"("cwd":"{}","message":{{"role":"user","content":[{{"type":"tool_result",)"
        R"("tool_use_id":"toolu_1","content":"a\u001b[31mred\u001b[0m\u0007\nnext \u202eline"}}]}}}})"
        "\n",
        Worktree);

    auto const page = pageOf(result);

    REQUIRE(page.entries.size() == 1);
    CHECK(page.entries[0].kind == "tool-result");
    CHECK(page.entries[0].text == "a[31mred[0m\nnext line");
}

TEST_CASE("an entry is cut at 2 KiB on a code-point boundary", "[cli][transcript]")
{
    auto umlauts = std::string {};
    for ([[maybe_unused]] auto const index: std::views::iota(0, 1500))
        umlauts += "ä";
    auto const page =
        pageOf(assistantLine(std::format(R"({{"type":"text","text":"{}\n{}"}})", umlauts, umlauts)));

    REQUIRE(page.entries.size() == 1);
    auto const& text = page.entries[0].text;
    CHECK(text.size() <= MaxTranscriptEntryBytes);
    CHECK(text.size() >= MaxTranscriptEntryBytes - 1);
    CHECK(text.size() % 2 == 0); // every "ä" is two bytes: none was cut in half
    CHECK_FALSE(text.contains('\n'));
}

TEST_CASE("a sanitised time and version", "[cli][transcript]")
{
    auto const line =
        std::format(R"({{"type":"user","timestamp":"2026-11-19T10:00:00\u001b]0;x\u0007.000Z",)"
                    R"("version":"2.1\u001b[2J.328","cwd":"{}","message":{{"role":"user","content":"hi"}}}})"
                    "\n",
                    Worktree);
    auto const page = pageOf(line);

    REQUIRE(page.entries.size() == 1);
    CHECK(page.entries[0].time == "2026-11-19T10:00:00]0;x.000Z");
    // Not a time it can read: no offset is claimed for it.
    CHECK_FALSE(page.entries[0].offset.has_value());
    CHECK(page.writtenBy == "2.1[2J.328");
    auto world = World {};
    world.addLog("-home-user-proj", line);
    for (auto const output: { TranscriptOutput::Text, TranscriptOutput::Json })
    {
        auto request = World::request();
        request.output = output;
        auto const printed = world.run(request).out;
        CHECK_FALSE(printed.empty());
        CHECK_FALSE(printed.contains('\x1b'));
        CHECK_FALSE(printed.contains('\x07'));
    }
}
// }}}

// {{{ finding the log
TEST_CASE("not a session id is refused", "[cli][transcript]")
{
    for (auto const session: { "../x"sv,
                               "00000000-0000-4000-8000-00000000000A"sv,
                               "{00000000-0000-4000-8000-000000000001}"sv,
                               "00000000-0000-4000-8000-000000000001/../x"sv,
                               ""sv })
    {
        INFO(session);
        auto world = World {};
        world.addLog("-home-user-proj", userLine("one"));
        auto request = World::request();
        request.session = std::string { session };

        auto const outcome = world.run(request);
        CHECK(outcome.status == exitCode(ExitStatus::BadUsage));
        CHECK(outcome.err.starts_with("not a session id: "));
        CHECK(world.opener.calls() == 0);
    }
}

TEST_CASE("a session with no log names the directory searched", "[cli][transcript]")
{
    auto world = World {};

    auto const outcome = world.run(World::request());
    CHECK(foundNoLog(outcome));
    CHECK(outcome.err
          == std::format("no session log for {} in {} under {}\n",
                         Session,
                         nativePath(Worktree),
                         projectsDirectory().string()));
}

TEST_CASE("a log from another directory is not this run's", "[cli][transcript]")
{
    auto world = World {};
    world.addLog("-home-user-other", fixture("synthetic/other-cwd.jsonl"));

    CHECK(foundNoLog(world.runIn(Worktree)));
}

TEST_CASE("a log whose agent went into a subdirectory is this run's", "[cli][transcript]")
{
    auto world = World {};
    world.addLog("-home-user-proj", fixture("synthetic/subdir-cwd.jsonl"));

    CHECK(world.runIn(Worktree).status == 0);

    // The recorded session, whose agent ran `cd sub`, is bound too.
    auto recorded = World {};
    recorded.addLog("-home-user-proj", fixture(std::filesystem::path { RecordedVersion } / "basic.jsonl"));
    CHECK(recorded.runIn(Worktree).status == 0);
}

TEST_CASE("a log whose agent left the worktree is not this run's", "[cli][transcript]")
{
    auto world = World {};
    world.addLog("-home-user-proj", fixture("synthetic/left-worktree.jsonl"));

    CHECK(foundNoLog(world.runIn(Worktree)));
}

TEST_CASE("a log with no cwd is not this run's", "[cli][transcript]")
{
    auto world = World {};
    world.addLog("-home-user-proj", fixture("synthetic/no-cwd.jsonl"));

    CHECK(foundNoLog(world.runIn(Worktree)));
}

TEST_CASE("a sibling directory sharing a prefix is not beneath", "[cli][transcript]")
{
    auto world = World {};
    world.addLog("-wt-ab", fixture("synthetic/prefix-sibling.jsonl"));
    CHECK(foundNoLog(world.runIn("/wt/a")));
    CHECK(world.runIn("/wt/ab").status == 0);
}

TEST_CASE("the run's directory binds case-insensitively on Windows only", "[cli][transcript]")
{
    auto world = World {};
    world.addLog("-home-user-proj", userLine("one", "/home/user/PROJ/Sub"));
#ifdef _WIN32
    CHECK(world.runIn(Worktree).status == 0);
#else
    CHECK(foundNoLog(world.runIn(Worktree)));
#endif
}

TEST_CASE("a log whose agent left the worktree after the page asked for is not this run's",
          "[cli][transcript]")
{
    // The binding is checked in the same pass that reads the page, to the log's end: an entry written outside
    // the worktree after page 1 still refuses the log.
    auto world = World {};
    world.addLog("-home-user-proj", generatedLog(65) + userLine("outside", "/home/user"));

    auto const outcome = world.run(World::request(Worktree, 1));

    CHECK(outcome.status == exitCode(ExitStatus::TargetNotFound));
    CHECK(outcome.out.empty());
    CHECK(outcome.err.starts_with("no session log for "));
}

TEST_CASE("the log is opened once and read once", "[cli][transcript]")
{
    auto world = World {};
    world.addLog("-home-user-proj", generatedLog(65));

    auto const outcome = world.run(World::request(Worktree, 2));

    CHECK(outcome.status == 0);
    CHECK(world.opener.calls() == 1);
}

TEST_CASE("a projects directory that is a link is not searched", "[cli][transcript]")
{
    auto world = World {};
    world.addLog("-home-user-proj", userLine("one"));
    world.opener.link(projectsDirectory() / "-home-user-proj");

    CHECK(foundNoLog(world.runIn(Worktree)));
    CHECK(world.opener.calls() == 0);
}

TEST_CASE("two logs in this run's directory are refused", "[cli][transcript]")
{
    auto world = World {};
    auto const first = world.addLog("-home-user-proj", userLine("one"));
    auto const second = world.addLog("-home-user-proj-old", userLine("two"));
    world.addLog("-home-user-other", userLine("three", "/home/user/other"));

    auto const outcome = world.run(World::request());
    CHECK(outcome.status == exitCode(ExitStatus::Failed));
    CHECK(outcome.err.starts_with(
        std::format("two session logs for {} in {}: ", Session, nativePath(Worktree))));
    CHECK(outcome.err.contains(first.string()));
    CHECK(outcome.err.contains(second.string()));
}

TEST_CASE("a log the opener refuses is named", "[cli][transcript]")
{
    auto world = World {};
    auto const log = world.addLog("-home-user-proj", userLine("one"));
    world.opener.refuse(log, TranscriptError::NotRegular);

    CHECK(world.run(World::request()).err == std::format("{} is not a regular file\n", log.string()));

    world.opener.refuse(log, TranscriptError::Unreadable);
    CHECK(world.run(World::request()).err == std::format("{}: cannot be read\n", log.string()));
}

TEST_CASE("CLAUDE_CONFIG_DIR wins over the home", "[cli][transcript]")
{
    auto world = World {};
    world.env.emplace(std::map<std::string, std::string, std::less<>> {
        { std::string { ClaudeHomeVariable }, nativePath(Home) },
        { "CLAUDE_CONFIG_DIR", nativePath("/cfg") } });
    world.addLog("-home-user-proj", userLine("in HOME"));
    world.addLog("-home-user-proj", userLine("in the config"), projectsIn("/cfg"));

    auto const outcome = world.run(World::request());
    CHECK(outcome.status == 0);
    CHECK(outcome.out.contains("in the config"));
    CHECK_FALSE(outcome.out.contains("in HOME"));
}

TEST_CASE("neither variable set is NoConfigDirectory", "[cli][transcript]")
{
    auto world = World {};
    world.env.emplace();
    world.addLog("-home-user-proj", userLine("one"));

    auto const outcome = world.run(World::request());
    CHECK(outcome.status == exitCode(ExitStatus::Failed));
    CHECK(outcome.err.starts_with("no Claude Code configuration directory"));

    CHECK(
        outcome.err.ends_with(std::format("none of CLAUDE_CONFIG_DIR and {} is set\n", ClaudeHomeVariable)));

    world.env.emplace(std::map<std::string, std::string, std::less<>> {
        { std::string { ClaudeHomeVariable }, nativePath(Home) } });
    CHECK(world.run(World::request()).status == 0);
}

TEST_CASE("the home is the one Claude Code reads: USERPROFILE on Windows, HOME elsewhere",
          "[cli][transcript]")
{
    // Git Bash and MSYS2 set HOME on Windows, often to another directory; Node's os.homedir(), which Claude
    // Code keeps .claude in, reads USERPROFILE there and never HOME.
    auto world = World {};
    world.env.emplace(std::map<std::string, std::string, std::less<>> {
        { "HOME", nativePath("/home/posix") }, { "USERPROFILE", nativePath("/home/profile") } });
    world.addLog("-home-user-proj", userLine("in HOME"), projectsIn("/home/posix/.claude"));
    world.addLog("-home-user-proj", userLine("in USERPROFILE"), projectsIn("/home/profile/.claude"));
#ifdef _WIN32
    constexpr auto Read = "in USERPROFILE"sv;
    constexpr auto Ignored = "in HOME"sv;
#else
    constexpr auto Read = "in HOME"sv;
    constexpr auto Ignored = "in USERPROFILE"sv;
#endif

    auto const outcome = world.run(World::request());
    INFO(outcome.err);
    CHECK(outcome.status == 0);
    CHECK(outcome.out.contains(Read));
    CHECK_FALSE(outcome.out.contains(Ignored));
}
// }}}

// {{{ printing
TEST_CASE("another format is refused with the pinned line", "[cli][transcript]")
{
    auto world = World {};
    world.addLog("-home-user-proj", userLine("one"));
    auto request = World::request();
    request.format = 2;
    request.output = TranscriptOutput::Json;

    auto const outcome = world.run(request);

    CHECK(outcome.status != 0);
    CHECK(outcome.out.empty());
    CHECK(outcome.err == "unsupported transcript format 2; this contour prints 1\n");
}

TEST_CASE("the text page", "[cli][transcript]")
{
    auto world = World {};
    world.addLog("-home-user-proj", fixture(std::filesystem::path { RecordedVersion } / "basic.jsonl"));

    auto const outcome = world.run(World::request());

    CHECK(outcome.status == 0);
    CHECK(outcome.err.empty());
    CHECK(outcome.out == fixture(std::filesystem::path { RecordedVersion } / "page-1.txt"));
}

TEST_CASE("each entry carries the offset its own instant had, across a daylight-saving change",
          "[cli][transcript]")
{
    auto const at = [](std::string_view time, std::string_view text) {
        return std::format(R"({{"type":"user","timestamp":"{}","version":"2.1.328","cwd":"{}",)"
                           R"("message":{{"role":"user","content":"{}"}}}})"
                           "\n",
                           time,
                           Worktree,
                           text);
    };
    auto world = World {};
    world.localTime = &berlinLocalTime;
    world.addLog("-home-user-proj",
                 at("2026-10-25T00:59:00.000Z", "summer") + at("2026-10-25T01:00:00Z", "winter")
                     + at("2026-10-25T01:00:00+01:00", "not UTC"));

    auto const page = world.printedPage();

    REQUIRE(page.entries.size() == 3);
    CHECK(page.entries[0].offset == 120);
    CHECK(page.entries[1].offset == 60);
    // A time that is not UTC is printed as it is, with no offset claimed for it.
    CHECK_FALSE(page.entries[2].offset.has_value());
    // The text page names its times UTC, whatever the reader's zone.
    auto const text = world.run(World::request()).out;
    CHECK(text.contains("── user 00:59:00 UTC\n"));
    CHECK(text.contains("── user 01:00:00 UTC\n"));
    CHECK(text.contains("── user 01:00:00\n"));
}

TEST_CASE("an entry's text cannot pass for a header in the text page", "[cli][transcript]")
{
    auto world = World {};
    world.addLog("-home-user-proj",
                 userLine(R"(fine\n── user 10:00:00\nsession x\n3 entries not understood)"));

    auto const outcome = world.run(World::request());

    REQUIRE(outcome.status == 0);
    CHECK(outcome.out.ends_with("── user 10:00:00 UTC\n"
                                "  fine\n"
                                "  ── user 10:00:00\n"
                                "  session x\n"
                                "  3 entries not understood\n"));
}

TEST_CASE("an empty log prints format 1 with no entries", "[cli][transcript]")
{
    auto world = World {};
    world.addLog("-home-user-proj", fixture("synthetic/empty.jsonl"));
    auto request = World::request();
    request.output = TranscriptOutput::Json;

    // empty.jsonl's only cwd-bearing line is its attachment, so the log is bound to the worktree.
    auto const outcome = world.run(request);

    CHECK(outcome.status == 0);
    CHECK(outcome.out
          == std::format(R"({{"format":1,"session":"{}","writtenBy":"","page":1,"pages":1,"notUnderstood":0,)"
                         R"("entries":[]}})"
                         "\n",
                         Session));
}

TEST_CASE("a large log is read within its time class", "[cli][transcript]")
{
    constexpr auto ShownEntries = 20'000;
    constexpr auto Blobs = 200;
    constexpr auto BlobBytes = std::size_t { 512 } * 1024;
    auto log = std::string {};
    log.reserve((Blobs * (BlobBytes + 128)) + (std::size_t { ShownEntries } * 256));
    auto const blob = metadataLine("api-request-blob", BlobBytes);
    for (auto const index: std::views::iota(0, ShownEntries))
    {
        log += userLine(std::format("prompt {}", index));
        if (index % (ShownEntries / Blobs) == 0)
            log += blob;
    }
    auto world = World {};
    world.addLog("-home-user-proj", log);
    auto const lastPage =
        static_cast<uint32_t>((ShownEntries + TranscriptEntriesPerPage - 1) / TranscriptEntriesPerPage);

    for (auto const page: { 1U, lastPage })
    {
        INFO(page);
        auto request = World::request(Worktree, page);
        request.output = TranscriptOutput::Json;
        auto const start = std::chrono::steady_clock::now();
        auto const outcome = world.run(request);
        auto const elapsed = std::chrono::steady_clock::now() - start;

        CHECK(outcome.status == 0);
        CHECK(outcome.out.contains(std::format(R"("page":{},"pages":{})", page, lastPage)));
        // The command's timeout class is 30 s; an ASan build must stay well within it.
        CHECK(elapsed < std::chrono::seconds(10));
        WARN(std::format("page {} of a {}-entry log with {} blobs of 512 KiB: {} ms",
                         page,
                         ShownEntries,
                         Blobs,
                         std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()));
    }
}
// }}}

// {{{ the orchestrator's contract
TEST_CASE("the verb prints the contract's pages", "[cli][transcript][contract]")
{
    struct ContractPage
    {
        std::string_view page;     ///< The page, in format-1/.
        std::filesystem::path log; ///< The log it was printed from, as the contract's README names it.
        uint32_t number = 1;       ///< The page number asked for.
    };
    auto const pages = std::array {
        ContractPage { .page = "basic-page-1.json",
                       .log = std::filesystem::path { RecordedVersion } / "basic.jsonl",
                       .number = 1 },
        ContractPage { .page = "empty-page-1.json", .log = "synthetic/empty.jsonl", .number = 1 },
    };
    auto const contract = std::filesystem::path { CONTOUR_AGENT_TRANSCRIPT_CONTRACT } / "format-1";

    // Every page of the contract is held to the verb: a page added there without a row here fails.
    for (auto const& entry: std::filesystem::directory_iterator { contract })
    {
        INFO(entry.path().string());
        CHECK(std::ranges::contains(pages, entry.path().filename().string(), [](ContractPage const& row) {
            return std::string { row.page };
        }));
    }

    for (auto const& row: pages)
    {
        INFO(row.page);
        auto world = World {};
        world.addLog("-home-user-proj", fixture(row.log));
        auto request = World::request(Worktree, row.number);
        request.output = TranscriptOutput::Json;

        auto const outcome = world.run(request);

        CHECK(outcome.status == 0);
        CHECK(outcome.err.empty());
        auto in = std::ifstream { contract / row.page, std::ios::binary };
        REQUIRE(in.good());
        CHECK(outcome.out
              == std::string { std::istreambuf_iterator<char> { in }, std::istreambuf_iterator<char> {} });
    }
}
// }}}
