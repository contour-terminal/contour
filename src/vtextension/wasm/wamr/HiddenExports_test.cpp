// SPDX-License-Identifier: Apache-2.0
//
// The rewrite that hides the exports WAMR would call during instantiation: what it renames, what it leaves
// alone, and the bytes it refuses.
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <initializer_list>
#include <ranges>
#include <string_view>
#include <vector>

#include <vtextension/wasm/wamr/HiddenExports.hpp>

using namespace std::string_view_literals;
using vtextension::wasm::wamr::ExportsHidden;
using vtextension::wasm::wamr::hideExports;
using vtextension::wasm::wamr::hostExportName;
using vtextension::wasm::wamr::isInstantiationHook;
using vtextension::wasm::wamr::ModuleBytesError;
using vtextension::wasm::wamr::runtimeExportName;

namespace
{

using Bytes = std::vector<std::byte>;

/// @return @p values as bytes.
[[nodiscard]] Bytes raw(std::initializer_list<uint8_t> values)
{
    return std::ranges::to<Bytes>(
        values | std::views::transform([](uint8_t value) { return static_cast<std::byte>(value); }));
}

/// @return The characters of @p chars as bytes, without a length.
[[nodiscard]] Bytes text(std::string_view chars)
{
    return std::ranges::to<Bytes>(chars
                                  | std::views::transform([](char c) { return static_cast<std::byte>(c); }));
}

/// @return @p parts, one after another.
[[nodiscard]] Bytes concat(std::initializer_list<Bytes> parts)
{
    return std::ranges::to<Bytes>(parts | std::views::join);
}

/// @return A name as the binary format encodes it: a one-byte length, then the characters.
[[nodiscard]] Bytes name(std::string_view chars)
{
    return concat({ raw({ static_cast<uint8_t>(chars.size()) }), text(chars) });
}

/// @return A section with id @p id and @p content, its size in one byte.
[[nodiscard]] Bytes section(uint8_t id, Bytes const& content)
{
    return concat({ raw({ id, static_cast<uint8_t>(content.size()) }), content });
}

/// @return An export section exporting function 0 under each of @p names.
[[nodiscard]] Bytes exportSection(std::initializer_list<std::string_view> names)
{
    auto content = raw({ static_cast<uint8_t>(names.size()) });
    for (auto const exportName: names)
        content = concat({ content, name(exportName), raw({ 0x00, 0x00 }) });
    return section(7, content);
}

/// @return The module header: magic and version.
[[nodiscard]] Bytes header()
{
    return raw({ 0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00 });
}

/// @return A type section with `() -> ()`, and a function section with one function of it.
[[nodiscard]] Bytes oneFunction()
{
    return concat({ section(1, raw({ 0x01, 0x60, 0x00, 0x00 })), section(3, raw({ 0x01, 0x00 })) });
}

/// @return A code section with the one function's empty body.
[[nodiscard]] Bytes emptyBody()
{
    return section(10, raw({ 0x01, 0x02, 0x00, 0x0b }));
}

/// @return Whether @p haystack holds @p needle anywhere.
[[nodiscard]] bool holds(Bytes const& haystack, std::string_view needle)
{
    return !std::ranges::search(haystack, text(needle)).empty();
}

/// @return The offsets at which @p before and @p after differ; both must be as long.
[[nodiscard]] std::vector<std::size_t> differences(Bytes const& before, Bytes const& after)
{
    REQUIRE(before.size() == after.size());
    return std::ranges::to<std::vector>(
        std::views::iota(std::size_t { 0 }, before.size())
        | std::views::filter([&](std::size_t index) { return before[index] != after[index]; }));
}

} // namespace

TEST_CASE("HiddenExports.withoutInitializer", "[vtextension]")
{
    auto const original = concat({ header(), oneFunction(), exportSection({ "idle" }), emptyBody() });
    auto module = original;
    CHECK(hideExports(module) == ExportsHidden::No);
    CHECK(module == original);
}

TEST_CASE("HiddenExports.initializer", "[vtextension]")
{
    auto const original =
        concat({ header(), oneFunction(), exportSection({ "idle", "_initialize", "other" }), emptyBody() });
    auto module = original;
    CHECK(hideExports(module) == ExportsHidden::Yes);
    // Renamed in place, to a name of the same length: no size anywhere changes.
    REQUIRE(module.size() == original.size());
    CHECK_FALSE(holds(module, "_initialize"));
    CHECK(holds(module, "~initialize"));
    // Only the one character that tells the names apart changed.
    CHECK(differences(original, module).size() == 1);
}

TEST_CASE("HiddenExports.onlyTheExactName", "[vtextension]")
{
    for (auto const exportName: { "_initialize_x"sv, "_initializ"sv, "initialize"sv, "_INITIALIZE"sv })
    {
        INFO(exportName);
        auto const original = concat({ header(), oneFunction(), exportSection({ exportName }), emptyBody() });
        auto module = original;
        CHECK(hideExports(module) == ExportsHidden::No);
        CHECK(module == original);
    }
}

TEST_CASE("HiddenExports.onlyTheExportSection", "[vtextension]")
{
    // The name in a data segment, and in a custom section before and after the exports.
    auto const memory = section(5, raw({ 0x01, 0x00, 0x01 }));
    auto const data =
        section(11, concat({ raw({ 0x01, 0x00, 0x41, 0x00, 0x0b, 0x0b }), text("_initialize") }));
    auto const custom = section(0, concat({ name("name"), name("_initialize") }));

    SECTION("not exported")
    {
        auto const original = concat({ header(),
                                       custom,
                                       oneFunction(),
                                       memory,
                                       exportSection({ "idle" }),
                                       emptyBody(),
                                       data,
                                       custom });
        auto module = original;
        CHECK(hideExports(module) == ExportsHidden::No);
        CHECK(module == original);
    }
    SECTION("exported as well")
    {
        auto const original = concat({ header(),
                                       custom,
                                       oneFunction(),
                                       memory,
                                       exportSection({ "_initialize" }),
                                       emptyBody(),
                                       data,
                                       custom });
        auto module = original;
        CHECK(hideExports(module) == ExportsHidden::Yes);
        // The export is the one place renamed: the other three still read `_initialize`.
        auto const changed = differences(original, module);
        REQUIRE(changed.size() == 1);
        auto const exportsAt = static_cast<std::size_t>(
            std::ranges::search(original, exportSection({ "_initialize" })).begin() - original.begin());
        CHECK(changed.front() > exportsAt);
        CHECK(changed.front() < exportsAt + exportSection({ "_initialize" }).size());
    }
}

TEST_CASE("HiddenExports.multiByteSizes", "[vtextension]")
{
    // The export section's size padded to four LEB128 bytes, the count to two, the name's length to five:
    // all valid encodings of small numbers.
    auto const content = concat({ raw({ 0x81, 0x00 }),
                                  raw({ 0x8b, 0x80, 0x80, 0x80, 0x00 }),
                                  text("_initialize"),
                                  raw({ 0x00, 0x00 }) });
    auto const size = static_cast<uint8_t>(content.size());
    auto const original = concat({ header(),
                                   oneFunction(),
                                   raw({ 0x07, static_cast<uint8_t>(0x80U | size), 0x80, 0x80, 0x00 }),
                                   content,
                                   emptyBody() });
    auto module = original;
    CHECK(hideExports(module) == ExportsHidden::Yes);
    CHECK(differences(original, module).size() == 1);
    CHECK(holds(module, "~initialize"));
}

TEST_CASE("HiddenExports.reservedAlias", "[vtextension]")
{
    // The alias is the backend's: a module exporting it could pass for one that exports `_initialize`.
    for (auto const& names:
         { std::vector { "~initialize"sv }, std::vector { "_initialize"sv, "~initialize"sv } })
    {
        auto content = raw({ static_cast<uint8_t>(names.size()) });
        for (auto const exportName: names)
            content = concat({ content, name(exportName), raw({ 0x00, 0x00 }) });
        auto const original = concat({ header(), oneFunction(), section(7, content), emptyBody() });
        auto module = original;
        CHECK(hideExports(module) == std::unexpected(ModuleBytesError::ReservedExportName));
        CHECK(module == original);
    }
}

TEST_CASE("HiddenExports.unreadable", "[vtextension]")
{
    // Refused with the bytes unchanged -- even where an `_initialize` export was already found -- since
    // WAMR could not load them either, and a half-read module must not reach it with the export intact.
    struct Case
    {
        std::string_view description;
        Bytes module;
        ModuleBytesError error;
    };
    auto const initializerEntry = concat({ name("_initialize"), raw({ 0x00, 0x00 }) });
    auto const cases = std::vector<Case> {
        { .description = "empty", .module = {}, .error = ModuleBytesError::NotAModule },
        { .description = "a short header",
          .module = raw({ 0x00, 0x61, 0x73 }),
          .error = ModuleBytesError::NotAModule },
        { .description = "the wrong magic",
          .module = raw({ 0x00, 0x61, 0x73, 0x6e, 0x01, 0x00, 0x00, 0x00 }),
          .error = ModuleBytesError::NotAModule },
        { .description = "a component, not a core module",
          .module = raw({ 0x00, 0x61, 0x73, 0x6d, 0x0d, 0x00, 0x01, 0x00 }),
          .error = ModuleBytesError::NotAModule },
        { .description = "a section id without a size",
          .module = concat({ header(), raw({ 0x01 }) }),
          .error = ModuleBytesError::Truncated },
        { .description = "a section running past the end",
          .module = concat({ header(), raw({ 0x01, 0x10, 0x00 }) }),
          .error = ModuleBytesError::Truncated },
        { .description = "a section size of six LEB128 bytes",
          .module = concat({ header(), raw({ 0x01, 0x80, 0x80, 0x80, 0x80, 0x80, 0x00 }) }),
          .error = ModuleBytesError::Malformed },
        { .description = "a section size beyond 32 bits",
          .module = concat({ header(), raw({ 0x01, 0x80, 0x80, 0x80, 0x80, 0x10 }) }),
          .error = ModuleBytesError::Malformed },
        { .description = "an export section claiming two entries but holding one",
          .module = concat({ header(), section(7, concat({ raw({ 0x02 }), initializerEntry })) }),
          .error = ModuleBytesError::Truncated },
        { .description = "an export section with bytes after its entries",
          .module =
              concat({ header(), section(7, concat({ raw({ 0x01 }), initializerEntry, raw({ 0x00 }) })) }),
          .error = ModuleBytesError::Malformed },
        { .description = "an export name running past its section",
          .module = concat({ header(), section(7, raw({ 0x01, 0x0b, '_', 'i', 'n' })) }),
          .error = ModuleBytesError::Truncated },
        { .description = "an export without its index",
          .module =
              concat({ header(), section(7, concat({ raw({ 0x01 }), name("_initialize"), raw({ 0x00 }) })) }),
          .error = ModuleBytesError::Truncated },
        { .description = "an export count of 0xFFFFFFFF",
          .module = concat(
              { header(), section(7, concat({ raw({ 0xff, 0xff, 0xff, 0xff, 0x0f }), initializerEntry })) }),
          .error = ModuleBytesError::Truncated },
        { .description = "a second export section, even an empty one",
          .module = concat({ header(),
                             section(7, concat({ raw({ 0x01 }), initializerEntry })),
                             section(7, raw({ 0x00 })) }),
          .error = ModuleBytesError::DuplicateSection },
        { .description = "a second export section before the one with the initializer",
          .module = concat({ header(),
                             section(7, raw({ 0x00 })),
                             section(7, concat({ raw({ 0x01 }), initializerEntry })) }),
          .error = ModuleBytesError::DuplicateSection },
    };
    for (auto const& [description, original, error]: cases)
    {
        INFO(description);
        auto module = original;
        CHECK(hideExports(module) == std::unexpected(error));
        CHECK(module == original);
    }
}

TEST_CASE("HiddenExports.unusualEntries", "[vtextension]")
{
    SECTION("an export with an empty name is no initializer")
    {
        auto const original = concat({ header(), oneFunction(), exportSection({ "" }), emptyBody() });
        auto module = original;
        CHECK(hideExports(module) == ExportsHidden::No);
        CHECK(module == original);
    }
    SECTION("an `_initialize` that is not a function is renamed all the same")
    {
        // Exported as a global (kind 3): hidden by its name, and judged by the world check under it.
        auto const original =
            concat({ header(),
                     section(6, raw({ 0x01, 0x7f, 0x00, 0x41, 0x00, 0x0b })),
                     section(7, concat({ raw({ 0x01 }), name("_initialize"), raw({ 0x03, 0x00 }) })) });
        auto module = original;
        CHECK(hideExports(module) == ExportsHidden::Yes);
        CHECK(differences(original, module).size() == 1);
    }
}

TEST_CASE("HiddenExports.instantiationHooks", "[vtextension]")
{
    // What WAMR runs by itself while instantiating, by the names it read; the alias is not among them.
    CHECK(isInstantiationHook("_initialize"));
    CHECK(isInstantiationHook("__post_instantiate"));
    CHECK(isInstantiationHook("__wasm_call_ctors"));
    CHECK_FALSE(isInstantiationHook("~initialize"));
    CHECK_FALSE(isInstantiationHook("_start"));
    CHECK_FALSE(isInstantiationHook("render"));
}

TEST_CASE("HiddenExports.names", "[vtextension]")
{
    // The host asks for `_initialize` and WAMR knows it as the alias; every other name is the same to both.
    CHECK(runtimeExportName("_initialize") == "~initialize");
    CHECK(hostExportName("~initialize") == "_initialize");
    CHECK(runtimeExportName("render") == "render");
    CHECK(hostExportName("render") == "render");
}
