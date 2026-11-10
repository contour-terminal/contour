// SPDX-License-Identifier: Apache-2.0
#include <contour/askpass/Secret.hpp>

#include <algorithm>

namespace contour::askpass
{

void wipeStorage(std::string& text) noexcept
{
    // Up to the capacity, which is within the allocation (or the small buffer) and so needs no growth.
    text.resize(text.capacity());
    auto* const bytes = text.data();
    for (auto index = std::size_t { 0 }; index < text.size(); ++index)
        static_cast<char volatile*>(bytes)[index] = '\0';
    text.clear();
}

void wipeBytes(std::span<std::byte> bytes) noexcept
{
    for (auto index = std::size_t { 0 }; index < bytes.size(); ++index)
        static_cast<std::byte volatile*>(bytes.data())[index] = std::byte { 0 };
}

void Secret::append(std::string_view more)
{
    if (_bytes.size() + more.size() <= _bytes.capacity())
    {
        _bytes.append(more);
        return;
    }
    auto grown = std::string {};
    grown.reserve(std::max(_bytes.capacity() * 2, _bytes.size() + more.size()));
    grown.append(_bytes);
    grown.append(more);
    wipeStorage(_bytes);
    _bytes.swap(grown);
    wipeStorage(grown);
}

} // namespace contour::askpass
