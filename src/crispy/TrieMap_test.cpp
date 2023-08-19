// SPDX-License-Identifier: Apache-2.0
#include <crispy/TrieMap.h>

#include <catch2/catch.hpp>

#include <cassert>
#include <exception>
#include <iostream>

TEST_CASE("trie_map.simple")
{
    auto m = crispy::trie_map<std::string, int> {};

    m.insert("aa", 1);
    m.insert("aba", 2);
    m.insert("abb", 3);
    REQUIRE(m.size() == 3);

    auto const aa = m.search("aa");
    CHECK(std::get<crispy::exact_match<int>>(aa).value == 1);

    auto const ab = m.search("ab");
    REQUIRE(std::holds_alternative<crispy::partial_match>(ab));

    auto const aba = m.search("aba");
    REQUIRE(std::holds_alternative<crispy::exact_match<int>>(aba));
    CHECK(std::get<crispy::exact_match<int>>(aba).value == 2);

    auto const abb = m.search("abb");
    REQUIRE(std::holds_alternative<crispy::exact_match<int>>(abb));
    CHECK(std::get<crispy::exact_match<int>>(abb).value == 3);

    auto const abz = m.search("abz");
    REQUIRE(std::holds_alternative<crispy::no_match>(abz));

    m.clear();
    REQUIRE(m.size() == 0);
    REQUIRE(!m.contains("aa"));
    REQUIRE(!m.contains("aba"));
    REQUIRE(!m.contains("abb"));
}
