// SPDX-License-Identifier: Apache-2.0
//
// Test-only: the receive a channel makes on macOS, where the system installs what came beside the bytes
// whether or not it is asked for, run on any POSIX system. Defined with the channel (Channel_posix.cpp).
#pragma once

#ifndef _WIN32
    #include <cstddef>
    #include <cstdint>
    #include <expected>
    #include <span>

    #include <vtextension/wasm/remote/transport/Channel.hpp>

namespace vtextension::wasm::remote::transport::testing
{

/// Receives at least one byte on @p channel as Channel::receiveSome() does on macOS: every descriptor
/// attached to the bytes is taken in, with room for all a message can carry, and closed at once, and
/// ancillary data that no peer sends -- other than descriptors, or more than a message carries -- is
/// discarded and told of once in the warning log (WarningLog.hpp).
/// @param channel The end received on.
/// @param buffer Where the bytes go. Not empty.
/// @return As Channel::receiveSome().
[[nodiscard]] std::expected<std::size_t, ChannelError> receiveSomeClosingAttachments(
    Channel& channel, std::span<uint8_t> buffer);

/// Lets the warning receiveSomeClosingAttachments() gives once per process be given again, so that a case
/// that counts it does not depend on whether another case in the same process gave it first.
void forgetToldForeignAncillaryData() noexcept;

} // namespace vtextension::wasm::remote::transport::testing
#endif
