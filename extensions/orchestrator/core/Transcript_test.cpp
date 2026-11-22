// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include <orchestrator/core/Fixtures.hpp>
#include <orchestrator/core/Transcript.hpp>

using namespace std::string_view_literals;
using namespace orchestrator;

namespace
{

[[nodiscard]] std::vector<TranscriptEntryKind> kindsOf(TranscriptPage const& page)
{
    auto kinds = std::vector<TranscriptEntryKind> {};
    for (auto const& entry: page.entries)
        kinds.push_back(entry.kind);
    return kinds;
}

} // namespace

TEST_CASE("format 1 pages parse", "[orchestrator][transcript]")
{
    SECTION("the recorded session")
    {
        auto const page = parseTranscriptPage(
            readFixture(FixtureRoot::Contract, "agent-transcript/format-1/basic-page-1.json"));

        REQUIRE(page.has_value());
        using enum TranscriptEntryKind;
        CHECK(kindsOf(*page)
              == std::vector {
                  User, ToolCall, ToolError, ToolCall, ToolResult, ToolCall, ToolResult, Assistant });
        CHECK(page->page == 1);
        CHECK(page->pages == 1);
        CHECK(page->notUnderstood == 0);
        CHECK(page->writtenBy == "2.1.328");
        CHECK(page->entries.front().text.starts_with("Create one.txt"));
        CHECK(page->entries.front().time.starts_with("2026-"));
        // Printed in UTC: every entry's offset is 0.
        CHECK(std::ranges::all_of(page->entries,
                                  [](TranscriptEntry const& entry) { return entry.utcOffsetMinutes == 0; }));
    }
    SECTION("an empty log")
    {
        auto const page = parseTranscriptPage(
            readFixture(FixtureRoot::Contract, "agent-transcript/format-1/empty-page-1.json"));

        REQUIRE(page.has_value());
        CHECK(page->entries.empty());
        CHECK(page->page == 1);
        CHECK(page->pages == 1);
    }
}

TEST_CASE("each entry keeps the offset its own instant had", "[orchestrator][transcript]")
{
    auto const page = parseTranscriptPage(
        R"({"format":1,"session":"s","writtenBy":"2.1.328","page":1,"pages":1,"notUnderstood":0,"entries":[)"
        R"({"kind":"user","time":"2026-10-25T00:59:00.000Z","offset":120,"text":"summer"},)"
        R"({"kind":"user","time":"2026-10-25T01:00:00.000Z","offset":60,"text":"winter"},)"
        R"({"kind":"user","time":"2026-10-25T01:00:00.000Z","text":"no offset"},)"
        R"({"kind":"user","time":"2026-10-25T01:00:00.000Z","offset":99999,"text":"no zone has it"}]})");

    REQUIRE(page.has_value());
    REQUIRE(page->entries.size() == 4);
    CHECK(page->entries[0].utcOffsetMinutes == 120);
    CHECK(page->entries[1].utcOffsetMinutes == 60);
    CHECK_FALSE(page->entries[2].utcOffsetMinutes.has_value());
    CHECK_FALSE(page->entries[3].utcOffsetMinutes.has_value());
}

TEST_CASE("an unknown kind is counted, not fatal", "[orchestrator][transcript]")
{
    auto const page = parseTranscriptPage(
        R"({"format":1,"session":"00000000-0000-4000-8000-000000000001","writtenBy":"9.9.9","page":1,"pages":1,)"
        R"("notUnderstood":2,"entries":[{"kind":"hologram","time":"t","text":"x"},)"
        R"({"kind":"assistant","time":"t","text":"kept"}],"aNewerKey":true})");

    REQUIRE(page.has_value());
    REQUIRE(page->entries.size() == 1);
    CHECK(page->entries.front().kind == TranscriptEntryKind::Assistant);
    CHECK(page->entries.front().text == "kept");
    CHECK(page->notUnderstood == 3);
}

TEST_CASE("another format is a format mismatch", "[orchestrator][transcript]")
{
    CHECK(parseTranscriptPage(R"({"format":2,"session":"s","page":1,"pages":1,"entries":[]})").error()
          == TranscriptReadError::FormatMismatch);
    // Its shape may change with its number: that is a mismatch too, not a wrong shape.
    CHECK(parseTranscriptPage(R"({"format":2,"entries":{"turns":7}})").error()
          == TranscriptReadError::FormatMismatch);
    CHECK(parseTranscriptPage(R"({"entries":[]})").error() == TranscriptReadError::FormatMismatch);
}

TEST_CASE("a page that is not JSON or not a page is refused", "[orchestrator][transcript]")
{
    CHECK(parseTranscriptPage("").error() == TranscriptReadError::NotJson);
    CHECK(parseTranscriptPage(R"({"format":1,"entries":[)").error() == TranscriptReadError::NotJson);
    CHECK(parseTranscriptPage(R"({"format":1} trailing)").error() == TranscriptReadError::NotJson);
    CHECK(parseTranscriptPage(R"({"format":1,"entries":5})").error() == TranscriptReadError::WrongShape);
}

TEST_CASE("the verb's refusal is a format mismatch", "[orchestrator][transcript]")
{
    CHECK(classifyTranscriptFailure("unsupported transcript format 1; this contour prints 2\n")
          == TranscriptReadError::FormatMismatch);
    CHECK(classifyTranscriptFailure(std::string { UnsupportedFormatPrefix } + "7; this contour prints 1")
          == TranscriptReadError::FormatMismatch);
    CHECK(classifyTranscriptFailure("no session log for x in /wt under /h/.claude/projects\n")
          == TranscriptReadError::NotJson);
    CHECK(classifyTranscriptFailure("") == TranscriptReadError::NotJson);
}
