// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <orchestrator/core/Stats.hpp>

/// A run as Run.cpp stores it; nothing else includes this header. Glaze reflects only types with linkage,
/// so these cannot sit in Run.cpp's anonymous namespace. The keys are the members' names (but for nearTab,
/// see Run.cpp's glz::meta); a member that only some states have is optional, every other one must be
/// present.
namespace orchestrator::run::detail
{

/// A preparation failure.
struct JsonFailure
{
    std::string step;
    std::string why;
};

/// A finished run's outcome: its own, or that of the kept run an ending checks again.
struct JsonFinished
{
    std::string ended;
    std::string fate;
    std::vector<std::string> entries;
    std::string seen;
    uint64_t endedWallMs = 0;
};

/// One change of the run's state.
struct JsonChange
{
    uint64_t wallMs = 0;
    std::string state;
};

/// The stored run. Enumerations are stored by name, so that a reordered enumeration cannot change what a
/// stored run means.
struct JsonRun
{
    uint32_t v = 0;
    std::string id;
    std::string repo;
    std::string repoPath;
    std::string forge;
    std::string remote;
    std::string base;
    std::string itemKind;
    std::optional<uint64_t> itemNumber;
    std::optional<std::string> itemKey;
    std::string url;
    std::optional<std::string> topic; ///< A topic run's text; absent for any other item.
    std::string task;
    std::string model;
    std::string prompt;
    std::optional<std::string> board; ///< Absent: no board asked for, as in a run stored before it existed.
    std::optional<std::string> permissions; ///< Absent: a run stored before profiles existed.
    std::optional<std::string> settings;    ///< Absent: as for permissions; its agent got `--settings {}`.
    std::optional<std::string> worker;      ///< Absent: a run stored before workers, the implicit one's.
    std::optional<std::string> agent;       ///< Absent: Claude; written only for another agent.
    std::optional<std::string> envSet;      ///< Absent: no set.
    std::string session;
    std::string branch;
    std::string worktree;
    std::optional<std::string> pane;
    std::optional<std::string> nearTab; ///< Absent in a run stored before it existed: read as none.
    std::string state;
    std::optional<std::string> step;
    std::optional<std::string> cause;
    std::optional<std::string> removal;
    std::optional<JsonFinished> finished;
    std::optional<std::string> lastStatus;
    std::optional<JsonFailure> failure;
    uint64_t startedWallMs = 0;
    uint32_t attempt = 0;
    std::optional<RunStats> stats; ///< Absent until read, and in a run stored before it existed.
    std::optional<std::vector<JsonChange>> history; ///< Absent in a run stored before it existed.
    std::optional<std::string> ledger;              ///< `written` once appended; absent before.
};

} // namespace orchestrator::run::detail
