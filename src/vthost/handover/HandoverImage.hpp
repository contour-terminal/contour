// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The live-upgrade handover image: everything that crosses the daemon's in-place `execv`, as plain data.
///
/// Nothing here owns a resource. A descriptor is a number the new image inherits; turning it back into a PTY,
/// a child or a listener is the adopter's job (@see vtpty::UnixPty::adopt, vtpty::Process::adopt).

#include <vtbackend/screen/Settings.hpp>

#include <vtpty/InheritedPty.hpp>
#include <vtpty/Process.hpp>

#include <cstdint>
#include <string>
#include <vector>

#include <vtcontrol/ObjectRef.hpp>
#include <vtcontrol/Uuid.hpp>
#include <vthost/CarriedDelivery.hpp>
#include <vthost/ObjectRegistry.hpp>
#include <vthost/SessionSettings.hpp>
#include <vtworkspace/Primitives.hpp>
#include <vtworkspace/WorkspaceState.hpp>

namespace vthost::handover
{

/// The newest handover format this build writes.
///
/// The encoding is tolerant (@see vtbackend::detail::FieldEncoder): a field added or dropped later is not a
/// new version. Only a change an older reader cannot skip over -- a field whose meaning changes under the
/// same id, a new framing -- is.
inline constexpr uint32_t HandoverFormatVersion = 1;

/// The oldest handover format this build can still read.
inline constexpr uint32_t OldestReadableHandoverFormat = 1;

// The object registry's own vocabulary (@see ObjectRegistry::snapshot()), named here as the image names it.
using vthost::BoundObject;
using vthost::LabeledObject;
using vthost::ObjectRegistrySnapshot;

/// A listener socket the new image inherits.
struct InheritedListener
{
    std::string name; ///< "control", "native", "imsg", "tmux-compat" or "native-tcp".
    int fd = -1;      ///< The descriptor.

    /// Lists the fields for the handover codec.
    void visitFields(this auto& self, auto& visitor)
    {
        visitor(1, self.name);
        visitor(2, self.fd);
    }
};

/// One hosted session, as it crosses the exec.
struct HandoverSession
{
    vtworkspace::SessionId id {};                    ///< Its id in the model.
    vtcontrol::Uuid uuid {};                         ///< Its UUID.
    std::string label;                               ///< Its label.
    std::string profileName;                         ///< The profile it was spawned under; empty for none.
    SettingsOrigin origin = SettingsOrigin::Profile; ///< Where its settings came from.
    /// The session's base settings. Only the emulation-relevant subset crosses (@see toWireSessionSettings);
    /// the rest comes from the configuration the new image loads. The terminal state carries what it was
    /// constructed with (@see vtbackend::settingsFor).
    vtbackend::Settings settings {};
    vtpty::InheritedPty pty {};           ///< Its PTY.
    vtpty::AdoptedChild child {};         ///< Its child process.
    std::vector<uint8_t> terminalState;   ///< vtbackend::encodeTerminalState() of its terminal.
    std::vector<CarriedDelivery> carried; ///< QuiescedSession::carried, oldest first.

    /// Lists the fields for the handover codec.
    void visitFields(this auto& self, auto& visitor)
    {
        visitor(1, self.id);
        visitor(2, self.uuid);
        visitor(3, self.label);
        visitor(4, self.profileName);
        visitor(5, self.origin);
        visitor(6, self.settings);
        visitor(7, self.pty);
        visitor(8, self.child);
        visitor(9, self.terminalState);
        visitor(10, self.carried);
    }
};

/// Everything the old image hands the new one.
struct HandoverImage
{
    uint64_t daemonInstance = 0;                ///< Which daemon instance wrote it.
    std::string fromRelease;                    ///< The release that wrote it.
    vtworkspace::WorkspaceTreeState tree {};    ///< The windows, tabs and panes.
    vtworkspace::ModelIdState modelIds {};      ///< The model's id counters.
    vtworkspace::SessionId nextSessionId { 1 }; ///< The id the next session is minted with.
    ObjectRegistrySnapshot objects {};          ///< The object registry.
    std::vector<HandoverSession> sessions;      ///< Every hosted session.
    std::vector<InheritedListener> listeners;   ///< The listeners to keep serving on.
    int leaseFd = -1;                           ///< The descriptor of the daemon image lease; -1 for none.

    /// Lists the fields for the handover codec.
    void visitFields(this auto& self, auto& visitor)
    {
        visitor(1, self.daemonInstance);
        visitor(2, self.fromRelease);
        visitor(3, self.tree);
        visitor(4, self.modelIds);
        visitor(5, self.nextSessionId);
        visitor(6, self.objects);
        visitor(7, self.sessions);
        visitor(8, self.listeners);
        visitor(9, self.leaseFd);
    }
};

} // namespace vthost::handover
