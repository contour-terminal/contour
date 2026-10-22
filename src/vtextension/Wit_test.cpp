// SPDX-License-Identifier: Apache-2.0
//
// The statusline WIT world's text and its README, the normative spec. That a wit-bindgen core module
// built from the world speaks the Canonical ABI the README documents is checked from the host side,
// by the codec's round trips through the echo guest (CanonicalAbi_test.cpp).
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <fstream>
#include <iterator>
#include <memory>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace std::string_view_literals;

namespace
{

#ifdef VTEXTENSION_WIT_DIR
/// @return The contents of the file @p name in the WIT package directory.
[[nodiscard]] std::string readWitFile(std::string_view name)
{
    auto const path = std::string(VTEXTENSION_WIT_DIR) + "/" + std::string(name);
    auto file = std::ifstream(path, std::ios::binary);
    REQUIRE(file.good());
    return { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
}
#endif

} // namespace

#ifdef VTEXTENSION_WIT_DIR
TEST_CASE("Wit.world.declaresThePackage")
{
    auto const world = readWitFile("world.wit");
    CHECK(world.contains("package terminal-extensions:statusline@0.1.0;"));
}

TEST_CASE("Wit.world.declaresTheTypes")
{
    // Each declaration, whole: a bare name would also match a field or a case of something else.
    auto const world = readWitFile("world.wit");
    for (auto const declaration: {
             "enum render-request {"sv,
             "enum input-mode {"sv,
             "enum progress-state {"sv,
             "enum log-level {"sv,
             "enum command-error {"sv,
             "record tab {"sv,
             "record progress {"sv,
             "record size {"sv,
             "record state-snapshot {"sv,
             "utc-offset-minutes: s32,"sv,
             "record font-metrics {"sv,
             "font-changed(font-metrics),"sv,
             "record command-output {"sv,
             "flags event-kinds {"sv,
             "variant event {"sv,
             "reply(list<u8>),"sv,
             "timer,"sv,
         })
        CHECK(world.contains(declaration));
}

TEST_CASE("Wit.world.declaresTheImports")
{
    auto const world = readWitFile("world.wit");
    CHECK(world.contains("interface host {"));
    CHECK(world.contains("import host;"));
    for (
        auto const declaration: {
            "request-frame: func(after-ms: u32);"sv,
            "log: func(level: log-level, message: string);"sv,
            "state: func() -> state-snapshot;"sv,
            "run-command: func(name: string, args: list<tuple<string, string>>) -> result<command-output, command-error>;"sv,
        })
        CHECK(world.contains(declaration));
}

TEST_CASE("Wit.world.declaresTheExports")
{
    auto const world = readWitFile("world.wit");
    CHECK(world.contains("world statusline {"));
    for (auto const declaration: {
             "export load: func(config: list<tuple<string, string>>) -> event-kinds;"sv,
             "export update: func(ev: event) -> render-request;"sv,
             "export render: func(cols: u32, rows: u32) -> list<u8>;"sv,
         })
        CHECK(world.contains(declaration));
}

TEST_CASE("Wit.readme.documentsTheBinaryContract")
{
    // The exact phrases a guest author targets; prose that merely mentions the names would not do.
    auto const readme = readWitFile("README.md");
    for (auto const phrase: {
             "module `terminal-extensions:statusline/host@0.1.0`"sv,
             "`request-frame`, `log`, `state`, `run-command`"sv,
             "`load`, `update`, `render`"sv,
             "`cabi_realloc`"sv,
             "`cabi_post_<name>`"sv,
             "`cabi_post_render`"sv,
             "`wasm32-wasip1`"sv,
             "Canonical ABI"sv,
         })
        CHECK(readme.contains(phrase));
}

TEST_CASE("Wit.readme.isTerminalNeutral")
{
    // The world is a standard other terminals can implement: it names no particular host, and
    // nothing of how a host draws.
    for (auto const* const file: { "README.md", "world.wit" })
    {
        auto const text = readWitFile(file);
        for (auto const word: { "Contour", "contour", "Qt", "Screen", "RenderBuffer", "pixel", "Pixel" })
            CHECK_FALSE(text.contains(word));
    }
}
#endif
