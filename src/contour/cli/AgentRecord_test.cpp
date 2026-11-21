// SPDX-License-Identifier: Apache-2.0
#include <contour/cli/AgentRecord.hpp>
#include <contour/cli/ExitStatus.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

#ifndef _WIN32
    #include <sys/stat.h>

    #include <unistd.h>
#endif

using namespace std::string_view_literals;
using namespace contour::cli;

namespace
{

/// Keeps what was appended, per file; can be told to fail.
class MemoryAppender final: public IRecordAppender
{
  public:
    [[nodiscard]] std::expected<void, std::string> append(std::filesystem::path const& file,
                                                          std::string_view bytes) override
    {
        if (failure)
            return std::unexpected(*failure);
        files[file] += bytes;
        return {};
    }

    std::map<std::filesystem::path, std::string> files;
    std::optional<std::string> failure;
};

struct Outcome
{
    int status = 0;
    std::string err;
};

auto const Ledger = std::filesystem::path { "state" } / AgentRunsFileName;

[[nodiscard]] Outcome record(MemoryAppender& appender, std::string_view line, uint32_t format = RecordFormat)
{
    auto err = std::ostringstream {};
    auto const status = runAgentRecord(
        RecordRequest { .line = std::string { line }, .format = format }, Ledger, appender, err);
    return Outcome { .status = status, .err = err.str() };
}

} // namespace

TEST_CASE("a run record is appended as one compact line", "[cli][agent][record]")
{
    auto appender = MemoryAppender {};
    REQUIRE(record(appender, R"({"format":1,"run":"pr-1","stats":null})").status == 0);
    // Pretty-printed, with a line break inside a string: still one line.
    REQUIRE(record(appender, "{\n  \"format\": 1,\n  \"run\": \"pr-2\",\n  \"note\": \"a\\nb\"\n}").status
            == 0);
    CHECK(appender.files[Ledger]
          == "{\"format\":1,\"run\":\"pr-1\",\"stats\":null}\n"
             "{\"format\":1,\"run\":\"pr-2\",\"note\":\"a\\nb\"}\n");
}

TEST_CASE("what is no run record of the format is refused", "[cli][agent][record]")
{
    auto appender = MemoryAppender {};
    for (auto const line: { "not json"sv, "[1,2]"sv, R"({"run":"pr-1"})"sv, R"({"format":2})"sv, ""sv })
    {
        CAPTURE(line);
        auto const outcome = record(appender, line);
        CHECK(outcome.status == exitCode(ExitStatus::BadUsage));
        CHECK(outcome.err.starts_with("not a run record"));
    }
    auto const otherFormat = record(appender, R"({"format":2})", 2);
    CHECK(otherFormat.err == "unsupported record format 2; this contour writes 1\n");
    CHECK(appender.files.empty());
}

TEST_CASE("a file that cannot be written is a failure", "[cli][agent][record]")
{
    auto appender = MemoryAppender {};
    appender.failure = "state/agent-runs.jsonl: Permission denied";
    auto const outcome = record(appender, R"({"format":1})");
    CHECK(outcome.status == exitCode(ExitStatus::Failed));
    CHECK(outcome.err == "state/agent-runs.jsonl: Permission denied\n");
}

#ifndef _WIN32
TEST_CASE("the native appender creates the file private and never follows a link", "[cli][agent][record]")
{
    auto const directory = std::filesystem::temp_directory_path()
                           / std::format("contour-record-test-{}", static_cast<unsigned>(::getpid()));
    std::filesystem::remove_all(directory);
    auto const file = directory / "state" / AgentRunsFileName;
    auto const appender = makeNativeRecordAppender();
    REQUIRE(appender->append(file, "one\n").has_value());
    REQUIRE(appender->append(file, "two\n").has_value());
    auto in = std::ifstream { file };
    CHECK(std::string { std::istreambuf_iterator<char> { in }, {} } == "one\ntwo\n");
    struct stat info {};
    REQUIRE(::stat(file.c_str(), &info) == 0);
    CHECK((info.st_mode & 0777) == 0600);

    auto const link = directory / "state" / "linked.jsonl";
    std::filesystem::create_symlink(file, link);
    CHECK_FALSE(appender->append(link, "three\n").has_value());
    std::filesystem::remove_all(directory);
}
#endif
