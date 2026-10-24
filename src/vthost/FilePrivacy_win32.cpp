// SPDX-License-Identifier: Apache-2.0
#ifdef _WIN32

// clang-format off
    #include <winsock2.h>
    #include <windows.h>
    #include <aclapi.h>
// clang-format on

    #include <format>
    #include <memory>
    #include <vector>

    #include <vthost/FilePrivacy.hpp>

namespace vthost
{

namespace
{
    struct HandleCloser
    {
        void operator()(HANDLE handle) const noexcept { CloseHandle(handle); }
    };

    using TokenHandle = std::unique_ptr<std::remove_pointer_t<HANDLE>, HandleCloser>;
} // namespace

std::expected<void, std::string> NativeFilePrivacy::restrictToOwner(std::filesystem::path const& path,
                                                                    PrivateEntry kind) const
{
    auto rawToken = HANDLE { nullptr };
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &rawToken))
        return std::unexpected(std::format("cannot open the process token: error {}", GetLastError()));
    auto const token = TokenHandle { rawToken };

    auto userSize = DWORD { 0 };
    GetTokenInformation(token.get(), TokenUser, nullptr, 0, &userSize);
    auto userBuffer = std::vector<std::byte>(userSize);
    if (!GetTokenInformation(token.get(), TokenUser, userBuffer.data(), userSize, &userSize))
        return std::unexpected(std::format("cannot read the token user: error {}", GetLastError()));
    auto* const sid = reinterpret_cast<TOKEN_USER const*>(userBuffer.data())->User.Sid;

    // One ACE for the current user. The ACL is DWORD-aligned, hence the DWORD buffer.
    auto const aclBytes = sizeof(ACL) + sizeof(ACCESS_ALLOWED_ACE) - sizeof(DWORD)
                          + static_cast<std::size_t>(GetLengthSid(sid));
    auto aclBuffer = std::vector<DWORD>((aclBytes + sizeof(DWORD) - 1) / sizeof(DWORD));
    auto* const acl = reinterpret_cast<PACL>(aclBuffer.data());
    if (!InitializeAcl(acl, static_cast<DWORD>(aclBuffer.size() * sizeof(DWORD)), ACL_REVISION))
        return std::unexpected(std::format("cannot build the ACL: error {}", GetLastError()));
    auto const inheritance =
        kind == PrivateEntry::Directory ? DWORD { OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE } : DWORD { 0 };
    if (!AddAccessAllowedAceEx(acl, ACL_REVISION, inheritance, GENERIC_ALL, sid))
        return std::unexpected(std::format("cannot build the ACE: error {}", GetLastError()));

    auto text = path.wstring();
    auto const status = SetNamedSecurityInfoW(text.data(),
                                              SE_FILE_OBJECT,
                                              DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                                              nullptr,
                                              nullptr,
                                              acl,
                                              nullptr);
    if (status != ERROR_SUCCESS)
        return std::unexpected(std::format("cannot restrict {}: error {}", path.string(), status));
    return {};
}

} // namespace vthost

#endif
