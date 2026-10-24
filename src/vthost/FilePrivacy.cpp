// SPDX-License-Identifier: Apache-2.0
#ifndef _WIN32

    #include <vthost/FilePrivacy.hpp>

    #include <format>
    #include <system_error>

namespace vthost
{

std::expected<void, std::string> NativeFilePrivacy::restrictToOwner(std::filesystem::path const& path,
                                                                    PrivateEntry kind) const
{
    namespace fs = std::filesystem;
    auto const permissions = kind == PrivateEntry::Directory ? fs::perms::owner_all
                                                             : fs::perms::owner_read | fs::perms::owner_write;
    auto error = std::error_code {};
    fs::permissions(path, permissions, fs::perm_options::replace, error);
    if (error)
        return std::unexpected(std::format("cannot restrict {}: {}", path.string(), error.message()));
    return {};
}

} // namespace vthost

#endif
