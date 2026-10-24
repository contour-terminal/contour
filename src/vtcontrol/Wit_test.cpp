// SPDX-License-Identifier: Apache-2.0
#include <core/Environment.hpp>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <format>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <tuple>

#include <vtcontrol/Catalog.hpp>
#include <vtcontrol/Wit.hpp>

namespace
{
/// @return The path of the file @p name in the control WIT package directory.
[[nodiscard]] std::string witPath(std::string_view name)
{
    return std::string(VTCONTROL_WIT_DIR) + "/" + std::string(name);
}

/// @return The contents of the file @p name in the control WIT package directory.
[[nodiscard]] std::string readWitFile(std::string_view name)
{
    auto file = std::ifstream(witPath(name), std::ios::binary);
    REQUIRE(file.good());
    return { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
}
} // namespace

TEST_CASE("the checked-in WIT is the rendered catalog", "[vtcontrol][wit]")
{
    auto const rendered = vtcontrol::renderControlWit();
    if (core::LiveEnvironment {}.get("UPDATE_WIT") == "1")
    {
        auto file = std::ofstream(witPath("world.wit"), std::ios::binary | std::ios::trunc);
        file << rendered;
        REQUIRE(file.good());
    }
    // A catalog row added or changed without regenerating the file fails here: run this test with
    // UPDATE_WIT=1 to regenerate it.
    CHECK(readWitFile("world.wit") == rendered);
}

TEST_CASE("the WIT declares the everywhere-verbs only", "[vtcontrol][wit]")
{
    auto const wit = vtcontrol::renderControlWit();
    CHECK(wit.contains("package terminal-extensions:control@0.1.0;"));
    CHECK(wit.contains("pane-split: func(request: split-request) -> result<split-reply, control-error>;"));
    CHECK(wit.contains("pane-close: func(request: close-pane-request) -> result<_, control-error>;"));
    CHECK_FALSE(wit.contains("block-wait"));
    CHECK_FALSE(wit.contains("events-subscribe"));
    CHECK_FALSE(wit.contains("events-unsubscribe"));
}

TEST_CASE("the WIT spells records, enums and fields in kebab-case", "[vtcontrol][wit]")
{
    auto const wit = vtcontrol::renderControlWit();
    CHECK(wit.contains("    record split-request {\n"
                       "        pane: string,\n"
                       "        direction: split-direction,\n"
                       "        ratio-percent: option<u8>,\n"
                       "        cwd: string,\n"
                       "        command: list<string>,\n"
                       "        label: string,\n"
                       "        focus: focus-policy,\n"
                       "    }\n"));
    CHECK(wit.contains("    enum control-error-code {\n        bad-request,\n        method-not-found,\n"));
    CHECK(wit.contains("    record control-error {\n"
                       "        code: control-error-code,\n"
                       "        message: string,\n"
                       "        candidates: list<string>,\n"
                       "    }\n"));
    CHECK(wit.contains("object-set-label: func(request: set-label-request) -> result<_, control-error>;"));
    CHECK(wit.contains("    /// Split a pane and start a shell or command in the new half.\n"
                       "    /// Needs `write`, and `input` as well for running a command.\n"
                       "    pane-split: func("));
    CHECK(wit.contains("input-schema: string,"));
    CHECK(wit.contains("exit-code: option<s32>,"));
    CHECK(wit.contains("is-active: bool,"));
    CHECK(wit.contains("world controller {\n    import control;\n}"));
}

TEST_CASE("each WIT func says what it needs granted", "[vtcontrol][wit]")
{
    // A guest's author reads the permission model where the funcs are declared: what dispatch() enforces.
    auto const wit = vtcontrol::renderControlWit();
    CHECK(wit.contains("    /// Needs `read`.\n    workspace-list: func("));
    CHECK(wit.contains("    /// Needs `input`.\n    pane-send: func("));
    CHECK(wit.contains(
        "    /// Needs `write`, and `input` as well for running a command.\n    tab-create: func("));
    CHECK(wit.contains("    /// Needs `write`.\n    pane-close: func("));
}

TEST_CASE("the WIT spells the control API's error codes, limit-reached included", "[vtcontrol][wit]")
{
    auto const wit = vtcontrol::renderControlWit();
    CHECK(wit.contains("        failed,\n        limit-reached,\n    }\n"));
}

TEST_CASE("the WIT declares no empty record", "[vtcontrol][wit]")
{
    // WIT has no empty record: an empty request is a func without parameters, an empty reply `_`.
    auto const wit = vtcontrol::renderControlWit();
    CHECK(wit.contains("daemon-info: func() -> result<daemon-info-reply, control-error>;"));
    CHECK(wit.contains("api-list: func() -> result<api-list-reply, control-error>;"));
    CHECK_FALSE(wit.contains("{\n    }"));
    CHECK_FALSE(wit.contains("empty-reply"));
}

TEST_CASE("a record is declared before its first use", "[vtcontrol][wit]")
{
    auto const wit = vtcontrol::renderControlWit();
    auto const positionOf = [&](std::string_view needle) {
        auto const position = wit.find(needle);
        REQUIRE(position != std::string::npos);
        return position;
    };
    CHECK(positionOf("record size {") < positionOf("record pane-info {"));
    CHECK(positionOf("record pane-info {") < positionOf("record tab-info {"));
    CHECK(positionOf("record tab-info {") < positionOf("record window-info {"));
    CHECK(positionOf("record window-info {") < positionOf("record list-reply {"));
    CHECK(positionOf("enum control-error-code {") < positionOf("record control-error {"));
}

TEST_CASE("kebabCase", "[vtcontrol][wit]")
{
    CHECK(vtcontrol::kebabCase("pane.split") == "pane-split");
    CHECK(vtcontrol::kebabCase("SplitRequest") == "split-request");
    CHECK(vtcontrol::kebabCase("ratioPercent") == "ratio-percent");
    CHECK(vtcontrol::kebabCase("methodNotFound") == "method-not-found");
    CHECK(vtcontrol::kebabCase("object.setLabel") == "object-set-label");
}

TEST_CASE("the control WIT is terminal-neutral", "[vtcontrol][wit]")
{
    for (auto const* name: { "world.wit", "README.md" })
    {
        INFO(name);
        auto const text = readWitFile(name);
        CHECK_FALSE(text.contains("Contour"));
        CHECK_FALSE(text.contains("contour"));
        CHECK_FALSE(text.contains("Qt"));
    }
}

TEST_CASE("the README names each verb's permission", "[vtcontrol][wit]")
{
    // The README's verb table is written by hand, so it is checked against the catalog here.
    auto const readme = readWitFile("README.md");
    auto const rowOf = [&](std::string_view verb) {
        auto const start = readme.find(std::format("| `{}` ", verb));
        REQUIRE(start != std::string::npos);
        return std::string_view { readme }.substr(start, readme.find('\n', start) - start);
    };
    std::apply(
        [&](auto const&... method) {
            auto const check = [&](auto const& row) {
                auto const verb = vtcontrol::kebabCase(row.name);
                INFO(verb);
                if (row.exposure == vtcontrol::Exposure::Everywhere)
                {
                    CHECK(rowOf(verb).contains(std::format("| `{}` ", vtcontrol::nameOf(row.permission))));
                    if (row.requestGrant)
                        CHECK(
                            rowOf(verb).contains(std::format("| `{}`, for {} ",
                                                             vtcontrol::nameOf(row.requestGrant->permission),
                                                             row.requestGrant->reason)));
                    else
                        CHECK(rowOf(verb).ends_with("|                                  |"));
                }
                else
                    CHECK_FALSE(readme.contains(std::format("| `{}` ", verb)));
            };
            (check(method), ...);
        },
        vtcontrol::Methods);
}
