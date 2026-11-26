// SPDX-License-Identifier: Apache-2.0
#include <core/testing/SuppressWindowsDialogs.hpp>

#define CATCH_CONFIG_RUNNER
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include <print>

#include <vtextension/testing/RunGate.hpp>

int main(int argc, char const* argv[])
{
    core::testing::suppressWindowsDialogs();

    int const result = Catch::Session().run(argc, argv);
    // What a listener found once a case had ended fails the run, naming the case.
    auto const& failures = vtextension::testing::runGateFailures();
    for (auto const& failure: failures)
        std::println(stderr, "run gate: {}", failure);
    return result != 0 || failures.empty() ? result : 1;
}
