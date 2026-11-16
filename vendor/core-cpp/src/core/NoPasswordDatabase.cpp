// SPDX-License-Identifier: Apache-2.0
#include <core/UserInfo.hpp>

namespace core
{

// A platform with no password database to read: Windows, whose user profile is another API, and WASI,
// whose guest has no users at all.
std::optional<PasswordEntry> currentUserPasswordEntry()
{
    return std::nullopt;
}

} // namespace core
