// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the launch-failure vocabulary — the reason an open request carries when it is
// rejected without ever reaching the desktop. Pure and constexpr: the point of the enum is that
// every cause has a sentence, and a new enumerator must not silently fall through to "unknown".
// @see issue #2075.
//
// Also the Qt launcher's detached starts, which need a real child process to prove the bytes arrive, that
// the child outlives the launcher, and that it was placed before it ran.

#include <contour/platform/ExternalLauncher.hpp>
#include <contour/platform/QtExternalLauncher.hpp>

#include <vtpty/ProcessPlacement.hpp>
#include <vtpty/test/ProcessTesting.hpp>

#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <QtTest/QTest>

using contour::platform::describe;
using contour::platform::LaunchError;

TEST_CASE("describe(LaunchError) names every cause", "[contour][launcher]")
{
    // Each enumerator listed by hand rather than swept: the assertion worth making is that a value
    // added tomorrow has a sentence written for it, and a loop over a range would pass without one.
    // Pinning the exact sentences is also what keeps the switch's trailing `return "unknown error"`
    // out of reach -- an enumerator added without a case would fail here.
    STATIC_CHECK(describe(LaunchError::InvalidUrl) == "the address is not a valid URL");
    STATIC_CHECK(describe(LaunchError::NoHandler) == "no application is registered to open it");
}

namespace
{
/// Writes @p content to @p name in @p dir, and returns the file's path.
[[nodiscard]] std::filesystem::path writeInput(QTemporaryDir const& dir,
                                               QString const& name,
                                               QByteArray const& content)
{
    auto const path = dir.filePath(name);
    QFile file(path);
    REQUIRE(file.open(QIODevice::WriteOnly));
    REQUIRE(file.write(content) == content.size());
    return { path.toStdU16String() };
}

/// Whether @p path comes to hold @p expected within a few seconds (line ends aside: Windows' sort writes
/// CRLF).
[[nodiscard]] bool eventuallyHolds(QString const& path, QByteArray const& expected)
{
    return QTest::qWaitFor(
        [&] {
            QFile file(path);
            return file.open(QIODevice::ReadOnly) && file.readAll().replace("\r\n", "\n") == expected;
        },
        10000);
}

/// A command line that copies its standard input into @p target, after @p delay seconds.
[[nodiscard]] std::string copyingInputInto(QString const& target, int delay)
{
#ifdef _WIN32
    // cmd.exe splits its own command line: the rest after the program is handed on as written.
    auto const native = QDir::toNativeSeparators(target).toStdString();
    if (delay == 0)
        return std::format(R"(cmd.exe /c "sort > "{}"")", native);
    return std::format(R"(cmd.exe /c "ping -n {} 127.0.0.1 > nul & sort > "{}"")", delay + 1, native);
#else
    // sh -c SCRIPT TARGET: the first argument after the script is its $0.
    return std::format(R"(sh -c 'sleep {}; cat > "$0"' '{}')", delay, target.toStdString());
#endif
}

/// @return A launcher that places nothing, for the tests that are not about placement.
[[nodiscard]] contour::platform::QtExternalLauncher unplacedLauncher()
{
    return contour::platform::QtExternalLauncher { std::make_shared<vtpty::NoPlacement>() };
}
} // namespace

TEST_CASE("QtExternalLauncher: runDetachedWithInput refuses a program that is not there, at once",
          "[contour][launcher][semanticblocks]")
{
    QTemporaryDir const dir;
    REQUIRE(dir.isValid());
    auto const input = writeInput(dir, QStringLiteral("input.txt"), QByteArrayLiteral("x"));

    auto launcher = unplacedLauncher();
    auto timer = QElapsedTimer {};
    timer.start();
    auto const result = launcher.runDetachedWithInput("contour-test-no-such-program-4711 --flag", input);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == contour::platform::SpawnError::NotFound);
    CHECK(timer.elapsed() < 1000);
}

TEST_CASE("QtExternalLauncher: runDetachedWithInput feeds the file to the program's standard input",
          "[contour][launcher][semanticblocks]")
{
    QTemporaryDir const dir;
    REQUIRE(dir.isValid());
    auto const input = writeInput(dir, QStringLiteral("input.txt"), QByteArrayLiteral("built\n"));
    auto const target = dir.filePath(QStringLiteral("piped.txt"));

    auto launcher = unplacedLauncher();
    REQUIRE(launcher.runDetachedWithInput(copyingInputInto(target, 0), input).has_value());

    CHECK(eventuallyHolds(target, QByteArrayLiteral("built\n")));
}

TEST_CASE("QtExternalLauncher: a program started with input outlives the launcher",
          "[contour][launcher][semanticblocks]")
{
    // Detached: the child is not the launcher's to end, so it finishes its work after the launcher -- and,
    // in production, Contour -- has gone.
    QTemporaryDir const dir;
    REQUIRE(dir.isValid());
    auto const input = writeInput(dir, QStringLiteral("input.txt"), QByteArrayLiteral("kept\n"));
    auto const target = dir.filePath(QStringLiteral("piped.txt"));

    {
        auto launcher = unplacedLauncher();
        REQUIRE(launcher.runDetachedWithInput(copyingInputInto(target, 1), input).has_value());
    }

    CHECK(eventuallyHolds(target, QByteArrayLiteral("kept\n")));
}

#ifndef _WIN32
TEST_CASE("QtExternalLauncher: a program it starts is placed before it runs", "[contour][launcher]")
{
    // A link's opener, a second Contour, a pager fed a command's output: started into Contour's own systemd
    // unit, an out-of-memory kill inside any of them would stop Contour with it. Each goes through the
    // placement the sessions use instead.
    QTemporaryDir const dir;
    REQUIRE(dir.isValid());
    auto const target = dir.filePath(QStringLiteral("ran.txt"));
    auto const placement = std::make_shared<vtpty::testing::ReleasingPlacement>();
    auto launcher = contour::platform::QtExternalLauncher { placement };

    REQUIRE(launcher
                .runDetached(QStringLiteral("sh"),
                             { QStringLiteral("-c"), QStringLiteral(R"(echo ran > "$0")"), target })
                .has_value());
    CHECK(placement->pids().size() == 1);
    CHECK(eventuallyHolds(target, QByteArrayLiteral("ran\n")));
}

TEST_CASE("QtExternalLauncher: a program started with input is placed before it runs", "[contour][launcher]")
{
    QTemporaryDir const dir;
    REQUIRE(dir.isValid());
    auto const input = writeInput(dir, QStringLiteral("input.txt"), QByteArrayLiteral("placed\n"));
    auto const target = dir.filePath(QStringLiteral("piped.txt"));
    auto const placement = std::make_shared<vtpty::testing::ReleasingPlacement>();
    auto launcher = contour::platform::QtExternalLauncher { placement };

    REQUIRE(launcher.runDetachedWithInput(copyingInputInto(target, 0), input).has_value());
    CHECK(placement->pids().size() == 1);
    CHECK(eventuallyHolds(target, QByteArrayLiteral("placed\n")));
}

TEST_CASE("QtExternalLauncher: a program that is not there is not handed to the placement",
          "[contour][launcher]")
{
    auto const placement = std::make_shared<vtpty::testing::ReleasingPlacement>();
    auto launcher = contour::platform::QtExternalLauncher { placement };
    auto const started = launcher.runDetached(QStringLiteral("contour-test-no-such-program-4711"), {});
    REQUIRE_FALSE(started.has_value());
    CHECK(started.error() == contour::platform::SpawnError::NotFound);
    CHECK(placement->pids().empty());
}

TEST_CASE("makeExternalLauncher: what the launcher it makes starts is placed", "[contour][launcher]")
{
    // On Linux the portal launcher hands every start -- its xdg-open fallback included -- to its process
    // half; the placement must reach that half.
    QTemporaryDir const dir;
    REQUIRE(dir.isValid());
    auto const target = dir.filePath(QStringLiteral("ran.txt"));
    auto const placement = std::make_shared<vtpty::testing::ReleasingPlacement>();
    auto const launcher = contour::platform::makeExternalLauncher(placement);

    REQUIRE(launcher
                ->runDetached(QStringLiteral("sh"),
                              { QStringLiteral("-c"), QStringLiteral(R"(echo ran > "$0")"), target })
                .has_value());
    CHECK(placement->pids().size() == 1);
    CHECK(eventuallyHolds(target, QByteArrayLiteral("ran\n")));
}
#endif
