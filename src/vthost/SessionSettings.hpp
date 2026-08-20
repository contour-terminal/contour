// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What settings a DAEMON-HOSTED session's terminal has: the invariants every hosted session must
/// satisfy whatever configured it, and the translation between those settings and their wire form.
///
/// Both directions of the wire translation live here on purpose. The client encodes and the server
/// decodes, and if the two ever disagreed about a field the symptom would be a session that emulates
/// almost right — so they read one another rather than two parallel field lists.
///
/// Deliberately free of I/O, sockets and event loops, so `SessionSettings_test.cpp` drives every
/// decision directly.

#include <vtbackend/screen/Settings.hpp>

#include <vthost/proto/Pdu.hpp>

namespace vthost
{

/// The scrollback a hosted session keeps when nothing configured it. THE canonical statement of why
/// a session host needs one; elsewhere just points here.
///
/// A bare `vtbackend::Settings` leaves `maxHistoryLineCount` at its variant's first alternative,
/// `LineCount(0)` — defensible for a terminal that only ever shows a page, ruinous for a session
/// HOST. With no history `Grid::stableRangeFloor()` equals the stream base after every scroll, so
/// nothing the connection has not already seen can be NAMED in an incremental delta; every batch
/// that scrolled is promoted to a full-grid snapshot instead (@see NativeSession::pushDelta). The
/// result is correct and ruinously expensive, and no client can ever be served scrollback at all.
/// Matches the GUI profile default (`history.limit`).
constexpr auto DefaultSessionHistoryLineCount = 1000;

/// The largest finite scrollback a hosted session is given, however much was asked for. A ceiling
/// rather than a judgement about what is useful: the request can arrive from a remote client, and a
/// line count is a per-session memory budget. `Infinite` is NOT capped — it is a deliberate choice
/// the configuration offers, and a client asking for it has already authenticated.
constexpr auto MaxSessionHistoryLineCount = 1'000'000;

/// The largest sixel color-register count a hosted session is given (vtbackend's own default is
/// 256). Same reasoning as the history ceiling: registers are allocated per session.
constexpr auto MaxSessionImageRegisterCount = 65536U;

/// The most word-delimiter codepoints a hosted session is given. A delimiter set is a handful of
/// punctuation characters; a megabyte of them is a mistake or an attack, never a preference.
constexpr auto MaxSessionWordDelimiters = 256U;

/// The invariants a hosted session's terminal settings must satisfy, applied to @p settings whatever
/// produced them — the daemon's own profile, a client's stated preference, or a bare
/// `vtbackend::Settings`.
///
/// Enforced here, at the mechanism, rather than only where the daemon builds its configuration:
/// `SessionHost` has many callers, and each new one would otherwise re-open the same defect.
///
/// - `maxHistoryLineCount` is replaced by @ref DefaultSessionHistoryLineCount when it is zero, and
///   capped at @ref MaxSessionHistoryLineCount when finite; `Infinite` passes through. Note the
///   default is not a FLOOR: a profile asking for 200 lines gets 200, because that configuration
///   works — only zero breaks delta addressing.
/// - `maxImageRegisterCount` is clamped to `[1, MaxSessionImageRegisterCount]`.
/// - `wordDelimiters` is truncated to @ref MaxSessionWordDelimiters codepoints.
/// - `goodImageProtocol` is forced ON. The knob is maturing toward always-on and non-configurable,
///   so daemon mode adopts that end state now — which is also why it is absent from
///   @ref proto::WireSessionSettings. A profile setting it false is deliberately overridden.
/// - Every other field passes through untouched.
///
/// @param settings The settings to normalize (taken by value; the adjusted copy is returned).
/// @return The settings a hosted session may actually be built with.
[[nodiscard]] vtbackend::Settings hostedSessionSettings(vtbackend::Settings settings);

/// The factory settings a daemon hosts sessions with when no profile was loaded.
/// @return @ref hostedSessionSettings applied to a bare `vtbackend::Settings`.
[[nodiscard]] vtbackend::Settings defaultSessionSettings();

/// The emulation-relevant subset of @p settings, in wire form, for a client to state as its
/// preference for the sessions it creates.
/// @param settings The client's own terminal settings.
/// @return The wire form; @see proto::WireSessionSettings for the two fields it omits and why.
[[nodiscard]] proto::WireSessionSettings toWireSessionSettings(vtbackend::Settings const& settings);

/// @p wire applied over @p base — the settings a client asked for, layered onto the ones this daemon
/// would otherwise have used.
///
/// Layered rather than translated outright so a field the wire does not carry (or carries
/// nonsensically) falls back to the DAEMON's value rather than to a wire default nobody chose. Every
/// field is validated, because a client is a peer and not a caller: an unknown `terminalId` number
/// keeps @p base's, an unrecognised frozen mode number is dropped, and the numeric limits are
/// clamped by @ref hostedSessionSettings, which this ends in.
/// @param wire The client's stated preference.
/// @param base The settings to fall back to, field by field.
/// @return The settings to build the session with.
[[nodiscard]] vtbackend::Settings fromWireSessionSettings(proto::WireSessionSettings const& wire,
                                                          vtbackend::Settings const& base);

} // namespace vthost
