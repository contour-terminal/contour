// SPDX-License-Identifier: Apache-2.0
//
// The finish-notification policy, table by table: when x where x how long x how it ended x how to
// deliver, and every word the desktop is shown. Qt-free on purpose -- the decision lives in a header
// with no Qt in it, which is what lets contour_test (built in both frontend configurations) cover it.

#include <contour/platform/FinishNotification.hpp>

#include <vtbackend/core/TerminalContext.hpp>
#include <vtbackend/shell/CommandBlock.hpp>
#include <vtbackend/shell/CommandLineText.hpp>
#include <vtbackend/vt/DesktopNotification.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>

using contour::config::FinishNotificationConfig;
using contour::config::NotifyAction;
using contour::config::NotifyClearOn;
using contour::config::NotifyOutcome;
using contour::config::NotifyWhen;
using contour::platform::commandShortName;
using contour::platform::effectsOf;
using contour::platform::FinishEffect;
using contour::platform::finishNoticeKind;
using contour::platform::finishNotificationBody;
using contour::platform::finishNotificationIdentifier;
using contour::platform::finishNotificationText;
using contour::platform::finishNotificationTitle;
using contour::platform::SessionVisibility;
using contour::platform::wordingOf;

using namespace std::chrono_literals;

namespace
{

constexpr auto Home = std::string_view { "/home/bob" };

/// A `make -j8` that ran for @p duration in ~/src/contour and ended with @p exitCode and @p outcome.
[[nodiscard]] vtbackend::CommandBlockSummary aFinish(std::optional<int> exitCode,
                                                     vtbackend::ContextOutcome outcome = {},
                                                     std::chrono::steady_clock::duration duration = 192s)
{
    auto summary = vtbackend::CommandBlockSummary {};
    summary.id = vtbackend::CommandBlockId { 7U };
    summary.commandLine = vtbackend::CommandLineText { "make -j8" };
    summary.exitCode = exitCode;
    summary.outcome = outcome;
    summary.duration = duration;
    summary.workingDirectory = vtbackend::WorkingDirectorySnapshot {
        .path = "/home/bob/src/contour",
        .locality = vtbackend::ContextLocality::Local,
    };
    return summary;
}

/// The short name of the command line @p raw, as it arrived.
[[nodiscard]] std::string shortNameOf(std::string raw)
{
    return commandShortName(vtbackend::CommandLineText { std::move(raw) });
}

/// @p piece, @p count times over.
[[nodiscard]] std::string repeated(std::string_view piece, size_t count)
{
    auto result = std::string {};
    result.reserve(piece.size() * count);
    for ([[maybe_unused]] auto const i: std::views::iota(size_t { 0 }, count))
        result += piece;
    return result;
}

} // namespace

TEST_CASE("FinishNotification.every way of ending has one wording row", "[notification]")
{
    using vtbackend::CommandEnding;
    CHECK(contour::platform::FinishWordingTable.size() == 6);
    for (auto const ending: { CommandEnding::Unknown,
                              CommandEnding::Succeeded,
                              CommandEnding::Failed,
                              CommandEnding::Killed,
                              CommandEnding::Crashed,
                              CommandEnding::Interrupted })
        CHECK(wordingOf(ending).ending == ending);
}

TEST_CASE("FinishNotification.titles name the command and how it ended", "[notification]")
{
    using vtbackend::ContextExit;
    using vtbackend::ContextOutcome;
    using vtbackend::ContextSignal;

    struct Row
    {
        std::optional<int> exitCode;
        ContextOutcome outcome;
        std::string_view expected;
    };
    auto const rows = std::array {
        Row { 0, {}, "✓ make finished" },
        Row { 2, {}, "✗ make failed (exit 2)" },
        Row { 139,
              ContextOutcome { .exit = ContextExit::Crash, .signal = ContextSignal::Segv },
              "✗ make killed (SIGSEGV)" },
        Row { 139, ContextOutcome { .exit = ContextExit::Crash }, "✗ make crashed" },
        Row { 139, {}, "✗ make killed (SIGSEGV)" }, // a shell's 128 + n, as the gutter's tooltip reads it
        Row { 130, {}, "✗ make killed (SIGINT)" },
        Row { 138, {}, "✗ make failed (exit 138)" }, // SIGUSR1 on Linux, SIGBUS elsewhere: no name
        // An explicit verdict that names no signal keeps a 128 + n code as the program's own.
        Row {
            130, ContextOutcome { .exit = ContextExit::Failure, .status = 130 }, "✗ make failed (exit 130)" },
        Row { 130, ContextOutcome { .exit = ContextExit::Interrupt }, "✗ make interrupted" },
        Row { std::nullopt,
              ContextOutcome { .exit = ContextExit::Failure, .status = 3 },
              "✗ make failed (exit 3)" },
        Row { std::nullopt, ContextOutcome { .exit = ContextExit::Success }, "✓ make finished" },
        // OSC 3008 alone: the code is asShellExitCode()'s, which reads a failure without a status, or with
        // one of 256, as 0. The verdict still says failed, and a 0 is no detail worth printing.
        Row { 0, ContextOutcome { .exit = ContextExit::Failure }, "✗ make failed" },
        Row { 0, ContextOutcome { .exit = ContextExit::Failure, .status = 256 }, "✗ make failed" },
        Row { std::nullopt, {}, "make finished" },
    };
    for (auto const& row: rows)
    {
        INFO(row.expected);
        CHECK(finishNotificationTitle(aFinish(row.exitCode, row.outcome)) == row.expected);
    }
}

TEST_CASE("FinishNotification.the short name is the first word of the command line", "[notification]")
{
    CHECK(shortNameOf("make -j8 all") == "make");
    CHECK(shortNameOf("   ls -la") == "ls");
    CHECK(shortNameOf("/usr/bin/cmake --build .") == "/usr/bin/cmake");
    CHECK(shortNameOf("") == "command");
    CHECK(shortNameOf("    ") == "command");
}

TEST_CASE("FinishNotification.the body says how long, where and in which tab", "[notification]")
{
    auto summary = aFinish(0);
    CHECK(finishNotificationBody(summary, "build", Home) == "took 3m 12s · ~/src/contour · build");

    SECTION("an unnamed tab leaves its segment out")
    {
        CHECK(finishNotificationBody(summary, "", Home) == "took 3m 12s · ~/src/contour");
    }

    SECTION("an unknown directory leaves its segment out")
    {
        summary.workingDirectory = {};
        CHECK(finishNotificationBody(summary, "build", Home) == "took 3m 12s · build");
    }

    SECTION("a directory on another machine is not shortened against this one's home")
    {
        summary.workingDirectory.locality = vtbackend::ContextLocality::Foreign;
        CHECK(finishNotificationBody(summary, "build", Home)
              == "took 3m 12s · /home/bob/src/contour · build");
    }

    SECTION("a directory not known to be on this machine is not shortened either")
    {
        // The rule of the pane header and the status line's {Cwd}: only a Local path is this home's.
        summary.workingDirectory.locality = vtbackend::ContextLocality::Unknown;
        CHECK(finishNotificationBody(summary, "build", Home)
              == "took 3m 12s · /home/bob/src/contour · build");
    }

    SECTION("an unknown home shortens nothing")
    {
        CHECK(finishNotificationBody(summary, "build", "") == "took 3m 12s · /home/bob/src/contour · build");
    }

    SECTION("an unknown duration leaves its segment out")
    {
        // A record from another process may carry no times at all: it took no time it could name, not 0ms.
        summary.duration.reset();
        CHECK(finishNotificationBody(summary, "build", Home) == "~/src/contour · build");
    }
}

TEST_CASE("FinishNotification.an unknown duration passes only a zero floor", "[notification]")
{
    auto summary = aFinish(2);
    summary.duration.reset();
    auto const policyWithFloor = [](std::chrono::seconds floor) {
        return FinishNotificationConfig { .when = NotifyWhen::Always,
                                          .minDuration = floor,
                                          .outcome = NotifyOutcome::Any,
                                          .action = NotifyAction::Notify,
                                          .clearOn = NotifyClearOn::Focus };
    };
    CHECK(finishNoticeKind(summary, policyWithFloor(0s), SessionVisibility::Hidden).has_value());
    CHECK_FALSE(finishNoticeKind(summary, policyWithFloor(1s), SessionVisibility::Hidden).has_value());
}

TEST_CASE("FinishNotification.the decision table", "[notification]")
{
    // when x visibility, spelled out rather than re-derived, so this does not restate the rule it
    // checks. Rows: never, unfocused, hidden, always. Columns: Focused, VisibleUnfocused, Hidden.
    constexpr auto WantedByVisibility = std::array {
        std::array { false, false, false },
        std::array { false, true, true },
        std::array { false, false, true },
        std::array { true, true, true },
    };

    enum class Ending : uint8_t
    {
        Success = 0,
        Failure,
        UnknownExit,
        Signal,
    };

    auto const when =
        GENERATE(NotifyWhen::Never, NotifyWhen::Unfocused, NotifyWhen::Hidden, NotifyWhen::Always);
    auto const visibility =
        GENERATE(SessionVisibility::Focused, SessionVisibility::VisibleUnfocused, SessionVisibility::Hidden);
    auto const duration = GENERATE(9s, 10s, 11s); // below, at and above the 10 s floor
    auto const ending = GENERATE(Ending::Success, Ending::Failure, Ending::UnknownExit, Ending::Signal);
    auto const wanted = GENERATE(NotifyOutcome::Any, NotifyOutcome::Failure);
    auto const action = GENERATE(NotifyAction::Notify, NotifyAction::Bell, NotifyAction::NotifyBell);

    auto summary = aFinish(std::nullopt, {}, duration);
    switch (ending)
    {
        case Ending::Success: summary.exitCode = 0; break;
        case Ending::Failure: summary.exitCode = 2; break;
        case Ending::UnknownExit: break;
        case Ending::Signal:
            summary.exitCode = 137;
            summary.outcome = vtbackend::ContextOutcome { .exit = vtbackend::ContextExit::Crash,
                                                          .signal = vtbackend::ContextSignal::Kill };
            break;
    }

    auto const policy = FinishNotificationConfig {
        .when = when, .minDuration = 10s, .outcome = wanted, .action = action, .clearOn = NotifyClearOn::Focus
    };
    // The decision alone: no tab name and no home in sight.
    auto const kind = finishNoticeKind(summary, policy, visibility);

    auto const isFailure = ending == Ending::Failure || ending == Ending::Signal;
    auto const expected = WantedByVisibility.at(static_cast<size_t>(when)).at(static_cast<size_t>(visibility))
                          && duration >= 10s && (wanted == NotifyOutcome::Any || isFailure);

    INFO(std::format("when={} visibility={} duration={}s ending={} outcome={} action={}",
                     static_cast<int>(when),
                     static_cast<int>(visibility),
                     duration.count(),
                     static_cast<int>(ending),
                     static_cast<int>(wanted),
                     static_cast<int>(action)));
    REQUIRE(kind.has_value() == expected);
    if (kind.has_value())
        CHECK(finishNotificationText(summary, *kind, "build", Home, action).action == action);
}

TEST_CASE("FinishNotification.a floor beyond any duration silences everything", "[notification]")
{
    // Ten billion seconds is more than steady_clock's nanoseconds can count: the floor must not wrap
    // around when it is compared with how long a command ran.
    auto const policy = FinishNotificationConfig { .when = NotifyWhen::Always,
                                                   .minDuration = std::chrono::seconds { 10'000'000'000 },
                                                   .outcome = NotifyOutcome::Any,
                                                   .action = NotifyAction::Notify,
                                                   .clearOn = NotifyClearOn::Focus };
    CHECK_FALSE(finishNoticeKind(aFinish(2, {}, 1h), policy, SessionVisibility::Hidden).has_value());
}

TEST_CASE("FinishNotification.a command a moment short of the floor stays silent", "[notification]")
{
    // The floor is compared in whole seconds; a part second must not round a short command up to it.
    auto const policy = FinishNotificationConfig { .when = NotifyWhen::Always,
                                                   .minDuration = 10s,
                                                   .outcome = NotifyOutcome::Any,
                                                   .action = NotifyAction::Notify,
                                                   .clearOn = NotifyClearOn::Focus };
    auto const at = [&](std::chrono::steady_clock::duration duration) {
        return finishNoticeKind(aFinish(2, {}, duration), policy, SessionVisibility::Hidden).has_value();
    };
    CHECK_FALSE(at(9999ms));
    CHECK(at(10s));
    CHECK(at(10001ms));
}

TEST_CASE("FinishNotification.an OSC 3008 failure with no status counts as a failure", "[notification]")
{
    // What the terminal records for `OSC 3008 ; end=… ; exit=failure` with no `status=`.
    auto const summary = aFinish(0, vtbackend::ContextOutcome { .exit = vtbackend::ContextExit::Failure });
    auto const policy = FinishNotificationConfig { .when = NotifyWhen::Always,
                                                   .minDuration = 0s,
                                                   .outcome = NotifyOutcome::Failure,
                                                   .action = NotifyAction::Notify,
                                                   .clearOn = NotifyClearOn::Focus };
    auto const kind = finishNoticeKind(summary, policy, SessionVisibility::Hidden);
    REQUIRE(kind.has_value());
    CHECK(finishNotificationText(summary, *kind, "build", Home, policy.action).title == "✗ make failed");
}

TEST_CASE("FinishNotification.a hostile command line reaches the desktop only as text", "[notification]")
{
    // The command line, the directory and the tab name all come from whatever wrote to the terminal.
    auto summary = aFinish(0);

    SECTION("an embedded bracketed-paste end and a newline")
    {
        summary.commandLine = vtbackend::CommandLineText { "\x1b[201~rm -rf /\nmake" };
        auto const title = finishNotificationTitle(summary);
        CHECK_FALSE(title.contains('\x1b'));
        CHECK_FALSE(title.contains('\n'));
    }

    SECTION("a bidi override")
    {
        // U+202E RIGHT-TO-LEFT OVERRIDE, then text. Spliced in byte by byte rather than written into a
        // literal, where it would be a misleading bidi character itself.
        auto const rightToLeftOverride = std::string { '\xE2', '\x80', '\xAE' };
        summary.commandLine = vtbackend::CommandLineText { rightToLeftOverride + "ekam" };
        CHECK_FALSE(finishNotificationTitle(summary).contains(rightToLeftOverride));
    }

    SECTION("a 64 KiB word is cut to the name budget")
    {
        // The three-byte "…" counts against MaxCommandNameBytes (vtbackend::truncateUtf8): 45 + 3 = 48.
        summary.commandLine = vtbackend::CommandLineText { std::string(65536, 'a') };
        CHECK(finishNotificationTitle(summary) == "✓ " + std::string(45, 'a') + "… finished");
    }

    SECTION("a cut never splits a character")
    {
        // "x" then 30 three-byte euro signs: the budget leaves 45 bytes before the ellipsis, and byte 45 is
        // inside the 15th euro, so the cut backs off to 43 -- "x" and 14 whole euros.
        summary.commandLine = vtbackend::CommandLineText { "x" + repeated("€", 30) };
        CHECK(finishNotificationTitle(summary) == "✓ x" + repeated("€", 14) + "… finished");
    }

    SECTION("a 64 KiB directory and tab name are each cut to the segment budget")
    {
        using contour::platform::MaxBodySegmentBytes;
        summary.workingDirectory.path = "/" + std::string(65536, 'd');
        auto const body = finishNotificationBody(summary, std::string(65536, 't'), Home);

        auto const separator = std::string_view { " · " };
        auto const duration = std::string_view { "took 3m 12s" };
        CHECK(body.size() <= duration.size() + (2 * separator.size()) + (2 * MaxBodySegmentBytes));

        auto const directoryEnd = body.find(separator, duration.size() + separator.size());
        REQUIRE(directoryEnd != std::string::npos);
        CHECK(body.substr(0, directoryEnd).ends_with("…"));
        CHECK(body.ends_with("…"));
    }

    SECTION("a cut comes after sanitising, so replaced control bytes cannot outgrow the budget")
    {
        // Each control byte becomes a three-byte replacement character.
        using contour::platform::MaxBodySegmentBytes;
        summary.workingDirectory = {};
        auto const body = finishNotificationBody(summary, std::string(65536, '\x01'), Home);
        CHECK(body.size() <= std::string_view { "took 3m 12s · " }.size() + MaxBodySegmentBytes);
    }

    SECTION("a hostile directory and tab name are sanitised too")
    {
        summary.workingDirectory.path = "/tmp/\x1b]0;pwned\x07";
        auto const body = finishNotificationBody(summary, "tab\x1b[31m", Home);
        CHECK_FALSE(body.contains('\x1b'));
        CHECK_FALSE(body.contains('\x07'));
    }
}

TEST_CASE("FinishNotification.each action names its effects", "[notification]")
{
    CHECK(effectsOf(NotifyAction::Notify).test(FinishEffect::Notification));
    CHECK_FALSE(effectsOf(NotifyAction::Notify).test(FinishEffect::Bell));
    CHECK(effectsOf(NotifyAction::Bell).test(FinishEffect::Bell));
    CHECK_FALSE(effectsOf(NotifyAction::Bell).test(FinishEffect::Notification));
    CHECK(effectsOf(NotifyAction::NotifyBell).test(FinishEffect::Notification));
    CHECK(effectsOf(NotifyAction::NotifyBell).test(FinishEffect::Bell));
}

TEST_CASE("FinishNotification.one identifier per session", "[notification]")
{
    CHECK(finishNotificationIdentifier(3) == "contour:command-finished:3");
    CHECK(finishNotificationIdentifier(3) != finishNotificationIdentifier(4));
}

TEST_CASE("FinishNotification.no OSC 99 identifier names a finish notification", "[notification]")
{
    // The notifier is shared with the session's OSC 99 notifications, so an application spelling the
    // identifier out must land on another one.
    auto const identifier = finishNotificationIdentifier(3);
    CHECK(vtbackend::parseOSC99(std::format("i={}:p=close;", identifier)).identifier != identifier);
    CHECK(vtbackend::parseOSC99(std::format("i={};title", identifier)).identifier != identifier);
}
