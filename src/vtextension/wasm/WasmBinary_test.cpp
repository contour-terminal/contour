// SPDX-License-Identifier: Apache-2.0
//
// The binary-format rules the host reads a module's bytes by itself: LEB128 numbers, names and sections.
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <initializer_list>
#include <ranges>
#include <string_view>
#include <vector>

#include <vtextension/wasm/WasmBinary.hpp>

using vtextension::wasm::binary::BinaryError;
using vtextension::wasm::binary::Reader;
using vtextension::wasm::binary::sections;

namespace
{

/// @return @p values as bytes.
[[nodiscard]] std::vector<std::byte> raw(std::initializer_list<uint8_t> values)
{
    return std::ranges::to<std::vector<std::byte>>(
        values | std::views::transform([](uint8_t value) { return static_cast<std::byte>(value); }));
}

} // namespace

TEST_CASE("WasmBinary.u32", "[vtextension]")
{
    struct Case
    {
        std::string_view description;
        std::vector<std::byte> bytes;
        std::expected<uint32_t, BinaryError> value;
    };
    auto const cases = std::vector<Case> {
        { .description = "one byte", .bytes = raw({ 0x0b }), .value = 11 },
        { .description = "two bytes", .bytes = raw({ 0xe5, 0x8e, 0x26 }), .value = 624485 },
        { .description = "padded to five bytes",
          .bytes = raw({ 0x8b, 0x80, 0x80, 0x80, 0x00 }),
          .value = 11 },
        { .description = "the largest",
          .bytes = raw({ 0xff, 0xff, 0xff, 0xff, 0x0f }),
          .value = 0xFFFFFFFFU },
        { .description = "beyond 32 bits",
          .bytes = raw({ 0xff, 0xff, 0xff, 0xff, 0x1f }),
          .value = std::unexpected(BinaryError::Malformed) },
        { .description = "six bytes",
          .bytes = raw({ 0x80, 0x80, 0x80, 0x80, 0x80, 0x00 }),
          .value = std::unexpected(BinaryError::Malformed) },
        { .description = "running out",
          .bytes = raw({ 0x80, 0x80 }),
          .value = std::unexpected(BinaryError::Truncated) },
        { .description = "nothing", .bytes = {}, .value = std::unexpected(BinaryError::Truncated) },
    };
    for (auto const& [description, bytes, value]: cases)
    {
        INFO(description);
        auto reader = Reader { bytes };
        CHECK(reader.u32() == value);
    }
}

TEST_CASE("WasmBinary.reader", "[vtextension]")
{
    auto const bytes = raw({ 0x02, 'h', 'i', 0x07, 0x05, 'x' });
    auto reader = Reader { bytes, 100 };
    auto const name = reader.name();
    REQUIRE(name.has_value());
    CHECK(name->size() == 2);
    CHECK(reader.position() == 103);
    CHECK(reader.byte() == uint8_t { 0x07 });
    // A name that claims more than is left takes nothing.
    CHECK(reader.name() == std::unexpected(BinaryError::Truncated));
    CHECK(reader.rest().size() == 1);
    CHECK(reader.atEnd());
    CHECK(reader.byte() == std::unexpected(BinaryError::Truncated));
}

TEST_CASE("WasmBinary.sections", "[vtextension]")
{
    auto const header = raw({ 0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00 });
    SECTION("each section's id, content and where it starts")
    {
        auto module = header;
        for (auto const& part:
             { raw({ 0x01, 0x02, 0xaa, 0xbb }), raw({ 0x00, 0x00 }), raw({ 0x07, 0x01, 0xcc }) })
            module.insert(module.end(), part.begin(), part.end());
        auto const found = sections(module);
        REQUIRE(found.has_value());
        REQUIRE(found->size() == 3);
        CHECK((*found)[0].id == 1);
        CHECK((*found)[0].offset == 10);
        CHECK((*found)[0].content.size() == 2);
        CHECK((*found)[1].content.empty());
        CHECK((*found)[2].id == 7);
        CHECK((*found)[2].offset == 16);
    }
    SECTION("a header alone has none")
    {
        CHECK(sections(header).value().empty());
    }
    SECTION("what is not a core module")
    {
        CHECK(sections({}) == std::unexpected(BinaryError::NotAModule));
        CHECK(sections(raw({ 0x00, 0x61, 0x73 })) == std::unexpected(BinaryError::NotAModule));
        // A component's layer field differs.
        CHECK(sections(raw({ 0x00, 0x61, 0x73, 0x6d, 0x0d, 0x00, 0x01, 0x00 }))
              == std::unexpected(BinaryError::NotAModule));
    }
    SECTION("a section running past the end")
    {
        auto module = header;
        for (auto const value: { 0x01, 0x10, 0x00 })
            module.push_back(static_cast<std::byte>(value));
        CHECK(sections(module) == std::unexpected(BinaryError::Truncated));
    }
}
