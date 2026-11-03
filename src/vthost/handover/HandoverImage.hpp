// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The live-upgrade handover image: everything that crosses the daemon's in-place `execv`, as plain data.
///
/// Nothing here owns a resource. A descriptor is a number the new image inherits; turning it back into a PTY,
/// a child or a listener is the adopter's job (@see vtpty::UnixPty::adopt, vtpty::Process::adopt).

#include <vtbackend/core/ColorPalette.hpp>

#include <vtpty/InheritedPty.hpp>
#include <vtpty/Process.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <vtcontrol/ObjectRef.hpp>
#include <vtcontrol/Uuid.hpp>
#include <vthost/CarriedDelivery.hpp>
#include <vthost/CommandInterruption.hpp>
#include <vthost/ObjectRegistry.hpp>
#include <vthost/ProfileCatalog.hpp>
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

/// A TLS certificate and its private key, both PEM-encoded.
struct TlsIdentity
{
    std::string certPem; ///< The certificate.
    std::string keyPem;  ///< Its private key, unencrypted.

    /// Lists the fields for the handover codec.
    void visitFields(this auto& self, auto& visitor)
    {
        visitor(1, self.certPem);
        visitor(2, self.keyPem);
    }
};

/// A listener socket the new image inherits.
struct InheritedListener
{
    std::string name; ///< "control", "native", "imsg", "tmux-compat" or "native-tcp".
    int fd = -1;      ///< The descriptor.
    /// The identity the listener presents where the daemon made it up itself (a self-signed certificate):
    /// presented again by the new image, so that a client that trusted it on first use goes on trusting it.
    /// nullopt for a listener without TLS, or with a configured certificate, which the new image reads again.
    std::optional<TlsIdentity> tlsIdentity {};

    /// Lists the fields for the handover codec.
    void visitFields(this auto& self, auto& visitor)
    {
        visitor(1, self.name);
        visitor(2, self.fd);
        visitor(3, self.tlsIdentity);
    }
};

/// What of a session's device crosses the exec.
enum class SessionDevice : uint8_t
{
    ChildOnPty = 0, ///< A child process on a PTY: both cross (HandoverSession::pty, HandoverSession::child).
    /// A PTY no child of the daemon's runs on -- one a handover served on without its child, which no longer
    /// was the daemon's to adopt: the PTY crosses alone, served until it closes, and HandoverSession::child
    /// names none.
    PtyOnly,
};

/// One hosted session, as it crosses the exec.
struct HandoverSession
{
    vtworkspace::SessionId id {};                    ///< Its id in the model.
    vtcontrol::Uuid uuid {};                         ///< Its UUID.
    std::string label;                               ///< Its label.
    std::string profileName;                         ///< The profile it was spawned under; empty for none.
    SettingsOrigin origin = SettingsOrigin::Profile; ///< Where its settings came from.
    /// The emulation settings a client stated for it, as that client stated them: what the new image builds a
    /// SettingsOrigin::Client session's settings from over its own host settings, as a client's connection
    /// does (@see fromWireSessionSettings). Unused for a profile's session, whose profile the new image
    /// resolves again. The terminal state carries what it was constructed with (@see vtbackend::settingsFor).
    proto::WireSessionSettings statedSettings {};
    vtpty::InheritedPty pty {};           ///< Its PTY.
    vtpty::AdoptedChild child {};         ///< Its child process; none for SessionDevice::PtyOnly.
    std::vector<uint8_t> terminalState;   ///< vtbackend::encodeTerminalState() of its terminal.
    std::vector<CarriedDelivery> carried; ///< QuiescedSession::carried, oldest first.
    /// The light/dark preference its palette was last seeded under; nullopt for HostedColorPreference, which
    /// no session records.
    std::optional<vtbackend::ColorPreference> colorPreference {};
    /// The command a restart stopped in it that it has not run since; nullopt for none.
    std::optional<CommandInterruption> interruption {};
    /// What of its device crosses. Absent from an image of a release that knew of no other, which is
    /// ChildOnPty; an image read by such a release finds a PtyOnly session's child no child of its own, and
    /// serves its PTY all the same.
    SessionDevice device = SessionDevice::ChildOnPty;

    /// Lists the fields for the handover codec. Field 6 carried the settings as vtbackend::Settings, which a
    /// client's session cannot be rebuilt from as it was built; retired, never to be reused.
    void visitFields(this auto& self, auto& visitor)
    {
        visitor(1, self.id);
        visitor(2, self.uuid);
        visitor(3, self.label);
        visitor(4, self.profileName);
        visitor(5, self.origin);
        visitor(7, self.pty);
        visitor(8, self.child);
        visitor(9, self.terminalState);
        visitor(10, self.carried);
        visitor(11, self.statedSettings);
        visitor(12, self.colorPreference);
        visitor(13, self.interruption);
        visitor(14, self.device);
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
    /// The light/dark preference the latest client stated, which new sessions are built under.
    vtbackend::ColorPreference defaultColorPreference = HostedColorPreference;

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
        visitor(10, self.defaultColorPreference);
    }
};

} // namespace vthost::handover
