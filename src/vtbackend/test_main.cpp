// SPDX-License-Identifier: Apache-2.0
#include <crispy/App.hpp>
#include <crispy/SuppressWindowsDialogs.hpp>
#include <crispy/environment.hpp>
#include <crispy/logstore.hpp>

#define CATCH_CONFIG_RUNNER
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

int main(int argc, char const* argv[])
{
    crispy::suppressWindowsDialogs();

    if (auto const logFilterString = crispy::defaultEnvironment().get("LOG"))
    {
        logstore::configure(*logFilterString);
        crispy::App::customizeLogStoreOutput();
    }
    int const result = Catch::Session().run(argc, argv);

    // avoid closing extern console to close on VScode/windows
    // system("pause");

    return result;
}
