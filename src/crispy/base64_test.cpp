// SPDX-License-Identifier: Apache-2.0
#include <crispy/base64.h>

#include <catch2/catch.hpp>

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
