// SPDX-License-Identifier: Apache-2.0
#include <core/log/LogSink.hpp>
#include <core/log/LogStore.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <vector>

// Single-threaded WebAssembly has no threads to start (Part I §1).
#if !defined(__EMSCRIPTEN__) || defined(__EMSCRIPTEN_PTHREADS__)
    #define CORE_CPP_TEST_THREADS 1
    #include <ranges>
    #include <thread>
#else
    #define CORE_CPP_TEST_THREADS 0
#endif

#include <sys/stat.h>

#include <fcntl.h>

#ifdef _WIN32
    #include <io.h>
    #include <process.h>
    #include <share.h>
#else
    #include <unistd.h>
#endif

using namespace std::string_view_literals;

namespace
{
/// A category that exists only for the duration of one test.
///
/// core::log::Category asserts name uniqueness process-wide and deregisters itself on
/// destruction, so a function-local category is the only way to build messages in a test
/// without colliding with the real ones.
struct TestCategory
{
    core::log::Category value;

    explicit TestCategory(std::string_view name):
        value { name, "Test-only category.", core::log::Category::State::Enabled }
    {
    }
};

/// @return The current process id, as the `[PID]` field prints it.
[[nodiscard]] int processId() noexcept
{
#ifdef _WIN32
    return ::_getpid();
#else
    return static_cast<int>(::getpid());
#endif
}

/// @return The current LOCAL time as `YYYY-MM-DD HH:MM`, the leading part of the timestamp field.
///
/// Spelled out here rather than taken from core::log::detail::localTime(), which is what the
/// formatter uses: checking an implementation against itself asserts nothing.
[[nodiscard]] std::string nowLocalToTheMinute()
{
    auto const now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    auto brokenDown = std::tm {};
#ifdef _WIN32
    ::localtime_s(&brokenDown, &now);
#else
    ::localtime_r(&now, &brokenDown);
#endif
    return std::format("{:04}-{:02}-{:02} {:02}:{:02}",
                       brokenDown.tm_year + 1900,
                       brokenDown.tm_mon + 1,
                       brokenDown.tm_mday,
                       brokenDown.tm_hour,
                       brokenDown.tm_min);
}

/// @return What is left to read of @p stream.
///
/// Through a string stream rather than std::istreambuf_iterator: GCC 14 at -O2 inlines that
/// iterator's reads into the caller and reports a possible null dereference inside libstdc++'s
/// streambuf (-Wnull-dereference), which no caller can act on.
[[nodiscard]] std::string contentsOf(std::istream& stream)
{
    auto buffer = std::ostringstream {};
    buffer << stream.rdbuf();
    return buffer.str();
}

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
        _path(std::filesystem::temp_directory_path() / std::format("core-cpp-logsink-{}.log", stem))
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

/// Puts a category's enablement back when the test leaves, however it leaves.
///
/// A plain `disable()` with a re-enable at the end of the body is not enough: a failing REQUIRE
/// unwinds past it, and the category stays off for every later case in the binary.
class ScopedCategoryState
{
  public:
    explicit ScopedCategoryState(core::log::Category& category):
        _category { category }, _wasEnabled { category.isEnabled() }
    {
    }

    ~ScopedCategoryState() { _category.enable(_wasEnabled); }

    ScopedCategoryState(ScopedCategoryState const&) = delete;
    ScopedCategoryState& operator=(ScopedCategoryState const&) = delete;
    ScopedCategoryState(ScopedCategoryState&&) = delete;
    ScopedCategoryState& operator=(ScopedCategoryState&&) = delete;

  private:
    core::log::Category& _category;
    bool _wasEnabled;
};

/// Points one of this process's standard descriptors at a file for this object's lifetime.
class ScopedRedirect
{
  public:
    ScopedRedirect(int fd, std::filesystem::path const& path): _fd { fd }
    {
        auto const target = openForWriting(path);
        if (target == -1)
            return;
        _saved = duplicate(fd);
        if (_saved != -1)
            duplicate2(target, fd);
        closeDescriptor(target);
    }

    ~ScopedRedirect()
    {
        if (_saved != -1)
        {
            duplicate2(_saved, _fd);
            closeDescriptor(_saved);
        }
    }

    ScopedRedirect(ScopedRedirect const&) = delete;
    ScopedRedirect& operator=(ScopedRedirect const&) = delete;
    ScopedRedirect(ScopedRedirect&&) = delete;
    ScopedRedirect& operator=(ScopedRedirect&&) = delete;

    /// @return Whether the descriptor could actually be redirected.
    [[nodiscard]] bool isActive() const noexcept { return _saved != -1; }

  private:
    /// @return A descriptor for @p path, truncated, or -1.
    [[nodiscard]] static int openForWriting(std::filesystem::path const& path) noexcept
    {
#ifdef _WIN32
        auto fd = -1;
        (void) ::_wsopen_s(
            &fd, path.c_str(), _O_WRONLY | _O_CREAT | _O_TRUNC, _SH_DENYNO, _S_IREAD | _S_IWRITE);
        return fd;
#else
        return ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
#endif
    }

    [[nodiscard]] static int duplicate(int fd) noexcept
    {
#ifdef _WIN32
        return ::_dup(fd);
#else
        return ::dup(fd);
#endif
    }

    static void duplicate2(int from, int to) noexcept
    {
#ifdef _WIN32
        (void) ::_dup2(from, to);
#else
        (void) ::dup2(from, to);
#endif
    }

    static void closeDescriptor(int fd) noexcept
    {
#ifdef _WIN32
        (void) ::_close(fd);
#else
        (void) ::close(fd);
#endif
    }

    int _fd;
    int _saved = -1;
};
} // namespace

TEST_CASE("parseLogFileSpec maps the standard-error spellings onto nullopt", "[log][logsink]")
{
    CHECK_FALSE(core::log::parseLogFileSpec("").has_value());
    CHECK_FALSE(core::log::parseLogFileSpec("-").has_value());
    CHECK(core::log::parseLogFileSpec("/tmp/core-cpp.log") == std::filesystem::path { "/tmp/core-cpp.log" });
}

TEST_CASE("the standard formatter lays a line out as configured", "[log][logsink]")
{
    auto category = TestCategory { "test.formatter" };
    auto capture = core::log::ScopedCapture { "test.formatter" };

    SECTION("uncoloured output carries no escape sequences")
    {
        category.value.setFormatter(core::log::makeStandardFormatter({ .colorize = false }));
        category.value()("hello");
        CHECK_FALSE(capture.contains("\033"));
        CHECK(capture.contains("[test.formatter]"));
        CHECK(capture.contains("hello"));
    }

    SECTION("colourised output does")
    {
        category.value.setFormatter(core::log::makeStandardFormatter({ .colorize = true }));
        category.value()("hello");
        CHECK(capture.contains("\033["));
    }

    SECTION("the process id appears only when asked for")
    {
        category.value.setFormatter(
            core::log::makeStandardFormatter({ .colorize = false, .showProcessId = true }));
        category.value()("hello");
        CHECK(capture.contains(std::format("[{}]", processId())));
    }

    SECTION("the timestamp is this moment, in local time")
    {
        // The broken-down conversion is a platform implementation (log/posix/, log/windows/), and
        // a std::tm it failed to fill renders 0000-00-00 00:00:00 -- which every other case here
        // would accept. Read on both sides of the line and either may match, so a minute ticking
        // over between the two cannot make this flap.
        category.value.setFormatter(core::log::makeStandardFormatter({ .colorize = false }));
        auto const before = nowLocalToTheMinute();
        category.value()("hello");
        auto const after = nowLocalToTheMinute();
        INFO("expected " << before << " or " << after << ", got: " << capture.text());
        CHECK((capture.contains(before) || capture.contains(after)));
    }

    SECTION("the timestamp can be suppressed")
    {
        category.value.setFormatter(
            core::log::makeStandardFormatter({ .colorize = false, .showTimestamp = false }));
        category.value()("hello");
        CHECK(capture.text().starts_with("[test.formatter] hello"));
    }

    SECTION("continuation lines are indented and carry no repeated tag")
    {
        category.value.setFormatter(
            core::log::makeStandardFormatter({ .colorize = false, .showTimestamp = false }));
        category.value()("first\nsecond");
        auto const lines = capture.lines();
        REQUIRE(lines.size() == 2);
        CHECK(lines[0] == "[test.formatter] first");
        CHECK(lines[1] == "        second");
    }
}

TEST_CASE("the error formatter tags its lines so they stand out", "[log][logsink]")
{
    auto category = TestCategory { "test.errorformat" };
    auto capture = core::log::ScopedCapture { "test.errorformat" };

    category.value.setFormatter(core::log::makeErrorFormatter({ .colorize = false, .showTimestamp = false }));
    category.value()("it broke");
    CHECK(capture.text() == "[error] it broke\n");
}

TEST_CASE("unmatchedFilters names filter patterns that select nothing", "[log][logsink]")
{
    auto category = TestCategory { "test.filters" };

    CHECK(core::log::unmatchedFilters("test.filters").empty());
    CHECK(core::log::unmatchedFilters("test.*").empty());
    CHECK(core::log::unmatchedFilters("all").empty());
    CHECK(core::log::unmatchedFilters("").empty());
    CHECK(core::log::unmatchedFilters("test.filtres") == std::vector<std::string> { "test.filtres" });
    CHECK(core::log::unmatchedFilters("test.filters,nope.*") == std::vector<std::string> { "nope.*" });
}

TEST_CASE("ScopedOutput writes to a file without escape sequences", "[log][logsink]")
{
    auto const log = ScratchLog { "file" };
    auto category = TestCategory { "test.filesink" };

    {
        auto output = core::log::ScopedOutput::create({ .file = log.path() });
        REQUIRE(output.has_value());
        category.value()("to the file");
    }

    auto stream = std::ifstream { log.path() };
    REQUIRE(stream.is_open());
    auto const contents = contentsOf(stream);
    CHECK(contents.contains("to the file"));
    CHECK(contents.contains("[test.filesink]"));
    // A file must never receive SGR escapes, whatever the terminal the daemon was started from.
    CHECK_FALSE(contents.contains('\033'));
}

TEST_CASE("ScopedOutput appends rather than truncating", "[log][logsink]")
{
    // A daemon restarted against the same --log-file must not erase the evidence of why the
    // previous run died.
    auto const log = ScratchLog { "append" };
    auto category = TestCategory { "test.appendsink" };

    for (auto const* const text: { "first run", "second run" })
    {
        auto output = core::log::ScopedOutput::create({ .file = log.path() });
        REQUIRE(output.has_value());
        category.value()("{}", text);
    }

    auto stream = std::ifstream { log.path() };
    auto const contents = contentsOf(stream);
    CHECK(contents.contains("first run"));
    CHECK(contents.contains("second run"));
}

TEST_CASE("ScopedOutput reports an unopenable log file", "[log][logsink]")
{
    // A DIRECTORY is the only destination portably guaranteed to refuse an ofstream: POSIX answers
    // EISDIR, Windows EACCES. A path under an unwritable directory is not — create() creates the
    // parent first, and an absolute POSIX path like "/proc/nope/x.log" is a perfectly valid
    // DRIVE-RELATIVE path on Windows, so create_directories() happily makes it and the open
    // succeeds. That is exactly how this test used to fail on Windows CI alone.
    auto const output = core::log::ScopedOutput::create({ .file = std::filesystem::temp_directory_path() });
    REQUIRE_FALSE(output.has_value());
    CHECK(output.error().contains("cannot open log file"));
}

TEST_CASE("ScopedOutput restores the previous sink", "[log][logsink]")
{
    // The regression test for Category's reference_wrapper hazard: a category left pointing at
    // a destroyed sink corrupts every later log call in the process.
    auto category = TestCategory { "test.restore" };
    auto const* const before = &category.value.sink();

    auto const log = ScratchLog { "restore" };
    {
        auto output = core::log::ScopedOutput::create({ .file = log.path() });
        REQUIRE(output.has_value());
        CHECK(&category.value.sink() != before);
    }

    CHECK(&category.value.sink() == before);
}

TEST_CASE("ScopedOutput leaves an empty filter alone", "[log][logsink]")
{
    // configure("") matches no pattern and would therefore DISABLE every category, `error`
    // included. An empty --log must mean "keep whatever $LOG set", never "log nothing".
    auto category = TestCategory { "test.emptyfilter" };
    REQUIRE(category.value.isEnabled());

    auto output = core::log::ScopedOutput::create({ .filter = "" });
    REQUIRE(output.has_value());
    CHECK(category.value.isEnabled());
    CHECK(core::log::errorLog.isEnabled());
}

#if CORE_CPP_TEST_THREADS
TEST_CASE("ScopedOutput serialises concurrent writers", "[log][logsink]")
{
    // A server logs from its event loop, a signal thread and its workers at once, while
    // Sink itself does no locking at all. Without the writer's mutex, lines tear.
    static constexpr auto ThreadCount = 4;
    static constexpr auto LinesPerThread = 200;

    auto const log = ScratchLog { "threads" };
    auto category = TestCategory { "test.threads" };

    {
        auto output = core::log::ScopedOutput::create({ .file = log.path() });
        REQUIRE(output.has_value());

        auto writers = std::vector<std::thread> {};
        for (auto const worker: std::views::iota(0, ThreadCount))
            writers.emplace_back([&category, worker] {
                for (auto const line: std::views::iota(0, LinesPerThread))
                    category.value()("worker {} line {}", worker, line);
            });
        for (auto& writer: writers)
            writer.join();
    }

    auto stream = std::ifstream { log.path() };
    auto lines = std::vector<std::string> {};
    auto text = std::string {};
    while (std::getline(stream, text))
        lines.push_back(text);

    CHECK(lines.size() == std::size_t { ThreadCount } * LinesPerThread);
    // Every line intact means no writer interleaved inside another's bytes.
    CHECK(std::ranges::all_of(lines, [](auto const& line) {
        return line.contains("[test.threads] worker ") && line.contains(" line ");
    }));
}
#endif

TEST_CASE("ScopedCapture enables and restores what it captures", "[log][logsink]")
{
    auto category = core::log::Category { "test.capture", "Test-only category." };
    REQUIRE_FALSE(category.isEnabled());
    auto const* const before = &category.sink();

    {
        auto capture = core::log::ScopedCapture { "test.capture" };
        CHECK(category.isEnabled()); // capturing a category implies enabling it
        category()("recorded");
        CHECK(capture.contains("recorded"));
        CHECK(capture.count("recorded") == 1);
    }

    CHECK_FALSE(category.isEnabled());
    CHECK(&category.sink() == before);
}

TEST_CASE("ScopedCapture's text() is still a reference into the capture", "[log][logsink]")
{
    // 0.4.1's signature, kept: a patch release does not change it. A by-value text() would turn a
    // caller's `std::string_view v = capture.text();` into a dangling view without a diagnostic.
    auto const capture = core::log::ScopedCapture { "test.capturetext" };
    static_assert(std::is_same_v<decltype(capture.text()), std::string const&>);
    static_assert(noexcept(capture.text()));
    static_assert(std::is_same_v<decltype(capture.snapshot()), std::string>);
    CHECK(&capture.text() == &capture.text());
}

#if CORE_CPP_TEST_THREADS
TEST_CASE("ScopedCapture takes lines from several threads while it is read", "[log][logsink][threads]")
{
    // contour's Windows heap crash: a test logged into a capture from two threads, and the sink
    // appended to one std::string unguarded. Readers poll while the writers run, through the
    // locking readers, because a read racing an append that reallocates is the other half of it.
    // Rounds, because an unguarded append does not fail every time.
    static constexpr auto Rounds = 20;
    static constexpr auto ThreadCount = 4;
    static constexpr auto LinesPerThread = 500;
    // Per round, for a cold two-core runner under a sanitizer; a round takes milliseconds.
    static constexpr auto Budget = std::chrono::seconds { 120 };

    auto category = TestCategory { "test.capturethreads" };
    for ([[maybe_unused]] auto const round: std::views::iota(0, Rounds))
    {
        auto capture = core::log::ScopedCapture { "test.capturethreads" };
        auto running = std::atomic<int> { ThreadCount };
        auto written = std::atomic<int> { 0 };
        auto writers = std::vector<std::thread> {};
        for (auto const worker: std::views::iota(0, ThreadCount))
            writers.emplace_back([&category, &running, &written, worker] {
                for (auto const line: std::views::iota(0, LinesPerThread))
                {
                    category.value()("worker {} line {}", worker, line);
                    written.fetch_add(1);
                }
                running.fetch_sub(1);
            });

        auto const deadline = std::chrono::steady_clock::now() + Budget;
        while (running.load() != 0 && std::chrono::steady_clock::now() < deadline)
        {
            [[maybe_unused]] auto const seen = capture.contains("worker 0 line");
            [[maybe_unused]] auto const text = capture.snapshot();
        }
        if (auto const stillRunning = running.load(); stillRunning != 0)
        {
            // Joining a writer that never returns would hang, and a hang names nothing. Say what
            // was waited for and whether it was still moving, then end the process: Catch2 reports
            // the abort against this case.
            auto const before = written.load();
            std::this_thread::sleep_for(std::chrono::seconds { 1 });
            auto const after = written.load();
            // std::cerr is unbuffered, so this is out before the abort.
            std::cerr << std::format(
                "ScopedCapture threads: {} of {} writers still running after {}s; {} of {} "
                "lines written, {} over the last second\n",
                stillRunning,
                ThreadCount,
                Budget.count(),
                after,
                ThreadCount * LinesPerThread,
                after == before ? "none" : "more");
            std::abort();
        }
        for (auto& writer: writers)
            writer.join();

        auto const lines = capture.lines();
        REQUIRE(lines.size() == std::size_t { ThreadCount } * LinesPerThread);
        // Whatever formatter an earlier case left installed, each worker's lines are found by the
        // text it wrote. A torn line, or two run together, breaks one of these counts; a lost one
        // breaks the total above.
        for (auto const worker: std::views::iota(0, ThreadCount))
            CHECK(capture.count(std::format("worker {} line ", worker)) == std::size_t { LinesPerThread });
        // Once every writer is joined, the reference and the copy agree.
        CHECK(capture.snapshot() == capture.text());
    }
}
#endif

TEST_CASE("configure's prefix match is bounded by the category name", "[log][logsink]")
{
    // Regression: the wildcard branch used a three-iterator std::equal, which bounds only the
    // PATTERN range. Any pattern longer than a registered category's name — "vthost.*" is 7
    // characters against the built-in "error" at 5 — read past the end of that name. ASan
    // caught it the first time a real `--log=vthost.*` ran.
    auto category = TestCategory { "test.prefixbound" };

    core::log::configure("averyveryverylongprefixthatnocategoryhas.*");
    CHECK_FALSE(category.value.isEnabled());

    core::log::configure("test.*");
    CHECK(category.value.isEnabled());

    // A pattern that is a strict prefix of another category's name must not match it whole.
    core::log::configure("test.prefixboundandmore*");
    CHECK_FALSE(category.value.isEnabled());

    core::log::configure("all"); // leave the process in a sane state for later tests
    core::log::configure("error");
}

TEST_CASE("an explicit filter never silences the error category", "[log][logsink]")
{
    // core::log::configure is a SELECTION: it disables everything the filter does not name.
    // Left alone, `--log vthost.trace.proto` would switch off the very failure lines an
    // operator turned logging on to see. Caught by running a real daemon, not by a unit test —
    // hence this one.
    auto category = TestCategory { "test.filterkeepserror" };

    // errorLog is process-wide and this case turns it off on purpose; the guard puts it back
    // even when a REQUIRE below unwinds out of the body.
    auto const restoreErrorLog = ScopedCategoryState { core::log::errorLog };
    core::log::errorLog.disable();

    auto output = core::log::ScopedOutput::create({ .filter = "test.filterkeepserror" });
    REQUIRE(output.has_value());

    CHECK(category.value.isEnabled());
    CHECK(core::log::errorLog.isEnabled());
}

// The Windows half of this answered `true` unconditionally, so a redirected stream was
// colourised -- against the header's contract, and into whatever file or pipe was reading.
TEST_CASE("a redirected standard stream is not a terminal", "[log][logsink]")
{
    auto const scratch = ScratchLog { "redirect" };

    SECTION("standard output")
    {
        auto const redirect = ScopedRedirect { 1, scratch.path() };
        if (!redirect.isActive())
            SKIP("standard output could not be redirected");
        CHECK(!core::log::isStdOutTerminal());
    }

    SECTION("standard error")
    {
        auto const redirect = ScopedRedirect { 2, scratch.path() };
        if (!redirect.isActive())
            SKIP("standard error could not be redirected");
        CHECK(!core::log::isStdErrTerminal());
    }
}
