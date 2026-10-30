// SPDX-License-Identifier: Apache-2.0
#include <text_shaper/BundledFonts.hpp>

#include <algorithm>
#include <system_error>

namespace text
{

std::expected<std::vector<std::filesystem::path>, FontRegistrationError> listBundledFontFiles(
    std::filesystem::path const& directory)
{
    auto error = std::error_code {};
    if (!std::filesystem::is_directory(directory, error))
        return std::unexpected { FontRegistrationError::DirectoryMissing };

    auto files = std::vector<std::filesystem::path> {};
    for (auto const& entry: std::filesystem::directory_iterator(directory, error))
        if (entry.is_regular_file(error) && entry.path().extension() == ".ttf")
            files.push_back(entry.path());
    if (error)
        return std::unexpected { FontRegistrationError::RegistrationFailed };
    std::ranges::sort(files);
    return files;
}

#ifdef _WIN32
// Windows ships no bundled fonts (it uses Cascadia Mono), so nothing is registered. Each directory is
// still checked, so that a missing one is reported here exactly as on the other platforms.
std::expected<void, FontRegistrationError> registerBundledFonts(BundledFontDirectories const& directories)
{
    auto result = std::expected<void, FontRegistrationError> {};
    for (auto const& directory: directories.paths)
        if (auto const files = listBundledFontFiles(directory); !files && result)
            result = std::unexpected { files.error() };
    return result;
}
#endif

} // namespace text
