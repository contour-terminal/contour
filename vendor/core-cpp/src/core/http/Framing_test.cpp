// SPDX-License-Identifier: Apache-2.0
//
// The pure layer's framing rules: the smuggling corpus through HeadParser, requestFraming and
// ChunkedDecoder, whole and one byte at a time; the RFC 9112 section 6.3 table for responses; the
// head parser's bounds; and the chunked codec in both directions.
#include <core/http/Framing.hpp>
#include <core/http/Message.hpp>
#include <core/http/SmugglingCorpus.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using core::http::BodyFraming;
using core::http::ChunkedDecoder;
using core::http::Fields;
using core::http::HeadParser;
using core::http::HttpErrorCode;
using core::http::HttpLimits;
using core::http::HttpVersion;
using core::http::MessageRole;
using core::http::Persistence;
using core::http::Progress;
using core::http::test::smugglingCorpus;
using core::http::test::smugglingLimits;
using core::http::test::SmugglingVerdict;

namespace
{

/// How many bytes each call to a parser is handed.
enum class Feeding : std::uint8_t
{
    Whole,   ///< Everything that is left, in one call.
    OneByte, ///< One byte per call.
};

/// What running a request through the pure layer came to.
struct Outcome
{
    std::optional<HttpErrorCode> error; ///< The refusal, if any.
    std::string body;                   ///< The decoded body of an accepted request.
    std::string detail;                 ///< Why, when it went wrong.
};

/// The next slice of @p rest that @p feeding hands a parser.
std::string_view slice(std::string_view rest, Feeding feeding)
{
    return feeding == Feeding::Whole ? rest : rest.substr(0, std::min<std::size_t>(rest.size(), 1));
}

/// Decodes the chunked body at the start of @p wire into @p body, or fails.
Outcome decodeChunked(MessageRole role, HttpLimits limits, std::string_view wire, Feeding feeding)
{
    auto decoder = ChunkedDecoder { role, limits };
    auto outcome = Outcome {};
    while (true)
    {
        auto const step = decoder.feed(slice(wire, feeding));
        if (!step.has_value())
            return Outcome { .error = step.error().code, .body = {}, .detail = step.error().toString() };
        outcome.body += step->data;
        wire.remove_prefix(step->consumed);
        if (step->progress == Progress::Complete)
            return outcome;
        if (wire.empty())
            return Outcome { .error = HttpErrorCode::InternalError, .body = {}, .detail = "body incomplete" };
    }
}

/// Runs @p wire, one message of @p role, through the head parser, the framing rules and the body's
/// framing; a response answers a GET.
Outcome runMessage(MessageRole role, std::string_view wire, HttpLimits limits, Feeding feeding)
{
    auto parser = HeadParser { role, limits };
    while (true)
    {
        auto const fed = parser.feed(slice(wire, feeding));
        if (!fed.has_value())
            return Outcome { .error = fed.error().code, .body = {}, .detail = fed.error().toString() };
        wire.remove_prefix(fed->consumed);
        if (fed->progress == Progress::Complete)
            break;
        if (wire.empty())
            return Outcome { .error = HttpErrorCode::InternalError, .body = {}, .detail = "head incomplete" };
    }
    auto const framing = [&] {
        if (role == MessageRole::Request)
        {
            auto const head = parser.takeRequest();
            return core::http::requestFraming(head.version, head.fields);
        }
        auto const head = parser.takeResponse();
        return core::http::responseFraming("GET", head.version, head.status, head.fields);
    }();
    if (!framing.has_value())
        return Outcome { .error = framing.error().code, .body = {}, .detail = framing.error().toString() };
    switch (framing->kind)
    {
        case BodyFraming::None: return Outcome {};
        case BodyFraming::Length:
            if (wire.size() < framing->length)
                return Outcome { .error = HttpErrorCode::InternalError, .body = {}, .detail = "body short" };
            return Outcome { .error = std::nullopt,
                             .body = std::string { wire.substr(0, framing->length) },
                             .detail = {} };
        case BodyFraming::Chunked: return decodeChunked(role, limits, wire, feeding);
        case BodyFraming::UntilClose:
            if (role == MessageRole::Response)
                return Outcome { .error = std::nullopt, .body = std::string { wire }, .detail = {} };
            break;
    }
    return Outcome { .error = HttpErrorCode::InternalError,
                     .body = {},
                     .detail = "a request read until close" };
}

/// The limits of the bounds cases: small, so a case's wire is short.
HttpLimits smallLimits(std::size_t maxFields)
{
    return HttpLimits { .maxHeadBytes = std::size_t { 64 } * 1024,
                        .maxFieldBytes = 1024,
                        .maxFields = maxFields,
                        .maxTrailerBytes = std::size_t { 16 } * 1024,
                        .maxChunkLineBytes = 64,
                        .maxReadChunk = 4096,
                        .maxBodyBytes = std::nullopt };
}

/// The fields of @p list, each appended or the case fails.
Fields fieldsOf(std::initializer_list<std::pair<std::string_view, std::string_view>> list)
{
    auto fields = Fields {};
    for (auto const& [name, value]: list)
        REQUIRE(fields.append(std::string { name }, std::string { value }).has_value());
    return fields;
}

} // namespace

TEST_CASE("Framing.smugglingCorpus", "[http][smuggling]")
{
    auto names = std::vector<std::string_view> {};
    for (auto const& row: smugglingCorpus())
    {
        CHECK(std::ranges::find(names, row.name) == names.end());
        names.push_back(row.name);
        for (auto const feeding: { Feeding::Whole, Feeding::OneByte })
        {
            INFO("row " << row.name << (feeding == Feeding::Whole ? " (whole)" : " (one byte at a time)"));
            auto const outcome = runMessage(row.role, row.wire, smugglingLimits(), feeding);
            INFO("outcome: " << outcome.detail);
            if (row.verdict == SmugglingVerdict::Refused)
            {
                REQUIRE(outcome.error.has_value());
                CHECK(*outcome.error == row.code);
                CHECK(row.status >= 400);
            }
            else
            {
                CHECK_FALSE(outcome.error.has_value());
                CHECK(outcome.body == row.body);
                CHECK(row.status == 200);
            }
        }
    }
    CHECK(names.size() >= 50);
}

TEST_CASE("Framing.requestTable", "[http]")
{
    struct Row
    {
        std::string_view name;
        HttpVersion version;
        Fields fields;
        BodyFraming kind;
        std::uint64_t length;
        Persistence persistence;
    };
    auto const rows = std::vector<Row> {
        { .name = "bodiless 1.1",
          .version = HttpVersion::Http11,
          .fields = {},
          .kind = BodyFraming::None,
          .length = 0,
          .persistence = Persistence::KeepAlive },
        { .name = "length",
          .version = HttpVersion::Http11,
          .fields = fieldsOf({ { "Content-Length", "5" } }),
          .kind = BodyFraming::Length,
          .length = 5,
          .persistence = Persistence::KeepAlive },
        { .name = "largest length",
          .version = HttpVersion::Http11,
          .fields = fieldsOf({ { "Content-Length", "18446744073709551615" } }),
          .kind = BodyFraming::Length,
          .length = UINT64_MAX,
          .persistence = Persistence::KeepAlive },
        { .name = "chunked",
          .version = HttpVersion::Http11,
          .fields = fieldsOf({ { "transfer-encoding", "Chunked" } }),
          .kind = BodyFraming::Chunked,
          .length = 0,
          .persistence = Persistence::KeepAlive },
        { .name = "connection close",
          .version = HttpVersion::Http11,
          .fields = fieldsOf({ { "Connection", "Upgrade, close" } }),
          .kind = BodyFraming::None,
          .length = 0,
          .persistence = Persistence::Close },
        { .name = "bodiless 1.0",
          .version = HttpVersion::Http10,
          .fields = {},
          .kind = BodyFraming::None,
          .length = 0,
          .persistence = Persistence::Close },
        { .name = "1.0 keep-alive",
          .version = HttpVersion::Http10,
          .fields = fieldsOf({ { "Connection", "Keep-Alive" }, { "Content-Length", "2" } }),
          .kind = BodyFraming::Length,
          .length = 2,
          .persistence = Persistence::KeepAlive },
    };
    for (auto const& row: rows)
    {
        INFO("row " << row.name);
        auto const framing = core::http::requestFraming(row.version, row.fields);
        REQUIRE(framing.has_value());
        CHECK(framing->kind == row.kind);
        CHECK(framing->length == row.length);
        CHECK(framing->persistence == row.persistence);
    }
}

TEST_CASE("Framing.responseTable", "[http]")
{
    struct Row
    {
        std::string_view name;
        std::string_view method;
        HttpVersion version;
        std::uint16_t status;
        Fields fields;
        BodyFraming kind;
        std::uint64_t length;
        Persistence persistence;
    };
    auto const lengthAndChunked = fieldsOf({ { "Content-Length", "3" }, { "Transfer-Encoding", "chunked" } });
    auto const rows = std::vector<Row> {
        { .name = "HEAD ignores the framing fields",
          .method = "HEAD",
          .version = HttpVersion::Http11,
          .status = 200,
          .fields = lengthAndChunked,
          .kind = BodyFraming::None,
          .length = 0,
          .persistence = Persistence::KeepAlive },
        { .name = "100 has no body",
          .method = "GET",
          .version = HttpVersion::Http11,
          .status = 100,
          .fields = fieldsOf({ { "Content-Length", "3" } }),
          .kind = BodyFraming::None,
          .length = 0,
          .persistence = Persistence::KeepAlive },
        { .name = "199 has no body",
          .method = "GET",
          .version = HttpVersion::Http11,
          .status = 199,
          .fields = {},
          .kind = BodyFraming::None,
          .length = 0,
          .persistence = Persistence::KeepAlive },
        { .name = "204 has no body",
          .method = "POST",
          .version = HttpVersion::Http11,
          .status = 204,
          .fields = fieldsOf({ { "Transfer-Encoding", "chunked" } }),
          .kind = BodyFraming::None,
          .length = 0,
          .persistence = Persistence::KeepAlive },
        { .name = "304 has no body",
          .method = "GET",
          .version = HttpVersion::Http11,
          .status = 304,
          .fields = fieldsOf({ { "Content-Length", "10" } }),
          .kind = BodyFraming::None,
          .length = 0,
          .persistence = Persistence::KeepAlive },
        { .name = "a 2xx to CONNECT is a tunnel",
          .method = "CONNECT",
          .version = HttpVersion::Http11,
          .status = 200,
          .fields = fieldsOf({ { "Content-Length", "10" } }),
          .kind = BodyFraming::None,
          .length = 0,
          .persistence = Persistence::Close },
        { .name = "a 407 to CONNECT has a body",
          .method = "CONNECT",
          .version = HttpVersion::Http11,
          .status = 407,
          .fields = fieldsOf({ { "Content-Length", "4" } }),
          .kind = BodyFraming::Length,
          .length = 4,
          .persistence = Persistence::KeepAlive },
        { .name = "a bare 1.1 response reads until close",
          .method = "GET",
          .version = HttpVersion::Http11,
          .status = 200,
          .fields = {},
          .kind = BodyFraming::UntilClose,
          .length = 0,
          .persistence = Persistence::Close },
        { .name = "TE and CL frame as chunked and close",
          .method = "GET",
          .version = HttpVersion::Http11,
          .status = 200,
          .fields = lengthAndChunked,
          .kind = BodyFraming::Chunked,
          .length = 0,
          .persistence = Persistence::Close },
        { .name = "chunked",
          .method = "GET",
          .version = HttpVersion::Http11,
          .status = 200,
          .fields = fieldsOf({ { "Transfer-Encoding", "gzip, chunked" } }),
          .kind = BodyFraming::Chunked,
          .length = 0,
          .persistence = Persistence::KeepAlive },
        { .name = "a quoted comma does not split the coding list",
          .method = "GET",
          .version = HttpVersion::Http11,
          .status = 200,
          .fields = fieldsOf({ { "Transfer-Encoding", "gzip;a=\"x, chunked\"" } }),
          .kind = BodyFraming::UntilClose,
          .length = 0,
          .persistence = Persistence::Close },
        { .name = "a response may carry other codings before chunked",
          .method = "GET",
          .version = HttpVersion::Http11,
          .status = 200,
          .fields = fieldsOf({ { "Transfer-Encoding", "gzip;q=1, chunked" } }),
          .kind = BodyFraming::Chunked,
          .length = 0,
          .persistence = Persistence::KeepAlive },
        { .name = "chunked, gzip reads until close",
          .method = "GET",
          .version = HttpVersion::Http11,
          .status = 200,
          .fields = fieldsOf({ { "Transfer-Encoding", "chunked, gzip" } }),
          .kind = BodyFraming::UntilClose,
          .length = 0,
          .persistence = Persistence::Close },
        { .name = "length",
          .method = "GET",
          .version = HttpVersion::Http11,
          .status = 200,
          .fields = fieldsOf({ { "Content-Length", "7" } }),
          .kind = BodyFraming::Length,
          .length = 7,
          .persistence = Persistence::KeepAlive },
        { .name = "1.0 without keep-alive closes",
          .method = "GET",
          .version = HttpVersion::Http10,
          .status = 200,
          .fields = fieldsOf({ { "Content-Length", "7" } }),
          .kind = BodyFraming::Length,
          .length = 7,
          .persistence = Persistence::Close },
        { .name = "1.0 with keep-alive persists",
          .method = "GET",
          .version = HttpVersion::Http10,
          .status = 200,
          .fields = fieldsOf({ { "Content-Length", "7" }, { "Connection", "keep-alive" } }),
          .kind = BodyFraming::Length,
          .length = 7,
          .persistence = Persistence::KeepAlive },
        { .name = "connection close",
          .method = "GET",
          .version = HttpVersion::Http11,
          .status = 200,
          .fields = fieldsOf({ { "Content-Length", "7" }, { "Connection", "CLOSE" } }),
          .kind = BodyFraming::Length,
          .length = 7,
          .persistence = Persistence::Close },
    };
    for (auto const& row: rows)
    {
        INFO("row " << row.name);
        auto const framing = core::http::responseFraming(row.method, row.version, row.status, row.fields);
        REQUIRE(framing.has_value());
        CHECK(framing->kind == row.kind);
        CHECK(framing->length == row.length);
        CHECK(framing->persistence == row.persistence);
    }
}

TEST_CASE("Framing.responseTableRefusals", "[http]")
{
    struct Row
    {
        std::string_view name;
        HttpVersion version;
        Fields fields;
    };
    auto const rows = std::vector<Row> {
        { .name = "two lengths",
          .version = HttpVersion::Http11,
          .fields = fieldsOf({ { "Content-Length", "3, 4" } }) },
        { .name = "a signed length",
          .version = HttpVersion::Http11,
          .fields = fieldsOf({ { "Content-Length", "+3" } }) },
        { .name = "chunked twice",
          .version = HttpVersion::Http11,
          .fields = fieldsOf({ { "Transfer-Encoding", "chunked, chunked" } }) },
        { .name = "TE on 1.0",
          .version = HttpVersion::Http10,
          .fields = fieldsOf({ { "Transfer-Encoding", "chunked" } }) },
        { .name = "chunked with a parameter",
          .version = HttpVersion::Http11,
          .fields = fieldsOf({ { "Transfer-Encoding", "chunked;q=1" } }) },
        { .name = "an escaped quote keeps the quoted-string open",
          .version = HttpVersion::Http11,
          .fields = fieldsOf({ { "Transfer-Encoding", R"(gzip;a="x\", chunked)" } }) },
        { .name = "an unterminated quote",
          .version = HttpVersion::Http11,
          .fields = fieldsOf({ { "Transfer-Encoding", "gzip;a=\"x, chunked" } }) },
        { .name = "a coding that is not a token",
          .version = HttpVersion::Http11,
          .fields = fieldsOf({ { "Transfer-Encoding", "gz(ip), chunked" } }) },
    };
    for (auto const& row: rows)
    {
        INFO("row " << row.name);
        auto const framing = core::http::responseFraming("GET", row.version, 200, row.fields);
        REQUIRE_FALSE(framing.has_value());
        CHECK(framing.error().code == HttpErrorCode::ProtocolError);
    }
}

TEST_CASE("Framing.framingFields", "[http]")
{
    for (auto const name:
         { "Transfer-Encoding", "content-length", "CONNECTION", "Keep-Alive", "te", "Upgrade" })
    {
        INFO("field " << name);
        CHECK(core::http::isFramingField(name));
    }
    for (auto const name: { "Host", "Trailer", "Content-Type", "Transfer-Encodings", "t", "" })
    {
        INFO("field " << name);
        CHECK_FALSE(core::http::isFramingField(name));
    }
}

TEST_CASE("HeadParser.response", "[http]")
{
    auto const wire =
        std::string_view { "HTTP/1.1 404 Not Found\r\nContent-Length: 3\r\nX-A:  b c \r\n\r\nabcNEXT" };
    for (auto const feeding: { Feeding::Whole, Feeding::OneByte })
    {
        auto parser = HeadParser { MessageRole::Response, HttpLimits {} };
        auto rest = wire;
        auto progress = Progress::NeedMore;
        while (progress == Progress::NeedMore)
        {
            auto const fed = parser.feed(slice(rest, feeding));
            REQUIRE(fed.has_value());
            rest.remove_prefix(fed->consumed);
            progress = fed->progress;
        }
        CHECK(rest == "abcNEXT");
        auto const head = parser.takeResponse();
        CHECK(head.status == 404);
        CHECK(head.reason.text() == "Not Found");
        CHECK(head.version == HttpVersion::Http11);
        REQUIRE(head.fields.entries().size() == 2);
        CHECK(head.fields.get("x-a") == std::vector<std::string_view> { "b c" });

        // A complete parser consumes nothing more.
        auto const again = parser.feed("more");
        REQUIRE(again.has_value());
        CHECK(again->consumed == 0);
        CHECK(again->progress == Progress::Complete);
    }
}

TEST_CASE("HeadParser.responseUnfoldsObsFold", "[http]")
{
    // RFC 9112 5.2: a user agent replaces an obs-fold with SP; a server refuses it (the corpus).
    auto parser = HeadParser { MessageRole::Response, HttpLimits {} };
    auto const fed = parser.feed("HTTP/1.1 200 OK\r\nX-A: a\r\n \t b\r\nX-B: c\r\n\r\n");
    REQUIRE(fed.has_value());
    REQUIRE(fed->progress == Progress::Complete);
    auto const head = parser.takeResponse();
    CHECK(head.fields.get("X-A") == std::vector<std::string_view> { "a b" });
    CHECK(head.fields.get("X-B") == std::vector<std::string_view> { "c" });
}

TEST_CASE("HeadParser.startLines", "[http]")
{
    struct Row
    {
        std::string_view wire;
        std::optional<HttpErrorCode> error;
    };
    auto const responses = std::vector<Row> {
        { .wire = "HTTP/1.1 200\r\n\r\n", .error = std::nullopt },
        { .wire = "HTTP/1.2 200 OK\r\n\r\n", .error = std::nullopt },
        { .wire = "HTTP/1.1 99 Low\r\n\r\n", .error = HttpErrorCode::ProtocolError },
        { .wire = "HTTP/1.1 099 Low\r\n\r\n", .error = HttpErrorCode::ProtocolError },
        { .wire = "HTTP/1.1 600 High\r\n\r\n", .error = HttpErrorCode::ProtocolError },
        { .wire = "HTTP/1.1 2000 Long\r\n\r\n", .error = HttpErrorCode::ProtocolError },
        { .wire = "HTTP/1.1  200 OK\r\n\r\n", .error = HttpErrorCode::ProtocolError },
        { .wire = "HTTP/2 200 OK\r\n\r\n", .error = HttpErrorCode::ProtocolError },
        { .wire = "http/1.1 200 OK\r\n\r\n", .error = HttpErrorCode::ProtocolError },
        { .wire = "HTTP/1.1 200 O\x01K\r\n\r\n", .error = HttpErrorCode::ProtocolError },
    };
    for (auto const& row: responses)
    {
        INFO("response " << row.wire);
        auto parser = HeadParser { MessageRole::Response, HttpLimits {} };
        auto const fed = parser.feed(row.wire);
        if (row.error)
        {
            REQUIRE_FALSE(fed.has_value());
            CHECK(fed.error().code == *row.error);
        }
        else
        {
            REQUIRE(fed.has_value());
            CHECK(fed->progress == Progress::Complete);
            CHECK(parser.takeResponse().version == HttpVersion::Http11);
        }
    }

    auto const requests = std::vector<Row> {
        { .wire = "\r\n\r\nGET / HTTP/1.1\r\nHost: a\r\n\r\n", .error = std::nullopt },
        { .wire = "G(T / HTTP/1.1\r\nHost: a\r\n\r\n", .error = HttpErrorCode::RequestMethodInvalid },
        { .wire = "GET /a#b HTTP/1.1\r\nHost: a\r\n\r\n", .error = HttpErrorCode::RequestUriInvalid },
        { .wire = "GET  / HTTP/1.1\r\nHost: a\r\n\r\n", .error = HttpErrorCode::ProtocolError },
        { .wire = "GET / HTTP/1.1 \r\nHost: a\r\n\r\n", .error = HttpErrorCode::ProtocolError },
        { .wire = "GET /\r\nHost: a\r\n\r\n", .error = HttpErrorCode::ProtocolError },
        { .wire = "GET / HTTP/1.1\r\n: a\r\n\r\n", .error = HttpErrorCode::ProtocolError },
        { .wire = "GET / HTTP/1.1\r\nHost a\r\n\r\n", .error = HttpErrorCode::ProtocolError },
    };
    for (auto const& row: requests)
    {
        INFO("request " << row.wire);
        auto parser = HeadParser { MessageRole::Request, HttpLimits {} };
        auto const fed = parser.feed(row.wire);
        if (row.error)
        {
            REQUIRE_FALSE(fed.has_value());
            CHECK(fed.error().code == *row.error);
        }
        else
        {
            REQUIRE(fed.has_value());
            CHECK(fed->progress == Progress::Complete);
            auto const head = parser.takeRequest();
            CHECK(head.method.text() == "GET");
            CHECK(head.target.text() == "/");
        }
    }
}

TEST_CASE("HeadParser.boundsWhatItHolds", "[http]")
{
    // A peer that never ends a line is refused once the line passes maxFieldBytes, not after
    // whatever it chose to send: the parser holds at most the limit.
    auto const limits = HttpLimits { .maxHeadBytes = std::size_t { 64 } * 1024,
                                     .maxFieldBytes = 1024,
                                     .maxFields = 100,
                                     .maxTrailerBytes = 1024,
                                     .maxChunkLineBytes = 64,
                                     .maxReadChunk = 4096,
                                     .maxBodyBytes = std::nullopt };
    for (auto const role: { MessageRole::Request, MessageRole::Response })
    {
        auto parser = HeadParser { role, limits };
        auto const start = role == MessageRole::Request
                               ? std::string_view { "GET / HTTP/1.1\r\nX-Endless: " }
                               : std::string_view { "HTTP/1.1 200 OK\r\nX-Endless: " };
        REQUIRE(parser.feed(start).has_value());
        auto const block = std::string(256, 'x');
        auto fedBytes = start.size();
        auto refusal = std::optional<core::http::HttpError> {};
        for ([[maybe_unused]] auto const round: std::views::iota(0, 1 << 12))
        {
            auto const fed = parser.feed(block);
            if (!fed.has_value())
            {
                refusal = fed.error();
                break;
            }
            CHECK(fed->consumed == block.size());
            fedBytes += fed->consumed;
        }
        REQUIRE(refusal.has_value());
        CHECK(refusal->code
              == (role == MessageRole::Request ? HttpErrorCode::RequestHeaderSize
                                               : HttpErrorCode::ResponseHeaderSize));
        CHECK(refusal->fieldName == std::optional<std::string> { "X-Endless" });
        CHECK(fedBytes <= limits.maxFieldBytes + start.size());

        // A refused parser stays refused.
        auto const after = parser.feed("\r\n\r\n");
        REQUIRE_FALSE(after.has_value());
        CHECK(after.error().code == refusal->code);
    }
}

TEST_CASE("HeadParser.headSectionSize", "[http]")
{
    auto const limits = HttpLimits { .maxHeadBytes = 200,
                                     .maxFieldBytes = 100,
                                     .maxFields = 100,
                                     .maxTrailerBytes = 100,
                                     .maxChunkLineBytes = 64,
                                     .maxReadChunk = 4096,
                                     .maxBodyBytes = std::nullopt };
    for (auto const role: { MessageRole::Request, MessageRole::Response })
    {
        auto parser = HeadParser { role, limits };
        auto wire = std::string { role == MessageRole::Request ? "GET / HTTP/1.1\r\nHost: a\r\n"
                                                               : "HTTP/1.1 200 OK\r\n" };
        for (auto const index: std::views::iota(0, 10))
            wire += "X-" + std::to_string(index) + ": " + std::string(40, 'v') + "\r\n";
        wire += "\r\n";
        auto const fed = parser.feed(wire);
        REQUIRE_FALSE(fed.has_value());
        CHECK(fed.error().code
              == (role == MessageRole::Request ? HttpErrorCode::RequestHeaderSectionSize
                                               : HttpErrorCode::ResponseHeaderSectionSize));
        REQUIRE(fed.error().size.has_value());
        CHECK(*fed.error().size > limits.maxHeadBytes);
    }
}

TEST_CASE("ChunkedDecoder.oneByteFeedsYieldDataAndTrailers", "[http]")
{
    auto const wire = std::string_view {
        "5;a=b\r\nhello\r\n1A\r\nabcdefghijklmnopqrstuvwxyz\r\n0\r\nX-T: 1\r\nX-U: two\r\n\r\nNEXT"
    };
    for (auto const feeding: { Feeding::Whole, Feeding::OneByte })
    {
        auto decoder = ChunkedDecoder { MessageRole::Response, HttpLimits {} };
        auto rest = wire;
        auto body = std::string {};
        auto progress = Progress::NeedMore;
        while (progress == Progress::NeedMore)
        {
            REQUIRE_FALSE(rest.empty());
            auto const step = decoder.feed(slice(rest, feeding));
            REQUIRE(step.has_value());
            CHECK(step->data.size() <= step->consumed);
            body += step->data;
            rest.remove_prefix(step->consumed);
            progress = step->progress;
        }
        CHECK(body == "helloabcdefghijklmnopqrstuvwxyz");
        CHECK(rest == "NEXT");
        auto const trailers = decoder.takeTrailers();
        CHECK(trailers.get("x-t") == std::vector<std::string_view> { "1" });
        CHECK(trailers.get("X-U") == std::vector<std::string_view> { "two" });

        auto const again = decoder.feed(rest);
        REQUIRE(again.has_value());
        CHECK(again->consumed == 0);
        CHECK(again->progress == Progress::Complete);
    }
}

TEST_CASE("ChunkedDecoder.dataIsClampedToMaxReadChunk", "[http]")
{
    auto limits = HttpLimits {};
    limits.maxReadChunk = 4;
    auto decoder = ChunkedDecoder { MessageRole::Request, limits };
    auto rest = std::string_view { "a\r\n0123456789\r\n0\r\n\r\n" };
    auto pieces = std::vector<std::string> {};
    auto progress = Progress::NeedMore;
    while (progress == Progress::NeedMore)
    {
        auto const step = decoder.feed(rest);
        REQUIRE(step.has_value());
        CHECK(step->data.size() <= limits.maxReadChunk);
        if (!step->data.empty())
            pieces.emplace_back(step->data);
        rest.remove_prefix(step->consumed);
        progress = step->progress;
    }
    CHECK(pieces == std::vector<std::string> { "0123", "4567", "89" });
}

TEST_CASE("ChunkedDecoder.limitsPerRole", "[http]")
{
    auto const limits = HttpLimits { .maxHeadBytes = 1024,
                                     .maxFieldBytes = 32,
                                     .maxFields = 100,
                                     .maxTrailerBytes = 48,
                                     .maxChunkLineBytes = 16,
                                     .maxReadChunk = 4096,
                                     .maxBodyBytes = 8 };
    struct Row
    {
        std::string_view name;
        std::string_view wire;
        HttpErrorCode request;
        HttpErrorCode response;
    };
    auto const rows = std::vector<Row> {
        { .name = "trailer section",
          .wire = "0\r\nX-A: 0123456789\r\nX-B: 0123456789\r\nX-C: 0123456789\r\n\r\n",
          .request = HttpErrorCode::RequestTrailerSectionSize,
          .response = HttpErrorCode::ResponseTrailerSectionSize },
        { .name = "trailer field",
          .wire = "0\r\nX-Long: 01234567890123456789012345678901\r\n\r\n",
          .request = HttpErrorCode::RequestTrailerSize,
          .response = HttpErrorCode::ResponseTrailerSize },
        { .name = "body",
          .wire = "5\r\nhello\r\n5\r\nworld\r\n0\r\n\r\n",
          .request = HttpErrorCode::RequestBodySize,
          .response = HttpErrorCode::ResponseBodySize },
        { .name = "chunk line",
          .wire = "5;name=0123456789\r\nhello\r\n0\r\n\r\n",
          .request = HttpErrorCode::ProtocolError,
          .response = HttpErrorCode::ProtocolError },
        { .name = "whitespace before the first trailer field",
          .wire = "0\r\n X-A: 1\r\n\r\n",
          .request = HttpErrorCode::ProtocolError,
          .response = HttpErrorCode::ProtocolError },
    };
    for (auto const& row: rows)
    {
        for (auto const role: { MessageRole::Request, MessageRole::Response })
        {
            for (auto const feeding: { Feeding::Whole, Feeding::OneByte })
            {
                INFO("row " << row.name << (role == MessageRole::Request ? " request" : " response"));
                auto const outcome = decodeChunked(role, limits, row.wire, feeding);
                INFO(outcome.detail);
                REQUIRE(outcome.error.has_value());
                CHECK(*outcome.error == (role == MessageRole::Request ? row.request : row.response));
            }
        }
    }

    // The trailer-field row names its field and its size.
    auto decoder = ChunkedDecoder { MessageRole::Response, limits };
    auto const step = decoder.feed(rows[1].wire);
    REQUIRE_FALSE(step.has_value());
    CHECK(step.error().fieldName == std::optional<std::string> { "X-Long" });
    REQUIRE(step.error().size.has_value());
    CHECK(*step.error().size > limits.maxFieldBytes);
}

TEST_CASE("HeadParser.maxFields", "[http]")
{
    for (auto const role: { MessageRole::Request, MessageRole::Response })
    {
        auto const start = std::string { role == MessageRole::Request ? "GET / HTTP/1.1\r\nHost: a\r\n"
                                                                      : "HTTP/1.1 200 OK\r\n" };
        auto const fieldsUpTo = [&](int count) {
            auto wire = start;
            // The request's Host is one of its fields.
            auto const first = role == MessageRole::Request ? 1 : 0;
            for (auto const index: std::views::iota(first, count))
                wire += "X-" + std::to_string(index) + ": v\r\n";
            return wire + "\r\n";
        };
        INFO((role == MessageRole::Request ? "request" : "response"));

        auto atLimit = HeadParser { role, smallLimits(100) };
        auto const accepted = atLimit.feed(fieldsUpTo(100));
        REQUIRE(accepted.has_value());
        CHECK(accepted->progress == Progress::Complete);

        auto pastLimit = HeadParser { role, smallLimits(100) };
        auto const refused = pastLimit.feed(fieldsUpTo(101));
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().code
              == (role == MessageRole::Request ? HttpErrorCode::RequestHeaderSectionSize
                                               : HttpErrorCode::ResponseHeaderSectionSize));
    }
}

TEST_CASE("ChunkedDecoder.maxFields", "[http]")
{
    auto const trailersUpTo = [](int count) {
        auto wire = std::string { "0\r\n" };
        for (auto const index: std::views::iota(0, count))
            wire += "X-" + std::to_string(index) + ": v\r\n";
        return wire + "\r\n";
    };
    for (auto const role: { MessageRole::Request, MessageRole::Response })
    {
        INFO((role == MessageRole::Request ? "request" : "response"));
        auto const accepted = decodeChunked(role, smallLimits(10), trailersUpTo(10), Feeding::Whole);
        CHECK_FALSE(accepted.error.has_value());
        auto const refused = decodeChunked(role, smallLimits(10), trailersUpTo(11), Feeding::Whole);
        REQUIRE(refused.error.has_value());
        CHECK(*refused.error
              == (role == MessageRole::Request ? HttpErrorCode::RequestTrailerSectionSize
                                               : HttpErrorCode::ResponseTrailerSectionSize));
    }
}

TEST_CASE("ChunkedDecoder.trailerObsFoldByRole", "[http]")
{
    // RFC 9112 5.2, as for a head: a user agent unfolds a response's trailers, a server refuses.
    auto const wire = std::string_view { "0\r\nX-A: a\r\n \t b\r\nX-B: c\r\n\r\n" };
    auto response = ChunkedDecoder { MessageRole::Response, HttpLimits {} };
    auto const step = response.feed(wire);
    REQUIRE(step.has_value());
    REQUIRE(step->progress == Progress::Complete);
    auto const trailers = response.takeTrailers();
    CHECK(trailers.get("X-A") == std::vector<std::string_view> { "a b" });
    CHECK(trailers.get("X-B") == std::vector<std::string_view> { "c" });

    auto request = ChunkedDecoder { MessageRole::Request, HttpLimits {} };
    auto const refused = request.feed(wire);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == HttpErrorCode::ProtocolError);
}

TEST_CASE("ChunkedEncoder.framesWhatTheDecoderReads", "[http]")
{
    CHECK(core::http::chunkHeader(26) == "1a\r\n");
    CHECK(core::http::chunkHeader(1) == "1\r\n");
    CHECK(core::http::chunkHeader(0xffff) == "ffff\r\n");
    CHECK(core::http::lastChunk(Fields {}) == "0\r\n\r\n");

    auto const trailers = fieldsOf({ { "X-T", "1" }, { "X-U", "a b" } });
    CHECK(core::http::lastChunk(trailers) == "0\r\nX-T: 1\r\nX-U: a b\r\n\r\n");

    auto const payload = std::string(300, 'p');
    auto const wire =
        core::http::chunkHeader(payload.size()) + payload + "\r\n" + core::http::lastChunk(trailers);
    auto decoder = ChunkedDecoder { MessageRole::Request, HttpLimits {} };
    auto rest = std::string_view { wire };
    auto body = std::string {};
    auto progress = Progress::NeedMore;
    while (progress == Progress::NeedMore)
    {
        REQUIRE_FALSE(rest.empty());
        auto const step = decoder.feed(rest);
        REQUIRE(step.has_value());
        body += step->data;
        rest.remove_prefix(step->consumed);
        progress = step->progress;
    }
    CHECK(body == payload);
    CHECK(rest.empty());
    CHECK(decoder.takeTrailers().get("X-U") == std::vector<std::string_view> { "a b" });
}
