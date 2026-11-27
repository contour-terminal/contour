// SPDX-License-Identifier: Apache-2.0
//
// The native name rules of a POSIX system.
#include <core/fs/PathPolicy.hpp>

namespace core::fs
{

PathPolicy const& nativePathPolicy() noexcept
{
    return posixPathPolicy();
}

} // namespace core::fs
