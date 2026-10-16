// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/shell/CommandLineText.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>

using vtbackend::CommandLineText;

TEST_CASE("CommandLineText keeps the bytes as they came, for the wire and comparison", "[commandline]")
{
    auto const text = CommandLineText { "ls\x1b[201~rm -rf ~\n" };
    CHECK(text.raw() == "ls\x1b[201~rm -rf ~\n");
    CHECK(text == CommandLineText { "ls\x1b[201~rm -rf ~\n" });
    CHECK(text != CommandLineText { "ls" });
    CHECK_FALSE(text.empty());
    CHECK(CommandLineText {}.empty());
    CHECK(CommandLineText {}.raw().empty());
}

TEST_CASE("CommandLineText hands a reader the bytes made safe for its purpose", "[commandline]")
{
    // The same purposes, and so the same text, as sanitizeCommandLine() gives.
    auto const text = CommandLineText { "ls\x1b[201~rm -rf ~\n" };
    CHECK(text.display() == "ls\xEF\xBF\xBD[201~rm -rf ~\xEF\xBF\xBD");
    CHECK(text.insertable() == "ls[201~rm -rf ~\n");
    CHECK(text.copyable() == "ls[201~rm -rf ~");
    CHECK(CommandLineText { "a\r\n\tb\n" }.multilineCopyable() == "a\n\tb");
}
