// SPDX-License-Identifier: Apache-2.0
//
// The spawn ladder is the parent-side equivalent of the fallback the POSIX child walks between
// fork() and exec(). It exists because Windows has no fork: a CreateProcess() failure surfaces in
// the parent, so the retry has to be decided there. Keeping that decision here — pure, in terms of
// strings — is what lets it be covered on every platform rather than only on the one that needs it.
//
// See issue #1711: the Windows retry passed the SAME working directory as the first attempt, so
// ERROR_DIRECTORY ("The directory name is invalid") failed both times by construction.

#include <vtpty/SpawnLadder.h>

#include <catch2/catch_test_macros.hpp>

using namespace std::string_literals;

using vtpty::buildSpawnLadder;

TEST_CASE("buildSpawnLadder drops the working directory before it drops the command", "[spawnladder]")
{
    auto const ladder = buildSpawnLadder("nu.exe"s, R"(\\wsl$\Ubuntu\home\user)"s, "powershell.exe"s);

    REQUIRE(ladder.size() == 3);

    // Rung 1 is exactly what was asked for, and says nothing because nothing was given up yet.
    CHECK(ladder[0].commandLine == "nu.exe");
    CHECK(ladder[0].workingDirectory == R"(\\wsl$\Ubuntu\home\user)");
    CHECK(ladder[0].diagnostic.empty());

    // Rung 2 keeps the user's shell and gives up only the directory. This is the rung that fixes
    // #1711: the old code retried with the same directory, so it could never recover from a
    // directory that was the problem.
    CHECK(ladder[1].commandLine == "nu.exe");
    CHECK(ladder[1].workingDirectory.empty());
    CHECK(ladder[1].diagnostic.contains(R"(\\wsl$\Ubuntu\home\user)"));

    // Rung 3 is the last resort, and only then is the user's chosen shell abandoned.
    CHECK(ladder[2].commandLine == "powershell.exe");
    CHECK(ladder[2].workingDirectory.empty());
    CHECK(ladder[2].diagnostic.contains("nu.exe"));
    CHECK(ladder[2].diagnostic.contains("powershell.exe"));
}

TEST_CASE("buildSpawnLadder never retries something it already tried", "[spawnladder]")
{
    SECTION("no working directory means no directory-dropping rung")
    {
        auto const ladder = buildSpawnLadder("nu.exe"s, ""s, "powershell.exe"s);

        REQUIRE(ladder.size() == 2);
        CHECK(ladder[0].commandLine == "nu.exe");
        CHECK(ladder[0].workingDirectory.empty());
        CHECK(ladder[1].commandLine == "powershell.exe");
    }

    SECTION("a command that already IS the login shell gets no login-shell rung")
    {
        auto const ladder = buildSpawnLadder("powershell.exe"s, R"(C:\gone)"s, "powershell.exe"s);

        // Dropping the directory is still worth trying; running powershell.exe a second time in the
        // same (already-failed) way is not.
        REQUIRE(ladder.size() == 2);
        CHECK(ladder[1].commandLine == "powershell.exe");
        CHECK(ladder[1].workingDirectory.empty());
    }

    SECTION("nothing left to give up leaves a single attempt")
    {
        auto const ladder = buildSpawnLadder("powershell.exe"s, ""s, "powershell.exe"s);

        REQUIRE(ladder.size() == 1);
        CHECK(ladder[0].diagnostic.empty());
    }

    SECTION("no login shell known")
    {
        auto const ladder = buildSpawnLadder("nu.exe"s, R"(C:\gone)"s, ""s);

        REQUIRE(ladder.size() == 2);
        CHECK(ladder[1].commandLine == "nu.exe");
        CHECK(ladder[1].workingDirectory.empty());
    }
}

TEST_CASE("buildSpawnLadder explains every rung but the first", "[spawnladder]")
{
    // Only the rung that actually succeeds gets reported to the user, so each diagnostic must stand
    // on its own — a rung reached after the directory was already dropped has to say so itself.
    auto const ladder = buildSpawnLadder("nu.exe"s, R"(C:\gone)"s, "powershell.exe"s);

    REQUIRE(ladder.size() == 3);
    CHECK(ladder[0].diagnostic.empty());
    for (auto const& attempt: { ladder[1], ladder[2] })
        CHECK_FALSE(attempt.diagnostic.empty());

    // The last rung gave up both the directory and the command, and names both.
    CHECK(ladder[2].diagnostic.contains(R"(C:\gone)"));
    CHECK(ladder[2].diagnostic.contains("nu.exe"));
    CHECK(ladder[2].diagnostic.contains("powershell.exe"));
}
