// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <format>
#include <string_view>

namespace vtbackend
{

/// Whether OSC 3008 may stand in for a shell integration that is not there.
///
/// A configuration policy, fixed at construction: two differently-configured terminals are two
/// different terminals, not one terminal in two states.
enum class ContextMarkPolicy : uint8_t
{
    Never = 0, ///< OSC 3008 never stamps a mark. Restores the behaviour from before it was consumed.
    WhenAlone, ///< OSC 3008 stamps marks only while OSC 133 has not spoken. The default.
};

/// Which of the two shell-boundary protocols owns the semantic line marks in this session.
///
/// Two independent streams describe the same prompt cycle: OSC 133, which a user opted into by
/// installing a shell integration, and OSC 3008, which systemd 258+ emits from /etc/profile.d with no
/// user action at all. Both would otherwise stamp LineFlag::Marked / OutputStart / CommandEnd and both
/// would feed the CommandBlockStore, so exactly one of them is the source of record.
///
/// The order is one-way. OSC 133 is the richer protocol -- it alone marks the prompt/input border
/// (;B) -- and it is the one the user asked for, so its first sequence wins the session for good.
/// OSC 3008 can take the marks only in its absence, and can never take them back. That asymmetry IS
/// the hysteresis: without it a session would flap between marker sources depending on which stream
/// happened to be quiet.
enum class MarkOwner : uint8_t
{
    /// Nothing has claimed the session yet, or OSC 3008 has claimed it for less than one full prompt
    /// cycle.
    ///
    /// OSC 3008 STAMPS FLAGS in this state -- they are terminal memory, they are idempotent bits on a
    /// logical line head, and they cost nothing if OSC 133 turns up and stamps the same heads -- but
    /// feeds NO command blocks, because those are not idempotent.
    Undecided = 0,

    /// OSC 133 has been seen. It stamps every mark and feeds every command block; OSC 3008 contributes
    /// metadata only -- cwd, exit=crash|interrupt, signal=, identity -- and never a mark.
    ShellIntegration,

    /// One complete OSC 3008 command cycle passed with no OSC 133 in sight. OSC 3008 stamps the marks
    /// and feeds the command blocks, until and unless OSC 133 speaks.
    ContextSignalling,
};

/// How many complete OSC 3008 command cycles must pass before OSC 3008 may feed the command blocks.
///
/// One: by the end of the first cycle a shell integration that exists has certainly spoken, since
/// OSC 133;C lands in the same cycle, immediately after the PS0 that opens the 3008 command context.
/// A constant rather than a literal so raising the bar is a one-line change and not a hunt through the
/// logic.
constexpr inline uint8_t ContextMarkDecisionCycles = 1;

/// Decides, per session, which boundary protocol may stamp semantic marks and feed the
/// CommandBlockStore. @see MarkOwner.
///
/// Deliberately dependency-free: this is the one decision in the OSC 3008 feature that has to be
/// exercised in isolation -- every interleaving of the two streams, and a shell integration sourced
/// halfway through a session -- with no Grid, no Screen and no Terminal behind it.
///
/// NOT reset by RIS or DECSTR. The OSC 3008 ancestry is reset-immune by specification, and this is a
/// property of the shells attached to the pty rather than of the screen; a program that emits RIS must
/// not be able to flip which protocol owns the user's markers. A vhangup -- a new pty, and so a new
/// Terminal -- starts a fresh arbiter, which is the only reset there is.
class MarkArbiter
{
  public:
    /// @param policy Whether OSC 3008 may stand in for OSC 133 at all.
    explicit constexpr MarkArbiter(ContextMarkPolicy policy = ContextMarkPolicy::WhenAlone) noexcept:
        _policy { policy }
    {
    }

    /// Replaces the policy, for a configuration reload: live reconfiguration is the feature. Who owns the
    /// marks is left as it is; the policy is read afresh by every question below.
    /// @param policy Whether OSC 3008 may stand in for OSC 133 at all.
    constexpr void setPolicy(ContextMarkPolicy policy) noexcept { _policy = policy; }

    /// Who owns the marks right now.
    [[nodiscard]] constexpr MarkOwner owner() const noexcept { return _owner; }

    /// Records that an OSC 133 sequence was seen. Terminal: this settles the session for good.
    constexpr void observedShellIntegration() noexcept { _owner = MarkOwner::ShellIntegration; }

    /// Records that an OSC 3008 `type=command` context was pushed.
    constexpr void observedContextCommandStart() noexcept
    {
        if (_owner == MarkOwner::Undecided)
            _hasOpenCommand = true;
    }

    /// Records that an OSC 3008 `type=command` context was closed.
    ///
    /// The decision point. A cycle that both began and ended without OSC 133 hands OSC 3008 the
    /// session. Counted at the END rather than the start so the arbiter decides on a cycle BOUNDARY
    /// and not on an arbitrary sequence in the middle of one, where the two streams interleave -- and
    /// "first completed cycle" is a stable property of the session where "first sequence seen" is a
    /// coin flip on hook-install order.
    constexpr void observedContextCommandEnd() noexcept
    {
        if (_owner != MarkOwner::Undecided || !_hasOpenCommand)
            return;
        _hasOpenCommand = false;
        if (++_completedCycles >= ContextMarkDecisionCycles)
            _owner = MarkOwner::ContextSignalling;
    }

    /// Whether OSC 3008 may stamp LineFlags right now.
    [[nodiscard]] constexpr bool contextMayMark() const noexcept
    {
        return _policy != ContextMarkPolicy::Never && _owner != MarkOwner::ShellIntegration;
    }

    /// Whether OSC 3008 may open, start and finish command blocks in the CommandBlockStore.
    ///
    /// Strictly narrower than @ref contextMayMark: a flag stamped twice is one bit, but a prompt start
    /// delivered twice is two blocks -- the second discarding the first -- and a 3008 command context
    /// racing OSC 133;C would start the same command twice.
    [[nodiscard]] constexpr bool contextMayFeedBlocks() const noexcept
    {
        return _policy != ContextMarkPolicy::Never && _owner == MarkOwner::ContextSignalling;
    }

    /// Whether OSC 3008 may add how a command ended to the command block OSC 133 tracks for it, opening and
    /// closing nothing.
    ///
    /// The metadata OSC 3008 still contributes once OSC 133 owns the session: an `end=` knows a signal or a
    /// crash that OSC 133;D's bare number cannot say. Not a mark, so ContextMarkPolicy::Never does not
    /// withhold it; while OSC 3008 feeds the blocks itself, its `end=` closes the block with the outcome
    /// instead.
    [[nodiscard]] constexpr bool contextMayEnrich() const noexcept
    {
        return _owner == MarkOwner::ShellIntegration;
    }

  private:
    ContextMarkPolicy _policy;
    MarkOwner _owner = MarkOwner::Undecided;
    uint8_t _completedCycles = 0;
    bool _hasOpenCommand = false; ///< Predicate: is a `type=command` context open right now?
};

} // namespace vtbackend

/// Spelled as the configuration spells it, which is what `contour generate config` writes back.
template <>
struct std::formatter<vtbackend::ContextMarkPolicy>: formatter<std::string_view>
{
    auto format(vtbackend::ContextMarkPolicy value, auto& ctx) const
    {
        string_view name;
        switch (value)
        {
            case vtbackend::ContextMarkPolicy::Never: name = "never"; break;
            case vtbackend::ContextMarkPolicy::WhenAlone: name = "when_alone"; break;
        }
        return formatter<string_view>::format(name, ctx);
    }
};
