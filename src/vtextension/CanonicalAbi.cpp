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
            return detail::readBytes<std::string>(memory, list, 1);
        });
}

} // namespace vtextension::abi
