// SPDX-License-Identifier: Apache-2.0
#include <core/Environment.hpp>
#include <core/cli/App.hpp>
#include <core/log/LogStore.hpp>
#include <core/testing/SuppressWindowsDialogs.hpp>

#define CATCH_CONFIG_RUNNER
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

int main(int argc, char const* argv[])
{
    core::testing::suppressWindowsDialogs();

    if (auto const logFilterString = core::defaultEnvironment().get("LOG"))
    {
        core::log::configure(*logFilterString);
        core::cli::App::customizeLogStoreOutput();
    }
    int const result = Catch::Session().run(argc, argv);

    // avoid closing extern console to close on VScode/windows
    // system("pause");

    return result;
}
