// SPDX-License-Identifier: Apache-2.0
//
// The native name rules of a Windows system.
#include <core/fs/PathPolicy.hpp>

namespace core::fs
{

PathPolicy const& nativePathPolicy() noexcept
{
    return windowsPathPolicy();
}

} // namespace core::fs
