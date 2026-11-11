// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The new release's side of a live upgrade, the one that differs by platform: taking over the image the
/// previous daemon handed over, as the argument `--handover=TOKEN` names it. POSIX reads it out of the memory
/// object its exec kept open (`fd=N`); Windows, a successor process, reads it out of the mapping it inherited
/// and holds the commit point with its predecessor (`handles=M,R,C,P`, @see receiveHandover()).

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string_view>

#include <vthost/handover/HandoverImage.hpp>

namespace vthost::handover
{

class HandoverMemory;

/// What a daemon that was named a handover it cannot take over does instead.
enum class TakeoverFailure : uint8_t
{
    /// Serves as a daemon that was handed nothing: POSIX, where this process already is the daemon, and no
    /// other one serves on.
    StartAfresh = 0,
    /// Ends without serving anything: Windows, where the predecessor, which never saw this one ready, serves
    /// on.
    EndWithoutServing,
};

/// Takes over what the previous daemon handed over in a live upgrade.
///
/// An interface, so that the daemon's start picks the native one with no platform test of its own, and so
/// that the token's parsing is tested on its own.
class ImageReceiver
{
  public:
    ImageReceiver() = default;
    ImageReceiver(ImageReceiver const&) = delete;
    ImageReceiver(ImageReceiver&&) = delete;
    ImageReceiver& operator=(ImageReceiver const&) = delete;
    ImageReceiver& operator=(ImageReceiver&&) = delete;
    virtual ~ImageReceiver() = default;

    /// Takes over the image @p token names. Run first of all, before the daemon opens anything of its own.
    /// @param token What follows `--handover=`; empty for a daemon that starts afresh.
    /// @return The image to take over; nullopt for none; or what to do instead (logged by the receiver).
    [[nodiscard]] virtual std::expected<std::optional<HandoverImage>, TakeoverFailure> takeOver(
        std::string_view token) = 0;
};

/// The operating system's receiver.
///
/// POSIX takes `fd=N`: reads and decodes the image in the memory object at descriptor N, closes it, then
/// every descriptor the image does not name. An image that cannot be read closes only the listening sockets
/// the previous image left -- which the daemon's own would otherwise find taken -- and starts afresh, keeping
/// every other descriptor open, so that no program it hosted is hung up on. Windows takes `handles=M,R,C,P`
/// and runs receiveHandover(); where that fails, the daemon ends without serving.
/// @param memory What reads the image (makeNativeHandoverMemory()). Not owned; outlives the receiver.
/// @param alsoKeep Descriptors kept open beside the image's on POSIX (the PTY keeper's channel); none
///        on Windows, where nothing crosses an exec.
/// @return The receiver.
[[nodiscard]] std::unique_ptr<ImageReceiver> makeNativeImageReceiver(HandoverMemory& memory,
                                                                     std::vector<int> alsoKeep = {});

} // namespace vthost::handover
