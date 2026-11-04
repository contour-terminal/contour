// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// A restored session's interrupted command, as the host keeps it and a live-upgrade capture carries it
/// across the exec. Apart from SessionHost.hpp so that the handover image, which SessionHost.hpp names, can
/// carry it without including the host.

#include <vtbackend/core/CommandBlockId.hpp>

#include <string>

namespace vthost
{

/// The command a restart stopped in a restored session, until the session runs one of its own.
struct CommandInterruption
{
    std::string commandLine;            ///< What the restart stopped.
    vtbackend::CommandBlockId after {}; ///< A block newer than this that left its prompt ends it.

    /// Lists the fields for the handover codec.
    void visitFields(this auto& self, auto& visitor)
    {
        visitor(1, self.commandLine);
        visitor(2, self.after);
    }

    bool operator==(CommandInterruption const&) const = default;
};

} // namespace vthost
