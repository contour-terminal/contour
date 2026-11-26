// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

/// The configuration as Config.cpp reads it; nothing else includes this header. Glaze reflects only types
/// with linkage, so these cannot sit in Config.cpp's anonymous namespace. Every member is optional, so that
/// an absent key is told apart from an empty value and reported by name.
namespace orchestrator::config::detail
{

/// A duration: a string such as "2m", or a bare 0, which a YAML-to-JSON conversion writes as a number.
using JsonDuration = std::variant<uint64_t, std::string>;

/// One `repos[].issues` entry.
struct JsonIssueSource
{
    std::optional<std::string> source;
    std::optional<std::string> query;         ///< A forge source's search.
    std::optional<std::string> jql;           ///< A Jira source's query.
    std::optional<std::string> inProgressJql; ///< A Jira source's `in_progress_jql`: its in-progress filter.
    std::optional<std::string> site;          ///< A Jira source's site.
};

/// One `repos` entry.
struct JsonRepo
{
    std::optional<std::string> name;
    std::optional<std::string> path;
    std::optional<std::string> forge;
    std::optional<std::string> remote;
    std::optional<std::string> base;
    std::optional<std::vector<JsonIssueSource>> issues;
};

/// One `plugins` entry.
struct JsonPluginRequirement
{
    std::optional<std::string> id;
    std::optional<std::string> marketplace;
};

/// One `permission_profiles` entry.
struct JsonPermissionProfile
{
    std::optional<std::string> name;
    std::optional<std::string> extends;
    std::optional<std::string> mode;
    std::optional<std::vector<std::string>> allow;
    std::optional<std::vector<std::string>> deny;
};

/// A key: a string such as "1" or "esc", or a bare digit, which a YAML-to-JSON conversion writes as a number.
using JsonKey = std::variant<uint64_t, std::string>;

/// One `quick_replies` entry.
struct JsonQuickReply
{
    std::optional<JsonKey> key;
    std::optional<std::string> label;
    std::optional<JsonKey> press;
    std::optional<std::string> then;
};

/// The `keys` map: whether the vim keys are among the defaults, and each navigation action's own keys, in
/// navActionRows() order; Config.cpp maps the snake_case names.
struct JsonKeys
{
    std::optional<bool> vim;
    std::optional<std::vector<JsonKey>> moveUp;
    std::optional<std::vector<JsonKey>> moveDown;
    std::optional<std::vector<JsonKey>> first;
    std::optional<std::vector<JsonKey>> last;
    std::optional<std::vector<JsonKey>> halfPageUp;
    std::optional<std::vector<JsonKey>> halfPageDown;
    std::optional<std::vector<JsonKey>> previousValue;
    std::optional<std::vector<JsonKey>> nextValue;
    std::optional<std::vector<JsonKey>> leaveField;
    std::optional<std::vector<JsonKey>> enterField;
    std::optional<std::vector<JsonKey>> close;
};

/// The `work` map: the groups folded at start and whether empty groups show; Config.cpp maps the snake_case
/// names.
struct JsonWork
{
    std::optional<std::vector<std::string>> folded; ///< The configNames of the groups folded at start.
    std::optional<bool> foldEmpty;                  ///< `fold_empty`.
};

/// One `automation.rules` entry.
struct JsonAutomationRule
{
    std::optional<std::string> when;
    std::optional<std::string> task;
    std::optional<std::string> worker;
    std::optional<std::string> model;
    std::optional<std::string> permissions;
};

/// The `automation` map; Config.cpp maps the snake_case names.
struct JsonAutomation
{
    std::optional<bool> enabled;
    std::optional<uint32_t> maxAgents;
    std::optional<std::vector<JsonAutomationRule>> rules;
};

/// One `workers` entry.
struct JsonWorker
{
    std::optional<std::string> id;
    std::optional<std::string> agent;
    std::optional<std::string> envSet; ///< `env_set`.
};

/// One `tasks` entry; `applies_to` is no member name, Config.cpp maps the key.
struct JsonTask
{
    std::optional<std::string> id;
    std::optional<std::string> key;
    std::optional<std::string> label;
    std::optional<std::string> appliesTo;
    std::optional<std::string> model;
    std::optional<std::string> prompt;
    std::optional<JsonDuration> endDoneAfter;
    std::optional<std::string> permissions;
    std::optional<std::string> worker;
    std::optional<std::string> role;
};

/// The `config` subtree; its snake_case keys are mapped in Config.cpp.
struct JsonConfig
{
    std::optional<std::string> worktreeRoot;
    std::optional<std::vector<std::string>> models;
    std::optional<uint32_t> maxAgents;
    std::optional<uint32_t> maxAgentsPerRepo;
    std::optional<JsonDuration> pollEvery;
    std::optional<JsonDuration> statusBoardAfter;
    std::optional<std::string> statusBoard;
    std::optional<JsonDuration> endDoneAfter;
    std::optional<JsonDuration> finishedRetention;
    std::optional<std::vector<JsonRepo>> repos;
    std::optional<std::vector<JsonWorker>> workers;
    std::optional<std::vector<JsonTask>> tasks;
    std::optional<std::vector<JsonPluginRequirement>> plugins;
    std::optional<std::string> permissions;
    std::optional<std::vector<JsonPermissionProfile>> permissionProfiles;
    std::optional<std::vector<JsonQuickReply>> quickReplies;
    std::optional<JsonKeys> keys;
    std::optional<JsonWork> work;
    std::optional<JsonAutomation> automation;
};

} // namespace orchestrator::config::detail
