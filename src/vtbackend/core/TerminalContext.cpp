// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/core/TerminalContext.hpp>

#include <crispy/Assert.hpp>

#include <algorithm>
#include <utility>

using std::optional;
using std::shared_ptr;
using std::span;
using std::string_view;

namespace vtbackend
{

namespace
{
    /// Assigns @p value to @p target if @p field was supplied, and clears it otherwise.
    /// @return Whether @p target changed.
    bool assignField(std::string& target, ContextFields present, ContextField field, string_view value)
    {
        auto const supplied = (present & field).any();
        auto const& wanted = supplied ? value : string_view {};
        if (target == wanted)
            return false;
        target.assign(wanted); // reuses the destination's capacity: the systemd hot path never allocates
        return true;
    }

    /// The scalar twin of @ref assignField, serving the Uint64 and Enum kinds.
    ///
    /// An OVERLOAD rather than a separate name, which is what lets the table below expand to one
    /// spelling for every row: the string overload wins for a `std::string` target (the template
    /// deduces T from both parameters and fails), and this one takes every other kind.
    template <typename T>
    bool assignField(T& target, ContextFields present, ContextField field, T value)
    {
        auto const wanted = (present & field).any() ? value : T {};
        if (target == wanted)
            return false;
        target = wanted;
        return true;
    }

    /// Reinitialises @p record from @p command, flushing every field the payload did NOT name back to
    /// its default -- what the specification means by "any previously set metadata fields are flushed
    /// out, reset to their defaults, and then reinitialized from the newly supplied data".
    ///
    /// Reports whether anything actually moved, because the systemd shell context is re-announced on
    /// every prompt with byte-identical metadata and a frontend must not be told to redraw for that.
    /// @return Whether @p record changed.
    bool reinitialize(TerminalContext& record, ContextCommand const& command)
    {
        auto changed = false;

        // Generated from VTBACKEND_CONTEXT_FIELDS, so a sixteenth field really is one row there. Written
        // out by hand this was the one consumer that could be forgotten SILENTLY: a missing line keeps a
        // stale value across a re-`start=`, which is precisely the flush the specification mandates, and
        // neither the compiler nor a test would say so.
#define VTBACKEND_CONTEXT_FIELD_ASSIGN(Name, Bit, Spelling, Member, Kind, Max) \
    changed |= assignField(record.Member, command.present, ContextField::Name, command.Member);
        VTBACKEND_CONTEXT_FIELDS(VTBACKEND_CONTEXT_FIELD_ASSIGN)
#undef VTBACKEND_CONTEXT_FIELD_ASSIGN

        if (record.present != command.present)
        {
            record.present = command.present;
            changed = true;
        }

        // A re-`start=` describes a context that is running again, so any outcome recorded from a
        // previous life is stale. Part of the flush, not an exception to it.
        if (record.outcome != ContextOutcome {})
        {
            record.outcome = {};
            changed = true;
        }

        return changed;
    }
} // namespace

ContextStack::ContextStack(ContextStackLimits limits) noexcept: _limits { limits }
{
    // A programmer error, not a recoverable one: an ancestor evicted from the store would leave the
    // chain holding a record nothing else can resolve.
    Require(_limits.maxRetained >= _limits.maxDepth);
    Require(_limits.maxDepth >= 1);
    // The depth is what popFrom() counts, and ContextTransition reports that count in a uint16_t.
    Require(_limits.maxDepth <= 0xFFFF);
}

optional<size_t> ContextStack::indexOf(string_view identifier) const noexcept
{
    for (auto index = _chain.size(); index-- > 0;)
        if (_chain[index].record->identifier == identifier)
            return index;
    return std::nullopt;
}

uint16_t ContextStack::popFrom(size_t firstIndex)
{
    auto retired = uint16_t {};
    while (_chain.size() > firstIndex)
    {
        _chain.pop_back();
        ++retired;
    }
    return retired;
}

bool ContextStack::isOnChain(ContextId id) const noexcept
{
    return std::ranges::any_of(_chain, [id](Entry const& entry) { return entry.record->id == id; });
}

void ContextStack::store(shared_ptr<TerminalContext> record)
{
    auto const id = record->id;
    _byId.insert_or_assign(id, std::move(record));
    _creationOrder.push_back(id);

    // Evict oldest-first, but never a context that is still an ancestor: the chain must be resolvable
    // through find(), and the constructor's maxRetained >= maxDepth requirement guarantees there is
    // always something else to drop. A chain-held id rotates to the back rather than being discarded
    // from the queue, so it is reconsidered once it leaves the ancestry.
    //
    // The bound is on ATTEMPTS, not on the queue: without it, a queue made entirely of ancestors would
    // rotate forever.
    auto attempts = _creationOrder.size();
    while (_byId.size() > _limits.maxRetained && attempts-- > 0)
    {
        auto const oldest = _creationOrder.front();
        _creationOrder.pop_front();

        if (isOnChain(oldest))
        {
            _creationOrder.push_back(oldest);
            continue;
        }

        // A caller holding the record through retain() keeps the OBJECT alive; dropping the map entry
        // only means this stack stops resolving the id, which is the defined answer for an aged-out
        // context.
        _byId.erase(oldest);
    }
}

ContextTransition ContextStack::apply(ContextCommand const& command)
{
    auto const known = indexOf(command.identifier);

    if (command.verb == ContextVerb::End)
    {
        // An `end=` naming something outside the ancestry does NOTHING. This is the protocol's stated
        // safety property expressed in code: if an unknown end could pop, a program could terminate the
        // context established above it by guessing an identifier.
        if (!known)
            return {};

        auto const record = _chain[*known].record;
        // The named context takes the payload's outcome; its descendants are terminated implicitly and
        // so have none to report.
        record->outcome = command.outcome;
        auto const retired = popFrom(*known);
        ++_revision;
        return { .kind = ContextTransitionKind::Ended,
                 .subject = record->id,
                 .subjectType = record->type,
                 .change = ContextChange::Yes,
                 .implicitlyEnded = static_cast<uint16_t>(retired - 1) };
    }

    if (known)
    {
        auto const isActive = *known + 1 == _chain.size();
        // Everything below the named context is implicitly terminated -- for an update of the active
        // context there is nothing below it, so the two cases share one line.
        auto const retired = popFrom(*known + 1);
        auto const record = _chain[*known].record;
        auto const changed = reinitialize(*record, command);
        if (changed || retired)
            ++_revision;
        return { .kind = isActive ? ContextTransitionKind::Updated : ContextTransitionKind::ReturnedTo,
                 .subject = record->id,
                 .subjectType = record->type,
                 .change = (changed || retired) ? ContextChange::Yes : ContextChange::None,
                 .implicitlyEnded = retired };
    }

    // The spec's overflow rule is the opposite of MaxSavedTitles': keep the EARLIER contexts and
    // discard the newer, so a program deep in the ancestry cannot evict the elevate context above it.
    // Output written while the refused context is notionally active is attributed to its parent, and
    // its matching `end=` falls through the unknown-identifier arm above -- which is consistent, and is
    // why that arm has to come first.
    if (_chain.size() >= _limits.maxDepth)
    {
        ++_droppedPushes;
        return { .kind = ContextTransitionKind::DepthExceeded };
    }

    auto record = std::make_shared<TerminalContext>();
    record->id = _nextId;
    record->parent = activeId();
    record->identifier.assign(command.identifier);
    reinitialize(*record, command);

    // Wraps at 65535 like HyperlinkId does, and skips zero because zero means "no context". With only
    // maxRetained records kept, an id old enough to be reused resolves to nothing long before it could
    // collide with a line still pointing at it.
    _nextId = ContextId { static_cast<uint16_t>(_nextId.value + 1) };
    if (!_nextId.value)
        _nextId = ContextId { 1 };

    auto const id = record->id;
    auto const type = record->type;
    store(record);
    _chain.push_back(Entry { .record = std::move(record) });
    ++_revision;

    return { .kind = ContextTransitionKind::Pushed,
             .subject = id,
             .subjectType = type,
             .change = ContextChange::Yes };
}

void ContextStack::adopt(TerminalContext record)
{
    Require(!!record.id);
    auto const id = record.id;

    if (auto const it = _byId.find(id); it != _byId.end())
    {
        // In place, so the ancestry's shared_ptr and every line already stamped with this id keep
        // resolving to the record they named.
        if (*it->second != record)
        {
            *it->second = std::move(record);
            ++_revision;
        }
        return;
    }

    store(std::make_shared<TerminalContext>(std::move(record)));
    ++_revision;

    // Keep the local counter ahead of anything adopted, so a context this stack later creates itself
    // cannot collide with a mirrored one.
    if (_nextId.value <= id.value)
    {
        _nextId = ContextId { static_cast<uint16_t>(id.value + 1) };
        if (!_nextId.value)
            _nextId = ContextId { 1 };
    }
}

void ContextStack::setChain(span<ContextId const> ids)
{
    auto rebuilt = std::vector<Entry> {};
    rebuilt.reserve(ids.size());
    for (auto const id: ids)
        if (auto const it = _byId.find(id); it != _byId.end())
            rebuilt.push_back(Entry { .record = it->second });

    if (rebuilt.size() == _chain.size()
        && std::ranges::equal(
            rebuilt, _chain, [](Entry const& a, Entry const& b) { return a.record == b.record; }))
        return;

    _chain = std::move(rebuilt);
    ++_revision;
}

void ContextStack::clear() noexcept
{
    _chain.clear();
    _byId.clear();
    _creationOrder.clear();
    _nextId = ContextId { 1 };
    _droppedPushes = 0;
    ++_revision;
}

TerminalContext const* ContextStack::find(ContextId id) const noexcept
{
    if (!id)
        return nullptr;
    auto const it = _byId.find(id);
    return it != _byId.end() ? it->second.get() : nullptr;
}

shared_ptr<TerminalContext const> ContextStack::retain(ContextId id) const noexcept
{
    if (!id)
        return {};
    auto const it = _byId.find(id);
    return it != _byId.end() ? it->second : shared_ptr<TerminalContext const> {};
}

std::vector<TerminalContext> ContextStack::records() const
{
    auto out = std::vector<TerminalContext> {};
    out.reserve(_creationOrder.size());
    for (auto const id: _creationOrder)
        if (auto const it = _byId.find(id); it != _byId.end())
            out.push_back(*it->second);
    return out;
}

ContextLocality ContextStack::localityOf(ContextId id, LocalIdentity const& self) const noexcept
{
    if (!id)
        return ContextLocality::Unknown;

    // Walk from the outermost down to the named context: a boundary crossed above it is never escaped
    // by a deeper context, so the first one found decides.
    auto found = false;
    auto sawIdentity = false;
    for (auto const& entry: _chain)
    {
        auto const& record = *entry.record;
        if (contextTypeCrossesHost(record.type) == HostBoundary::Crossed)
            return ContextLocality::Foreign;

        if (!record.machineId.empty() && !self.machineId.empty())
        {
            if (record.machineId != self.machineId)
                return ContextLocality::Foreign;
            sawIdentity = true;
        }
        else if (!record.hostname.empty() && !self.hostname.empty())
        {
            if (record.hostname != self.hostname)
                return ContextLocality::Foreign;
            sawIdentity = true;
        }

        if (record.id == id)
        {
            found = true;
            break;
        }
    }

    if (!found)
        return ContextLocality::Unknown;

    // Nothing on the ancestry claimed an identity, so nothing proved this is the same machine. Unknown
    // rather than Local, deliberately: nothing emits a `remote` context for ssh today, so a remote
    // shell's own sequences look entirely local and a remote path may well exist here too.
    return sawIdentity ? ContextLocality::Local : ContextLocality::Unknown;
}

optional<EffectiveWorkingDirectory> ContextStack::effectiveWorkingDirectory(
    LocalIdentity const& self) const noexcept
{
    for (auto index = _chain.size(); index-- > 0;)
    {
        auto const& record = *_chain[index].record;
        // The present-bit, not the string: a context that said `cwd=` and meant it stops the walk, and
        // one that never mentioned a cwd is skipped so a `type=command` context inherits the shell's.
        if (!(record.present & ContextField::WorkingDirectory).any())
            continue;
        if (record.workingDirectory.empty())
            continue;
        return EffectiveWorkingDirectory { .path = record.workingDirectory,
                                           .owner = record.id,
                                           .locality = localityOf(record.id, self) };
    }
    return std::nullopt;
}

} // namespace vtbackend
