// SPDX-License-Identifier: Apache-2.0
#include <core/UserInfo.hpp>

// currentUserPasswordEntry() is per platform: PosixUserInfo.cpp reads the password database, and
// NoPasswordDatabase.cpp answers for a platform that has none.

namespace core
{

std::string userHomeDirectory()
{
    if (auto const entry = currentUserPasswordEntry())
        return entry->homeDirectory;
    return {};
}

} // namespace core
