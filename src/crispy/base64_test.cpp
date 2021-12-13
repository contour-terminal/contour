/**
 * This file is part of the "contour" project.
 *   Copyright (c) 2020 Christian Parpart <christian@parpart.family>
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
#include <crispy/base64.h>

#include <catch2/catch_all.hpp>

using namespace crispy;

TEST_CASE("base64.encode", "[base64]")
{
    CHECK("YQ==" == base64::encode("a"));
    CHECK("YWI=" == base64::encode("ab"));
    CHECK("YWJj" == base64::encode("abc"));
    CHECK("YWJjZA==" == base64::encode("abcd"));
    CHECK("Zm9vOmJhcg==" == base64::encode("foo:bar"));
}

TEST_CASE("base64.decode", "[base64]")
{
    CHECK("a" == base64::decode("YQ=="));
    CHECK("ab" == base64::decode("YWI="));
    CHECK("abc" == base64::decode("YWJj"));
    CHECK("abcd" == base64::decode("YWJjZA=="));
    CHECK("foo:bar" == base64::decode("Zm9vOmJhcg=="));
}
