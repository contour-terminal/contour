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
#include <crispy/utils.h>
#include <catch2/catch.hpp>

TEST_CASE("utils.split.0")
{
    auto result = crispy::splitKeyValuePairs("", ':');
    CHECK(result.size() == 0);
}

TEST_CASE("utils.split.1")
{
    auto result = crispy::splitKeyValuePairs("foo=bar", ':');
    CHECK(result.size() == 1);
    CHECK(result["foo"] == "bar");

    auto result2 = crispy::splitKeyValuePairs("foo=bar::", ':');
    CHECK(result2.size() == 1);
    CHECK(result2["foo"] == "bar");

    auto result3 = crispy::splitKeyValuePairs("::foo=bar", ':');
    CHECK(result3.size() == 1);
    CHECK(result3["foo"] == "bar");
}

TEST_CASE("utils.split.2")
{
    auto result = crispy::splitKeyValuePairs("foo=bar:fnord=tar", ':');
    CHECK(result.size() == 2);
    CHECK(result["foo"] == "bar");
    CHECK(result["fnord"] == "tar");

    auto result2 = crispy::splitKeyValuePairs("foo=bar::fnord=tar", ':');
    CHECK(result2["foo"] == "bar");
    CHECK(result2["fnord"] == "tar");
    CHECK(result2.size() == 2);
}

