// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/shell/CommandBlock.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <random>
#include <vector>

namespace vtbackend
{

/// A mode 2034 session token: 64 bits, carried as the four 16-bit parameters T1..T4 of the DECSET 2034 reply
/// and of every SBQUERY.
using SemanticBlockToken = std::array<uint16_t, 4>;

/// The largest value a part of a SemanticBlockToken takes: one short of 0xFFFF.
///
/// A CSI parameter saturates: the parser reads every number past 0xFFFF as 0xFFFF, so a query's 0xFFFF
/// stands for all of them. A token part of 0xFFFF would be matched by any number out of range, and SBQUERY
/// refuses that value instead.
inline constexpr uint16_t MaxSemanticBlockTokenPart = 0xFFFE;

/// Mints the token of each mode 2034 reader session. Injected, so that a test can pin the token a session is
/// handed. Every part it mints is at most MaxSemanticBlockTokenPart: a part above that never authenticates.
using SemanticBlockTokenSource = std::function<SemanticBlockToken()>;

/// Draws a session token from @p bits, every part uniform in [0, MaxSemanticBlockTokenPart].
/// @param bits A source of random bits.
/// @return The token.
template <std::uniform_random_bit_generator Bits>
[[nodiscard]] SemanticBlockToken drawSemanticBlockToken(Bits& bits)
{
    auto parts = std::uniform_int_distribution<unsigned> { 0, MaxSemanticBlockTokenPart };
    auto token = SemanticBlockToken {};
    for (auto& part: token)
        part = static_cast<uint16_t>(parts(bits));
    return token;
}

/// What production passes for a SemanticBlockTokenSource.
/// @return A source drawing every token from std::random_device.
[[nodiscard]] SemanticBlockTokenSource randomSemanticBlockTokens();

/// SBQUERY (CSI > Ps ; Pn b) query type parameter values.
struct SBQueryType
{
    static constexpr unsigned LastCommand = 1;          ///< Last completed command block.
    static constexpr unsigned LastNumberOfCommands = 2; ///< Last N completed command blocks (Pn = count).
    static constexpr unsigned InProgress = 3;           ///< Current in-progress command.
    /// The block whose id is `Ph * 65536 + Pn`, in any state (@see sbQueryBlockId).
    static constexpr unsigned BlockById = 4;
};

/// The reader side of DEC mode 2034 (Semantic Block Reader Protocol): the session token, and which of the
/// terminal's command blocks a reader may see.
///
/// It keeps NO blocks of its own. The blocks are the terminal's CommandBlockStore, recorded whether or not
/// mode 2034 is on; this decides only what the protocol may report. A reader sees the blocks whose command
/// had not started when it enabled the mode -- the prompt on screen then, and every block after it -- so
/// enabling 2034 is never a way to read back commands that ran before.
class SemanticBlockTracker
{
  public:
    /// 64-bit session token represented as 4 x uint16_t for CSI parameter encoding.
    using Token = SemanticBlockToken;

    /// @param tokens Where each session's token comes from; drawn once per enable().
    explicit SemanticBlockTracker(SemanticBlockTokenSource tokens);

    /// Starts a reader session: mints a fresh token and hides every block whose command has started already.
    /// A block still at its prompt stays visible: its command is yet to come.
    /// @param store The terminal's blocks, as they stand when the session starts.
    void enable(CommandBlockStore const& store);

    /// Ends the reader session: the token is invalidated and nothing is reported any more.
    void disable() noexcept;

    /// @return Whether a reader session is active.
    [[nodiscard]] bool isEnabled() const noexcept;

    /// @return The current session token, while a session is active.
    [[nodiscard]] std::optional<Token> const& token() const noexcept;

    /// @param candidate The token a query carried.
    /// @return Whether it matches the current session's.
    [[nodiscard]] bool validateToken(Token const& candidate) const noexcept;

    /// The @p count most recent blocks this reader may see that FINISHED with a reported end, newest first.
    /// A block a newer prompt closed implicitly (a nested shell) is left out: it has no exit code to report.
    /// @param store The terminal's blocks.
    /// @param count How many at most.
    /// @return Pointers into @p store, valid until it next changes.
    [[nodiscard]] std::vector<CommandBlockRecord const*> finishedBlocks(CommandBlockStore const& store,
                                                                        size_t count) const;

    /// @param store The terminal's blocks.
    /// @return The block in progress (prompting or running), if this reader may see it, else nullptr.
    [[nodiscard]] CommandBlockRecord const* inProgressBlock(CommandBlockStore const& store) const noexcept;

    /// @param store The terminal's blocks.
    /// @param id A block's id, in the store's current id epoch.
    /// @return The block @p id names, in any state, if this reader may see it, else nullptr.
    [[nodiscard]] CommandBlockRecord const* blockById(CommandBlockStore const& store,
                                                      CommandBlockId id) const noexcept;

  private:
    /// Whether @p record's command had not started when this reader session did.
    /// @param store The store @p record is in, whose id epoch says whether the floor still counts its ids.
    /// @param record The block.
    [[nodiscard]] bool isVisible(CommandBlockStore const& store,
                                 CommandBlockRecord const& record) const noexcept;

    SemanticBlockTokenSource _tokens; ///< Fixed at construction.
    std::optional<Token> _token;
    CommandBlockId _hiddenThrough {}; ///< Blocks up to this id had started their commands before the session.
    uint64_t _hiddenEpoch = 0;        ///< The CommandBlockStore::idEpoch() @ref _hiddenThrough counts in.
};

} // namespace vtbackend
