/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019 Christian Parpart <christian@parpart.family>
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
#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>
#include <ground/StringUtils.h>

TEST_CASE("parseEscapedString", "[strings]")
{
    CHECK(ground::parseEscaped("") == "");
    CHECK(ground::parseEscaped("Text") == "Text");
    CHECK(ground::parseEscaped("\\033") == "\033");
    CHECK(ground::parseEscaped("\\x1b") == "\x1b");
    CHECK(ground::parseEscaped("Hello\\x20World") == "Hello World");
}
