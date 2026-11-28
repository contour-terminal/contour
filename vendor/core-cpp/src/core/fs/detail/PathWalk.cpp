// SPDX-License-Identifier: Apache-2.0
#include <core/fs/detail/PathWalk.hpp>

#include <core/fs/FsError.hpp>
#include <core/fs/PathPolicy.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace core::fs::detail
{

namespace
{

    std::unexpected<FsError> refuse(FsErrorCode code)
    {
        return std::unexpected { FsError { .code = code, .systemCode = 0, .context = {} } };
    }

    /// The length of the well-formed UTF-8 sequence that starts @p text, or 0 where none does
    /// (Unicode 15, table 3-7: no overlong form, no surrogate, nothing past U+10FFFF).
    std::size_t sequenceLength(std::string_view text) noexcept
    {
        struct Lead
        {
            std::uint8_t first;     ///< The lowest lead byte of the row.
            std::uint8_t last;      ///< The highest.
            std::uint8_t secondMin; ///< The range the second byte must be in.
            std::uint8_t secondMax;
            std::uint8_t length; ///< Bytes in the sequence.
        };
        static constexpr auto Leads = std::to_array<Lead>({
            { .first = 0x00, .last = 0x7F, .secondMin = 0x00, .secondMax = 0x00, .length = 1 },
            { .first = 0xC2, .last = 0xDF, .secondMin = 0x80, .secondMax = 0xBF, .length = 2 },
            { .first = 0xE0, .last = 0xE0, .secondMin = 0xA0, .secondMax = 0xBF, .length = 3 },
            { .first = 0xE1, .last = 0xEC, .secondMin = 0x80, .secondMax = 0xBF, .length = 3 },
            { .first = 0xED, .last = 0xED, .secondMin = 0x80, .secondMax = 0x9F, .length = 3 },
            { .first = 0xEE, .last = 0xEF, .secondMin = 0x80, .secondMax = 0xBF, .length = 3 },
            { .first = 0xF0, .last = 0xF0, .secondMin = 0x90, .secondMax = 0xBF, .length = 4 },
            { .first = 0xF1, .last = 0xF3, .secondMin = 0x80, .secondMax = 0xBF, .length = 4 },
            { .first = 0xF4, .last = 0xF4, .secondMin = 0x80, .secondMax = 0x8F, .length = 4 },
        });
        auto const byteAt = [text](std::size_t index) {
            return static_cast<std::uint8_t>(text[index]);
        };
        auto const lead = std::ranges::find_if(
            Leads, [first = byteAt(0)](Lead const& row) { return row.first <= first && first <= row.last; });
        if (lead == Leads.end() || text.size() < lead->length)
            return 0;
        if (lead->length == 1)
            return 1;
        if (byteAt(1) < lead->secondMin || byteAt(1) > lead->secondMax)
            return 0;
        auto const rest = text.substr(2, lead->length - 2);
        auto const continuation = [](char c) {
            return (static_cast<std::uint8_t>(c) & 0xC0U) == 0x80U;
        };
        return std::ranges::all_of(rest, continuation) ? lead->length : 0;
    }

    /// NUL is refused under every policy, and a name crosses the API as UTF-8.
    FsResult<void> checkText(std::string_view text)
    {
        if (text.contains('\0'))
            return refuse(FsErrorCode::Invalid);
        if (!isUtf8(text))
            return refuse(FsErrorCode::IllegalByteSequence);
        return {};
    }

    std::vector<std::string_view> splitOn(std::string_view text, std::string_view separators)
    {
        auto pieces = std::vector<std::string_view> {};
        while (!text.empty())
        {
            auto const end = text.find_first_of(separators);
            auto const piece = text.substr(0, end);
            if (!piece.empty())
                pieces.push_back(piece);
            text.remove_prefix(end == std::string_view::npos ? text.size() : end + 1);
        }
        return pieces;
    }

    bool equalsIgnoringAsciiCase(std::string_view a, std::string_view b) noexcept
    {
        auto const fold = [](char c) {
            return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
        };
        return std::ranges::equal(a, b, {}, fold, fold);
    }

    /// The part of @p name Windows compares against its device names: up to the first dot, trailing
    /// spaces dropped, so `nul.txt` and `NUL ` are both the device.
    std::string_view stemOf(std::string_view name) noexcept
    {
        auto stem = name.substr(0, name.find('.'));
        while (stem.ends_with(' '))
            stem.remove_suffix(1);
        return stem;
    }

    FsResult<void> checkComponent(std::string_view name, PathPolicy const& policy)
    {
        if (name == "." || name == "..")
            return {};
        if (name.find_first_of(policy.forbiddenCharacters) != std::string_view::npos)
            return refuse(FsErrorCode::Invalid);
        if (policy.trailing == TrailingDotsAndSpaces::Refused && (name.ends_with('.') || name.ends_with(' ')))
            return refuse(FsErrorCode::Invalid);
        auto const stem = stemOf(name);
        if (std::ranges::any_of(policy.reservedNames, [stem](std::string_view reserved) {
                return equalsIgnoringAsciiCase(stem, reserved);
            }))
            return refuse(FsErrorCode::NotPermitted);
        return {};
    }

    /// Which kind of text @c componentsOf splits.
    enum class TextKind : std::uint8_t
    {
        Path,          ///< A path argument, split on the policy's separators.
        SymlinkTarget, ///< A link's target, split on its target separators; a volume is absolute.
    };

    /// The components of a path or a target, and whether its last one must be a directory.
    struct Components
    {
        std::vector<std::string> names;
        FinalComponent finalComponent = FinalComponent::Any;
    };

    /// The components of @p text, with `.` dropped and every other one checked. A trailing separator
    /// or a trailing `.` makes the last component one that must be a directory, as POSIX resolves
    /// `name/` and `name/.`. A path argument keeps a trailing `.` as its last component: `name/.`
    /// names the directory itself, not an entry of its parent, so an operation on that entry (rmdir,
    /// mkdir) answers for the directory, as Linux does. A link's target drops it, since an operation
    /// that follows a final link never acts on the entry.
    FsResult<Components> componentsOf(std::string_view text, TextKind kind, PathPolicy const& policy)
    {
        auto const separators = kind == TextKind::Path ? policy.separators : policy.targetSeparators;
        if (text.empty())
            return refuse(FsErrorCode::NoEntry);
        if (auto checked = checkText(text); !checked)
            return std::unexpected { std::move(checked.error()) };
        auto const rooted = kind == TextKind::SymlinkTarget ? isRootedLinkTarget(text, policy)
                                                            : separators.contains(text.front());
        if (rooted)
            return refuse(FsErrorCode::NotPermitted);
        auto const pieces = splitOn(text, separators);
        auto components = Components {};
        if (separators.contains(text.back()) || (!pieces.empty() && pieces.back() == "."))
            components.finalComponent = FinalComponent::Directory;
        for (auto const piece: pieces)
        {
            if (piece == ".")
                continue;
            if (auto checked = checkComponent(piece, policy); !checked)
                return std::unexpected { std::move(checked.error()) };
            components.names.emplace_back(piece);
        }
        if (kind == TextKind::Path && !pieces.empty() && pieces.back() == ".")
            components.names.emplace_back(".");
        return components;
    }

} // namespace

bool isUtf8(std::string_view text) noexcept
{
    while (!text.empty())
    {
        auto const length = sequenceLength(text);
        if (length == 0)
            return false;
        text.remove_prefix(length);
    }
    return true;
}

FsResult<PathPlan> planRelativePath(std::string_view path, PathPolicy const& policy)
{
    return componentsOf(path, TextKind::Path, policy).transform([](Components components) {
        if (components.names.empty())
            components.names.emplace_back(".");
        return PathPlan { .components = std::move(components.names),
                          .finalComponent = components.finalComponent };
    });
}

FsResult<void> checkSymlinkTarget(std::string_view target, PathPolicy const& policy)
{
    return componentsOf(target, TextKind::SymlinkTarget, policy).transform([](Components const&) {});
}

FsResult<std::string> refuseRootedTarget(std::string target, PathPolicy const& policy)
{
    if (isRootedLinkTarget(target, policy))
        return refuse(FsErrorCode::NotPermitted);
    return target;
}

FsResult<void> spliceSymlink(PathPlan& plan,
                             std::size_t at,
                             std::string_view target,
                             PathPolicy const& policy)
{
    if (at >= plan.components.size())
        return refuse(FsErrorCode::Invalid);
    return componentsOf(target, TextKind::SymlinkTarget, policy)
        .transform([&plan, at](Components components) {
            auto const isFinal = at + 1 == plan.components.size();
            auto const link = plan.components.begin() + static_cast<std::ptrdiff_t>(at);
            auto const next = plan.components.erase(link);
            plan.components.insert(next,
                                   std::make_move_iterator(components.names.begin()),
                                   std::make_move_iterator(components.names.end()));
            if (isFinal && components.finalComponent == FinalComponent::Directory)
                plan.finalComponent = FinalComponent::Directory;
        });
}

namespace
{

    /// @return Whether @p component would reach the kernel as more than one name.
    FsResult<void> checkKernelName(std::string_view component, WalkRules const& rules)
    {
        if (!rules.kernelSeparators.empty()
            && component.find_first_of(rules.kernelSeparators) != std::string_view::npos)
            return refuse(FsErrorCode::Invalid);
        return {};
    }

    /// One walk: the plan as it is spliced, where it has got to, and how many links it followed.
    class Walk
    {
      public:
        Walk(IBeneathCursor& cursor, PathPlan plan, WalkRules const& rules):
            _cursor { &cursor }, _plan { std::move(plan) }, _rules { rules }
        {
        }

        FsResult<WalkEnd> run()
        {
            while (_index < _plan.components.size())
            {
                if (_index == 0 && _cursor->atRoot())
                    if (auto taken = offerShortcut(); !taken)
                        return std::unexpected { std::move(taken.error()) };
                if (_index >= _plan.components.size())
                    break;
                auto name = _plan.components[_index];
                auto const isFinal = _index + 1 == _plan.components.size();
                if (name == ".")
                {
                    ++_index;
                    continue;
                }
                if (name == "..")
                {
                    if (_cursor->atRoot())
                        return refuse(FsErrorCode::NotPermitted);
                    _cursor->leave();
                    ++_index;
                    continue;
                }
                if (auto checked = checkKernelName(name, _rules); !checked)
                    return std::unexpected { std::move(checked.error()) };
                if (!isFinal)
                {
                    auto const kind = _cursor->enter(name);
                    if (!kind)
                        return std::unexpected { kind.error() };
                    if (*kind == EntryKind::SymbolicLink)
                    {
                        if (auto followed = follow(name); !followed)
                            return std::unexpected { std::move(followed.error()) };
                        continue;
                    }
                    ++_index;
                    continue;
                }
                auto const kind = _cursor->inspect(name);
                if (!kind)
                    return std::unexpected { kind.error() };
                auto const asksForDirectory = _plan.finalComponent == FinalComponent::Directory;
                if (*kind == EntryKind::SymbolicLink)
                {
                    if (asksForDirectory && _rules.finalPolicy == SymlinkPolicy::NoFollow
                        && _rules.onLink == TrailingSlashOnLink::Refuse)
                        return refuse(FsErrorCode::NotDirectory);
                    if (_rules.finalPolicy == SymlinkPolicy::Follow || asksForDirectory)
                    {
                        if (auto followed = follow(name); !followed)
                            return std::unexpected { std::move(followed.error()) };
                        continue;
                    }
                }
                if (asksForDirectory && *kind != EntryKind::Missing && *kind != EntryKind::Directory)
                    return refuse(FsErrorCode::NotDirectory);
                return WalkEnd { .name = std::move(name),
                                 .kind = *kind,
                                 .finalComponent = _plan.finalComponent };
            }
            return WalkEnd { .name = {},
                             .kind = EntryKind::Directory,
                             .finalComponent = FinalComponent::Directory };
        }

      private:
        /// Lets the cursor resolve everything before the final name at once, or the whole path where
        /// it ends in `.` or `..`.
        FsResult<void> offerShortcut()
        {
            auto const& components = _plan.components;
            auto const last = components.back();
            auto const length = last == "." || last == ".." ? components.size() : components.size() - 1;
            if (length == 0)
                return {};
            auto const prefix = std::span<std::string const> { components.data(), length };
            for (auto const& component: prefix)
                if (auto checked = checkKernelName(component, _rules); !checked)
                    return checked;
            return _cursor->shortcut(prefix).transform([this, length](Shortcut taken) {
                if (taken == Shortcut::Taken)
                {
                    _index = length;
                    _shortcut = Shortcut::Taken;
                }
            });
        }

        /// Replaces the link at the current component with its target. A walk that stands where a
        /// shortcut put it starts again from the root, since it holds no directory to climb back to.
        FsResult<void> follow(std::string const& name)
        {
            if (++_hops > _rules.maxSymlinkHops)
                return refuse(FsErrorCode::Loop);
            auto target = _cursor->readLink(name);
            if (!target)
            {
                // No longer a link: it changed under the walk, which looks at it again.
                if (target.error().code == FsErrorCode::Invalid)
                    return {};
                return std::unexpected { std::move(target.error()) };
            }
            if (auto spliced = spliceSymlink(_plan, _index, *target, *_rules.policy); !spliced)
                return spliced;
            if (_shortcut == Shortcut::Taken)
            {
                _cursor->restart();
                _index = 0;
                _shortcut = Shortcut::Declined;
            }
            return {};
        }

        IBeneathCursor* _cursor;
        PathPlan _plan;
        WalkRules _rules;
        std::size_t _index = 0;
        std::size_t _hops = 0;
        Shortcut _shortcut = Shortcut::Declined;
    };

} // namespace

FsResult<WalkEnd> walkBeneath(IBeneathCursor& cursor, PathPlan plan, WalkRules const& rules)
{
    return Walk { cursor, std::move(plan), rules }.run();
}

} // namespace core::fs::detail
