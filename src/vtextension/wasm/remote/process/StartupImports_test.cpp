// SPDX-License-Identifier: Apache-2.0
//
// What starts contained must start without a window station: the extension host, and the probe that stands in
// for it, import nothing that loads user32 (Windows only). Compiled into both helper suites, each of which
// reads its own extension host -- Contour's, on the build's engine, and the twin on WAMR's -- and the probe.
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#ifdef _WIN32
    #include <algorithm>
    #include <array>
    #include <cctype>
    #include <cstddef>
    #include <cstring>
    #include <filesystem>
    #include <fstream>
    #include <iterator>
    #include <ranges>
    #include <string>
    #include <string_view>
    #include <vector>

    #define WIN32_LEAN_AND_MEAN
    #include <Windows.h>

namespace
{

/// What a process that may run contained must not load as it starts: user32, whose initialisation connects
/// to the process's window station -- which, a service's (a CI runner's, an SSH session's), admits no app
/// container, so that the loader ends the process with STATUS_DLL_INIT_FAILED (0xC0000142) -- and the system
/// libraries that load it themselves.
constexpr auto WindowStationLibraries = std::to_array<std::string_view>({
    "user32.dll",
    "gdi32.dll",
    "ole32.dll",
    "oleaut32.dll",
    "shell32.dll",
    "shlwapi.dll",
    "comctl32.dll",
});

/// @return The libraries the executable @p file imports as it starts, lower-cased: its import directory's,
///         not those it delay-loads.
[[nodiscard]] std::vector<std::string> startupImportsOf(std::filesystem::path const& file)
{
    auto stream = std::ifstream { file, std::ios::binary };
    REQUIRE(stream.good());
    auto const bytes = std::vector<char> { std::istreambuf_iterator<char> { stream }, {} };
    auto const read = [&]<typename T>(std::size_t offset) {
        REQUIRE(offset + sizeof(T) <= bytes.size());
        auto value = T {};
        std::memcpy(&value, bytes.data() + offset, sizeof(T));
        return value;
    };
    auto const dos = read.operator()<IMAGE_DOS_HEADER>(0);
    REQUIRE(dos.e_magic == IMAGE_DOS_SIGNATURE);
    auto const headersAt = static_cast<std::size_t>(dos.e_lfanew);
    auto const headers = read.operator()<IMAGE_NT_HEADERS64>(headersAt);
    REQUIRE(headers.Signature == IMAGE_NT_SIGNATURE);
    REQUIRE(headers.OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC);
    auto const sectionsAt =
        headersAt + offsetof(IMAGE_NT_HEADERS64, OptionalHeader) + headers.FileHeader.SizeOfOptionalHeader;
    // Where in the file the image's address @p rva lies.
    auto const fileOffsetOf = [&](DWORD rva) -> std::size_t {
        for (auto const index: std::views::iota(WORD { 0 }, headers.FileHeader.NumberOfSections))
        {
            auto const section =
                read.operator()<IMAGE_SECTION_HEADER>(sectionsAt + (index * sizeof(IMAGE_SECTION_HEADER)));
            auto const size = std::max<DWORD>(section.Misc.VirtualSize, section.SizeOfRawData);
            if (rva >= section.VirtualAddress && rva < section.VirtualAddress + size)
                return rva - section.VirtualAddress + section.PointerToRawData;
        }
        FAIL("address 0x" << std::hex << rva << " lies in no section");
        return 0;
    };
    auto const directory = headers.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    auto imports = std::vector<std::string> {};
    if (directory.VirtualAddress == 0)
        return imports;
    auto const first = fileOffsetOf(directory.VirtualAddress);
    for (auto const index: std::views::iota(std::size_t { 0 }))
    {
        auto const descriptor =
            read.operator()<IMAGE_IMPORT_DESCRIPTOR>(first + (index * sizeof(IMAGE_IMPORT_DESCRIPTOR)));
        if (descriptor.Name == 0)
            break;
        auto const nameAt = fileOffsetOf(descriptor.Name);
        auto name =
            std::string { bytes.data() + nameAt, ::strnlen(bytes.data() + nameAt, bytes.size() - nameAt) };
        std::ranges::transform(name, name.begin(), [](char c) {
            return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        });
        imports.push_back(std::move(name));
    }
    return imports;
}

} // namespace

TEST_CASE("StartupImports.containedExecutablesLoadNoWindowStation", "[vtextension][appcontainer]")
{
    // What starts contained -- the extension host, and the probe that stands in for it -- must start without
    // a window station: one it may not use ends it in the loader, before it can say why.
    for (auto const* const executable: { VTEXTENSION_HELPER_PATH, VTEXTENSION_PROBE_PATH })
    {
        INFO(executable);
        auto const imports = startupImportsOf(executable);
        // Read at all: every executable imports kernel32.
        CHECK(std::ranges::contains(imports, std::string { "kernel32.dll" }));
        for (auto const& library: imports)
        {
            INFO(library);
            CHECK_FALSE(std::ranges::contains(WindowStationLibraries, std::string_view { library }));
        }
    }
}
#endif
