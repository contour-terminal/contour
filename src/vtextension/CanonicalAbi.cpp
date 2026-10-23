// SPDX-License-Identifier: Apache-2.0
#include <vtextension/CanonicalAbi.hpp>

namespace vtextension::abi
{

Result<std::string> loadByteString(IGuestMemory& memory, Address address, std::size_t maxLength)
{
    return detail::loadPointerAndLength(memory, address)
        .and_then([&](detail::PointerAndLength list) -> Result<std::string> {
            if (list.length > maxLength)
                return std::unexpected(ExtensionError::OutputTooLarge);
            return detail::viewElements(memory, list, 1).transform([](std::span<std::byte> bytes) {
                return detail::copyBytes<std::string>(bytes);
            });
        });
}

} // namespace vtextension::abi
