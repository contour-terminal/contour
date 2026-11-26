// SPDX-License-Identifier: Apache-2.0
#include <vtextension/wasm/remote/process/HelperLocation.hpp>

#include <array>
#include <expected>
#include <filesystem>
#include <functional>
#include <string_view>

namespace vtextension::wasm::remote::process
{

namespace
{
    /// Where the extension host may be, relative to the directory of the program that starts it, in the order
    /// they are tried.
#ifdef _WIN32
    constexpr auto Candidates = std::array<std::string_view, 1> { "." };
#elifdef __APPLE__
    constexpr auto Candidates =
        std::array<std::string_view, 3> { "../Helpers", CONTOUR_EXTENSION_HOST_RELATIVE_DIR, "." };
#else
    constexpr auto Candidates = std::array<std::string_view, 2> { CONTOUR_EXTENSION_HOST_RELATIVE_DIR, "." };
#endif
} // namespace

std::expected<std::filesystem::path, HelperLocationError> locateExtensionHost(
    std::filesystem::path const& selfExecutable,
    std::function<bool(std::filesystem::path const&)> const& isExecutable)
{
    auto const directory = selfExecutable.parent_path();
    for (auto const candidate: Candidates)
    {
        auto path = (directory / candidate / ExtensionHostName).lexically_normal();
        if (isExecutable(path))
            return path;
    }
    return std::unexpected(HelperLocationError::NotFound);
}

} // namespace vtextension::wasm::remote::process
