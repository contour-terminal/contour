// SPDX-License-Identifier: Apache-2.0
//
// What every system's mapping of a shared memory object shares: mapping and committing are each system's own
// (SharedObject_posix.cpp, SharedObject_win32.cpp), owning the mapping is not.
#include <vtextension/wasm/remote/transport/SharedObject.hpp>

#include <cstdint>
#include <expected>
#include <span>
#include <utility>

namespace vtextension::wasm::remote::transport
{

SharedMapping::SharedMapping(std::span<std::byte> bytes, MapAccess access) noexcept:
    _bytes { bytes }, _access { access }
{
}

SharedMapping::SharedMapping(SharedMapping&& other) noexcept:
    _bytes { std::exchange(other._bytes, {}) }, _access { other._access }
{
}

SharedMapping& SharedMapping::operator=(SharedMapping&& other) noexcept
{
    if (this != &other)
    {
        release();
        _bytes = std::exchange(other._bytes, {});
        _access = other._access;
    }
    return *this;
}

SharedMapping::~SharedMapping()
{
    release();
}

std::expected<void, SharedObjectError> SharedMapping::commit(uint64_t from, uint64_t to) noexcept
{
    if (from > to || to > _bytes.size())
        return std::unexpected(SharedObjectError::OutOfRange);
    if (from == to)
        return {};
    return commitRange(from, to);
}

} // namespace vtextension::wasm::remote::transport
