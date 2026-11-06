// SPDX-License-Identifier: Apache-2.0
//
// The version resource the built contour.exe carries, read the way Explorer and Windows Installer read it:
// through the version API, never by running the executable.

#include <core/Utf8Path.hpp>
#include <core/WideString.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <Windows.h>

namespace
{

/// The language and code page the resource's strings are in: US English, Unicode.
constexpr auto StringTableKey = std::wstring_view { L"\\StringFileInfo\\040904B0\\" };

/// The version resource of contour.exe, for as long as it lives.
class ContourVersionInfo
{
  public:
    ContourVersionInfo()
    {
        auto const executable = core::pathFromTrustedUtf8(CONTOUR_BINARY);
        auto ignored = DWORD {};
        auto const size = GetFileVersionInfoSizeW(executable.c_str(), &ignored);
        if (size == 0)
            return;
        _data.resize(size);
        if (!GetFileVersionInfoW(executable.c_str(), 0, size, _data.data()))
            _data.clear();
    }

    [[nodiscard]] bool isValid() const noexcept { return !_data.empty(); }

    /// @return The string @p name of the resource's string table, in UTF-8; nullopt where there is none.
    [[nodiscard]] std::optional<std::string> value(std::wstring_view name) const
    {
        auto const key = std::wstring { StringTableKey } + std::wstring { name };
        auto* text = static_cast<wchar_t*>(nullptr);
        auto length = UINT {};
        if (!VerQueryValueW(_data.data(), key.c_str(), reinterpret_cast<void**>(&text), &length)
            || length == 0)
            return std::nullopt;
        return core::toUtf8(std::wstring_view { text });
    }

    /// @return The four fields of the fixed file version, as "major.minor.patch.build".
    [[nodiscard]] std::optional<std::string> fixedFileVersion() const
    {
        return fixedVersion([](VS_FIXEDFILEINFO const& fixed) {
            return versionText(fixed.dwFileVersionMS, fixed.dwFileVersionLS);
        });
    }

    /// @return The four fields of the fixed product version, as "major.minor.patch.build".
    [[nodiscard]] std::optional<std::string> fixedProductVersion() const
    {
        return fixedVersion([](VS_FIXEDFILEINFO const& fixed) {
            return versionText(fixed.dwProductVersionMS, fixed.dwProductVersionLS);
        });
    }

  private:
    /// @return The four fields of a (most, least significant) pair, as "major.minor.patch.build".
    [[nodiscard]] static std::string versionText(DWORD mostSignificant, DWORD leastSignificant)
    {
        return std::to_string(HIWORD(mostSignificant)) + '.' + std::to_string(LOWORD(mostSignificant)) + '.'
               + std::to_string(HIWORD(leastSignificant)) + '.' + std::to_string(LOWORD(leastSignificant));
    }

    /// @return What @p spell makes of the fixed file information; nullopt where there is none.
    template <typename Spell>
    [[nodiscard]] std::optional<std::string> fixedVersion(Spell const& spell) const
    {
        auto* fixed = static_cast<VS_FIXEDFILEINFO*>(nullptr);
        auto length = UINT {};
        if (!VerQueryValueW(_data.data(), L"\\", reinterpret_cast<void**>(&fixed), &length)
            || fixed == nullptr)
            return std::nullopt;
        return spell(*fixed);
    }

    std::vector<std::byte> _data;
};

} // namespace

TEST_CASE("contour.exe names its product, publisher and purpose", "[versioninfo]")
{
    auto const info = ContourVersionInfo {};
    REQUIRE(info.isValid());
    CHECK(info.value(L"ProductName") == "Contour");
    CHECK(info.value(L"CompanyName") == "Contour Terminal");
    CHECK(info.value(L"FileDescription") == "Contour Terminal Emulator");
    CHECK(info.value(L"InternalName") == "contour");
    CHECK(info.value(L"OriginalFilename") == "contour.exe");
    auto const copyright = info.value(L"LegalCopyright");
    REQUIRE(copyright.has_value());
    CHECK(copyright->find("Christian Parpart") != std::string::npos);
}

TEST_CASE("contour.exe's versions are this build's", "[versioninfo]")
{
    auto const info = ContourVersionInfo {};
    REQUIRE(info.isValid());
    CHECK(info.fixedFileVersion() == CONTOUR_EXPECTED_FILE_VERSION);
    CHECK(info.fixedProductVersion() == CONTOUR_EXPECTED_FILE_VERSION);
    CHECK(info.value(L"FileVersion") == CONTOUR_EXPECTED_FILE_VERSION);
    CHECK(info.value(L"ProductVersion") == CONTOUR_EXPECTED_PRODUCT_VERSION);
}
