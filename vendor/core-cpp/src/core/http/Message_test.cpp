// SPDX-License-Identifier: Apache-2.0
//
// The message vocabulary: the error codes and their payload, and the validating factories every
// outbound field, method, target and reason phrase is built through.
#include <core/http/Message.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

using core::http::Fields;
using core::http::HttpError;
using core::http::HttpErrorCode;
using core::http::Method;
using core::http::ReasonPhrase;
using core::http::RequestTarget;
using core::http::RequestTargetForm;
using namespace std::string_view_literals;

namespace
{

/// Every code below the @c Last sentinel.
auto allCodes()
{
    return std::views::iota(std::size_t { 0 }, static_cast<std::size_t>(HttpErrorCode::Last))
           | std::views::transform([](std::size_t index) { return static_cast<HttpErrorCode>(index); });
}

} // namespace

// The payload's field types are the pinned wasi:http `error-code` payloads' (R26): a DNS rcode is a
// string, its info code a u16; sizes are u64 (P14 narrows the u32 cases).
static_assert(std::is_same_v<decltype(HttpError::code), HttpErrorCode>);
static_assert(std::is_same_v<decltype(HttpError::size), std::optional<std::uint64_t>>);
static_assert(std::is_same_v<decltype(HttpError::fieldName), std::optional<std::string>>);
static_assert(std::is_same_v<decltype(HttpError::dnsRcode), std::optional<std::string>>);
static_assert(std::is_same_v<decltype(HttpError::dnsInfoCode), std::optional<std::uint16_t>>);
static_assert(std::is_same_v<decltype(HttpError::coding), std::optional<std::string>>);
static_assert(std::is_same_v<decltype(HttpError::tls), std::optional<core::net::TlsFailure>>);
static_assert(std::is_same_v<decltype(HttpError::detail), std::string>);

TEST_CASE("Message.everyErrorCodeHasADistinctName", "[http]")
{
    auto names = std::set<std::string_view> {};
    for (auto const code: allCodes())
    {
        auto const name = core::http::toString(code);
        INFO("code " << static_cast<int>(code));
        CHECK_FALSE(name.empty());
        CHECK(name != core::http::toString(HttpErrorCode::Last));
        names.insert(name);
    }
    CHECK(names.size() == static_cast<std::size_t>(HttpErrorCode::Last));
    CHECK(core::http::toString(HttpErrorCode::DnsTimeout) == "DNS timeout");
    CHECK(core::http::toString(HttpErrorCode::InternalError) == "internal error");
}

TEST_CASE("Message.errorToStringCarriesItsPayload", "[http]")
{
    auto const error = HttpError { .code = HttpErrorCode::RequestHeaderSize,
                                   .size = 17000,
                                   .fieldName = std::string { "X-Big" },
                                   .dnsRcode = std::nullopt,
                                   .dnsInfoCode = std::nullopt,
                                   .coding = std::nullopt,
                                   .tls = std::nullopt,
                                   .detail = "field line too long",
                                   .reason = core::http::RefusalReason::Unclassified };
    auto const text = error.toString();
    CHECK(text.contains("request header size"));
    CHECK(text.contains("X-Big"));
    CHECK(text.contains("17000"));
    CHECK(text.contains("field line too long"));
}

TEST_CASE("Message.fieldsAppendRefusesInvalidNamesAndValues", "[http]")
{
    struct Row
    {
        std::string_view name;
        std::string_view value;
    };
    auto const refused = std::vector<Row> {
        { .name = "X", .value = "a\r\nb" }, // response splitting
        { .name = "X", .value = "a\rb" },   // a bare CR
        { .name = "X", .value = "a\nb" },   // a bare LF
        { .name = "X", .value = "a\0b"sv }, // NUL
        { .name = "X",
          .value = "a\x01"
                   "b" },                   // another control character
        { .name = "X", .value = "a\x7f" },  // DEL
        { .name = "X", .value = " a" },     // leading whitespace
        { .name = "X", .value = "a\t" },    // trailing whitespace
        { .name = "", .value = "a" },       // an empty name
        { .name = "X Y", .value = "a" },    // a space in the name
        { .name = "X:", .value = "a" },     // a colon in the name
        { .name = "X\r\nY", .value = "a" }, // CRLF in the name
        { .name = "Ä", .value = "a" },      // a non-token octet in the name
    };
    for (auto const& row: refused)
    {
        auto fields = Fields {};
        INFO("name '" << row.name << "' value '" << row.value << "'");
        auto const result = fields.append(std::string { row.name }, std::string { row.value });
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == HttpErrorCode::ProtocolError);
        CHECK(fields.entries().empty());
    }
}

TEST_CASE("Message.fieldsAcceptFramingFieldsAndInnerWhitespace", "[http]")
{
    // Content-Length and TE are legal guest fields: P14 strips them from the head and passes the
    // length on as knownLength. Refusing them is the connection's job (D7), not the field type's.
    auto fields = Fields {};
    REQUIRE(fields.append("Content-Length", "3").has_value());
    REQUIRE(fields.append("TE", "trailers").has_value());
    REQUIRE(fields.append("Transfer-Encoding", "chunked").has_value());
    REQUIRE(fields.append("X-Spaced", "a b\tc").has_value());
    REQUIRE(fields.append("X-Empty", "").has_value());
    REQUIRE(fields.append("X-Obs-Text", "caf\xc3\xa9").has_value());
    CHECK(fields.entries().size() == 6);
}

TEST_CASE("Message.fieldsGetIsCaseInsensitiveAndKeepsOrder", "[http]")
{
    auto fields = Fields {};
    REQUIRE(fields.append("Set-Cookie", "a=1").has_value());
    REQUIRE(fields.append("Host", "example.com").has_value());
    REQUIRE(fields.append("set-cookie", "b=2").has_value());

    auto const cookies = fields.get("SET-COOKIE");
    REQUIRE(cookies.size() == 2);
    CHECK(cookies[0] == "a=1");
    CHECK(cookies[1] == "b=2");
    CHECK(fields.get("Absent").empty());

    CHECK(fields.erase("SET-cookie") == 2);
    CHECK(fields.erase("SET-cookie") == 0);
    REQUIRE(fields.entries().size() == 1);
    CHECK(fields.entries()[0].name == "Host");
    CHECK(fields.entries()[0].value == "example.com");
}

TEST_CASE("Message.methodParse", "[http]")
{
    for (auto const text: { "GE T"sv, ""sv, "GET\r\n"sv, "G(T"sv, "GET\0"sv })
    {
        INFO("method '" << text << "'");
        auto const method = Method::parse(text);
        REQUIRE_FALSE(method.has_value());
        CHECK(method.error().code == HttpErrorCode::RequestMethodInvalid);
    }
    auto const method = Method::parse("PROPFIND");
    REQUIRE(method.has_value());
    CHECK(method->text() == "PROPFIND");
    CHECK(Method {}.text() == "GET");
}

TEST_CASE("Message.requestTargetParse", "[http]")
{
    struct Row
    {
        std::string_view text;
        RequestTargetForm form;
    };
    auto const accepted = std::vector<Row> {
        { .text = "/", .form = RequestTargetForm::Origin },
        { .text = "/a/b?c=d&e=%20f", .form = RequestTargetForm::Origin },
        { .text = "/a:b@c!$'()*+,;=-._~", .form = RequestTargetForm::Origin },
        { .text = "http://example.com/x?y", .form = RequestTargetForm::Absolute },
        { .text = "https://[::1]:8443/", .form = RequestTargetForm::Absolute },
        { .text = "example.com:443", .form = RequestTargetForm::Authority },
        { .text = "[::1]:443", .form = RequestTargetForm::Authority },
        { .text = "*", .form = RequestTargetForm::Asterisk },
    };
    for (auto const& row: accepted)
    {
        INFO("target '" << row.text << "'");
        auto const target = RequestTarget::parse(row.text);
        REQUIRE(target.has_value());
        CHECK(target->text() == row.text);
        CHECK(target->form() == row.form);
    }

    for (auto const text: { "/a b"sv,
                            ""sv,
                            "/a\r\nb"sv,
                            "/a#frag"sv,
                            "/%zz"sv,
                            "/%2"sv,
                            "a/b"sv,
                            "example.com"sv,
                            "example.com:"sv,
                            "example.com:8x"sv,
                            "**"sv,
                            "/\x80"sv,
                            "1http://x/"sv })
    {
        INFO("target '" << text << "'");
        auto const target = RequestTarget::parse(text);
        REQUIRE_FALSE(target.has_value());
        CHECK(target.error().code == HttpErrorCode::RequestUriInvalid);
    }
    CHECK(RequestTarget {}.text() == "/");
}

TEST_CASE("Message.reasonPhraseParse", "[http]")
{
    for (auto const text: { "OK\r\n"sv, "O\rK"sv, "O\nK"sv, "\x01"sv })
    {
        INFO("reason '" << text << "'");
        auto const reason = ReasonPhrase::parse(text);
        REQUIRE_FALSE(reason.has_value());
        CHECK(reason.error().code == HttpErrorCode::ProtocolError);
    }
    for (auto const text: { "OK"sv, ""sv, "Not Found"sv, "Tab\there"sv, "caf\xc3\xa9"sv })
    {
        INFO("reason '" << text << "'");
        auto const reason = ReasonPhrase::parse(text);
        REQUIRE(reason.has_value());
        CHECK(reason->text() == text);
    }
}
