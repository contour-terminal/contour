// SPDX-License-Identifier: Apache-2.0
//
// Test-only: an IDaemonHostServices that records every call and answers what the test sets.
#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <vtextension/DaemonImports.hpp>
#include <vtextension/DaemonWorld.hpp>
#include <vtextension/IDaemonHostServices.hpp>
#include <vtextension/IHostServices.hpp>
#include <vtextension/testing/FakePaneService.hpp>

namespace vtextension::testing
{

/// A message a daemon guest logged.
struct DaemonLogged
{
    LogLevel level = LogLevel::Info; ///< How loud.
    std::string message;             ///< What it said.

    [[nodiscard]] bool operator==(DaemonLogged const&) const = default;
};

/// A `proc-start` a daemon guest made.
struct ProcStartCall
{
    std::string command; ///< The declared command.
    ConfigMap args;      ///< Its placeholders' values.
    std::string cwd;     ///< Its working directory.
    std::string tag;     ///< The guest's label.

    [[nodiscard]] bool operator==(ProcStartCall const&) const = default;
};

/// Records what a daemon guest asks of the host, in order, and answers from its public fields.
class FakeDaemonHostServices final: public IDaemonHostServices
{
  public:
    void log(LogLevel level, std::string_view message) override
    {
        called(DaemonImport::Log);
        logged.push_back(DaemonLogged { .level = level, .message = std::string(message) });
    }

    void wake(std::chrono::milliseconds after) override
    {
        called(DaemonImport::Wake);
        wakes.push_back(after);
    }

    [[nodiscard]] ClockReading now() override
    {
        called(DaemonImport::Now);
        return clock;
    }

    [[nodiscard]] std::expected<uint32_t, HostError> procStart(std::string_view command,
                                                               ConfigMap const& args,
                                                               std::string_view cwd,
                                                               std::string_view tag) override
    {
        called(DaemonImport::ProcStart);
        procStarts.push_back(ProcStartCall { .command = std::string(command),
                                             .args = args,
                                             .cwd = std::string(cwd),
                                             .tag = std::string(tag) });
        return procHandle;
    }

    [[nodiscard]] std::expected<std::vector<ProcInfo>, HostError> procs() override
    {
        called(DaemonImport::Procs);
        return processes;
    }

    [[nodiscard]] std::expected<Chunk, HostError> procTake(uint32_t /*handle*/,
                                                           ProcStream /*stream*/,
                                                           uint32_t /*offset*/) override
    {
        called(DaemonImport::ProcTake);
        return chunk;
    }

    [[nodiscard]] std::expected<void, HostError> procRelease(uint32_t /*handle*/) override
    {
        called(DaemonImport::ProcRelease);
        return {};
    }

    [[nodiscard]] std::expected<std::optional<ByteList>, HostError> storeGet(
        std::string_view /*key*/) override
    {
        called(DaemonImport::StoreGet);
        return storedValue;
    }

    [[nodiscard]] std::expected<void, HostError> storeSet(std::string_view key,
                                                          std::span<uint8_t const> value) override
    {
        called(DaemonImport::StoreSet);
        storeWrites.emplace_back(std::string(key), ByteList(value.begin(), value.end()));
        return storeAnswer;
    }

    [[nodiscard]] std::expected<void, HostError> storeRemove(std::string_view key) override
    {
        called(DaemonImport::StoreRemove);
        storeRemovals.emplace_back(key);
        return storeAnswer;
    }

    [[nodiscard]] std::expected<std::vector<std::string>, HostError> storeKeys(
        std::string_view /*prefix*/) override
    {
        called(DaemonImport::StoreKeys);
        return std::vector<std::string> {};
    }

    [[nodiscard]] std::expected<std::string, HostError> paneOpen(
        std::string_view paneTemplate,
        ConfigMap const& args,
        std::string_view cwd,
        std::string_view title,
        std::string_view tag,
        std::optional<std::string_view> nearTab) override
    {
        called(DaemonImport::PaneOpen);
        paneOpens.push_back(PaneOpenCall {
            .paneTemplate = std::string(paneTemplate),
            .args = args,
            .cwd = std::string(cwd),
            .title = std::string(title),
            .tag = std::string(tag),
            .nearTab = nearTab.transform([](std::string_view tab) { return std::string(tab); }),
        });
        return paneId;
    }

    [[nodiscard]] std::expected<void, HostError> paneClose(std::string_view pane) override
    {
        called(DaemonImport::PaneClose);
        paneCloses.emplace_back(pane);
        return paneAnswer;
    }

    [[nodiscard]] std::expected<std::vector<PaneInfo>, HostError> panes() override
    {
        called(DaemonImport::Panes);
        return listedPanes;
    }

    [[nodiscard]] std::expected<void, HostError> paneReveal(std::string_view pane, Origin origin) override
    {
        called(DaemonImport::PaneReveal);
        paneReveals.emplace_back(std::string(pane), origin);
        return paneAnswer;
    }

    [[nodiscard]] std::expected<std::string, HostError> tabOpen(std::string_view scheme,
                                                                Origin origin) override
    {
        called(DaemonImport::TabOpen);
        tabOpens.emplace_back(std::string(scheme), origin);
        return tabId;
    }

    [[nodiscard]] std::expected<void, HostError> tabClose(std::string_view tab) override
    {
        called(DaemonImport::TabClose);
        tabCloses.emplace_back(tab);
        return tabAnswer;
    }

    [[nodiscard]] std::expected<void, HostError> tabReveal(std::string_view tab, Origin origin) override
    {
        called(DaemonImport::TabReveal);
        tabReveals.emplace_back(std::string(tab), origin);
        return tabAnswer;
    }

    [[nodiscard]] std::expected<std::vector<TabInfo>, HostError> tabs() override
    {
        called(DaemonImport::Tabs);
        return servedTabs;
    }

    [[nodiscard]] std::expected<ByteList, HostError> tabRead(std::string_view /*tab*/) override
    {
        called(DaemonImport::TabRead);
        return tabInput;
    }

    [[nodiscard]] std::expected<void, HostError> tabWrite(std::string_view tab,
                                                          std::span<uint8_t const> bytes) override
    {
        called(DaemonImport::TabWrite);
        tabWrites.emplace_back(std::string(tab), ByteList(bytes.begin(), bytes.end()));
        return tabAnswer;
    }

    [[nodiscard]] std::expected<std::vector<ActionInfo>, HostError> actionsTake() override
    {
        called(DaemonImport::ActionsTake);
        return std::exchange(actions, {});
    }

    [[nodiscard]] std::expected<void, HostError> summarySet(std::string_view shortText,
                                                            std::string_view detail) override
    {
        called(DaemonImport::SummarySet);
        summary = { std::string(shortText), std::string(detail) };
        return {};
    }

    [[nodiscard]] std::expected<FileContent, FileError> fileRead(std::string_view root,
                                                                 std::string_view path) override
    {
        called(DaemonImport::FileRead);
        fileReads.emplace_back(std::string(root), std::string(path));
        return fileContent;
    }

    std::vector<DaemonImport> calls;                           ///< Every service call, in order.
    std::function<void(DaemonImport)> onCall;                  ///< Runs first in every service call.
    std::vector<DaemonLogged> logged;                          ///< Every log().
    std::vector<std::chrono::milliseconds> wakes;              ///< Every wake().
    ClockReading clock {};                                     ///< What now() answers.
    std::vector<ProcStartCall> procStarts;                     ///< Every procStart().
    std::expected<uint32_t, HostError> procHandle = 1U;        ///< What procStart() answers.
    std::vector<ProcInfo> processes;                           ///< What procs() answers.
    Chunk chunk;                                               ///< What procTake() answers.
    std::optional<ByteList> storedValue;                       ///< What storeGet() answers.
    std::vector<std::pair<std::string, ByteList>> storeWrites; ///< Every storeSet().
    std::vector<std::string> storeRemovals;                    ///< Every storeRemove().
    std::expected<void, HostError> storeAnswer;                ///< What storeSet() and storeRemove() answer.
    std::pair<std::string, std::string> summary;               ///< The last summarySet().
    std::vector<PaneOpenCall> paneOpens;                       ///< Every paneOpen().
    std::expected<std::string, HostError> paneId = std::string { "pane-1" }; ///< What paneOpen() answers.
    std::vector<std::string> paneCloses;                                     ///< Every paneClose().
    std::vector<std::pair<std::string, Origin>> paneReveals;                 ///< Every paneReveal().
    std::expected<void, HostError> paneAnswer; ///< What paneClose() and paneReveal() answer.
    std::expected<std::vector<PaneInfo>, HostError> listedPanes;           ///< What panes() answers.
    std::vector<std::pair<std::string, Origin>> tabOpens;                  ///< Every tabOpen().
    std::expected<std::string, HostError> tabId = std::string { "tab-1" }; ///< What tabOpen() answers.
    std::vector<std::string> tabCloses;                                    ///< Every tabClose().
    std::vector<std::pair<std::string, Origin>> tabReveals;                ///< Every tabReveal().
    std::vector<TabInfo> servedTabs;                                       ///< What tabs() answers.
    ByteList tabInput;                                                     ///< What tabRead() answers.
    std::vector<std::pair<std::string, ByteList>> tabWrites;               ///< Every tabWrite().
    std::expected<void, HostError> tabAnswer; ///< What tabClose(), tabReveal(), tabWrite() answer.
    std::vector<ActionInfo> actions;          ///< What the next actionsTake() answers.
    std::vector<std::pair<std::string, std::string>> fileReads; ///< Every fileRead(): root and path.
    std::expected<FileContent, FileError> fileContent;          ///< What fileRead() answers.

  private:
    void called(DaemonImport import)
    {
        calls.push_back(import);
        if (onCall)
            onCall(import);
    }
};

} // namespace vtextension::testing
