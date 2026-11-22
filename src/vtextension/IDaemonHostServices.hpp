// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <vtextension/DaemonWorld.hpp>
#include <vtextension/Event.hpp>
#include <vtextension/IHostServices.hpp>

namespace vtextension
{

/// What a daemon-world extension may ask of the host: one method per import of
/// `terminal-extensions:daemon/host@0.1.0` this host serves (ImportAvailability::Served in
/// DaemonImportRows). An import not served yet never reaches it: DaemonInstance answers it
/// `not-supported-yet` itself.
///
/// DaemonInstance calls it on the thread that calls into the instance, from inside a guest call, with
/// every argument lifted and checked against the Canonical ABI; what the arguments mean (a declared
/// command, a key's length) is the implementation's to check. An implementation must not call back into
/// the same instance.
class IDaemonHostServices
{
  public:
    IDaemonHostServices() = default;
    IDaemonHostServices(IDaemonHostServices const&) = delete;
    IDaemonHostServices(IDaemonHostServices&&) = delete;
    IDaemonHostServices& operator=(IDaemonHostServices const&) = delete;
    IDaemonHostServices& operator=(IDaemonHostServices&&) = delete;
    virtual ~IDaemonHostServices() = default;

    /// `log`: records a message of the extension's.
    /// @param level How loud it is.
    /// @param message The message, valid UTF-8.
    virtual void log(LogLevel level, std::string_view message) = 0;

    /// `wake`: asks for an `update(wake)` once @p after has passed.
    /// @param after How long from now.
    virtual void wake(std::chrono::milliseconds after) = 0;

    /// `now`: the host's clocks, read at once.
    /// @return The monotonic and the wall-clock milliseconds.
    [[nodiscard]] virtual ClockReading now() = 0;

    /// `proc-start`: starts a declared command.
    /// @param command The declared command's name.
    /// @param args The values of its placeholders, by name.
    /// @param cwd Its working directory.
    /// @param tag The extension's own label for the process.
    /// @return The process's handle; or why it was not started.
    [[nodiscard]] virtual std::expected<uint32_t, HostError> procStart(std::string_view command,
                                                                       ConfigMap const& args,
                                                                       std::string_view cwd,
                                                                       std::string_view tag) = 0;

    /// `procs`: the processes the extension started and has not released.
    /// @return Each with its state; or why they cannot be listed.
    [[nodiscard]] virtual std::expected<std::vector<ProcInfo>, HostError> procs() = 0;

    /// `proc-take`: a page of a finished process's output.
    /// @param handle The process.
    /// @param stream Which of its streams.
    /// @param offset The first byte of the page.
    /// @return The page; or why there is none.
    [[nodiscard]] virtual std::expected<Chunk, HostError> procTake(uint32_t handle,
                                                                   ProcStream stream,
                                                                   uint32_t offset) = 0;

    /// `proc-release`: forgets a process and its output; one still running is stopped.
    /// @param handle The process.
    /// @return Nothing; or why it was not released.
    [[nodiscard]] virtual std::expected<void, HostError> procRelease(uint32_t handle) = 0;

    /// `store-get`: a value of the extension's durable store.
    /// @param key Its key.
    /// @return The value; `std::nullopt` if there is none; or why it could not be read.
    [[nodiscard]] virtual std::expected<std::optional<ByteList>, HostError> storeGet(
        std::string_view key) = 0;

    /// `store-set`: writes a value durably. Called with the call's deadline paused: the write's time is
    /// the disk's, not the guest's.
    /// @param key Its key.
    /// @param value The value.
    /// @return Nothing once it is durable; or why it is not.
    [[nodiscard]] virtual std::expected<void, HostError> storeSet(std::string_view key,
                                                                  std::span<uint8_t const> value) = 0;

    /// `store-remove`: removes a value durably. Called with the call's deadline paused, as storeSet().
    /// @param key Its key.
    /// @return Nothing once the removal is durable; or why it is not.
    [[nodiscard]] virtual std::expected<void, HostError> storeRemove(std::string_view key) = 0;

    /// `store-keys`: the keys of the extension's durable store.
    /// @param prefix What every key returned starts with.
    /// @return The keys; or why they cannot be listed.
    [[nodiscard]] virtual std::expected<std::vector<std::string>, HostError> storeKeys(
        std::string_view prefix) = 0;

    /// `pane-open`: opens a pane running a declared template, without taking the focus (IPaneService).
    /// @param paneTemplate The template's name.
    /// @param args The values of its placeholders, by name.
    /// @param cwd The directory it starts in.
    /// @param title The tab's name.
    /// @param tag The extension's own label for the pane.
    /// @param nearTab A served tab of the extension's whose workspace it opens in; none for the focused one.
    /// @return The pane's id; or why it was not opened.
    [[nodiscard]] virtual std::expected<std::string, HostError> paneOpen(
        std::string_view paneTemplate,
        ConfigMap const& args,
        std::string_view cwd,
        std::string_view title,
        std::string_view tag,
        std::optional<std::string_view> nearTab) = 0;

    /// `pane-close`: closes a pane the extension opened.
    /// @param pane Its id.
    /// @return Nothing; or why it was not closed.
    [[nodiscard]] virtual std::expected<void, HostError> paneClose(std::string_view pane) = 0;

    /// `panes`: the panes the extension opened, listed until their sessions ended.
    /// @return Each with its tag, status and program; or why they cannot be listed now (`limit`: retry).
    [[nodiscard]] virtual std::expected<std::vector<PaneInfo>, HostError> panes() = 0;

    /// `pane-reveal`: brings a pane the extension opened in front of the origin's window (IPaneService).
    /// @param pane Its id.
    /// @param origin The connection the invoked action came from; one the host does not know reveals it where
    ///               the user looked last.
    /// @return Nothing; or why it was not revealed.
    [[nodiscard]] virtual std::expected<void, HostError> paneReveal(std::string_view pane, Origin origin) = 0;

    /// `pane-send`: types text the user wrote into a pane the extension opened (IPaneService).
    /// @param pane The pane's id.
    /// @param text What the user wrote.
    /// @return Nothing; or why it was not typed.
    [[nodiscard]] virtual std::expected<void, HostError> paneSend(std::string_view pane,
                                                                  std::string_view text) = 0;

    /// `tab-open`: opens the extension's served tab where the origin's window is (IServedTabService).
    /// @param scheme What the tab is served as.
    /// @param origin The connection the invoked action came from.
    /// @return The tab's id; or why it was not opened.
    [[nodiscard]] virtual std::expected<std::string, HostError> tabOpen(std::string_view scheme,
                                                                        Origin origin) = 0;

    /// `tab-close`: closes one of the extension's served tabs.
    /// @param tab Its id.
    /// @return Nothing; or why it was not closed.
    [[nodiscard]] virtual std::expected<void, HostError> tabClose(std::string_view tab) = 0;

    /// `tab-reveal`: brings one of the extension's served tabs in front of the origin's window.
    /// @param tab Its id.
    /// @param origin The connection the invoked action came from.
    /// @return Nothing; or why it was not revealed.
    [[nodiscard]] virtual std::expected<void, HostError> tabReveal(std::string_view tab, Origin origin) = 0;

    /// `tabs`: the extension's served tabs.
    /// @return Each with its scheme and size; or why they cannot be listed.
    [[nodiscard]] virtual std::expected<std::vector<TabInfo>, HostError> tabs() = 0;

    /// `tab-read`: the input a served tab's terminal wrote since the last read.
    /// @param tab Its id.
    /// @return The bytes; or why there are none.
    [[nodiscard]] virtual std::expected<ByteList, HostError> tabRead(std::string_view tab) = 0;

    /// `tab-write`: draws into a served tab.
    /// @param tab Its id.
    /// @param bytes Its terminal's output.
    /// @return Nothing; or why it was not written.
    [[nodiscard]] virtual std::expected<void, HostError> tabWrite(std::string_view tab,
                                                                  std::span<uint8_t const> bytes) = 0;

    /// `actions-take`: the actions the user invoked since the last take, oldest first.
    /// @return Each with its origin; or why they cannot be taken.
    [[nodiscard]] virtual std::expected<std::vector<ActionInfo>, HostError> actionsTake() = 0;

    /// `summary-set`: what the extension shows of itself.
    /// @param shortText The one-line summary.
    /// @param detail The longer text.
    /// @return Nothing; or why it was not taken.
    [[nodiscard]] virtual std::expected<void, HostError> summarySet(std::string_view shortText,
                                                                    std::string_view detail) = 0;

    /// `file-read`: a regular file inside a root declared readable (IFileReadService).
    /// @param root The label of the root.
    /// @param path The file, relative to the root's directory.
    /// @return Its bytes and modification time; or why it was not read.
    [[nodiscard]] virtual std::expected<FileContent, FileError> fileRead(std::string_view root,
                                                                         std::string_view path) = 0;
};

} // namespace vtextension
