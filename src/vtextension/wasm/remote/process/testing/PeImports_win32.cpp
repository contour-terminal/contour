// SPDX-License-Identifier: Apache-2.0
#include <vtextension/wasm/remote/process/testing/PeImports_win32.hpp>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <functional>
#include <ios>
#include <iterator>
#include <memory>
#include <ranges>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace vtextension::wasm::remote::process::testing
{

namespace
{
    /// The prefixes of API set names: contracts the loader maps onto libraries, not files.
    constexpr auto ApiSetPrefixes = std::to_array<std::string_view>({ "api-ms-win-", "ext-ms-win-" });

    /// Where the system lists its known DLLs: libraries the loader maps from the system directory alone.
    constexpr wchar_t const* KnownDllsKey = LR"(SYSTEM\CurrentControlSet\Control\Session Manager\KnownDLLs)";

    /// Closes a registry key when destroyed.
    struct RegistryKeyClose
    {
        void operator()(HKEY key) const noexcept { ::RegCloseKey(key); }
    };

    /// @return The system directory, as the loader searches it.
    [[nodiscard]] std::filesystem::path systemDirectory()
    {
        auto directory = std::wstring(MAX_PATH, L'\0');
        auto const length = ::GetSystemDirectoryW(directory.data(), static_cast<UINT>(directory.size()));
        REQUIRE(length != 0);
        REQUIRE(length < directory.size());
        directory.resize(length);
        return std::filesystem::path { directory }.make_preferred();
    }
} // namespace

std::vector<std::string> startupImportsOf(std::filesystem::path const& file)
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

std::vector<std::string> knownDlls()
{
    auto names = std::vector<std::string> {};
    HKEY opened = nullptr;
    if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, KnownDllsKey, 0, KEY_QUERY_VALUE, &opened) != ERROR_SUCCESS)
        return names;
    auto const key = std::unique_ptr<std::remove_pointer_t<HKEY>, RegistryKeyClose> { opened };
    for (auto const index: std::views::iota(DWORD { 0 }))
    {
        auto valueName = std::wstring(256, L'\0');
        auto valueNameLength = static_cast<DWORD>(valueName.size());
        auto data = std::wstring(MAX_PATH, L'\0');
        auto dataBytes = static_cast<DWORD>(data.size() * sizeof(wchar_t));
        auto type = DWORD { 0 };
        auto const result = ::RegEnumValueW(key.get(),
                                            index,
                                            valueName.data(),
                                            &valueNameLength,
                                            nullptr,
                                            &type,
                                            reinterpret_cast<BYTE*>(data.data()),
                                            &dataBytes);
        if (result == ERROR_NO_MORE_ITEMS)
            break;
        if (result != ERROR_SUCCESS || type != REG_SZ)
            continue;
        data.resize(::wcsnlen(data.c_str(), dataBytes / sizeof(wchar_t)));
        auto name = std::string {};
        for (auto const c: data)
            name += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        names.push_back(std::move(name));
    }
    return names;
}

ImportClosure importClosureOf(std::filesystem::path const& executable)
{
    auto const besideIt = std::filesystem::path { executable }.make_preferred().parent_path();
    auto const system = systemDirectory();
    auto const known = knownDlls();
    auto closure = ImportClosure {};
    auto seen = std::vector<std::string> {};
    // Depth first, each library listed after what it imports.
    auto const visit = [&](this auto const& self, std::filesystem::path const& image) -> void {
        for (auto const& name: startupImportsOf(image))
        {
            if (std::ranges::contains(seen, name))
                continue;
            seen.push_back(name);
            // An API set is loaded by its name, so that a library behind it that fails is named apart from
            // the one importing it.
            if (std::ranges::any_of(ApiSetPrefixes, [&](auto prefix) { return name.starts_with(prefix); }))
            {
                closure.libraries.emplace_back(name);
                continue;
            }
            auto candidates = std::vector<std::filesystem::path> {};
            if (!std::ranges::contains(known, name))
                candidates.push_back(besideIt / name);
            candidates.push_back(system / name);
            auto const found = std::ranges::find_if(
                candidates, [](auto const& path) { return std::filesystem::is_regular_file(path); });
            if (found == candidates.end())
            {
                closure.unresolved.push_back(name);
                continue;
            }
            self(*found);
            closure.libraries.push_back(*found);
        }
    };
    visit(executable);
    return closure;
}

} // namespace vtextension::wasm::remote::process::testing
