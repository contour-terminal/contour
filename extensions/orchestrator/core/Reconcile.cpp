// SPDX-License-Identifier: Apache-2.0
#include <orchestrator/core/Reconcile.hpp>

#include <algorithm>
#include <array>
#include <ranges>
#include <utility>

#include <orchestrator/core/Naming.hpp>
#include <orchestrator/core/Overloaded.hpp>

namespace orchestrator
{

namespace
{
    // {{{ reconcile

    /// What reconciliation watches a run for, by the state it is in.
    enum class Watch : uint8_t
    {
        Nothing, ///< Nothing: it prepares before its tab, checks its worktree, or is finished.
        Opening, ///< It waits for its tab to open: a listed tab is adopted.
        Agent,   ///< Its agent runs: its status is followed, and a tab whose agent is gone is closed.
        Closing, ///< Its tab is being closed: it hears of it once no tab tagged with it is listed.
    };

    [[nodiscard]] Watch watchFor(Run const& run) noexcept
    {
        return std::visit(Overloaded {
                              [](Preparing const& preparing) {
                                  return preparing.step == PrepareStep::OpenPane ? Watch::Opening
                                                                                 : Watch::Nothing;
                              },
                              [](Ending const& ending) {
                                  return ending.step == EndStep::ClosePane ? Watch::Closing : Watch::Nothing;
                              },
                              [](Finished const&) { return Watch::Nothing; },
                              [](auto const&) { return Watch::Agent; }, // Running, Blocked, Done
                          },
                          run.state);
    }

    /// Reports a status the run has not taken in; a pane that reports nothing changes nothing.
    void followStatus(Run const& run, PaneFact const& pane, std::vector<Decision>& decisions)
    {
        if (pane.status && pane.status != run.lastStatus)
            decisions.emplace_back(StatusChanged { .run = run.id, .status = *pane.status });
    }

    // }}}
    // {{{ worktreeEntries

    /// The fields of a porcelain worktree listing.
    enum class Key : uint8_t
    {
        Worktree, ///< Starts an entry: its path.
        Head,     ///< Its HEAD commit.
        Branch,   ///< Its branch, as a full ref.
        Detached, ///< Its HEAD is detached.
        Bare,     ///< It is a bare repository.
        Locked,   ///< It is locked, with an optional reason.
        Prunable, ///< Its directory is gone, with an optional reason.
    };

    /// Whether a field carries a value.
    enum class Value : uint8_t
    {
        None,     ///< The field is the key alone.
        Required, ///< A space and a non-empty value follow.
        Optional, ///< Either.
    };

    /// A set of keys: the fields one may follow.
    using Keys = uint8_t;

    [[nodiscard]] constexpr Keys bit(Key key) noexcept
    {
        return static_cast<Keys>(1U << static_cast<unsigned>(key));
    }

    /// A field that follows the HEAD git names: `bare`, `HEAD`, or what HEAD is.
    constexpr auto AfterHead =
        Keys { bit(Key::Bare) | bit(Key::Head) | bit(Key::Branch) | bit(Key::Detached) };

    struct KeyRow
    {
        std::string_view word;
        Key key;
        Value value;
        Keys follows; ///< The fields it may come right after, in git's fixed order.
    };

    constexpr auto KeyRows = std::array {
        KeyRow { .word = "HEAD", .key = Key::Head, .value = Value::Required, .follows = bit(Key::Worktree) },
        KeyRow { .word = "bare", .key = Key::Bare, .value = Value::None, .follows = bit(Key::Worktree) },
        KeyRow { .word = "branch", .key = Key::Branch, .value = Value::Required, .follows = bit(Key::Head) },
        KeyRow { .word = "detached", .key = Key::Detached, .value = Value::None, .follows = bit(Key::Head) },
        KeyRow { .word = "locked", .key = Key::Locked, .value = Value::Optional, .follows = AfterHead },
        KeyRow { .word = "prunable",
                 .key = Key::Prunable,
                 .value = Value::Optional,
                 .follows = AfterHead | bit(Key::Locked) },
    };

    constexpr auto WorktreeWord = std::string_view { "worktree " };
    constexpr auto HeadsPrefix = std::string_view { "refs/heads/" };

    /// The entry being read: complete once its empty field is read.
    struct Pending
    {
        WorktreeEntry entry;
        Key last = Key::Worktree; ///< The field read last.
    };

    /// Whether a field was of the listing's shape.
    enum class Parsed : uint8_t
    {
        Malformed,
        Read,
    };

    /// A field a later git may add: its key is a lowercase word, `[a-z][a-z-]*`.
    [[nodiscard]] constexpr bool isLaterKey(std::string_view word) noexcept
    {
        return !word.empty() && word.front() >= 'a' && word.front() <= 'z'
               && std::ranges::all_of(word, [](char c) { return (c >= 'a' && c <= 'z') || c == '-'; });
    }

    [[nodiscard]] constexpr bool isObjectId(std::string_view text) noexcept
    {
        return (text.size() == 40 || text.size() == 64) && std::ranges::all_of(text, [](char c) {
                   return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
               });
    }

    [[nodiscard]] constexpr bool valueFits(Value value, std::string_view field, std::size_t space) noexcept
    {
        switch (value)
        {
            case Value::None: return space == std::string_view::npos;
            case Value::Required: return space != std::string_view::npos && space + 1 < field.size();
            case Value::Optional: return true;
        }
        return false;
    }

    /// Reads one field of an entry, its `worktree` field already read.
    [[nodiscard]] Parsed readField(Pending& pending, std::string_view field)
    {
        auto const space = field.find(' ');
        auto const word = field.substr(0, space);
        auto const row = std::ranges::find(KeyRows, word, &KeyRow::word);
        if (row == KeyRows.end())
        {
            // With -z nothing forges a field, so one a later git adds is skipped; it takes no place in the
            // order.
            return isLaterKey(word) ? Parsed::Read : Parsed::Malformed;
        }
        if ((row->follows & bit(pending.last)) == 0 || !valueFits(row->value, field, space))
            return Parsed::Malformed;
        pending.last = row->key;
        auto const value = space == std::string_view::npos ? std::string_view {} : field.substr(space + 1);
        switch (row->key)
        {
            case Key::Head: return isObjectId(value) ? Parsed::Read : Parsed::Malformed;
            case Key::Branch: {
                auto const branch = value.starts_with(HeadsPrefix) ? value.substr(HeadsPrefix.size()) : value;
                if (branch.empty())
                    return Parsed::Malformed;
                pending.entry.branch = std::string(branch);
                return Parsed::Read;
            }
            case Key::Bare: pending.entry.kind = WorktreeKind::Bare; return Parsed::Read;
            case Key::Detached:
            case Key::Locked:
            case Key::Prunable: return Parsed::Read;
            case Key::Worktree: return Parsed::Malformed;
        }
        return Parsed::Malformed;
    }

    /// Starts an entry: its first field must be its path.
    [[nodiscard]] std::optional<Pending> startEntry(std::string_view field)
    {
        if (!field.starts_with(WorktreeWord) || field.size() == WorktreeWord.size())
            return std::nullopt;
        return Pending { .entry = WorktreeEntry { .path = std::string(field.substr(WorktreeWord.size())),
                                                  .branch = std::nullopt,
                                                  .kind = WorktreeKind::Checkout },
                         .last = Key::Worktree };
    }

    // }}}
    // {{{ orphans

    /// Whether a run still accounts for its worktree: unfinished, or finished with its worktree kept.
    [[nodiscard]] bool holdsWorktree(Run const& run) noexcept
    {
        auto const* const finished = std::get_if<Finished>(&run.state);
        return finished == nullptr || finished->fate == Fate::Kept;
    }

    [[nodiscard]] bool accountsFor(Run const& run,
                                   WorktreeEntry const& entry,
                                   uint64_t listedMonoMs,
                                   PathStyle style)
    {
        return (holdsWorktree(run) || finishedSince(run, listedMonoMs))
               && (sameHostPath(entry.path, run.worktree, style) || entry.branch == run.branch);
    }

    [[nodiscard]] bool isAgentBranch(std::optional<std::string> const& branch) noexcept
    {
        return branch && branch->size() > BranchPrefix.size() && branch->starts_with(BranchPrefix);
    }

    /// Whether @p path lies below @p root, which is compared by sameHostPath()'s rule, any trailing separator
    /// aside: byte for byte on Posix; on Windows ASCII letters folded and `/` and `\` both separators, as
    /// git spells a Windows path `C:/…`.
    ///
    /// Not isStrictlyInside(), which takes @p root normalised, where only a Windows drive root ends in a
    /// separator: it would miss everything under a POSIX root of `/`, and under a root written with a
    /// trailing separator. Here any trailing separators are dropped first; the comparison is the same.
    [[nodiscard]] bool isUnder(std::string_view path, std::string_view root, PathStyle style) noexcept
    {
        if (root.empty())
            return false;
        auto const separators =
            style == PathStyle::Windows ? std::string_view { "/\\" } : std::string_view { "/" };
        auto const last = root.find_last_not_of(separators);
        auto const base = last == std::string_view::npos ? std::string_view {} : root.substr(0, last + 1);
        auto const rest = afterHostPath(path, base, style);
        return rest && rest->size() > 1 && separators.contains(rest->front());
    }

    // }}}
} // namespace

std::vector<Decision> reconcile(std::span<Run const> runs,
                                std::span<PaneFact const> panes,
                                uint64_t requestedMonoMs,
                                PaneListing listing)
{
    auto decisions = std::vector<Decision> {};
    for (auto const& run: runs)
    {
        // A listing asked for before the run's last change may predate its tab.
        if (run.changedMonoMs >= requestedMonoMs)
            continue;
        auto const isTagged = [&](PaneFact const& pane) {
            return pane.tag == run.id;
        };
        auto const isAlive = [&](PaneFact const& pane) {
            return isTagged(pane) && pane.program == ProgramLiveness::Alive;
        };
        auto const closeWhere = [&](auto const& wanted) {
            for (auto const& pane: panes | std::views::filter(wanted))
                decisions.emplace_back(ClosePane { .pane = pane.pane });
        };
        auto const alive = std::ranges::find_if(panes, isAlive);
        switch (watchFor(run))
        {
            case Watch::Nothing: break;
            case Watch::Opening:
                // A tab whose agent is gone is left to the run's own close, once its opening fails.
                if (alive != panes.end())
                {
                    decisions.emplace_back(Adopt { .run = run.id, .pane = alive->pane });
                    followStatus(run, *alive, decisions);
                }
                break;
            case Watch::Agent:
                if (std::ranges::none_of(panes, isTagged))
                {
                    // Only the host's listing tells that a tab is gone: the driver's own records list none.
                    if (listing == PaneListing::Tagged)
                        decisions.emplace_back(PaneWentAway { .run = run.id });
                    break;
                }
                closeWhere([&](PaneFact const& pane) {
                    return isTagged(pane) && pane.program == ProgramLiveness::Gone;
                });
                if (alive != panes.end())
                    followStatus(run, *alive, decisions);
                break;
            case Watch::Closing:
                if (std::ranges::none_of(panes, isTagged))
                {
                    // A listing of the driver's own records cannot see a tab it never recorded.
                    if (listing == PaneListing::Tagged || run.pane)
                        decisions.emplace_back(PaneWentAway { .run = run.id });
                    break;
                }
                // Asked again on every listing: the close may have been lost.
                closeWhere(isTagged);
                break;
        }
    }
    return decisions;
}

std::vector<WorktreeEntry> worktreeEntries(std::string_view porcelain)
{
    // Only fields a NUL closes count: whatever follows the last one was cut.
    auto const lastNul = porcelain.rfind('\0');
    if (lastNul == std::string_view::npos)
        return {};
    auto entries = std::vector<WorktreeEntry> {};
    auto pending = std::optional<Pending> {};
    for (auto const part: std::views::split(porcelain.substr(0, lastNul), '\0'))
    {
        auto const field = std::string_view(part.begin(), part.end());
        if (!pending)
        {
            pending = startEntry(field);
            if (!pending)
                break;
            continue;
        }
        if (field.empty())
        {
            // An entry ends after what its HEAD is; a path alone is no entry.
            if (pending->last == Key::Worktree)
                break;
            entries.push_back(std::move(pending->entry));
            pending.reset();
            continue;
        }
        if (readField(*pending, field) == Parsed::Malformed)
            break;
    }
    // An entry its empty field did not close was cut or malformed, and is left out with all after it.
    return entries;
}

bool finishedSince(Run const& run, uint64_t listedMonoMs) noexcept
{
    // A listing asked for in the very step a run finished in was asked for after it: the host runs it later.
    return std::holds_alternative<Finished>(run.state) && run.changedMonoMs > listedMonoMs;
}

std::vector<WorktreeEntry> orphans(std::span<Run const> runs,
                                   std::string_view worktreeRoot,
                                   std::span<WorktreeEntry const> entries,
                                   uint64_t listedMonoMs,
                                   PathStyle style)
{
    auto found = std::vector<WorktreeEntry> {};
    for (auto const& entry: entries | std::views::drop(1))
    {
        if (!isAgentBranch(entry.branch) && !isUnder(entry.path, worktreeRoot, style))
            continue;
        if (std::ranges::any_of(runs,
                                [&](Run const& run) { return accountsFor(run, entry, listedMonoMs, style); }))
            continue;
        found.push_back(entry);
    }
    return found;
}

std::optional<std::string_view> mainCheckout(std::span<WorktreeEntry const> entries) noexcept
{
    if (entries.empty() || entries.front().kind == WorktreeKind::Bare)
        return std::nullopt;
    return std::string_view { entries.front().path };
}

} // namespace orchestrator
