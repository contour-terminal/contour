/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019-2020 Christian Parpart <christian@parpart.family>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
// #define CATCH_CONFIG_MAIN
#define CATCH_CONFIG_RUNNER
#include <catch2/catch.hpp>

int main(int argc, char const* argv[])
{
    int const result = Catch::Session().run(argc, argv);

    // avoid closing extern console to close on VScode/windows
    // system("pause");

    return result;
}
