// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/Registry.hpp>

#ifdef _WIN32
    #include <core/WideString.hpp>

    #include <algorithm>
    #include <memory>
    #include <ranges>
    #include <type_traits>
    #include <utility>
    #include <vector>

    #include <Windows.h>
#endif

namespace contour::platform
{

#ifdef _WIN32
namespace
{

    /// The predefined key a RegistryRoot names.
    [[nodiscard]] HKEY hiveOf(RegistryRoot root) noexcept
    {
        switch (root)
        {
            case RegistryRoot::LocalMachine: return HKEY_LOCAL_MACHINE;
            case RegistryRoot::CurrentUser: return HKEY_CURRENT_USER;
        }
        return HKEY_CURRENT_USER;
    }

    /// A key's path and a value's name in the UTF-16 the wide registry API takes.
    struct WidePath
    {
        std::wstring key;
        std::wstring value;
    };

    /// @return @p key and @p value in UTF-16, or nullopt when either is not UTF-8.
    [[nodiscard]] std::optional<WidePath> widePathOf(std::string_view key, std::string_view value)
    {
        auto wideKey = core::toUtf16(key);
        auto wideValue = core::toUtf16(value);
        if (!wideKey || !wideValue)
            return std::nullopt;
        return WidePath { .key = std::move(*wideKey), .value = std::move(*wideValue) };
    }

    /// The system registry, through the wide API.
    class Win32Registry final: public IRegistry
    {
      public:
        [[nodiscard]] std::optional<std::string> readString(RegistryRoot root,
                                                            std::string_view key,
                                                            std::string_view value) const override
        {
            auto const path = widePathOf(key, value);
            if (!path)
                return std::nullopt;
            auto const hive = hiveOf(root);
            auto size = DWORD { 0 };
            if (RegGetValueW(
                    hive, path->key.c_str(), path->value.c_str(), RRF_RT_REG_SZ, nullptr, nullptr, &size)
                != ERROR_SUCCESS)
                return std::nullopt;
            auto text = std::wstring(size / sizeof(wchar_t), L'\0');
            if (RegGetValueW(
                    hive, path->key.c_str(), path->value.c_str(), RRF_RT_REG_SZ, nullptr, text.data(), &size)
                != ERROR_SUCCESS)
                return std::nullopt;
            // A string ends at its first NUL, which RegGetValueW always writes and counts in size.
            text.resize(std::min(text.find(L'\0'), size / sizeof(wchar_t)));
            return core::toUtf8(text);
        }

        [[nodiscard]] std::optional<std::uint32_t> readDword(RegistryRoot root,
                                                             std::string_view key,
                                                             std::string_view value) const override
        {
            auto const path = widePathOf(key, value);
            if (!path)
                return std::nullopt;
            auto data = DWORD { 0 };
            auto size = static_cast<DWORD>(sizeof(data));
            if (RegGetValueW(hiveOf(root),
                             path->key.c_str(),
                             path->value.c_str(),
                             RRF_RT_REG_DWORD,
                             nullptr,
                             &data,
                             &size)
                != ERROR_SUCCESS)
                return std::nullopt;
            return static_cast<std::uint32_t>(data);
        }

        [[nodiscard]] std::vector<std::string> subkeys(RegistryRoot root, std::string_view key) const override
        {
            auto const wideKey = core::toUtf16(key);
            if (!wideKey)
                return {};
            auto handle = HKEY { nullptr };
            if (RegOpenKeyExW(hiveOf(root), wideKey->c_str(), 0, KEY_READ, &handle) != ERROR_SUCCESS)
                return {};
            auto const owned =
                std::unique_ptr<std::remove_pointer_t<HKEY>, decltype(&RegCloseKey)> { handle, &RegCloseKey };
            auto count = DWORD { 0 };
            auto longestName = DWORD { 0 }; // in characters, without the terminating NUL
            if (RegQueryInfoKeyW(handle,
                                 nullptr,
                                 nullptr,
                                 nullptr,
                                 &count,
                                 &longestName,
                                 nullptr,
                                 nullptr,
                                 nullptr,
                                 nullptr,
                                 nullptr,
                                 nullptr)
                != ERROR_SUCCESS)
                return {};
            auto names = std::vector<std::string> {};
            for (auto const index: std::views::iota(DWORD { 0 }, count))
            {
                auto name = std::wstring(longestName + 1, L'\0');
                auto length = static_cast<DWORD>(name.size());
                if (RegEnumKeyExW(handle, index, name.data(), &length, nullptr, nullptr, nullptr, nullptr)
                    != ERROR_SUCCESS)
                    continue; // removed meanwhile, or renamed longer than the key said
                name.resize(length);
                names.push_back(core::toUtf8(name));
            }
            return names;
        }

        [[nodiscard]] std::expected<void, std::string> writeString(RegistryRoot root,
                                                                   std::string_view key,
                                                                   std::string_view value,
                                                                   std::string_view data) override
        {
            auto const path = widePathOf(key, value);
            auto const wideData = core::toUtf16(data);
            if (!path || !wideData)
                return std::unexpected(std::string { "the name or text is not UTF-8" });
            // REG_SZ data is measured in bytes and includes the terminating NUL.
            auto const bytes = static_cast<DWORD>((wideData->size() + 1) * sizeof(wchar_t));
            auto const status = RegSetKeyValueW(
                hiveOf(root), path->key.c_str(), path->value.c_str(), REG_SZ, wideData->c_str(), bytes);
            if (status != ERROR_SUCCESS)
                return std::unexpected(core::systemErrorMessage(static_cast<unsigned long>(status)));
            return {};
        }
    };

} // namespace
#endif

std::unique_ptr<IRegistry> makePlatformRegistry()
{
#ifdef _WIN32
    return std::make_unique<Win32Registry>();
#else
    return std::make_unique<NullRegistry>();
#endif
}

} // namespace contour::platform
