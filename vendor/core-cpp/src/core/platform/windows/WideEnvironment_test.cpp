// SPDX-License-Identifier: Apache-2.0
//
// The environment on Windows is UTF-16, and core-cpp's strings are UTF-8: every reader and writer
// converts between the two, rather than going through the ANSI code page, which cannot spell most
// of Unicode and spells the rest differently on every machine (core-cpp#7). A user profile path
// with an umlaut is the case a consumer meets first; the Greek letter is one no Western code page
// has, so a reader that narrows through one answers '?' for it.
#include <core/Environment.hpp>
#include <core/platform/ProcessEnvironment.hpp>
#include <core/platform/WorkingDirectory.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <string>
#include <system_error>
#include <tuple>

#include <windows.h>

namespace
{

/// "C:\Users\Jürgen\Ω", in UTF-16 and in UTF-8.
constexpr auto WideValue = L"C:\\Users\\J\u00fcrgen\\\u03a9";
constexpr auto Utf8Value = "C:\\Users\\J\xc3\xbcrgen\\\xce\xa9";

/// @return What the process's own block holds for @p name, read through the wide API.
[[nodiscard]] std::wstring wideValueOf(wchar_t const* name)
{
    auto buffer = std::array<wchar_t, 256> {};
    auto const written = ::GetEnvironmentVariableW(name, buffer.data(), static_cast<DWORD>(buffer.size()));
    return std::wstring { buffer.data(), written };
}

/// Removes a variable this file wrote, however the case leaves.
struct WrittenVariable
{
    wchar_t const* name;
    ~WrittenVariable() { std::ignore = ::SetEnvironmentVariableW(name, nullptr); }
};

} // namespace

TEST_CASE("LiveEnvironment reads a value the ANSI code page cannot spell, as UTF-8",
          "[platform][environment]")
{
    auto const cleanup = WrittenVariable { .name = L"CORE_CPP_WIDE_READ_TEST" };
    REQUIRE(::SetEnvironmentVariableW(cleanup.name, WideValue) != 0);

    CHECK(core::LiveEnvironment {}.get("CORE_CPP_WIDE_READ_TEST") == Utf8Value);
    CHECK(core::platform::nativeProcessEnvironment()->get("CORE_CPP_WIDE_READ_TEST") == Utf8Value);
}

TEST_CASE("setProcessEnvironmentVariable writes UTF-8 as the UTF-16 it spells", "[platform][environment]")
{
    auto const cleanup = WrittenVariable { .name = L"CORE_CPP_WIDE_WRITE_TEST" };

    REQUIRE(core::setProcessEnvironmentVariable("CORE_CPP_WIDE_WRITE_TEST", Utf8Value).has_value());
    CHECK(wideValueOf(cleanup.name) == WideValue);

    // And the process environment's exporter, which writes through it.
    auto const environment = core::platform::nativeProcessEnvironment();
    REQUIRE(environment->setAndExport("CORE_CPP_WIDE_WRITE_TEST", "\xce\xa9").has_value());
    CHECK(wideValueOf(cleanup.name) == L"\u03a9");
}

TEST_CASE("the native ProcessEnvironment lists a name the ANSI code page cannot spell, as UTF-8",
          "[platform][environment]")
{
    // The keys come from the wide block, and were narrowed a code unit at a time: U+03A9 became
    // the byte 0xA9.
    auto const cleanup = WrittenVariable { .name = L"CORE_CPP_WIDE_\u03a9" };
    REQUIRE(::SetEnvironmentVariableW(cleanup.name, L"set") != 0);

    auto const keys = core::platform::nativeProcessEnvironment()->keys();
    CHECK(std::ranges::find(keys, std::string { "CORE_CPP_WIDE_\xce\xa9" }) != keys.end());
    CHECK(core::LiveEnvironment {}.get("CORE_CPP_WIDE_\xce\xa9") == "set");
}

TEST_CASE("a name that is not UTF-8 is refused, not narrowed", "[platform][environment]")
{
    // 0xFF begins no UTF-8 sequence. The code-page API took it as whatever character the machine's
    // code page gives it; the wide one has no spelling for it at all.
    CHECK_FALSE(core::setProcessEnvironmentVariable("CORE_CPP_\xff", "value").has_value());
    CHECK_FALSE(core::setProcessEnvironmentVariable("CORE_CPP_WIDE_BAD_VALUE", "\xff").has_value());
    CHECK_FALSE(core::LiveEnvironment {}.get("CORE_CPP_WIDE_BAD_VALUE").has_value());
}

TEST_CASE("the native WorkingDirectory round-trips a directory the ANSI code page cannot spell",
          "[platform][environment]")
{
    // What `currentDirectory()` answers is what `changeDirectory()` must take back: a UTF-8 string
    // converted to a path reads through the code page on Windows, which cannot spell this name
    // (review L4 of 0.5.0, the #26 class of defect).
    auto const original = std::filesystem::current_path();
    auto const directory = std::filesystem::temp_directory_path() / L"core-cpp-J\u00fcrgen-\u03a9";
    std::filesystem::create_directories(directory);
    auto const cwd = core::platform::nativeWorkingDirectory();

    REQUIRE(cwd->changeDirectory(directory).has_value());
    auto const here = cwd->currentDirectory();
    auto ec = std::error_code {};
    CHECK(std::filesystem::equivalent(here, directory, ec));
    CHECK(cwd->changeDirectory(original).has_value());
    CHECK(cwd->changeDirectory(here).has_value());
    CHECK(std::filesystem::equivalent(std::filesystem::current_path(), directory, ec));

    std::filesystem::current_path(original);
    std::filesystem::remove(directory, ec);
}

TEST_CASE("the native ProcessEnvironment folds case outside ASCII, as Windows does",
          "[platform][environment]")
{
    // Windows matches environment names case-insensitively in all of Unicode, and the variables
    // set but not exported compared ASCII letters only: `get` missed a local value by another
    // spelling, and `keys` listed a local name beside the exported one it names (review N1 of 0.5.0).
    auto const environment = core::platform::nativeProcessEnvironment();
    REQUIRE(environment->set("CORE_CPP_WIDE_\xC3\x84X", "local").has_value());
    CHECK(environment->get("CORE_CPP_WIDE_\xC3\x84X") == "local");
    CHECK(environment->get("CORE_CPP_WIDE_\xC3\xA4X") == "local");

    auto const cleanup = WrittenVariable { .name = L"CORE_CPP_WIDE_\u00c4Y" };
    REQUIRE(::SetEnvironmentVariableW(cleanup.name, L"exported") != 0);
    REQUIRE(environment->set("CORE_CPP_WIDE_\xC3\xA4y", "local").has_value());
    auto const keys = environment->keys();
    auto const spelled = std::ranges::count_if(keys, [](std::string const& key) {
        return key == "CORE_CPP_WIDE_\xC3\x84Y" || key == "CORE_CPP_WIDE_\xC3\xA4y";
    });
    CHECK(spelled == 1);
}
