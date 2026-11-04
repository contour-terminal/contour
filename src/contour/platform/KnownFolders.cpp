// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/KnownFolders.hpp>

#ifdef _WIN32
    #include <memory>

    // ShlObj.h brings Windows.h, and KnownFolders.h after it: the latter cannot be included first.
    #include <ShlObj.h>
#endif

namespace contour::platform
{

#ifdef _WIN32
namespace
{

    /// The shell's known folders.
    class Win32KnownFolders final: public IKnownFolders
    {
      public:
        [[nodiscard]] std::optional<std::filesystem::path> documents() const override
        {
            auto* raw = PWSTR { nullptr };
            auto const result = SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr, &raw);
            // The buffer is the caller's to free even when the call fails.
            auto const owned = std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> { raw, &CoTaskMemFree };
            if (FAILED(result) || owned == nullptr)
                return std::nullopt;
            return std::filesystem::path { owned.get() };
        }
    };

} // namespace
#endif

std::unique_ptr<IKnownFolders> makePlatformKnownFolders()
{
#ifdef _WIN32
    return std::make_unique<Win32KnownFolders>();
#else
    return std::make_unique<NullKnownFolders>();
#endif
}

} // namespace contour::platform
