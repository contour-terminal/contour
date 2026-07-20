// SPDX-License-Identifier: Apache-2.0
#include <crispy/App.h>
#include <crispy/SuppressWindowsDialogs.hpp>
#include <crispy/environment.h>
#include <crispy/logstore.h>

#define CATCH_CONFIG_RUNNER
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

int main(int argc, char const* argv[])
{
    crispy::suppressWindowsDialogs();

    if (auto const logFilterString = crispy::environment::get("LOG"))
    {
        logstore::configure(*logFilterString);
        crispy::app::customizeLogStoreOutput();
    }
    int const result = Catch::Session().run(argc, argv);

    // avoid closing extern console to close on VScode/windows
    // system("pause");

    return result;
}
