// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the OSC 533 screenshot extension's wire format: how a request is decoded and
// defaulted against the page, and how a reply is framed. Both halves are grid-free by design, so
// every rule here is checked without a Screen, a Terminal or a PTY in the way.

#include <vtbackend/vt/Screenshot.hpp>

#include <crispy/Base64.hpp>

#include <catch2/catch_test_macros.hpp>

#include <ranges>
#include <string>
#include <string_view>
#include <vector>

using namespace std::string_view_literals;
using namespace vtbackend;
using namespace vtbackend::screenshot;

namespace
{
constexpr auto TestPage = PageSize { .lines = LineCount(24), .columns = ColumnCount(80) };

/// @param message One complete reply message.
/// @return Its base64 payload, without the header or the terminator.
[[nodiscard]] std::string_view payloadOf(std::string_view message)
{
    auto const start = message.rfind(';') + 1;
    return message.substr(start, message.size() - start - 2 /* ESC \\ */);
}

/// A grid-format capture: bytes, and no pixel extent, because cells have none.
[[nodiscard]] Capture textCapture(std::string_view content)
{
    return Capture { .content = std::string { content }, .pixelSize = {} };
}

/// Collects the reply messages a writer emits.
[[nodiscard]] std::vector<std::string> collect(auto&& writer)
{
    auto messages = std::vector<std::string> {};
    writer([&](std::string_view message) { messages.emplace_back(message); });
    return messages;
}
} // namespace

// {{{ request parsing

TEST_CASE("Screenshot.parse.empty_payload_is_the_whole_page", "[screenshot]")
{
    // `OSC 533 ST` -- no parameters at all. The region is the main page, the format is plain text.
    auto const request = parseRequest(""sv, TestPage);
    REQUIRE(request.has_value());
    CHECK(request->id == 0);
    CHECK(*request->area.top == 0);
    CHECK(*request->area.left == 0);
    CHECK(*request->area.bottom == 23);
    CHECK(*request->area.right == 79);
    CHECK(request->format == Format::PlainText);
}

TEST_CASE("Screenshot.parse.explicit_region_is_one_based_inclusive", "[screenshot]")
{
    // Rows 1..5 and columns 1..20 name 5 rows of 20 cells, as zero-based offsets 0..4 and 0..19.
    auto const request = parseRequest("7;1;1;5;20;1"sv, TestPage);
    REQUIRE(request.has_value());
    CHECK(request->id == 7);
    CHECK(*request->area.top == 0);
    CHECK(*request->area.left == 0);
    CHECK(*request->area.bottom == 4);
    CHECK(*request->area.right == 19);
    CHECK(request->format == Format::VTSequences);
}

TEST_CASE("Screenshot.parse.omitted_parameters_take_their_default", "[screenshot]")
{
    // Only the top row is named; everything after it defaults.
    auto const request = parseRequest(";3"sv, TestPage);
    REQUIRE(request.has_value());
    CHECK(request->id == 0);
    CHECK(*request->area.top == 2);
    CHECK(*request->area.left == 0);
    CHECK(*request->area.bottom == 23);
    CHECK(*request->area.right == 79);
}

TEST_CASE("Screenshot.parse.empty_parameters_take_their_default", "[screenshot]")
{
    // Given but empty is the same as not given -- including in the middle of the list.
    auto const request = parseRequest("4;;;10;40;"sv, TestPage);
    REQUIRE(request.has_value());
    CHECK(request->id == 4);
    CHECK(*request->area.top == 0);
    CHECK(*request->area.left == 0);
    CHECK(*request->area.bottom == 9);
    CHECK(*request->area.right == 39);
    CHECK(request->format == Format::PlainText);
}

TEST_CASE("Screenshot.parse.zero_coordinate_takes_its_default", "[screenshot]")
{
    // Zero is not a row: like every other rectangular-area sequence, it folds onto the default.
    auto const request = parseRequest("0;0;0;0;0"sv, TestPage);
    REQUIRE(request.has_value());
    CHECK(*request->area.top == 0);
    CHECK(*request->area.left == 0);
    CHECK(*request->area.bottom == 23);
    CHECK(*request->area.right == 79);
}

TEST_CASE("Screenshot.parse.region_is_clamped_to_the_page", "[screenshot]")
{
    auto const request = parseRequest("0;1;1;999;999"sv, TestPage);
    REQUIRE(request.has_value());
    CHECK(*request->area.bottom == 23);
    CHECK(*request->area.right == 79);
}

TEST_CASE("Screenshot.parse.oversized_coordinate_saturates_rather_than_failing", "[screenshot]")
{
    // A number too large for the type still means "the edge", not "malformed".
    auto const request = parseRequest("0;1;1;99999999999999999999;99999999999999999999"sv, TestPage);
    REQUIRE(request.has_value());
    CHECK(*request->area.bottom == 23);
    CHECK(*request->area.right == 79);
}

TEST_CASE("Screenshot.parse.inverted_region_is_rejected", "[screenshot]")
{
    SECTION("rows")
    {
        auto const request = parseRequest("9;10;1;5;20"sv, TestPage);
        REQUIRE(!request.has_value());
        CHECK(request.error().status == Status::EmptyRegion);
        // The id survives the rejection, so the refusal can be matched to what it refuses.
        CHECK(request.error().id == 9);
    }
    SECTION("columns")
    {
        auto const request = parseRequest("9;1;40;5;20"sv, TestPage);
        REQUIRE(!request.has_value());
        CHECK(request.error().status == Status::EmptyRegion);
        CHECK(request.error().id == 9);
    }
}

TEST_CASE("Screenshot.parse.malformed_parameter_is_rejected", "[screenshot]")
{
    SECTION("not a number")
    {
        auto const request = parseRequest("0;1;1;abc;20"sv, TestPage);
        REQUIRE(!request.has_value());
        CHECK(request.error().status == Status::Malformed);
    }
    SECTION("trailing garbage after a number")
    {
        auto const request = parseRequest("0;1;1;5x;20"sv, TestPage);
        REQUIRE(!request.has_value());
        CHECK(request.error().status == Status::Malformed);
    }
    SECTION("negative")
    {
        auto const request = parseRequest("0;-1;1;5;20"sv, TestPage);
        REQUIRE(!request.has_value());
        CHECK(request.error().status == Status::Malformed);
    }
    SECTION("more parameters than the grammar has")
    {
        auto const request = parseRequest("13;1;1;5;20;0;99"sv, TestPage);
        REQUIRE(!request.has_value());
        CHECK(request.error().status == Status::Malformed);
        // One parameter too many still names itself, and every reply echoes Pid back.
        CHECK(request.error().id == 13);
    }
    SECTION("an unreadable id leaves nothing to correlate against")
    {
        auto const request = parseRequest("zz;1;1;5;20"sv, TestPage);
        REQUIRE(!request.has_value());
        CHECK(request.error().status == Status::Malformed);
        CHECK(request.error().id == 0);
    }
}

TEST_CASE("Screenshot.parse.reserved_and_unknown_formats_are_refused", "[screenshot]")
{
    // Sixel has a number but no encoder behind it, and 5 upwards name nothing at all. An application
    // asking for one is told so rather than met with silence.
    for (auto const format: { "2"sv, "5"sv, "255"sv })
    {
        auto const request = parseRequest(std::string("11;1;1;5;20;") + std::string(format), TestPage);
        REQUIRE(!request.has_value());
        CHECK(request.error().status == Status::UnsupportedFormat);
        CHECK(request.error().id == 11);
    }
}

TEST_CASE("Screenshot.parse.renderer_formats_are_accepted_here", "[screenshot]")
{
    // Whether pixels can actually be produced is a question about the SESSION, answered later by the
    // frontend; the grammar accepts the request either way. @see Status::Unavailable.
    for (auto const [text, format]: { std::pair { "3"sv, Format::Png }, std::pair { "4"sv, Format::Rgba } })
    {
        auto const request = parseRequest(std::string("11;1;1;5;20;") + std::string(text), TestPage);
        REQUIRE(request.has_value());
        CHECK(request->format == format);
    }
}

// }}}
// {{{ format table

TEST_CASE("Screenshot.formats.table_names_every_enumerator", "[screenshot]")
{
    CHECK(isSupported(Format::PlainText));
    CHECK(isSupported(Format::VTSequences));
    CHECK(!isSupported(Format::Sixel));
    CHECK(isSupported(Format::Png));
    CHECK(isSupported(Format::Rgba));
    CHECK(formatInfo(static_cast<Format>(200)) == nullptr);
}

TEST_CASE("Screenshot.formats.the_table_says_who_produces_each", "[screenshot]")
{
    // This column is what routes a request: Terminal::answerScreenshot() reads it rather than
    // switching on the format itself, so a format added to the table is routed without touching it.
    CHECK(producerOf(Format::PlainText) == Producer::Grid);
    CHECK(producerOf(Format::VTSequences) == Producer::Grid);
    CHECK(producerOf(Format::Sixel) == Producer::Renderer);
    CHECK(producerOf(Format::Png) == Producer::Renderer);
    CHECK(producerOf(Format::Rgba) == Producer::Renderer);
}

// }}}
// {{{ reply framing

TEST_CASE("Screenshot.reply.is_a_PM_and_never_an_OSC", "[screenshot]")
{
    // The reply must not be re-readable as a request. PM (ESC ^) is a different string type than the
    // OSC (ESC ]) that asked, so a captured reply replayed into a terminal cannot start a new
    // screenshot.
    auto const request =
        Request { .id = 3,
                  .area = Rect { .top = Top(0), .left = Left(0), .bottom = Bottom(1), .right = Right(4) },
                  .format = Format::PlainText };
    auto const messages = collect([&](auto const& sink) { writeReply(request, textCapture("hi\n"), sink); });

    REQUIRE(messages.size() == 2);
    CHECK(messages[0].starts_with("\033^533;3;1;"));
    CHECK(!messages[0].starts_with("\033]"));
    CHECK(messages[0].ends_with("\033\\"));
}

TEST_CASE("Screenshot.reply.echoes_the_resolved_region_one_based", "[screenshot]")
{
    auto const request =
        Request { .id = 12,
                  .area = Rect { .top = Top(0), .left = Left(0), .bottom = Bottom(4), .right = Right(19) },
                  .format = Format::VTSequences };
    auto const messages = collect([&](auto const& sink) { writeReply(request, textCapture("x"), sink); });

    REQUIRE(messages.size() == 2);
    // Pid=12, Ps=1 (Data), the region back in the units the request used, Pf=1, and a zero pixel
    // extent -- a grid format has none.
    CHECK(messages[0].starts_with("\033^533;12;1;1;1;5;20;1;0;0;"));
}

TEST_CASE("Screenshot.reply.echoes_the_pixel_extent_of_a_renderer_format", "[screenshot]")
{
    // Raw RGBA is a flat run of bytes and says nothing about its own shape, so the reply must. Kitty's
    // graphics protocol requires the same of raw pixels, for the same reason.
    auto const request =
        Request { .id = 1,
                  .area = Rect { .top = Top(0), .left = Left(0), .bottom = Bottom(1), .right = Right(3) },
                  .format = Format::Rgba };
    auto const capture =
        Capture { .content = std::string(4 * 8 * 8, '\0'), .pixelSize = ImageSize { Width(8), Height(8) } };
    auto const messages = collect([&](auto const& sink) { writeReply(request, capture, sink); });

    REQUIRE(messages.size() == 2);
    CHECK(messages[0].starts_with("\033^533;1;1;1;1;2;4;4;8;8;"));
}

TEST_CASE("Screenshot.reply.payload_is_base64_and_round_trips", "[screenshot]")
{
    auto const content = "line one\nline two\n"sv;
    auto const request =
        Request { .id = 0,
                  .area = Rect { .top = Top(0), .left = Left(0), .bottom = Bottom(1), .right = Right(7) },
                  .format = Format::PlainText };
    auto const messages = collect([&](auto const& sink) { writeReply(request, textCapture(content), sink); });

    REQUIRE(messages.size() == 2);
    auto const body = std::string_view { messages[0] };
    auto const start = body.rfind(';') + 1;
    auto const encoded = body.substr(start, body.size() - start - 2 /* ESC \ */);
    CHECK(crispy::base64::decode(encoded) == content);
}

TEST_CASE("Screenshot.reply.control_characters_never_reach_the_wire", "[screenshot]")
{
    // The whole reason the payload is encoded: screen content, and the VT-sequence format in
    // particular, carries ESC. A raw ESC \ would end the reply early and leave the rest of the
    // screenshot to be read as input by whatever asked for it.
    auto const content = "\033[1mbold\033\\ and \007 bell\033"sv;
    auto const request =
        Request { .id = 1,
                  .area = Rect { .top = Top(0), .left = Left(0), .bottom = Bottom(0), .right = Right(9) },
                  .format = Format::VTSequences };
    auto const messages = collect([&](auto const& sink) { writeReply(request, textCapture(content), sink); });

    REQUIRE(messages.size() == 2);
    // The only ESC in the message is the PM introducer and the final ST.
    auto const body = std::string_view { messages[0] };
    auto const payload = body.substr(0, body.size() - 2);
    CHECK(payload.find('\033', 1) == std::string_view::npos);
    CHECK(payload.find('\007') == std::string_view::npos);
}

TEST_CASE("Screenshot.reply.ends_with_an_end_of_data_message", "[screenshot]")
{
    auto const request =
        Request { .id = 5,
                  .area = Rect { .top = Top(0), .left = Left(0), .bottom = Bottom(0), .right = Right(0) },
                  .format = Format::PlainText };

    SECTION("with content")
    {
        auto const messages = collect([&](auto const& sink) { writeReply(request, textCapture("a"), sink); });
        REQUIRE(messages.size() == 2);
        CHECK(messages[1] == "\033^533;5;0\033\\");
    }
    SECTION("an empty screenshot is still terminated")
    {
        auto const messages = collect([&](auto const& sink) { writeReply(request, textCapture(""), sink); });
        REQUIRE(messages.size() == 1);
        CHECK(messages[0] == "\033^533;5;0\033\\");
    }
}

TEST_CASE("Screenshot.reply.splits_content_into_chunks", "[screenshot]")
{
    auto const content = std::string(MaxChunkSize * 2 + 10, 'x');
    auto const request =
        Request { .id = 0,
                  .area = Rect { .top = Top(0), .left = Left(0), .bottom = Bottom(0), .right = Right(0) },
                  .format = Format::PlainText };
    auto const messages = collect([&](auto const& sink) { writeReply(request, textCapture(content), sink); });

    // Three data messages plus the terminator.
    REQUIRE(messages.size() == 4);
    CHECK(messages[3] == "\033^533;0;0\033\\");

    // Concatenating the decoded chunks reproduces the content exactly.
    auto rebuilt = std::string {};
    for (auto const& message: messages | std::views::take(3))
        rebuilt += crispy::base64::decode(payloadOf(message));
    CHECK(rebuilt == content);
}

TEST_CASE("Screenshot.reply.chunks_reassemble_either_way", "[screenshot]")
{
    // The chunk size is a multiple of three, so only the final chunk can carry base64 padding. Both
    // ways of putting the reply back together therefore agree: decode each chunk and concatenate, or
    // concatenate the encoded chunks and decode once.
    CHECK(MaxChunkSize % 3 == 0);

    auto content = std::string {};
    for (auto const i: std::views::iota(0u, MaxChunkSize * 2 + 7))
        content += static_cast<char>('a' + (i % 26));

    auto const request =
        Request { .id = 0,
                  .area = Rect { .top = Top(0), .left = Left(0), .bottom = Bottom(0), .right = Right(0) },
                  .format = Format::PlainText };
    auto const messages = collect([&](auto const& sink) { writeReply(request, textCapture(content), sink); });
    REQUIRE(messages.size() == 4);

    auto perChunk = std::string {};
    auto concatenated = std::string {};
    for (auto const& message: messages | std::views::take(3))
    {
        perChunk += crispy::base64::decode(payloadOf(message));
        concatenated += payloadOf(message);
    }
    CHECK(perChunk == content);
    CHECK(crispy::base64::decode(concatenated) == content);
}

TEST_CASE("Screenshot.reply.error_is_a_single_message_naming_the_request", "[screenshot]")
{
    auto const messages = collect([&](auto const& sink) { writeError(77, Status::Denied, sink); });
    REQUIRE(messages.size() == 1);
    CHECK(messages[0] == "\033^533;77;2\033\\");
}

// }}}
