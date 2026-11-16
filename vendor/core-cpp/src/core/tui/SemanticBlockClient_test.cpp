// SPDX-License-Identifier: Apache-2.0
#include <core/tui/SemanticBlockClient.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace core::tui;

// ============================================================================
// parseBlockJson() unit tests (static method, no mocks needed)
// ============================================================================

TEST_CASE("SemanticBlockClient.parseBlockJson.full_response", "[tui,semantic-block]")
{
    auto const* const json =
        R"({"version":1,"command":"ls /nonexistent","output":"ls: cannot access '/nonexistent': No such file or directory\n","exitCode":2,"finished":true,"outputLineCount":1})";
    auto const result = SemanticBlockClient::parseBlockJson(json);
    REQUIRE(result.has_value());
    CHECK(result->command == "ls /nonexistent");
    CHECK(result->output == "ls: cannot access '/nonexistent': No such file or directory\n");
    CHECK(result->exitCode == 2);
    CHECK(result->finished == true);
    CHECK(result->outputLineCount == 1);
}

TEST_CASE("SemanticBlockClient.parseBlockJson.escaped_characters", "[tui,semantic-block]")
{
    auto const* const json =
        R"({"version":1,"command":"echo \"hello\tworld\"","output":"hello\tworld\n","exitCode":0,"finished":true,"outputLineCount":1})";
    auto const result = SemanticBlockClient::parseBlockJson(json);
    REQUIRE(result.has_value());
    CHECK(result->command == "echo \"hello\tworld\"");
    CHECK(result->output == "hello\tworld\n");
    CHECK(result->exitCode == 0);
}

TEST_CASE("SemanticBlockClient.parseBlockJson.negative_exit_code", "[tui,semantic-block]")
{
    auto const* const json =
        R"({"version":1,"command":"./crash","output":"Segfault\n","exitCode":-11,"finished":true,"outputLineCount":1})";
    auto const result = SemanticBlockClient::parseBlockJson(json);
    REQUIRE(result.has_value());
    CHECK(result->command == "./crash");
    CHECK(result->exitCode == -11);
}

TEST_CASE("SemanticBlockClient.parseBlockJson.unfinished_command", "[tui,semantic-block]")
{
    auto const* const json =
        R"({"version":1,"command":"sleep 100","output":"","exitCode":0,"finished":false,"outputLineCount":0})";
    auto const result = SemanticBlockClient::parseBlockJson(json);
    REQUIRE(result.has_value());
    CHECK(result->command == "sleep 100");
    CHECK(result->finished == false);
    CHECK(result->output.empty());
}

TEST_CASE("SemanticBlockClient.parseBlockJson.empty_command", "[tui,semantic-block]")
{
    auto const* const json =
        R"({"version":1,"command":"","output":"","exitCode":0,"finished":true,"outputLineCount":0})";
    auto const result = SemanticBlockClient::parseBlockJson(json);
    REQUIRE(result.has_value());
    CHECK(result->command.empty());
    CHECK(result->output.empty());
    CHECK(result->exitCode == 0);
}

TEST_CASE("SemanticBlockClient.parseBlockJson.partial_fields", "[tui,semantic-block]")
{
    // Only command field present — other fields should use defaults.
    auto const* const json = R"({"version":1,"command":"whoami"})";
    auto const result = SemanticBlockClient::parseBlockJson(json);
    REQUIRE(result.has_value());
    CHECK(result->command == "whoami");
    CHECK(result->output.empty());
    CHECK(result->exitCode == -1);
    CHECK(result->finished == true);
    CHECK(result->outputLineCount == 0);
}

TEST_CASE("SemanticBlockClient.parseBlockJson.multiline_output", "[tui,semantic-block]")
{
    auto const* const json =
        R"({"version":1,"command":"ls","output":"file1.txt\nfile2.txt\nfile3.txt\n","exitCode":0,"finished":true,"outputLineCount":3})";
    auto const result = SemanticBlockClient::parseBlockJson(json);
    REQUIRE(result.has_value());
    CHECK(result->output == "file1.txt\nfile2.txt\nfile3.txt\n");
    CHECK(result->outputLineCount == 3);
}

TEST_CASE("SemanticBlockClient.parseBlockJson.large_exit_code", "[tui,semantic-block]")
{
    auto const* const json =
        R"({"version":1,"command":"exit 255","output":"","exitCode":255,"finished":true,"outputLineCount":0})";
    auto const result = SemanticBlockClient::parseBlockJson(json);
    REQUIRE(result.has_value());
    CHECK(result->exitCode == 255);
}

// ============================================================================
// SemanticBlockStatus / SemanticBlockResult default tests
// ============================================================================

TEST_CASE("SemanticBlockResult.default_status", "[tui,semantic-block]")
{
    auto const result = SemanticBlockResult {};
    CHECK(result.status == SemanticBlockStatus::Unsupported);
    CHECK(!result.block.has_value());
}
