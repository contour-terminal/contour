// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <string>

#include <vtcontrol/ControlError.hpp>
#include <vtcontrol/Json.hpp>
#include <vtcontrol/testing/JsonProbe.hpp>

using vtcontrol::testing::Probe;
using vtcontrol::testing::Probe2;

TEST_CASE("EnumTable maps both ways", "[vtcontrol][json]")
{
    CHECK(vtcontrol::nameOf(Probe2::Keep) == "keep");
    CHECK(vtcontrol::enumFromName<Probe2>("take") == Probe2::Take);
    CHECK_FALSE(vtcontrol::enumFromName<Probe2>("grab"));
    CHECK(vtcontrol::nameOf(vtcontrol::ControlErrorCode::MethodNotFound) == "methodNotFound");
}

TEST_CASE("decodeJson is strict about keys and types, lenient about absence", "[vtcontrol][json]")
{
    CHECK(vtcontrol::encodeJson(Probe { .name = "a" }) == R"({"name":"a","count":7,"focus":"take"})");
    CHECK(vtcontrol::decodeJson<Probe>(R"({"name":"b"})")->count == 7);
    CHECK(vtcontrol::decodeJson<Probe>("")->count == 7);
    CHECK(vtcontrol::decodeJson<Probe>(R"({"focus":"keep"})")->focus == Probe2::Keep);
    auto const unknownKey = vtcontrol::decodeJson<Probe>(R"({"label":"b"})");
    REQUIRE_FALSE(unknownKey);
    CHECK(unknownKey.error().code == vtcontrol::ControlErrorCode::BadRequest);
    CHECK(unknownKey.error().message.contains("label"));
    CHECK_FALSE(vtcontrol::decodeJson<Probe>(R"({"count":"7"})"));
    CHECK_FALSE(vtcontrol::decodeJson<Probe>(R"({"focus":"grab"})"));
    CHECK_FALSE(vtcontrol::decodeJson<Probe>("not json"));
    CHECK(vtcontrol::toValidUtf8("a\xff"
                                 "b")
          == "a\xEF\xBF\xBD"
             "b");
    CHECK(vtcontrol::encodeJson(Probe { .name = "\x1b[0m" }).contains(R"(\u001b)"));
}

TEST_CASE("an error code round-trips through JSON by its spelling", "[vtcontrol][json]")
{
    auto const error = vtcontrol::ControlError { .code = vtcontrol::ControlErrorCode::Ambiguous,
                                                 .message = "two panes",
                                                 .candidates = { "pane:a", "pane:b" } };
    auto const text = vtcontrol::encodeJson(error);
    CHECK(text == R"({"code":"ambiguous","message":"two panes","candidates":["pane:a","pane:b"]})");
    auto const back = vtcontrol::decodeJson<vtcontrol::ControlError>(text);
    REQUIRE(back);
    CHECK(back->code == vtcontrol::ControlErrorCode::Ambiguous);
    CHECK(back->candidates.size() == 2);
}

TEST_CASE("toValidUtf8 keeps valid text and repairs the rest", "[vtcontrol][json]")
{
    CHECK(vtcontrol::toValidUtf8("h\xC3\xA9llo \xE2\x82\xAC \xF0\x9F\x98\x80")
          == "h\xC3\xA9llo \xE2\x82\xAC \xF0\x9F\x98\x80");
    CHECK(vtcontrol::toValidUtf8("\xE2\x82") == "\xEF\xBF\xBD\xEF\xBF\xBD");
    CHECK(vtcontrol::toValidUtf8(std::string_view { "a\0b", 3 }) == std::string_view { "a\0b", 3 });
    CHECK(vtcontrol::toValidUtf8("\xC0\x80") == "\xEF\xBF\xBD\xEF\xBF\xBD");
}

TEST_CASE("strings keep the JSON valid whatever the pane printed", "[vtcontrol][json]")
{
    auto const nasty = std::string { "a\x1b[0m\x01\x7f\n\"\\\0z\xff", 13 };
    auto const text = vtcontrol::encodeJson(Probe { .name = nasty });
    CHECK(text
          == R"({"name":"a\u001b[0m\u0001\u007f\n\"\\\u0000z)"
             "\xEF\xBF\xBD"
             R"(","count":7,"focus":"take"})");
    auto const back = vtcontrol::decodeJson<Probe>(text);
    REQUIRE(back);
    CHECK(back->name == std::string { "a\x1b[0m\x01\x7f\n\"\\\0z\xEF\xBF\xBD", 15 });
    CHECK(vtcontrol::quoteJsonString("plain") == R"("plain")");
}
