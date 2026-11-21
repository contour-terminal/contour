// SPDX-License-Identifier: Apache-2.0
#include <contour/cli/AgentTranscript.hpp>

#include <core/testing/ScopedTempDir.hpp>

#include <catch2/catch_test_macros.hpp>

#include <sys/stat.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

using contour::cli::makeNativeSessionLogOpener;
using contour::cli::TranscriptError;

TEST_CASE("a regular log is read", "[cli][transcript]")
{
    auto const directory = core::testing::ScopedTempDir { "contour-session-log" };
    auto const log = directory / "session.jsonl";
    std::ofstream { log } << "{\"type\":\"user\"}\n";

    auto stream = makeNativeSessionLogOpener()->open(log);

    REQUIRE(stream.has_value());
    auto const content =
        std::string { std::istreambuf_iterator<char> { **stream }, std::istreambuf_iterator<char> {} };
    CHECK(content == "{\"type\":\"user\"}\n");
}

TEST_CASE("a log that is a FIFO is refused without blocking", "[cli][transcript]")
{
    auto const directory = core::testing::ScopedTempDir { "contour-session-log" };
    auto const log = directory / "session.jsonl";
    REQUIRE(::mkfifo(log.c_str(), 0600) == 0);

    // Without O_NONBLOCK this open would wait for a writer that never comes.
    CHECK(makeNativeSessionLogOpener()->open(log).error() == TranscriptError::NotRegular);
}

TEST_CASE("a log that is a symlink is refused", "[cli][transcript]")
{
    auto const directory = core::testing::ScopedTempDir { "contour-session-log" };
    auto const target = directory / "elsewhere.jsonl";
    std::ofstream { target } << "{\"type\":\"user\"}\n";
    auto const log = directory / "session.jsonl";
    std::filesystem::create_symlink(target, log);

    CHECK(makeNativeSessionLogOpener()->open(log).error() == TranscriptError::NotRegular);
}

TEST_CASE("a log that is a directory is refused", "[cli][transcript]")
{
    auto const directory = core::testing::ScopedTempDir { "contour-session-log" };
    auto const log = directory / "session.jsonl";
    std::filesystem::create_directory(log);

    CHECK(makeNativeSessionLogOpener()->open(log).error() == TranscriptError::NotRegular);
}

TEST_CASE("a projects directory that is a symlink is a linked directory", "[cli][transcript]")
{
    auto const directory = core::testing::ScopedTempDir { "contour-session-log" };
    auto const real = directory / "real";
    std::filesystem::create_directory(real);
    auto const linked = directory / "linked";
    std::filesystem::create_directory_symlink(real, linked);
    auto const opener = makeNativeSessionLogOpener();

    CHECK_FALSE(opener->isLinkedDirectory(real));
    CHECK(opener->isLinkedDirectory(linked));
}
