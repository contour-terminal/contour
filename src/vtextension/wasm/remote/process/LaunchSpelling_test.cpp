// SPDX-License-Identifier: Apache-2.0
//
// How a launch is spelled for CreateProcessW(): the environment block the system parses, the variables a
// contained launch cannot go without, and the command line the child's C runtime splits back.
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <vtextension/wasm/remote/process/LaunchSpelling_win32.hpp>

using namespace std::string_view_literals;
using vtextension::wasm::remote::process::appendArgument;
using vtextension::wasm::remote::process::containedEnvironment;
using vtextension::wasm::remote::process::ContainerFolders;
using vtextension::wasm::remote::process::containerFoldersOf;
using vtextension::wasm::remote::process::ContainerVariables;
using vtextension::wasm::remote::process::environmentBlock;
using vtextension::wasm::remote::process::EnvironmentEntry;

TEST_CASE("LaunchSpelling.emptyEnvironmentIsTwoTerminators", "[vtextension][process]")
{
    // An empty block is still a block: one empty string's terminator, then the block's.
    auto const block = environmentBlock({});
    REQUIRE(block.has_value());
    CHECK(*block == std::wstring(2, L'\0'));
}

TEST_CASE("LaunchSpelling.environmentBlockIsSortedAndTerminated", "[vtextension][process]")
{
    struct Row
    {
        std::string_view what;
        std::vector<EnvironmentEntry> environment;
        std::wstring_view block;
    };
    auto const rows = std::to_array<Row>({
        { .what = "one entry, then the block's terminator",
          .environment = { { "SystemRoot", "C:\\Windows" } },
          .block = L"SystemRoot=C:\\Windows\0\0"sv },
        { .what = "sorted by name, ignoring case",
          .environment = { { "TMP", "t" }, { "b", "2" }, { "A", "1" } },
          .block = L"A=1\0b=2\0TMP=t\0\0"sv },
        // Ordinal on the upper case: '_' (0x5F) sorts after 'A' (0x41), though before 'a' (0x61).
        { .what = "ordinal, on the upper case",
          .environment = { { "_X", "u" }, { "a", "l" } },
          .block = L"a=l\0_X=u\0\0"sv },
        { .what = "an empty value keeps its entry",
          .environment = { { "EMPTY", "" } },
          .block = L"EMPTY=\0\0"sv },
        { .what = "UTF-8 is spelled in UTF-16",
          .environment = { { "NAME", "\xC3\xA4" } },
          .block = L"NAME=\u00E4\0\0"sv },
    });
    for (auto const& row: rows)
    {
        INFO(row.what);
        auto const block = environmentBlock(row.environment);
        REQUIRE(block.has_value());
        CHECK(*block == row.block);
    }
}

TEST_CASE("LaunchSpelling.environmentNotUtf8IsRefused", "[vtextension][process]")
{
    auto const environment = std::vector<EnvironmentEntry> { { "NAME", "\xFF" } };
    CHECK_FALSE(environmentBlock(environment).has_value());
}

namespace
{

/// A container's folder, as GetAppContainerFolderPath() names one.
constexpr auto ContainerFolder = std::wstring_view { LR"(C:\Users\u\AppData\Local\Packages\c\AC)" };

/// @return The folders of ContainerFolder, which must read as a container's.
[[nodiscard]] ContainerFolders containerFolders()
{
    auto folders = containerFoldersOf(std::filesystem::path { ContainerFolder });
    REQUIRE(folders.has_value());
    return *std::move(folders);
}

} // namespace

TEST_CASE("LaunchSpelling.containerFoldersAreReadFromTheContainersFolder", "[vtextension][process]")
{
    CHECK(containerFolders()
          == ContainerFolders { .container = std::filesystem::path { ContainerFolder },
                                .profile = std::filesystem::path { LR"(C:\Users\u\AppData\Local)" } });
    // A trailing separator names the same folder; the names are the system's, in whatever case.
    CHECK(containerFoldersOf(std::filesystem::path { LR"(C:\Local\packages\c\ac\)" })
          == ContainerFolders { .container = std::filesystem::path { LR"(C:\Local\packages\c\ac)" },
                                .profile = std::filesystem::path { LR"(C:\Local)" } });
    // Anything else yields no profile folder for CreateProcessW() to extend.
    for (auto const folder: { LR"(C:\AC)",
                              LR"(C:\Local\Packages\AC)",
                              LR"(C:\Local\Other\c\AC)",
                              LR"(C:\Local\Packages\c\Temp)",
                              LR"(Packages\c\AC)",
                              LR"(Local\Packages\c\AC)",
                              LR"(\Local\Packages\c\AC)" })
    {
        INFO(std::filesystem::path { folder }.string());
        CHECK_FALSE(containerFoldersOf(std::filesystem::path { folder }).has_value());
    }
}

TEST_CASE("LaunchSpelling.containedEnvironmentNamesTheContainersFolders", "[vtextension][process]")
{
    // What it was asked for of these is replaced, whatever the case of its name; the rest stays as asked.
    // LOCALAPPDATA is handed the profile's folder, which CreateProcessW() extends to the container's; TEMP
    // and TMP what the child is to see.
    auto const environment = std::vector<EnvironmentEntry> { { "SystemRoot", "C:\\Windows" },
                                                             { "temp", "asked" },
                                                             { "LocalAppData", R"(C:\Elsewhere)" } };
    CHECK(containedEnvironment(environment, containerFolders())
          == std::vector<EnvironmentEntry> { { "SystemRoot", "C:\\Windows" },
                                             { "LOCALAPPDATA", R"(C:\Users\u\AppData\Local)" },
                                             { "TEMP", R"(C:\Users\u\AppData\Local\Packages\c\AC\Temp)" },
                                             { "TMP", R"(C:\Users\u\AppData\Local\Packages\c\AC\Temp)" } });

    // Asked for none of them, it still names every one: the system refuses a contained launch without them.
    CHECK(containedEnvironment({}, containerFolders()).size() == ContainerVariables.size());
}

TEST_CASE("LaunchSpelling.helperEnvironmentContainedIsOneBlock", "[vtextension][process]")
{
    // The block a contained launch hands CreateProcessW(): SystemRoot and every container variable, sorted.
    auto const environment = std::vector<EnvironmentEntry> { { "SystemRoot", "C:\\Windows" } };
    auto const block = environmentBlock(containedEnvironment(environment, containerFolders()));
    REQUIRE(block.has_value());
    CHECK(*block
          == L"LOCALAPPDATA=C:\\Users\\u\\AppData\\Local\0SystemRoot=C:\\Windows\0"
             L"TEMP=C:\\Users\\u\\AppData\\Local\\Packages\\c\\AC\\Temp\0"
             L"TMP=C:\\Users\\u\\AppData\\Local\\Packages\\c\\AC\\Temp\0\0"sv);
}

TEST_CASE("LaunchSpelling.argumentsAreQuotedAsTheRuntimeSplits", "[vtextension][process]")
{
    struct Row
    {
        std::wstring_view argument;
        std::wstring_view spelled;
    };
    auto const rows = std::to_array<Row>({
        { .argument = L"plain", .spelled = L"first plain" },
        { .argument = L"", .spelled = L"first \"\"" },
        { .argument = L"two words", .spelled = L"first \"two words\"" },
        { .argument = L"say \"hi\"", .spelled = L"first \"say \\\"hi\\\"\"" },
        { .argument = L"C:\\dir with space\\", .spelled = L"first \"C:\\dir with space\\\\\"" },
    });
    for (auto const& row: rows)
    {
        auto commandLine = std::wstring { L"first" };
        appendArgument(commandLine, row.argument);
        CHECK(commandLine == row.spelled);
    }
}
