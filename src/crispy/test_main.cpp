// SPDX-License-Identifier: Apache-2.0
#include <core/testing/SuppressWindowsDialogs.hpp>

#define CATCH_CONFIG_RUNNER
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

int main(int argc, char const* argv[])
{
    core::testing::suppressWindowsDialogs();

    int const result = Catch::Session().run(argc, argv);
    return result;
}
