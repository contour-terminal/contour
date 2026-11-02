// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/state/StateFields.hpp>
#include <vtbackend/state/TerminalStateCodec.hpp>
#include <vtbackend/state/golden/GoldenSession.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <vector>

using namespace vtbackend;

namespace
{
/// @return The golden file of @p version with @p extension, or nullopt when it cannot be read.
std::optional<std::string> readGolden(uint32_t version, std::string_view extension)
{
    auto const path = std::filesystem::path { VTBACKEND_GOLDEN_DIR }
                      / std::format("terminal-state-v{}.{}", version, extension);
    auto file = std::ifstream { path, std::ios::binary };
    if (!file)
        return std::nullopt;
    return std::string { std::istreambuf_iterator<char> { file }, std::istreambuf_iterator<char> {} };
}

/// @return The golden stream of @p version.
std::vector<uint8_t> goldenBytes(uint32_t version)
{
    auto const text = readGolden(version, "bin");
    REQUIRE(text.has_value());
    return std::vector<uint8_t> { text->begin(), text->end() };
}
} // namespace

TEST_CASE("format v1 golden decodes and restores to the recorded screen", "[StateGolden]")
{
    auto const decoded = decodeTerminalState(goldenBytes(1));
    REQUIRE(decoded.has_value());

    auto const setup = test::goldenSetup();
    auto const settings = settingsFor(*decoded, setup.settings());
    REQUIRE(settings.has_value());
    auto target = setup.makeTerminal(*settings);
    REQUIRE(target->terminal.restoreState(*decoded).has_value());

    // The recorded session scrolled into the history, drew two images and left a CSI open.
    CHECK(unbox<int>(target->terminal.primaryScreen().grid().historyLineCount()) > 0);
    CHECK_FALSE(decoded->images.images.empty());
    CHECK(decoded->inFlightTail == std::vector<uint8_t> { 0x1B, '[', '3' });
    CHECK(target->terminal.windowTitle() == "golden session");

    // Text and behaviour: every probe a program can ask is answered as the recording terminal answered it.
    auto const expected = readGolden(1, "txt");
    REQUIRE(expected.has_value());
    CHECK(test::describeTerminal(*target) == *expected);
}

TEST_CASE("every readable format version has a golden", "[StateGolden]")
{
    for (auto const version:
         std::views::iota(OldestReadableTerminalStateFormat, TerminalStateFormatVersion + 1))
    {
        DYNAMIC_SECTION("format v" << version)
        {
            auto const bytes = readGolden(version, "bin");
            REQUIRE(bytes.has_value());
            CHECK(readGolden(version, "txt").has_value());
            auto const decoded = decodeTerminalState(std::vector<uint8_t> { bytes->begin(), bytes->end() });
            CHECK(decoded.has_value());
        }
    }
}

TEST_CASE("the encoder still writes the format v1 golden byte for byte", "[StateGolden]")
{
    // The current format is the one the checked-in stream was written in; a change of its bytes is a new
    // format version, with a new golden, not an edit of this one.
    STATIC_REQUIRE(TerminalStateFormatVersion == 1);
    auto const mock = test::goldenSetup().makeTerminal();
    mock->writeToScreen(test::goldenSession());
    mock->terminal.flushInput();
    CHECK(encodeTerminalState(mock->terminal.captureState()) == goldenBytes(1));
}

namespace
{
/// The top-level fields of a stream, as key and the bytes of the field.
struct TopLevelField
{
    uint64_t key = 0;
    std::vector<uint8_t> bytes; ///< The whole field: its key and its value.
};

/// @return The fields of the state frame of @p stream.
std::vector<TopLevelField> topLevelFields(std::span<uint8_t const> stream)
{
    auto reader = StateReader::open(stream);
    REQUIRE(reader.has_value());
    auto const frame = reader->bytes();
    REQUIRE(frame.has_value());
    auto in = StateReader::ofPayload(*frame);
    auto fields = std::vector<TopLevelField> {};
    while (!in.atEnd())
    {
        auto const before = in.remaining();
        auto const key = in.varint();
        REQUIRE(key.has_value());
        if ((*key & 1U) == 0)
            REQUIRE(in.varint().has_value());
        else
            REQUIRE(in.bytes().has_value());
        auto const length = before - in.remaining();
        auto const start = frame->size() - before;
        fields.push_back({ .key = *key,
                           .bytes = { frame->begin() + static_cast<ptrdiff_t>(start),
                                      frame->begin() + static_cast<ptrdiff_t>(start + length) } });
    }
    return fields;
}

/// @return A stream of @p fields.
std::vector<uint8_t> streamOf(std::span<TopLevelField const> fields)
{
    auto writer = StateWriter {};
    auto const frame = writer.beginFrame();
    for (auto const& field: fields)
        writer.raw(field.bytes);
    writer.endFrame(frame);
    return std::move(writer).take();
}

/// The id of the area of TerminalState holding the OSC 99 notifications.
constexpr auto NotificationsFieldId = uint32_t { 23 };
} // namespace

TEST_CASE("a terminal stream from a newer writer, with areas and fields this build lacks, restores",
          "[StateGolden]")
{
    auto fields = topLevelFields(goldenBytes(1));
    // A newer release added an area of its own, and a field to an area this build knows.
    auto future = StateWriter::payload();
    future.varint(detail::fieldKey(4000, detail::WireKind::Framed));
    future.string("an area of a release yet to come");
    future.varint(detail::fieldKey(4001, detail::WireKind::Varint));
    future.varint(12345);
    fields.push_back({ .key = 0, .bytes = std::move(future).take() });

    auto const decoded = decodeTerminalState(streamOf(fields));
    REQUIRE(decoded.has_value());
    auto const setup = test::goldenSetup();
    auto target = setup.makeTerminal(*settingsFor(*decoded, setup.settings()));
    REQUIRE(target->terminal.restoreState(*decoded).has_value());
    CHECK(target->terminal.windowTitle() == "golden session");
}

TEST_CASE("a terminal stream from an older writer, without an area added since, restores with its default",
          "[StateGolden]")
{
    auto source = test::goldenSetup().makeTerminal();
    source->writeToScreen(test::goldenSession());
    source->terminal.desktopNotificationManager().addActiveNotification("alive");
    auto fields = topLevelFields(encodeTerminalState(source->terminal.captureState()));
    // The release before this one had no notifications area.
    auto const before = fields.size();
    std::erase_if(fields,
                  [](TopLevelField const& field) { return (field.key >> 1U) == NotificationsFieldId; });
    REQUIRE(fields.size() == before - 1);

    auto const decoded = decodeTerminalState(streamOf(fields));
    REQUIRE(decoded.has_value());
    CHECK(decoded->notifications.active.empty());
    auto const setup = test::goldenSetup();
    auto target = setup.makeTerminal(*settingsFor(*decoded, setup.settings()));
    REQUIRE(target->terminal.restoreState(*decoded).has_value());
    CHECK(target->terminal.desktopNotificationManager().activeNotifications().empty());
}
