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
#include <terminal/Parser.h>
#include <catch2/catch.hpp>

using namespace std;
using namespace terminal;

TEST_CASE("Parser.utf8_single", "[Parser]")
{
    std::vector<char32_t> text;
    auto p = parser::Parser(
        [&](parser::ActionClass /*_actionClass*/, parser::Action _action, char32_t _char) {
            if (_action == parser::Action::Print)
                text.push_back(_char);
        },
        [&](std::string const& _errorString) {
            INFO(fmt::format("Parser error received. {}", _errorString));
        }
    );

    p.parseFragment("\xC3\xB6");  // ö

    REQUIRE(text.size() == 1);
    CHECK(0xF6 == static_cast<unsigned>(text.at(0)));
}

