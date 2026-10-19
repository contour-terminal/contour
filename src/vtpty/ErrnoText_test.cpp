// SPDX-License-Identifier: Apache-2.0
#include <vtpty/ErrnoText.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <array>
#include <cerrno>
#include <string_view>

TEST_CASE("ErrnoText: what chdir() and exec report reads as strerror() says it", "[process]")
{
    // A table, so that a child between fork() and exec() never asks strerror() or the locale.
    auto const [code, text] = GENERATE(table<int, std::string_view>({
        { ENOENT, "No such file or directory" },
        { EACCES, "Permission denied" },
        { ENOEXEC, "Exec format error" },
        { ENOTDIR, "Not a directory" },
        { ELOOP, "Too many levels of symbolic links" },
        { ENAMETOOLONG, "File name too long" },
        { E2BIG, "Argument list too long" },
        { ETXTBSY, "Text file busy" },
        { ENOMEM, "Cannot allocate memory" },
    }));
    auto buffer = std::array<char, 32> {};
    CHECK(vtpty::errnoText(code, buffer) == text);
}

TEST_CASE("ErrnoText: any other value reads as its number", "[process]")
{
    auto buffer = std::array<char, 32> {};
    CHECK(vtpty::errnoText(4242, buffer) == "error 4242");
    CHECK(vtpty::errnoText(-7, buffer) == "error -7");

    // Too small for the number: no overrun, and still something to write.
    auto tiny = std::array<char, 8> {};
    CHECK(vtpty::errnoText(4242424, tiny) == "error");
    auto none = std::array<char, 0> {};
    CHECK(vtpty::errnoText(4242, none) == "error");
}

TEST_CASE("ErrnoText: the table is usable at compile time", "[process]")
{
    static_assert([] {
        auto buffer = std::array<char, 16> {};
        return vtpty::errnoText(ENOENT, buffer) == "No such file or directory"
               && vtpty::errnoText(99999, buffer) == "error 99999";
    }());
    SUCCEED();
}
