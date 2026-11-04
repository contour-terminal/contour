// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What a live upgrade moves the daemon onto: the vocabulary the orchestrator and the protocol that requests
/// an upgrade share.

#include <string>

namespace vthost::handover
{

/// What a live upgrade moves the daemon onto.
struct UpgradeTarget
{
    /// The image of the daemon's store to exec, by its name (`<release>-<fingerprint>`); empty for the
    /// daemon's own installed executable, which only a daemon that runs in place (a packaged Linux install)
    /// upgrades to. Never a path: nothing outside the store can be named.
    std::string imageId;
};

} // namespace vthost::handover
