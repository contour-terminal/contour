// SPDX-License-Identifier: Apache-2.0
#include <vtpty/SpawnTrampoline.hpp>

#ifndef _WIN32
    #include <vthost/testing/KeeperFixture.hpp>
#endif

#include <core/testing/SuppressWindowsDialogs.hpp>

#define CATCH_CONFIG_RUNNER
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

int main(int argc, char const* argv[])
{
    // First of all: a trampoline invocation runs as nothing but the trampoline. @see SpawnTrampoline.hpp.
    vtpty::runSpawnTrampolineIfRequested(argc, argv);
#ifndef _WIN32
    // Then the keeper harness: `--keeper` runs a PTY keeper, `--keeper-serve` the daemon it starts. Both
    // before Catch, which must not see their arguments, and before anything starts a thread.
    if (auto const harness = vthost::testing::runKeeperHarnessIfRequested(argc, argv))
        return *harness;
#endif

    core::testing::suppressWindowsDialogs();

    int const result = Catch::Session().run(argc, argv);
    return result;
}
