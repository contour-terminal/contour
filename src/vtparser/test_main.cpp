// SPDX-License-Identifier: Apache-2.0
#include <crispy/App.h>
#include <crispy/logstore.h>

#define CATCH_CONFIG_RUNNER
#include <catch2/catch.hpp>

int main(int argc, char const* argv[])
{
    char const* logFilterString = getenv("LOG");
    if (logFilterString)
    {
        logstore::configure(logFilterString);
        crispy::app::customizeLogStoreOutput();
    }
    int const result = Catch::Session().run(argc, argv);

    // avoid closing extern console to close on VScode/windows
    // system("pause");

    return result;
}
