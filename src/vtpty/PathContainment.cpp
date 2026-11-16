// SPDX-License-Identifier: Apache-2.0
#include <vtpty/PathContainment.hpp>

#include <algorithm>
#include <filesystem>

namespace vtpty
{

namespace
{
    /// @return @p path lexically normalised, without the empty last component a trailing separator leaves.
    [[nodiscard]] std::filesystem::path normalised(std::filesystem::path const& path)
    {
        auto result = path.lexically_normal();
        if (!result.has_filename() && result.has_relative_path())
            result = result.parent_path();
        return result;
    }

    /// @return @p c folded to lower case if it is an ASCII upper-case letter; any other unit unchanged.
    template <typename Char>
    [[nodiscard]] constexpr Char foldedAscii(Char c) noexcept
    {
        return c >= 'A' && c <= 'Z' ? static_cast<Char>(c - 'A' + 'a') : c;
    }
} // namespace

bool isAtOrBeneath(std::filesystem::path const& path, std::filesystem::path const& directory, PathCase rule)
{
    auto const judged = normalised(path);
    auto const base = normalised(directory);
    if (!judged.is_absolute() || !base.is_absolute())
        return false;
    auto const sameComponent = [rule](std::filesystem::path const& a, std::filesystem::path const& b) {
        switch (rule)
        {
            case PathCase::Sensitive: return a == b;
            case PathCase::Insensitive:
                return std::ranges::equal(a.native(),
                                          b.native(),
                                          {},
                                          foldedAscii<std::filesystem::path::value_type>,
                                          foldedAscii<std::filesystem::path::value_type>);
        }
        return false;
    };
    auto const [baseEnd, judgedEnd] = std::ranges::mismatch(base, judged, sameComponent);
    return baseEnd == base.end();
}

} // namespace vtpty
