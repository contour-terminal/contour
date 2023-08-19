// SPDX-License-Identifier: Apache-2.0
#define CATCH_CONFIG_RUNNER
#include <catch2/catch.hpp>

int main(int argc, char const* argv[])
{
    int const result = Catch::Session().run(argc, argv);

    // avoid closing extern console to close on VScode/windows
    // system("pause");

    return result;
}
