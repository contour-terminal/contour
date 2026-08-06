// SPDX-License-Identifier: Apache-2.0
#include <crispy/logsink.hpp>
#include <crispy/logstore.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#ifndef _WIN32
    #include <unistd.h>
#endif

using namespace std::string_view_literals;

namespace
{
/// A category that exists only for the duration of one test.
///
/// logstore::Category asserts name uniqueness process-wide and deregisters itself on
/// destruction, so a function-local category is the only way to build messages in a test
/// without colliding with the real ones.
struct TestCategory
{
    logstore::Category value;

    explicit TestCategory(std::string_view name):
        value { name, "Test-only category.", logstore::Category::State::Enabled }
    {
    }
};

/// A scratch log-file path, deleted when the test ends.
///
/// Declare it FIRST in the test: the deletion happens in the destructor, so it runs after every
/// sink and reader declared below it has closed. Windows refuses to delete a file any handle still
/// holds open — unlike POSIX, where unlinking an open file is fine — so a `remove` written inline
/// after an `ifstream` throws there while passing everywhere else.
///
/// Deletion is the non-throwing overload, because a destructor must not throw and a leftover file
/// in the temp directory is not worth failing a test over.
class ScratchLog
{
  public:
    explicit ScratchLog(std::string_view stem):
        _path(std::filesystem::temp_directory_path() / std::format("crispy-logsink-{}.log", stem))
    {
        remove(); // a leftover from an earlier crashed run would make an append test lie
    }

    ~ScratchLog() { remove(); }

    ScratchLog(ScratchLog const&) = delete;
    ScratchLog& operator=(ScratchLog const&) = delete;
    ScratchLog(ScratchLog&&) = delete;
    ScratchLog& operator=(ScratchLog&&) = delete;

    [[nodiscard]] std::filesystem::path const& path() const noexcept { return _path; }

  private:
    void remove() const noexcept
    {
        auto ec = std::error_code {};
        std::filesystem::remove(_path, ec);
    }

    std::filesystem::path _path;
};
} // namespace

TEST_CASE("parseLogFileSpec maps the standard-error spellings onto nullopt", "[crispy][logsink]")
{
    CHECK_FALSE(logstore::parseLogFileSpec("").has_value());
    CHECK_FALSE(logstore::parseLogFileSpec("-").has_value());
    CHECK(logstore::parseLogFileSpec("/tmp/contour.log") == std::filesystem::path { "/tmp/contour.log" });
}

TEST_CASE("the standard formatter lays a line out as configured", "[crispy][logsink]")
{
    auto category = TestCategory { "test.formatter" };
    auto capture = logstore::ScopedCapture { "test.formatter" };

    SECTION("uncoloured output carries no escape sequences")
    {
        category.value.setFormatter(logstore::makeStandardFormatter({ .colorize = false }));
        category.value()("hello");
        CHECK_FALSE(capture.contains("\033"));
        CHECK(capture.contains("[test.formatter]"));
        CHECK(capture.contains("hello"));
    }

    SECTION("colourised output does")
    {
        category.value.setFormatter(logstore::makeStandardFormatter({ .colorize = true }));
        category.value()("hello");
        CHECK(capture.contains("\033["));
    }

    SECTION("the process id appears only when asked for")
    {
        category.value.setFormatter(
            logstore::makeStandardFormatter({ .colorize = false, .showProcessId = true }));
        category.value()("hello");
        CHECK(capture.contains(std::format("[{}]", ::getpid())));
    }

    SECTION("the timestamp can be suppressed")
    {
        category.value.setFormatter(
            logstore::makeStandardFormatter({ .colorize = false, .showTimestamp = false }));
        category.value()("hello");
        CHECK(capture.text().starts_with("[test.formatter] hello"));
    }

    SECTION("continuation lines are indented and carry no repeated tag")
    {
        category.value.setFormatter(
            logstore::makeStandardFormatter({ .colorize = false, .showTimestamp = false }));
        category.value()("first\nsecond");
        auto const lines = capture.lines();
        REQUIRE(lines.size() == 2);
        CHECK(lines[0] == "[test.formatter] first");
        CHECK(lines[1] == "        second");
    }
}

TEST_CASE("the error formatter tags its lines so they stand out", "[crispy][logsink]")
{
    auto category = TestCategory { "test.errorformat" };
    auto capture = logstore::ScopedCapture { "test.errorformat" };

    category.value.setFormatter(logstore::makeErrorFormatter({ .colorize = false, .showTimestamp = false }));
    category.value()("it broke");
    CHECK(capture.text() == "[error] it broke\n");
}

TEST_CASE("unmatchedFilters names filter patterns that select nothing", "[crispy][logsink]")
{
    auto category = TestCategory { "test.filters" };

    CHECK(logstore::unmatchedFilters("test.filters").empty());
    CHECK(logstore::unmatchedFilters("test.*").empty());
    CHECK(logstore::unmatchedFilters("all").empty());
    CHECK(logstore::unmatchedFilters("").empty());
    CHECK(logstore::unmatchedFilters("test.filtres") == std::vector<std::string> { "test.filtres" });
    CHECK(logstore::unmatchedFilters("test.filters,nope.*") == std::vector<std::string> { "nope.*" });
}

TEST_CASE("ScopedOutput writes to a file without escape sequences", "[crispy][logsink]")
{
    auto const log = ScratchLog { "file" };
    auto category = TestCategory { "test.filesink" };

    {
        auto output = logstore::ScopedOutput::create({ .file = log.path() });
        REQUIRE(output.has_value());
        category.value()("to the file");
    }

    auto stream = std::ifstream { log.path() };
    REQUIRE(stream.is_open());
    auto contents = std::string { std::istreambuf_iterator<char> { stream }, {} };
    CHECK(contents.contains("to the file"));
    CHECK(contents.contains("[test.filesink]"));
    // A file must never receive SGR escapes, whatever the terminal the daemon was started from.
    CHECK_FALSE(contents.contains('\033'));
}

TEST_CASE("ScopedOutput appends rather than truncating", "[crispy][logsink]")
{
    // A daemon restarted against the same --log-file must not erase the evidence of why the
    // previous run died.
    auto const log = ScratchLog { "append" };
    auto category = TestCategory { "test.appendsink" };

    for (auto const* const text: { "first run", "second run" })
    {
        auto output = logstore::ScopedOutput::create({ .file = log.path() });
        REQUIRE(output.has_value());
        category.value()("{}", text);
    }

    auto stream = std::ifstream { log.path() };
    auto const contents = std::string { std::istreambuf_iterator<char> { stream }, {} };
    CHECK(contents.contains("first run"));
    CHECK(contents.contains("second run"));
}

TEST_CASE("ScopedOutput reports an unopenable log file", "[crispy][logsink]")
{
    // A DIRECTORY is the only destination portably guaranteed to refuse an ofstream: POSIX answers
    // EISDIR, Windows EACCES. A path under an unwritable directory is not — create() creates the
    // parent first, and an absolute POSIX path like "/proc/nope/x.log" is a perfectly valid
    // DRIVE-RELATIVE path on Windows, so create_directories() happily makes it and the open
    // succeeds. That is exactly how this test used to fail on Windows CI alone.
    auto const output = logstore::ScopedOutput::create({ .file = std::filesystem::temp_directory_path() });
    REQUIRE_FALSE(output.has_value());
    CHECK(output.error().contains("cannot open log file"));
}

TEST_CASE("ScopedOutput restores the previous sink", "[crispy][logsink]")
{
    // The regression test for logstore's reference_wrapper hazard: a category left pointing at
    // a destroyed sink corrupts every later log call in the process.
    auto category = TestCategory { "test.restore" };
    auto const* const before = &category.value.sink();

    auto const log = ScratchLog { "restore" };
    {
        auto output = logstore::ScopedOutput::create({ .file = log.path() });
        REQUIRE(output.has_value());
        CHECK(&category.value.sink() != before);
    }

    CHECK(&category.value.sink() == before);
}

TEST_CASE("ScopedOutput leaves an empty filter alone", "[crispy][logsink]")
{
    // configure("") matches no pattern and would therefore DISABLE every category, `error`
    // included. An empty --log must mean "keep whatever $LOG set", never "log nothing".
    auto category = TestCategory { "test.emptyfilter" };
    REQUIRE(category.value.isEnabled());

    auto output = logstore::ScopedOutput::create({ .filter = "" });
    REQUIRE(output.has_value());
    CHECK(category.value.isEnabled());
    CHECK(logstore::errorLog.isEnabled());
}

TEST_CASE("ScopedOutput serialises concurrent writers", "[crispy][logsink]")
{
    // The daemon logs from its event loop, its sigwait thread and every PTY pump thread, while
    // logstore's own sink does no locking at all. Without the writer's mutex, lines tear.
    constexpr auto ThreadCount = 4;
    constexpr auto LinesPerThread = 200;

    auto const log = ScratchLog { "threads" };
    auto category = TestCategory { "test.threads" };

    {
        auto output = logstore::ScopedOutput::create({ .file = log.path() });
        REQUIRE(output.has_value());

        auto writers = std::vector<std::thread> {};
        for (auto worker = 0; worker < ThreadCount; ++worker)
            writers.emplace_back([&category, worker] {
                for (auto line = 0; line < LinesPerThread; ++line)
                    category.value()("worker {} line {}", worker, line);
            });
        for (auto& writer: writers)
            writer.join();
    }

    auto stream = std::ifstream { log.path() };
    auto lines = std::vector<std::string> {};
    for (auto text = std::string {}; std::getline(stream, text);)
        lines.push_back(text);

    CHECK(lines.size() == std::size_t { ThreadCount } * LinesPerThread);
    // Every line intact means no writer interleaved inside another's bytes.
    CHECK(std::ranges::all_of(lines, [](auto const& line) {
        return line.contains("[test.threads] worker ") && line.contains(" line ");
    }));
}

TEST_CASE("ScopedCapture enables and restores what it captures", "[crispy][logsink]")
{
    auto category = logstore::Category { "test.capture", "Test-only category." };
    REQUIRE_FALSE(category.isEnabled());
    auto const* const before = &category.sink();

    {
        auto capture = logstore::ScopedCapture { "test.capture" };
        CHECK(category.isEnabled()); // capturing a category implies enabling it
        category()("recorded");
        CHECK(capture.contains("recorded"));
        CHECK(capture.count("recorded") == 1);
    }

    CHECK_FALSE(category.isEnabled());
    CHECK(&category.sink() == before);
}

TEST_CASE("configure's prefix match is bounded by the category name", "[crispy][logsink]")
{
    // Regression: the wildcard branch used a three-iterator std::equal, which bounds only the
    // PATTERN range. Any pattern longer than a registered category's name — "vthost.*" is 7
    // characters against the built-in "error" at 5 — read past the end of that name. ASan
    // caught it the first time a real `--log=vthost.*` ran.
    auto category = TestCategory { "test.prefixbound" };

    logstore::configure("averyveryverylongprefixthatnocategoryhas.*");
    CHECK_FALSE(category.value.isEnabled());

    logstore::configure("test.*");
    CHECK(category.value.isEnabled());

    // A pattern that is a strict prefix of another category's name must not match it whole.
    logstore::configure("test.prefixboundandmore*");
    CHECK_FALSE(category.value.isEnabled());

    logstore::configure("all"); // leave the process in a sane state for later tests
    logstore::configure("error");
}

TEST_CASE("an explicit filter never silences the error category", "[crispy][logsink]")
{
    // logstore::configure is a SELECTION: it disables everything the filter does not name.
    // Left alone, `--log vthost.trace.proto` would switch off the very failure lines an
    // operator turned logging on to see. Caught by running a real daemon, not by a unit test —
    // hence this one.
    auto category = TestCategory { "test.filterkeepserror" };
    logstore::errorLog.disable();

    auto output = logstore::ScopedOutput::create({ .filter = "test.filterkeepserror" });
    REQUIRE(output.has_value());

    CHECK(category.value.isEnabled());
    CHECK(logstore::errorLog.isEnabled());
}
