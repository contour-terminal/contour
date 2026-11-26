// SPDX-License-Identifier: Apache-2.0
//
// The rewrite that holds WAMR to the runtime's table cap: which tables it caps, what it leaves alone, and the
// bytes it refuses.
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
#include <vtextension/wasm/wamr/TableCaps.hpp>

using vtextension::wasm::binary::BinaryError;
using vtextension::wasm::binary::tableDeclarations;
using vtextension::wasm::wamr::capTableMaxima;

namespace
{

using Bytes = std::vector<std::byte>;

/// @return @p values as bytes.
[[nodiscard]] Bytes raw(std::initializer_list<uint8_t> values)
{
    return std::ranges::to<Bytes>(
        values | std::views::transform([](uint8_t value) { return static_cast<std::byte>(value); }));
}

/// @return The module header followed by @p sections.
[[nodiscard]] Bytes module(std::initializer_list<uint8_t> sections)
{
    auto result = raw({ 0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00 });
    for (auto const value: sections)
        result.push_back(static_cast<std::byte>(value));
    return result;
}

/// The cap every case is rewritten under: 1024, two bytes as LEB128.
constexpr auto Cap = uint32_t { 1024 };

/// The most tables a case's module is read with: two, so that the rewrite of each is seen.
constexpr auto MaxTablesRead = uint32_t { 2 };

} // namespace

TEST_CASE("TableCaps.capTableMaxima", "[vtextension]")
{
    struct Case
    {
        std::string_view description;
        Bytes input;
        std::expected<Bytes, BinaryError> output;
    };
    auto const cases = std::vector<Case> {
        { .description = "no table: unchanged",
          .input = module({ 0x05, 0x03, 0x01, 0x00, 0x01 }),
          .output = module({ 0x05, 0x03, 0x01, 0x00, 0x01 }) },
        { .description = "a maximum within the cap: unchanged",
          .input = module({ 0x04, 0x05, 0x01, 0x70, 0x01, 0x02, 0x08 }),
          .output = module({ 0x04, 0x05, 0x01, 0x70, 0x01, 0x02, 0x08 }) },
        { .description = "no maximum: the cap is declared, and the section grows",
          .input = module({ 0x04, 0x04, 0x01, 0x70, 0x00, 0x02 }),
          .output = module({ 0x04, 0x06, 0x01, 0x70, 0x01, 0x02, 0x80, 0x08 }) },
        { .description = "a maximum above the cap is lowered to it",
          .input = module({ 0x04, 0x07, 0x01, 0x6f, 0x01, 0x02, 0x80, 0x80, 0x04 }),
          .output = module({ 0x04, 0x06, 0x01, 0x6f, 0x01, 0x02, 0x80, 0x08 }) },
        { .description = "an initial size above the cap is left for instantiation to refuse",
          .input = module({ 0x04, 0x05, 0x01, 0x70, 0x00, 0x81, 0x08 }),
          .output = module({ 0x04, 0x05, 0x01, 0x70, 0x00, 0x81, 0x08 }) },
        { .description = "the sections around the table section are copied as they were",
          .input = module({ 0x01,
                            0x04,
                            0x01,
                            0x60,
                            0x00,
                            0x00,
                            0x04,
                            0x04,
                            0x01,
                            0x70,
                            0x00,
                            0x01,
                            0x05,
                            0x03,
                            0x01,
                            0x00,
                            0x01 }),
          .output = module({ 0x01,
                             0x04,
                             0x01,
                             0x60,
                             0x00,
                             0x00,
                             0x04,
                             0x06,
                             0x01,
                             0x70,
                             0x01,
                             0x01,
                             0x80,
                             0x08,
                             0x05,
                             0x03,
                             0x01,
                             0x00,
                             0x01 }) },
        { .description = "of two tables, only the one that needs it changes",
          .input = module({ 0x04, 0x08, 0x02, 0x70, 0x01, 0x01, 0x02, 0x70, 0x00, 0x03 }),
          .output = module({ 0x04, 0x0a, 0x02, 0x70, 0x01, 0x01, 0x02, 0x70, 0x01, 0x03, 0x80, 0x08 }) },
        { .description = "bytes after the last table",
          .input = module({ 0x04, 0x05, 0x01, 0x70, 0x00, 0x02, 0xff }),
          .output = std::unexpected(BinaryError::Malformed) },
        { .description = "more tables than are read",
          .input = module({ 0x04, 0x0a, 0x03, 0x70, 0x00, 0x01, 0x70, 0x00, 0x01, 0x70, 0x00, 0x01 }),
          .output = std::unexpected(BinaryError::TooMany) },
        { .description = "a second table section",
          .input = module({ 0x04, 0x04, 0x01, 0x70, 0x00, 0x01, 0x04, 0x04, 0x01, 0x70, 0x00, 0x01 }),
          .output = std::unexpected(BinaryError::Malformed) },
        { .description = "a typed reference table",
          .input = module({ 0x04, 0x05, 0x01, 0x64, 0x70, 0x00, 0x01 }),
          .output = std::unexpected(BinaryError::Malformed) },
        { .description = "a 64-bit table",
          .input = module({ 0x04, 0x04, 0x01, 0x70, 0x04, 0x01 }),
          .output = std::unexpected(BinaryError::Malformed) },
        { .description = "a table cut short",
          .input = module({ 0x04, 0x03, 0x01, 0x70, 0x01 }),
          .output = std::unexpected(BinaryError::Truncated) },
        { .description = "no module",
          .input = raw({ 0x00 }),
          .output = std::unexpected(BinaryError::NotAModule) },
    };
    for (auto const& [description, input, output]: cases)
    {
        INFO(description);
        // As the backend does: the tables are read once, and the rewrite is handed what was read.
        auto const rewritten = tableDeclarations(input, MaxTablesRead).and_then([&](auto const& tables) {
            return capTableMaxima(input, tables, Cap);
        });
        CHECK(rewritten == output);
    }
}
