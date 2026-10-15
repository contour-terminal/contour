// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/shell/CommandBlockPaths.hpp>
#include <vtbackend/testing/TestHelpers.hpp>

#include <catch2/catch_test_macros.hpp>

#include <expected>
#include <optional>
#include <string>
#include <utility>

using namespace vtbackend;
using vtbackend::test::ranIn;

namespace
{

/// @return The terminal's directory for opening locally being @p cwd, on this machine; none when empty.
[[nodiscard]] TerminalDirectory here(std::string cwd)
{
    auto const locality = cwd.empty() ? ContextLocality::Unknown : ContextLocality::Local;
    return TerminalDirectory { .openLocally = std::move(cwd), .locality = locality };
}

} // namespace

TEST_CASE("ranInLocalDirectory.onlyARecordedDirectoryThisMachineVouchesFor", "[semanticblocks][blockpaths]")
{
    CHECK(ranInLocalDirectory(ranIn(1, "/src/a", ContextLocality::Local)));
    CHECK_FALSE(ranInLocalDirectory(ranIn(1, "", ContextLocality::Local)));
    CHECK_FALSE(ranInLocalDirectory(ranIn(1, "/src/a", ContextLocality::Unknown)));
    CHECK_FALSE(ranInLocalDirectory(ranIn(1, "/src/a", ContextLocality::Foreign)));
}

TEST_CASE("localPathBaseFor.aRowOutsideAnyBlockKeepsTheTerminalsDirectory", "[semanticblocks][blockpaths]")
{
    CHECK(localPathBaseFor(nullptr, here("/home/u")) == PathBases { "/home/u" });
    CHECK(localPathBaseFor(nullptr, here("")) == PathBases {});
}

TEST_CASE("localPathBaseFor.aBlockResolvesAgainstItsOwnDirectory", "[semanticblocks][blockpaths]")
{
    auto const local = ranIn(1, "/src/a", ContextLocality::Local);
    CHECK(localPathBaseFor(&local, here("/home/u")) == PathBases { "/src/a" });
    CHECK(localPathBaseFor(&local, here("")) == PathBases { "/src/a" });
}

TEST_CASE("localPathBaseFor.aBlockThatRanElsewhereRefuses", "[semanticblocks][blockpaths]")
{
    auto const foreign = ranIn(1, "/srv/app", ContextLocality::Foreign);
    CHECK(localPathBaseFor(&foreign, here("/home/u")) == std::unexpected(PathRefusal::RanElsewhere));

    auto const foreignWithoutPath = ranIn(1, "", ContextLocality::Foreign);
    CHECK(localPathBaseFor(&foreignWithoutPath, here("/home/u"))
          == std::unexpected(PathRefusal::RanElsewhere));
}

TEST_CASE("localPathBaseFor.aBlockWithoutALocalDirectoryKeepsTheTerminalsDirectory",
          "[semanticblocks][blockpaths]")
{
    auto const unrecorded = ranIn(1, "", ContextLocality::Unknown);
    CHECK(localPathBaseFor(&unrecorded, here("/home/u")) == PathBases { "/home/u" });
    CHECK(localPathBaseFor(&unrecorded, here("")) == PathBases {});
    auto const localWithoutPath = ranIn(1, "", ContextLocality::Local);
    CHECK(localPathBaseFor(&localWithoutPath, here("/home/u")) == PathBases { "/home/u" });

    // An OSC 3008 `cwd=` no machine id, host name or boundary vouches for: it may name a directory on the
    // far side of an ssh. Passed over, as the working directory for opening locally passes over it.
    auto const unvouched = ranIn(1, "/src/b", ContextLocality::Unknown);
    CHECK(localPathBaseFor(&unvouched, here("/home/u")) == PathBases { "/home/u" });
}

TEST_CASE("localPathBaseFor.aBlockStillPrintingRefusesOnceTheTerminalsDirectoryTurnedForeign",
          "[semanticblocks][blockpaths]")
{
    // `ssh host` started here; the remote shell reports its directory but marks no commands, so what it
    // prints lands in the local block.
    auto const running = ranIn(1, "/src/a", ContextLocality::Local);
    auto const remote = TerminalDirectory { .openLocally = {}, .locality = ContextLocality::Foreign };
    CHECK(localPathBaseFor(&running, remote) == std::unexpected(PathRefusal::RanElsewhere));
    CHECK(localPathBaseFor(&running, here("/home/u")) == PathBases { "/src/a" });

    auto prompting = ranIn(1, "", ContextLocality::Unknown);
    prompting.state = CommandBlockState::Prompting;
    CHECK(localPathBaseFor(&prompting, remote) == std::unexpected(PathRefusal::RanElsewhere));

    // A block that finished before printed nothing since: an `ls` before the `ssh` keeps its directory.
    auto finished = running;
    finished.state = CommandBlockState::Finished;
    CHECK(localPathBaseFor(&finished, remote) == PathBases { "/src/a" });
    CHECK(localPathBaseFor(nullptr, remote) == PathBases {});
}

TEST_CASE("localPathBaseFor.aFinishedBlockAShellElsewherePrintedIntoStillRefuses",
          "[semanticblocks][blockpaths]")
{
    // `ssh host`, back at the local prompt: the terminal's directory is local again, but the remote shell
    // reported one on its own machine while the block was open.
    auto finished = ranIn(1, "/src/a", ContextLocality::Local);
    finished.state = CommandBlockState::Finished;
    finished.foreignDirectory = ForeignDirectory::Reported;
    CHECK(localPathBaseFor(&finished, here("/src/a")) == std::unexpected(PathRefusal::RanElsewhere));

    finished.foreignDirectory = ForeignDirectory::Unreported;
    CHECK(localPathBaseFor(&finished, here("/src/a")) == PathBases { "/src/a" });
}

TEST_CASE("localPathBaseFor.thenTheDirectoryTheCommandLeftTheShellIn", "[semanticblocks][blockpaths]")
{
    // `cd build && make` started in /src/a and printed paths relative to /src/a/build, which the shell
    // reported only at the next prompt.
    auto record = ranIn(1, "/src/a", ContextLocality::Local);
    record.state = CommandBlockState::Finished;
    record.endWorkingDirectory =
        WorkingDirectorySnapshot { .path = "/src/a/build", .locality = ContextLocality::Local };
    CHECK(localPathBaseFor(&record, here("/home/u")) == (PathBases { "/src/a", "/src/a/build" }));

    // Left where it started: looked in once.
    record.endWorkingDirectory.path = "/src/a";
    CHECK(localPathBaseFor(&record, here("/home/u")) == PathBases { "/src/a" });

    // Left in a directory nothing vouches is on this machine: not looked in.
    record.endWorkingDirectory =
        WorkingDirectorySnapshot { .path = "/srv/app", .locality = ContextLocality::Foreign };
    CHECK(localPathBaseFor(&record, here("/home/u")) == PathBases { "/src/a" });
    record.endWorkingDirectory =
        WorkingDirectorySnapshot { .path = "/src/b", .locality = ContextLocality::Unknown };
    CHECK(localPathBaseFor(&record, here("/home/u")) == PathBases { "/src/a" });

    // Its own directory unrecorded: the terminal's stands in, and where it left the shell follows.
    auto unrecorded = ranIn(1, "", ContextLocality::Unknown);
    unrecorded.state = CommandBlockState::Finished;
    unrecorded.endWorkingDirectory =
        WorkingDirectorySnapshot { .path = "/src/a/build", .locality = ContextLocality::Local };
    CHECK(localPathBaseFor(&unrecorded, here("/home/u")) == (PathBases { "/home/u", "/src/a/build" }));
    CHECK(localPathBaseFor(&unrecorded, here("")) == PathBases { "/src/a/build" });
}
