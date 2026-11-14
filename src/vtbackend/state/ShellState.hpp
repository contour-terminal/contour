// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/CommandBlockId.hpp>
#include <vtbackend/core/ContextId.hpp>
#include <vtbackend/core/RowMove.hpp>
#include <vtbackend/core/TerminalContext.hpp>
#include <vtbackend/shell/CommandBlock.hpp>
#include <vtbackend/shell/MarkArbiter.hpp>
#include <vtbackend/shell/SemanticBlockTracker.hpp>
#include <vtbackend/state/StateFields.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <vector>

namespace vtbackend
{

/// Everything a CommandBlockStore holds, as plain data.
struct CommandBlockStoreState
{
    std::vector<CommandBlockRecord> records {}; ///< Oldest first.
    std::vector<uint64_t> changeStamps {};      ///< The content revision each record last changed at.

    /// How long the running command has run, as of the capture: its start is the restoring process's
    /// steady-clock now less this, so the elapsed time carries over a clock that starts anew.
    std::optional<std::chrono::nanoseconds> runningFor {};
    std::optional<ContextId>
        awaitedCommandContextParent {}; ///< @see CommandBlockStore::commandContextStarted
    uint64_t nextId = 1;                ///< The id the next block takes.
    uint64_t idEpoch = 0;               ///< How often the ids started over.
    uint64_t revision = 0;              ///< @see CommandBlockStore::revision
    uint64_t contentRevision = 0;       ///< @see CommandBlockStore::contentRevision

    /// Lists the fields for the state codec.
    void visitFields(this auto& self, auto& visitor)
    {
        visitor(1, self.records);
        visitor(2, self.changeStamps);
        visitor(3, self.runningFor);
        visitor(4, self.awaitedCommandContextParent);
        visitor(5, self.nextId);
        visitor(6, self.idEpoch);
        visitor(7, self.revision);
        visitor(8, self.contentRevision);
    }
};

/// What a SemanticBlockTracker holds, as plain data.
struct SemanticBlockTrackerState
{
    std::optional<SemanticBlockToken> token {}; ///< The reader session's token, if one is open.
    CommandBlockId hiddenThrough {};            ///< Blocks up to this id started before the session.
    uint64_t hiddenEpoch = 0;                   ///< The id epoch @ref hiddenThrough counts in.

    /// Lists the fields for the state codec.
    void visitFields(this auto& self, auto& visitor)
    {
        visitor(1, self.token);
        visitor(2, self.hiddenThrough);
        visitor(3, self.hiddenEpoch);
    }
};

/// What a MarkArbiter holds, as plain data.
struct MarkArbiterState
{
    MarkOwner owner = MarkOwner::Undecided; ///< Who owns the session's marks.
    uint8_t completedCycles = 0;            ///< Prompt cycles OSC 3008 completed alone.
    bool hasOpenCommand = false;            ///< Whether a `type=command` context is open.

    /// Lists the fields for the state codec.
    void visitFields(this auto& self, auto& visitor)
    {
        visitor(1, self.owner);
        visitor(2, self.completedCycles);
        visitor(3, self.hasOpenCommand);
    }
};

/// What a ContextStack holds, as plain data.
struct ContextStackState
{
    std::vector<TerminalContext> records {}; ///< Every retained record, oldest first.
    std::vector<ContextId> chain {};         ///< The ancestry, outermost first; each is in @ref records.
    uint16_t nextId = 1;                     ///< The id the next context takes.
    uint64_t revision = 0;                   ///< @see ContextStack::revision
    uint64_t droppedPushes = 0;              ///< @see ContextStack::droppedPushes

    /// Lists the fields for the state codec.
    void visitFields(this auto& self, auto& visitor)
    {
        visitor(1, self.records);
        visitor(2, self.chain);
        visitor(3, self.nextId);
        visitor(4, self.revision);
        visitor(5, self.droppedPushes);
    }
};

/// The shell integration state of a terminal: blocks, contexts, marks and folds.
struct ShellState
{
    CommandBlockStoreState commandBlocks {};     ///< Every command block of the primary screen.
    SemanticBlockTrackerState semanticBlocks {}; ///< The mode 2034 reader session.
    ContextStackState contexts {};               ///< The OSC 3008 ancestry.
    MarkArbiterState markArbiter {};             ///< Who owns the marks.
    std::vector<int64_t> userMarks {};           ///< The user-marked lines, by stable id.
    uint64_t userMarksGeneration = 0;            ///< The stable-id generation they were minted in.
    uint64_t userMarkChanges = 0;                ///< @see Terminal::_userMarkChanges
    std::vector<int64_t> collapsedFolds {};      ///< The head ids of the collapsed folds.
    uint64_t foldStateGeneration = 0;            ///< The stable-id generation the folds were reconciled in.
    std::optional<int64_t> foldStateFloor {};    ///< The scrollback floor they were pruned against.
    uint64_t commandBlockHeadsGeneration = 0;    ///< The generation the block heads were recorded in.
    uint64_t commandBlockHeadsRowChanges = 0;    ///< The primary screen's block row changes at that point.
    int64_t commandBlockHeadsFloor = 0;          ///< The primary grid's floor at that point.
    std::optional<StableIdSpan>
        commandBlockRowsMoved {};      ///< Ids a scroll moved since the heads were looked at.
    uint64_t commandBlockHeadWork = 0; ///< @see Terminal::commandBlockHeadWork
    std::optional<CommandBlockId> hoveredCommandBlock {}; ///< The block the pointer hovers.
    std::optional<CommandBlockId> focusedCommandBlock {}; ///< The block the keyboard focuses.

    /// Lists the fields for the state codec.
    void visitFields(this auto& self, auto& visitor)
    {
        visitor(1, self.commandBlocks);
        visitor(2, self.semanticBlocks);
        visitor(3, self.contexts);
        visitor(4, self.markArbiter);
        visitor(5, self.userMarks);
        visitor(6, self.userMarksGeneration);
        visitor(7, self.userMarkChanges);
        visitor(8, self.collapsedFolds);
        visitor(9, self.foldStateGeneration);
        visitor(10, self.foldStateFloor);
        visitor(11, self.commandBlockHeadsGeneration);
        visitor(12, self.commandBlockHeadsRowChanges);
        visitor(13, self.commandBlockHeadsFloor);
        visitor(14, self.commandBlockRowsMoved);
        visitor(15, self.commandBlockHeadWork);
        visitor(16, self.hoveredCommandBlock);
        visitor(17, self.focusedCommandBlock);
    }
};

namespace detail
{
    // {{{ field lists of the foreign types a ShellState holds

    template <typename Visitor, typename T>
        requires std::same_as<std::remove_const_t<T>, StableIdSpan>
    void visitForeignFields(Visitor& visitor, T& span)
    {
        visitor(1, span.first);
        visitor(2, span.last);
    }

    template <typename Visitor, typename T>
        requires std::same_as<std::remove_const_t<T>, ContextOutcome>
    void visitForeignFields(Visitor& visitor, T& outcome)
    {
        visitor(1, outcome.exit);
        visitor(2, outcome.signal);
        visitor(3, outcome.status);
    }

    template <typename Visitor, typename T>
        requires std::same_as<std::remove_const_t<T>, WorkingDirectorySnapshot>
    void visitForeignFields(Visitor& visitor, T& snapshot)
    {
        visitor(1, snapshot.path);
        visitor(2, snapshot.locality);
    }

    template <typename Visitor, typename T>
        requires std::same_as<std::remove_const_t<T>, TerminalContext>
    void visitForeignFields(Visitor& visitor, T& context)
    {
        visitor(1, context.id);
        visitor(2, context.parent);
        visitor(3, context.identifier);
        visitor(4, context.type);
        visitor(5, context.user);
        visitor(6, context.hostname);
        visitor(7, context.machineId);
        visitor(8, context.bootId);
        visitor(9, context.comm);
        visitor(10, context.workingDirectory);
        visitor(11, context.commandLine);
        visitor(12, context.vm);
        visitor(13, context.container);
        visitor(14, context.targetUser);
        visitor(15, context.targetHost);
        visitor(16, context.sessionId);
        visitor(17, context.pid);
        visitor(18, context.pidFdId);
        visitor(19, context.present);
        visitor(20, context.outcome);
    }

    /// A command line crosses the wire as the raw text it was reported or recovered as.
    template <typename Visitor>
    void visitForeignFields(Visitor& visitor, CommandLineText const& text)
    {
        visitor(1, text.raw());
    }

    template <typename Visitor>
        requires std::same_as<Visitor, FieldDecoder>
    void visitForeignFields(Visitor& visitor, CommandLineText& text)
    {
        auto raw = std::string {};
        visitor(1, raw);
        text = CommandLineText { std::move(raw) };
    }

    template <typename Visitor, typename T>
        requires std::same_as<std::remove_const_t<T>, CommandBlockRecord>
    void visitForeignFields(Visitor& visitor, T& record)
    {
        visitor(1, record.id);
        visitor(2, record.state);
        visitor(3, record.end);
        visitor(4, record.commandLine);
        visitor(5, record.commandLineSource);
        visitor(6, record.exitCode);
        visitor(7, record.outcome);
        visitor(8, record.promptStartedAt);
        visitor(9, record.commandStartedAt);
        visitor(10, record.duration);
        visitor(11, record.workingDirectory);
        visitor(12, record.endWorkingDirectory);
        visitor(13, record.foreignDirectory);
        visitor(14, record.commandContext);
        visitor(15, record.headStableId);
        visitor(16, record.headIdGeneration);
    }

    // }}}
} // namespace detail

} // namespace vtbackend
