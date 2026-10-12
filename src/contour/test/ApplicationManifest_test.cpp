// SPDX-License-Identifier: Apache-2.0
//
// The manifest the built contour.exe carries, read the way the loader reads it: through an activation
// context created from the executable's manifest resource, never by running it.

#include <core/Utf8Path.hpp>
#include <core/WideString.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <Windows.h>

namespace
{

/// The manifest resource of an executable, which CreateProcess reads its settings from.
constexpr auto ProcessManifestResource = 1;

/// The `supportedOS` id of Windows 10 and Windows 11.
constexpr auto Windows10 =
    GUID { 0x8e0f7a12, 0xbfb3, 0x4fe8, { 0xb9, 0xa5, 0x48, 0xfd, 0x50, 0xa1, 0x5a, 0x9a } };

/// The activation context contour.exe's manifest makes, for as long as it lives.
class ContourActivationContext
{
  public:
    ContourActivationContext()
    {
        auto const executable = core::pathFromTrustedUtf8(CONTOUR_BINARY);
        auto context = ACTCTXW {};
        context.cbSize = sizeof(context);
        context.dwFlags = ACTCTX_FLAG_RESOURCE_NAME_VALID;
        context.lpSource = executable.c_str();
        context.lpResourceName = MAKEINTRESOURCEW(ProcessManifestResource);
        _handle = CreateActCtxW(&context);
    }

    ~ContourActivationContext()
    {
        if (_handle != INVALID_HANDLE_VALUE)
            ReleaseActCtx(_handle);
    }

    ContourActivationContext(ContourActivationContext const&) = delete;
    ContourActivationContext& operator=(ContourActivationContext const&) = delete;
    ContourActivationContext(ContourActivationContext&&) = delete;
    ContourActivationContext& operator=(ContourActivationContext&&) = delete;

    [[nodiscard]] bool isValid() const noexcept { return _handle != INVALID_HANDLE_VALUE; }

    /// @return The value of the windowsSettings element @p name in the namespace @p settingsNamespace, in
    ///         UTF-8; nullopt where the manifest has none there.
    [[nodiscard]] std::optional<std::string> setting(wchar_t const* settingsNamespace,
                                                     wchar_t const* name) const
    {
        auto value = std::array<wchar_t, 64> {};
        auto written = SIZE_T {};
        if (!QueryActCtxSettingsW(0, _handle, settingsNamespace, name, value.data(), value.size(), &written))
            return std::nullopt;
        return core::toUtf8(std::wstring_view { value.data() });
    }

    /// @return The operating systems the manifest declares itself compatible with.
    [[nodiscard]] std::vector<GUID> supportedOperatingSystems() const
    {
        auto required = SIZE_T {};
        QueryActCtxW(0, _handle, nullptr, CompatibilityInformationInActivationContext, nullptr, 0, &required);
        auto buffer = std::vector<std::byte>(required);
        if (required == 0
            || !QueryActCtxW(0,
                             _handle,
                             nullptr,
                             CompatibilityInformationInActivationContext,
                             buffer.data(),
                             buffer.size(),
                             &required))
            return {};
        auto const* const information =
            reinterpret_cast<ACTIVATION_CONTEXT_COMPATIBILITY_INFORMATION const*>(buffer.data());
        auto ids = std::vector<GUID> {};
        for (auto const& element: std::span { information->Elements, information->ElementCount })
            if (element.Type == ACTCTX_COMPATIBILITY_ELEMENT_TYPE_OS)
                ids.push_back(element.Id);
        return ids;
    }

  private:
    HANDLE _handle = INVALID_HANDLE_VALUE;
};

} // namespace

TEST_CASE("contour.exe runs with UTF-8 as its process code page", "[manifest]")
{
    auto const context = ContourActivationContext {};
    REQUIRE(context.isValid());
    // Windows reads activeCodePage in this namespace only, and ignores the element under any other.
    CHECK(context.setting(L"http://schemas.microsoft.com/SMI/2019/WindowsSettings", L"activeCodePage")
          == "UTF-8");
}

TEST_CASE("contour.exe declares Windows 10 and long paths, as the manifest Qt generated for it did",
          "[manifest]")
{
    auto const context = ContourActivationContext {};
    REQUIRE(context.isValid());
    CHECK(context.setting(L"http://schemas.microsoft.com/SMI/2016/WindowsSettings", L"longPathAware")
          == "true");
    CHECK(std::ranges::contains(context.supportedOperatingSystems(), Windows10));
}
