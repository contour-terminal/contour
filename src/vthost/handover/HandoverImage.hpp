// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The live-upgrade handover image: everything that crosses the daemon's in-place `execv` on POSIX, or that
/// its successor process inherits on Windows, as plain data.
///
/// Nothing here owns a resource. A descriptor or a handle is a number the new image inherits; turning it back
/// into a PTY, a child or a listener is the adopter's job (@see vtpty::UnixPty::adopt, vtpty::ConPty::adopt,
/// vtpty::Process::adopt). A listener's and the lease's descriptor or handle crosses as a
/// vtpty::CrossingHandle, whatever the platform.

#include <vtbackend/core/ColorPalette.hpp>

#include <vtpty/CrossingHandle.hpp>
#include <vtpty/ImageSize.hpp>
#include <vtpty/InheritedPty.hpp>
#include <vtpty/Process.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

#include <vtcontrol/ObjectRef.hpp>
#include <vtcontrol/Uuid.hpp>
#include <vthost/AttentionLedger.hpp>
#include <vthost/CarriedDelivery.hpp>
#include <vthost/CommandInterruption.hpp>
#include <vthost/ObjectRegistry.hpp>
#include <vthost/ProfileCatalog.hpp>
#include <vthost/SessionSettings.hpp>
#include <vthost/proto/Pdu.hpp>
#include <vtworkspace/Primitives.hpp>
#include <vtworkspace/WorkspaceState.hpp>

namespace vthost::handover
{

/// The newest handover format this build writes.
///
/// The encoding is tolerant (@see vtbackend::detail::FieldEncoder): a field added or dropped later is not a
/// new version. Only a change an older reader cannot skip over -- a field whose meaning changes under the
/// same id, a new framing -- is. Format 2 carries a listener's descriptor or handle, and the lease's, as one
/// field each (vtpty::CrossingHandle), where format 1 carried a descriptor and a Windows handle apart. Under
/// the same field ids, InheritedConPty's handles and AdoptedChild::processHandle changed encoding too: each
/// was a uint64 with none as 0, and is a nested int64 with none as -1.
inline constexpr uint32_t HandoverFormatVersion = 2;

/// The oldest handover format this build can still read.
inline constexpr uint32_t OldestReadableHandoverFormat = 2;

// The object registry's own vocabulary (@see ObjectRegistry::snapshot()), named here as the image names it.
using vthost::BoundObject;
using vthost::NamedObject;
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
    /// The listening socket: its descriptor on POSIX, its handle on Windows.
    vtpty::CrossingHandle handle {};
    /// The identity the listener presents where the daemon made it up itself (a self-signed certificate):
    /// presented again by the new image, so that a client that trusted it on first use goes on trusting it.
    /// nullopt for a listener without TLS, or with a configured certificate, which the new image reads again.
    std::optional<TlsIdentity> tlsIdentity {};

    /// Lists the fields for the handover codec. Field 4 carried the Windows socket apart from the descriptor
    /// in format 1; retired, never to be reused.
    void visitFields(this auto& self, auto& visitor)
    {
        visitor(1, self.name);
        visitor(2, self.handle);
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
    /// A child the daemon's PTY keeper holds, on a PTY: the PTY crosses, and the child is named by
    /// HandoverSession::keeperSession rather than carried (HandoverSession::child names none).
    KeeperChild,
};

/// One hosted session, as it crosses the exec.
struct HandoverSession
{
    vtworkspace::SessionId id {};                    ///< Its id in the model.
    vtcontrol::Uuid uuid {};                         ///< Its UUID.
    std::string name;                                ///< Its name.
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
    /// The keeper's name for the child, for SessionDevice::KeeperChild (a keeper::KeeperSessionId's value);
    /// 0 otherwise.
    uint64_t keeperSession = 0;

    /// Lists the fields for the handover codec. Field 6 carried the settings as vtbackend::Settings, which a
    /// client's session cannot be rebuilt from as it was built; retired, never to be reused.
    void visitFields(this auto& self, auto& visitor)
    {
        visitor(1, self.id);
        visitor(2, self.uuid);
        visitor(3, self.name);
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
        visitor(15, self.keeperSession);
    }
};

/// One pane's entry of the attention ledger, as it crosses the exec.
struct HandoverPaneAttention
{
    uint64_t session = 0;    ///< The pane's session.
    PaneAttention attention; ///< What the ledger states of it.

    /// Lists the fields for the handover codec.
    void visitFields(this auto& self, auto& visitor)
    {
        visitor(1, self.session);
        visitor(2, self.attention);
    }

    bool operator==(HandoverPaneAttention const&) const = default;
};

/// The daemon's attention ledger, as it crosses the exec: what each pane raised and what a window saw of it,
/// and the serial counter, so that the new image never hands out a serial a window already holds.
struct HandoverAttention
{
    uint64_t lastSerial = 0;                  ///< The last serial the ledger handed out.
    std::vector<HandoverPaneAttention> panes; ///< Every pane's entry, by ascending session.

    /// Lists the fields for the handover codec.
    void visitFields(this auto& self, auto& visitor)
    {
        visitor(1, self.lastSerial);
        visitor(2, self.panes);
    }

    bool operator==(HandoverAttention const&) const = default;
};

/// A workspace a promotion made, by its creation token, as it crosses the exec (SessionHost::MadeForToken).
/// The connection whose ask made it does not cross: every connection ends at the exec, and the new image
/// numbers its connections afresh, so an old one's id would name a new, unrelated connection.
struct HandoverMadeForToken
{
    std::array<uint8_t, 16>
        token {};           ///< The promotion's creation token (proto::CreationToken), byte for byte.
    uint64_t workspace = 0; ///< The workspace it made.

    /// Lists the fields for the handover codec.
    void visitFields(this auto& self, auto& visitor)
    {
        visitor(1, self.token);
        visitor(2, self.workspace);
    }

    bool operator==(HandoverMadeForToken const&) const = default;
};

/// Whether a visitFields() pass over a @p Self encodes: the handover codec writes an image from a const one
/// and reads into a mutable one, so the constness of `self` is what tells a field that may be left out of the
/// written image from one that is always read.
/// @tparam Self The type of `self` in visitFields() (decltype(self)).
template <typename Self>
inline constexpr bool IsEncodingPass = std::is_const_v<std::remove_reference_t<Self>>;
static_assert(IsEncodingPass<int const&> && !IsEncodingPass<int&>);

/// Everything the old image hands the new one.
struct HandoverImage
{
    uint64_t daemonInstance = 0;                ///< Which daemon instance wrote it.
    std::string fromRelease;                    ///< The release that wrote it.
    vtworkspace::WorkspaceTreeState tree {};    ///< The workspaces, tabs and panes.
    vtworkspace::ModelIdState modelIds {};      ///< The model's id counters.
    vtworkspace::SessionId nextSessionId { 1 }; ///< The id the next session is minted with.
    /// The model id of the workspace a creation naming none lands in (SessionHost's own workspace); 0 for an
    /// image from before the field, which has the first one.
    uint64_t defaultWorkspace = 0;
    ObjectRegistrySnapshot objects {};        ///< The object registry.
    std::vector<HandoverSession> sessions;    ///< Every hosted session.
    std::vector<InheritedListener> listeners; ///< The listeners to keep serving on.
    /// The descriptor (POSIX) or handle (Windows) the daemon image lease is held through; none for none.
    vtpty::CrossingHandle lease {};
    /// The light/dark preference the latest client stated, which new sessions are built under.
    vtbackend::ColorPreference defaultColorPreference = HostedColorPreference;
    /// How many live upgrades the daemon that wrote it had completed
    /// (proto::DaemonUpgradeRecord::generation); the image that reads it serves as one more. An image from
    /// before the field reads as 0.
    uint64_t upgradeGeneration = 0;
    /// The cell size in pixels a client last stated (SessionHost::learnedCellPixelSize()); none when none
    /// ever did, and for an image from before the field, which has none: the successor uses its default
    /// until a client reports.
    /// Unchecked: the host that adopts the image bounds it first.
    std::optional<vtpty::ImageSize> cellPixelSize {};
    /// The attention ledger (SessionHost::paneAttention()); empty for an image from before the field, whose
    /// panes then start with nothing raised.
    HandoverAttention attention {};
    /// The workspaces recent promotions made, by creation token, oldest first (SessionHost::madeForTokens()):
    /// a promotion asked again across the upgrade selects what its first ask made instead of making a second
    /// workspace. Empty for an image from before the field.
    std::vector<HandoverMadeForToken> madeForTokens {};

    /// Lists the fields for the handover codec. Field 12 carried the Windows lease handle apart from the
    /// descriptor in format 1, and field 14 the release records of the follow model; both retired, never to
    /// be reused. The decoder skips a field it does not list, so an image from before reads on.
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
        visitor(9, self.lease);
        visitor(10, self.defaultColorPreference);
        visitor(11, self.upgradeGeneration);
        visitor(13, self.cellPixelSize);
        visitor(15, self.defaultWorkspace);
        // Read always, written only when the ledger holds something: the image of a host that holds none is
        // written byte for byte as a release before the field wrote it (testing::RecordedHandoverNames).
        if (!IsEncodingPass<decltype(self)> || self.attention != HandoverAttention {})
            visitor(16, self.attention);
        // Likewise: written only when a promotion made a workspace.
        if (!IsEncodingPass<decltype(self)> || !self.madeForTokens.empty())
            visitor(17, self.madeForTokens);
    }
};

} // namespace vthost::handover
