// SPDX-License-Identifier: Apache-2.0
//
// The process token's user, and the absolute security descriptors an owner-only creation is made
// with: owner that user, a protected DACL with one ALLOW entry for that user alone.
#include <core/fs/windows/OwnerSecurity.hpp>

#include <core/fs/FsError.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <utility>
#include <vector>

#include <windows.h>

namespace core::fs::detail
{

namespace
{

    std::unexpected<FsError> lastError(char const* call)
    {
        return std::unexpected { fsErrorFromSystemCode(std::int64_t { ::GetLastError() }, call) };
    }

    struct HandleCloser
    {
        void operator()(void* handle) const noexcept { ::CloseHandle(handle); }
    };

    /// @return The user SID of the process token, copied out of it.
    FsResult<std::vector<std::byte>> tokenUser()
    {
        auto token = HANDLE {};
        if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token) == FALSE)
            return lastError("OpenProcessToken");
        auto const closer = std::unique_ptr<void, HandleCloser> { token };
        auto size = DWORD { 0 };
        ::GetTokenInformation(token, TokenUser, nullptr, 0, &size);
        if (size == 0)
            return lastError("GetTokenInformation(TokenUser)");
        auto buffer = std::vector<std::byte>(size);
        if (::GetTokenInformation(token, TokenUser, buffer.data(), size, &size) == FALSE)
            return lastError("GetTokenInformation(TokenUser)");
        auto const* const user = reinterpret_cast<TOKEN_USER const*>(buffer.data());
        auto sid = std::vector<std::byte>(::GetLengthSid(user->User.Sid));
        if (::CopySid(static_cast<DWORD>(sid.size()), sid.data(), user->User.Sid) == FALSE)
            return lastError("CopySid");
        return sid;
    }

} // namespace

OwnerSecurity::OwnerSecurity(std::vector<std::byte> user): _user { std::move(user) }
{
}

FsResult<std::shared_ptr<OwnerSecurity const>> OwnerSecurity::ofProcess()
{
    return tokenUser().and_then(
        [](std::vector<std::byte> user) -> FsResult<std::shared_ptr<OwnerSecurity const>> {
            // The constructor is private, so make_shared cannot reach it; the pointer is owned at once.
            auto security = std::shared_ptr<OwnerSecurity> { new OwnerSecurity { std::move(user) } };
            return security->build(security->_file, 0)
                .and_then([&] {
                    return security->build(security->_directory, OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE);
                })
                .transform([&] { return std::shared_ptr<OwnerSecurity const> { std::move(security) }; });
        });
}

FsResult<void> OwnerSecurity::build(Descriptor& into, BYTE inheritance)
{
    // One ACCESS_ALLOWED_ACE, whose SID starts at its SidStart member, rounded up to a DWORD.
    auto const entrySize = sizeof(ACCESS_ALLOWED_ACE) - sizeof(DWORD) + _user.size();
    auto const aclSize = (sizeof(ACL) + entrySize + sizeof(DWORD) - 1) & ~(sizeof(DWORD) - 1);
    into.acl.assign(aclSize, std::byte { 0 });
    auto* const acl = reinterpret_cast<PACL>(into.acl.data());
    auto* const user = static_cast<PSID>(_user.data());
    if (::InitializeAcl(acl, static_cast<DWORD>(aclSize), ACL_REVISION) == FALSE)
        return lastError("InitializeAcl");
    if (::AddAccessAllowedAceEx(acl, ACL_REVISION, inheritance, FILE_ALL_ACCESS, user) == FALSE)
        return lastError("AddAccessAllowedAceEx");
    if (::InitializeSecurityDescriptor(&into.descriptor, SECURITY_DESCRIPTOR_REVISION) == FALSE)
        return lastError("InitializeSecurityDescriptor");
    if (::SetSecurityDescriptorOwner(&into.descriptor, user, FALSE) == FALSE)
        return lastError("SetSecurityDescriptorOwner");
    if (::SetSecurityDescriptorDacl(&into.descriptor, TRUE, acl, FALSE) == FALSE)
        return lastError("SetSecurityDescriptorDacl");
    // Protected: nothing the parent directory would hand down is added to it.
    if (::SetSecurityDescriptorControl(&into.descriptor, SE_DACL_PROTECTED, SE_DACL_PROTECTED) == FALSE)
        return lastError("SetSecurityDescriptorControl");
    return {};
}

PSID OwnerSecurity::user() const noexcept
{
    // The Win32 calls take a SID by non-const pointer and read it only.
    return const_cast<std::byte*>(_user.data());
}

PSECURITY_DESCRIPTOR OwnerSecurity::forFile() const noexcept
{
    // NtCreateFile takes the descriptor by non-const pointer and reads it only.
    return const_cast<SECURITY_DESCRIPTOR*>(&_file.descriptor);
}

PSECURITY_DESCRIPTOR OwnerSecurity::forDirectory() const noexcept
{
    return const_cast<SECURITY_DESCRIPTOR*>(&_directory.descriptor);
}

} // namespace core::fs::detail
