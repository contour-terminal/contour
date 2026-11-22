// SPDX-License-Identifier: Apache-2.0
//
// IGuestHost over the bindings `wit-bindgen c` generated from the daemon world (bindings/daemon.h). What the
// guest passes is borrowed for the call; what the host returns is allocated in the guest's memory through
// `cabi_realloc` and belongs to the guest, which copies it and frees it with the bindings' `*_free`
// functions. WebAssembly only.
#include <orchestrator/guest/WitHost.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <bindings/daemon.h>

namespace orchestrator::guest
{

namespace
{
    // The bindings' records as the Canonical ABI lays them out for wasm32, the same rows the host's codec is
    // tested against (DaemonWorld: layouts follow the Canonical ABI): a binding generated from another WIT
    // fails to build here rather than read the host's answers wrong.
    static_assert(sizeof(terminal_extensions_daemon_host_proc_info_t) == 52);
    static_assert(alignof(terminal_extensions_daemon_host_proc_info_t) == 4);
    static_assert(sizeof(terminal_extensions_daemon_host_pane_info_t) == 32);
    static_assert(alignof(terminal_extensions_daemon_host_pane_info_t) == 4);
    static_assert(sizeof(terminal_extensions_daemon_host_chunk_t) == 12);
    static_assert(alignof(terminal_extensions_daemon_host_chunk_t) == 4);
    static_assert(sizeof(terminal_extensions_daemon_host_file_content_t) == 16);
    static_assert(alignof(terminal_extensions_daemon_host_file_content_t) == 8);
    static_assert(sizeof(terminal_extensions_daemon_host_tab_info_t) == 24);
    static_assert(alignof(terminal_extensions_daemon_host_tab_info_t) == 4);
    static_assert(sizeof(terminal_extensions_daemon_host_action_t) == 12);
    static_assert(alignof(terminal_extensions_daemon_host_action_t) == 4);

    using HostErrorCode = terminal_extensions_daemon_host_host_error_t;
    using FileErrorCode = terminal_extensions_daemon_host_file_error_t;

    /// @return @p text, borrowed by an import for the call; the bindings never write through it.
    [[nodiscard]] daemon_string_t borrowed(std::string_view text) noexcept
    {
        return daemon_string_t { .ptr = reinterpret_cast<uint8_t*>(const_cast<char*>(text.data())),
                                 .len = text.size() };
    }

    [[nodiscard]] daemon_list_u8_t borrowedBytes(std::string_view bytes) noexcept
    {
        return daemon_list_u8_t { .ptr = reinterpret_cast<uint8_t*>(const_cast<char*>(bytes.data())),
                                  .len = bytes.size() };
    }

    /// @return A copy of a string the host returned; the caller frees the original.
    [[nodiscard]] std::string copied(daemon_string_t const& text)
    {
        return { reinterpret_cast<char const*>(text.ptr), text.len };
    }

    [[nodiscard]] std::string copied(daemon_list_u8_t const& bytes)
    {
        return { reinterpret_cast<char const*>(bytes.ptr), bytes.len };
    }

    // The WIT's error cases, counted as the native enums count them.
    static_assert(TERMINAL_EXTENSIONS_DAEMON_HOST_HOST_ERROR_PROGRAM_MISSING + 1U == HostErrorCount);
    static_assert(TERMINAL_EXTENSIONS_DAEMON_HOST_FILE_ERROR_NOT_SUPPORTED_YET + 1U == FileErrorCount);

    [[nodiscard]] HostError hostErrorOf(HostErrorCode code) noexcept
    {
        // An error the bindings do not know cannot come from the WIT they were generated from.
        return code <= TERMINAL_EXTENSIONS_DAEMON_HOST_HOST_ERROR_PROGRAM_MISSING
                   ? static_cast<HostError>(code)
                   : HostError::BadArgument;
    }

    [[nodiscard]] FileError fileErrorOf(FileErrorCode code) noexcept
    {
        return code <= TERMINAL_EXTENSIONS_DAEMON_HOST_FILE_ERROR_NOT_SUPPORTED_YET
                   ? static_cast<FileError>(code)
                   : FileError::Unreadable;
    }

    [[nodiscard]] OutputExtent extentOf(terminal_extensions_daemon_host_extent_t extent) noexcept
    {
        return extent == TERMINAL_EXTENSIONS_DAEMON_HOST_EXTENT_TRUNCATED ? OutputExtent::Truncated
                                                                          : OutputExtent::Whole;
    }

    [[nodiscard]] ProcCompletion completionOf(
        terminal_extensions_daemon_host_completion_t completion) noexcept
    {
        switch (completion)
        {
            case TERMINAL_EXTENSIONS_DAEMON_HOST_COMPLETION_SIGNALLED: return ProcCompletion::Signalled;
            case TERMINAL_EXTENSIONS_DAEMON_HOST_COMPLETION_TIMED_OUT: return ProcCompletion::TimedOut;
            default: return ProcCompletion::Exited;
        }
    }

    [[nodiscard]] ProcInfo procInfoOf(terminal_extensions_daemon_host_proc_info_t const& info)
    {
        auto result = std::optional<ProcResult> {};
        if (info.state.tag == TERMINAL_EXTENSIONS_DAEMON_HOST_PROC_STATE_FINISHED)
        {
            auto const& finished = info.state.val.finished;
            result = ProcResult {
                .completion = completionOf(finished.completion),
                .exitCode =
                    finished.exit_code.is_some ? std::optional { finished.exit_code.val } : std::nullopt,
                .stdoutBytes = finished.stdout_bytes,
                .stdoutExtent = extentOf(finished.stdout_extent),
                .stderrBytes = finished.stderr_bytes,
                .stderrExtent = extentOf(finished.stderr_extent),
            };
        }
        return ProcInfo {
            .handle = info.handle, .tag = copied(info.tag), .command = copied(info.command), .result = result
        };
    }

    [[nodiscard]] PaneInfo paneInfoOf(terminal_extensions_daemon_host_pane_info_t const& info)
    {
        // The world's agent-status, indexed by its case: `blocked` is a wait asking cannot be told from.
        constexpr auto AgentStatusOfCase = std::array {
            AgentStatus::Idle,    // idle
            AgentStatus::Working, // working
            AgentStatus::Done,    // done
            AgentStatus::Waiting, // blocked
            AgentStatus::Error,   // error
            AgentStatus::Asking,  // asking
        };
        static_assert(AgentStatusOfCase.size() == TERMINAL_EXTENSIONS_DAEMON_HOST_AGENT_STATUS_ASKING + 1U);
        auto status = std::optional<AgentStatus> {};
        if (info.status.is_some && info.status.val < AgentStatusOfCase.size())
            status = AgentStatusOfCase.at(info.status.val);
        return PaneInfo { .pane = copied(info.pane),
                          .tag = copied(info.tag),
                          .status = status,
                          .message = copied(info.message),
                          .program = info.program == TERMINAL_EXTENSIONS_DAEMON_HOST_PROGRAM_ALIVE
                                         ? ProgramLiveness::Alive
                                         : ProgramLiveness::Gone };
    }

    /// A call's arguments as the bindings take them: (placeholder, value) pairs borrowed from @p arguments.
    class BorrowedArguments
    {
      public:
        explicit BorrowedArguments(std::span<Argument const> arguments)
        {
            _pairs.reserve(arguments.size());
            for (auto const& argument: arguments)
                _pairs.push_back(daemon_tuple2_string_string_t { .f0 = borrowed(argument.name),
                                                                 .f1 = borrowed(argument.value) });
        }

        [[nodiscard]] daemon_list_tuple2_string_string_t list() noexcept
        {
            return daemon_list_tuple2_string_string_t { .ptr = _pairs.data(), .len = _pairs.size() };
        }

      private:
        std::vector<daemon_tuple2_string_string_t> _pairs;
    };

    /// The daemon world's imports.
    class WitHost final: public IGuestHost
    {
      public:
        void log(LogLevel level, std::string_view message) override
        {
            auto text = borrowed(message);
            terminal_extensions_daemon_host_log(
                static_cast<terminal_extensions_daemon_host_log_level_t>(level), &text);
        }

        void wake(uint32_t afterMs) override { terminal_extensions_daemon_host_wake(afterMs); }

        [[nodiscard]] ClockReading now() override
        {
            auto reading = terminal_extensions_daemon_host_clock_reading_t {};
            terminal_extensions_daemon_host_now(&reading);
            return ClockReading { .monoMs = reading.monotonic_ms, .wallMs = reading.wall_ms };
        }

        [[nodiscard]] std::expected<uint32_t, HostError> procStart(std::string_view command,
                                                                   std::span<Argument const> arguments,
                                                                   std::string_view cwd,
                                                                   std::string_view tag) override
        {
            auto commandText = borrowed(command);
            auto borrowedArguments = BorrowedArguments { arguments };
            auto list = borrowedArguments.list();
            auto cwdText = borrowed(cwd);
            auto tagText = borrowed(tag);
            auto handle = terminal_extensions_daemon_host_proc_handle_t {};
            auto error = HostErrorCode {};
            if (!terminal_extensions_daemon_host_proc_start(
                    &commandText, &list, &cwdText, &tagText, &handle, &error))
                return std::unexpected(hostErrorOf(error));
            return handle;
        }

        [[nodiscard]] std::expected<std::vector<ProcInfo>, HostError> procs() override
        {
            auto listed = terminal_extensions_daemon_host_list_proc_info_t {};
            auto error = HostErrorCode {};
            if (!terminal_extensions_daemon_host_procs(&listed, &error))
                return std::unexpected(hostErrorOf(error));
            auto processes = std::vector<ProcInfo> {};
            processes.reserve(listed.len);
            for (auto const& info: std::span(listed.ptr, listed.len))
                processes.push_back(procInfoOf(info));
            terminal_extensions_daemon_host_list_proc_info_free(&listed);
            return processes;
        }

        [[nodiscard]] std::expected<Chunk, HostError> procTake(uint32_t handle,
                                                               ProcStream stream,
                                                               uint32_t offset) override
        {
            auto chunk = terminal_extensions_daemon_host_chunk_t {};
            auto error = HostErrorCode {};
            if (!terminal_extensions_daemon_host_proc_take(
                    handle,
                    static_cast<terminal_extensions_daemon_host_proc_stream_t>(stream),
                    offset,
                    &chunk,
                    &error))
                return std::unexpected(hostErrorOf(error));
            auto page = Chunk { .bytes = copied(chunk.bytes), .remaining = chunk.remaining };
            terminal_extensions_daemon_host_chunk_free(&chunk);
            return page;
        }

        [[nodiscard]] std::expected<void, HostError> procRelease(uint32_t handle) override
        {
            auto error = HostErrorCode {};
            if (!terminal_extensions_daemon_host_proc_release(handle, &error))
                return std::unexpected(hostErrorOf(error));
            return {};
        }

        [[nodiscard]] std::expected<std::optional<std::string>, HostError> storeGet(
            std::string_view key) override
        {
            auto keyText = borrowed(key);
            auto value = daemon_option_list_u8_t {};
            auto error = HostErrorCode {};
            if (!terminal_extensions_daemon_host_store_get(&keyText, &value, &error))
                return std::unexpected(hostErrorOf(error));
            auto stored = value.is_some ? std::optional { copied(value.val) } : std::nullopt;
            daemon_option_list_u8_free(&value);
            return stored;
        }

        [[nodiscard]] std::expected<void, HostError> storeSet(std::string_view key,
                                                              std::string_view value) override
        {
            auto keyText = borrowed(key);
            auto bytes = borrowedBytes(value);
            auto error = HostErrorCode {};
            if (!terminal_extensions_daemon_host_store_set(&keyText, &bytes, &error))
                return std::unexpected(hostErrorOf(error));
            return {};
        }

        [[nodiscard]] std::expected<void, HostError> storeRemove(std::string_view key) override
        {
            auto keyText = borrowed(key);
            auto error = HostErrorCode {};
            if (!terminal_extensions_daemon_host_store_remove(&keyText, &error))
                return std::unexpected(hostErrorOf(error));
            return {};
        }

        [[nodiscard]] std::expected<std::vector<std::string>, HostError> storeKeys(
            std::string_view prefix) override
        {
            auto prefixText = borrowed(prefix);
            auto listed = daemon_list_string_t {};
            auto error = HostErrorCode {};
            if (!terminal_extensions_daemon_host_store_keys(&prefixText, &listed, &error))
                return std::unexpected(hostErrorOf(error));
            auto keys = std::vector<std::string> {};
            keys.reserve(listed.len);
            for (auto const& key: std::span(listed.ptr, listed.len))
                keys.push_back(copied(key));
            daemon_list_string_free(&listed);
            return keys;
        }

        [[nodiscard]] std::expected<std::string, HostError> paneOpen(
            std::string_view paneTemplate,
            std::span<Argument const> arguments,
            std::string_view cwd,
            std::string_view title,
            std::string_view tag,
            std::optional<std::string_view> nearTab) override
        {
            auto templateText = borrowed(paneTemplate);
            auto borrowedArguments = BorrowedArguments { arguments };
            auto list = borrowedArguments.list();
            auto cwdText = borrowed(cwd);
            auto titleText = borrowed(title);
            auto tagText = borrowed(tag);
            auto nearText = borrowed(nearTab.value_or(std::string_view {}));
            auto pane = daemon_string_t {};
            auto error = HostErrorCode {};
            if (!terminal_extensions_daemon_host_pane_open(&templateText,
                                                           &list,
                                                           &cwdText,
                                                           &titleText,
                                                           &tagText,
                                                           nearTab ? &nearText : nullptr,
                                                           &pane,
                                                           &error))
                return std::unexpected(hostErrorOf(error));
            auto id = copied(pane);
            daemon_string_free(&pane);
            return id;
        }

        [[nodiscard]] std::expected<void, HostError> paneClose(std::string_view pane) override
        {
            auto paneText = borrowed(pane);
            auto error = HostErrorCode {};
            if (!terminal_extensions_daemon_host_pane_close(&paneText, &error))
                return std::unexpected(hostErrorOf(error));
            return {};
        }

        [[nodiscard]] std::expected<void, HostError> paneReveal(std::string_view pane,
                                                                uint32_t origin) override
        {
            auto paneText = borrowed(pane);
            auto error = HostErrorCode {};
            if (!terminal_extensions_daemon_host_pane_reveal(&paneText, origin, &error))
                return std::unexpected(hostErrorOf(error));
            return {};
        }

        [[nodiscard]] std::expected<void, HostError> paneSend(std::string_view pane,
                                                              std::string_view text) override
        {
            auto paneText = borrowed(pane);
            auto sentText = borrowed(text);
            auto error = HostErrorCode {};
            if (!terminal_extensions_daemon_host_pane_send(&paneText, &sentText, &error))
                return std::unexpected(hostErrorOf(error));
            return {};
        }

        [[nodiscard]] std::expected<std::vector<PaneInfo>, HostError> panes() override
        {
            auto listed = terminal_extensions_daemon_host_list_pane_info_t {};
            auto error = HostErrorCode {};
            if (!terminal_extensions_daemon_host_panes(&listed, &error))
                return std::unexpected(hostErrorOf(error));
            auto panes = std::vector<PaneInfo> {};
            panes.reserve(listed.len);
            for (auto const& info: std::span(listed.ptr, listed.len))
                panes.push_back(paneInfoOf(info));
            terminal_extensions_daemon_host_list_pane_info_free(&listed);
            return panes;
        }

        [[nodiscard]] std::expected<FileContent, FileError> fileRead(std::string_view root,
                                                                     std::string_view path) override
        {
            auto rootText = borrowed(root);
            auto pathText = borrowed(path);
            auto content = terminal_extensions_daemon_host_file_content_t {};
            auto error = FileErrorCode {};
            if (!terminal_extensions_daemon_host_file_read(&rootText, &pathText, &content, &error))
                return std::unexpected(fileErrorOf(error));
            auto file =
                FileContent { .bytes = copied(content.bytes), .modifiedUnixMs = content.modified_unix_ms };
            terminal_extensions_daemon_host_file_content_free(&content);
            return file;
        }

        [[nodiscard]] std::expected<std::string, HostError> tabOpen(std::string_view scheme,
                                                                    uint32_t origin) override
        {
            auto schemeText = borrowed(scheme);
            auto tab = daemon_string_t {};
            auto error = HostErrorCode {};
            if (!terminal_extensions_daemon_host_tab_open(&schemeText, origin, &tab, &error))
                return std::unexpected(hostErrorOf(error));
            auto id = copied(tab);
            daemon_string_free(&tab);
            return id;
        }

        [[nodiscard]] std::expected<void, HostError> tabReveal(std::string_view tab, uint32_t origin) override
        {
            auto tabText = borrowed(tab);
            auto error = HostErrorCode {};
            if (!terminal_extensions_daemon_host_tab_reveal(&tabText, origin, &error))
                return std::unexpected(hostErrorOf(error));
            return {};
        }

        [[nodiscard]] std::expected<std::vector<TabInfo>, HostError> tabs() override
        {
            auto listed = terminal_extensions_daemon_host_list_tab_info_t {};
            auto error = HostErrorCode {};
            if (!terminal_extensions_daemon_host_tabs(&listed, &error))
                return std::unexpected(hostErrorOf(error));
            auto tabs = std::vector<TabInfo> {};
            tabs.reserve(listed.len);
            for (auto const& info: std::span(listed.ptr, listed.len))
                tabs.push_back(TabInfo { .tab = copied(info.tab),
                                         .scheme = copied(info.scheme),
                                         .cols = info.cols,
                                         .rows = info.rows });
            terminal_extensions_daemon_host_list_tab_info_free(&listed);
            return tabs;
        }

        [[nodiscard]] std::expected<std::string, HostError> tabRead(std::string_view tab) override
        {
            auto tabText = borrowed(tab);
            auto bytes = daemon_list_u8_t {};
            auto error = HostErrorCode {};
            if (!terminal_extensions_daemon_host_tab_read(&tabText, &bytes, &error))
                return std::unexpected(hostErrorOf(error));
            auto input = copied(bytes);
            daemon_list_u8_free(&bytes);
            return input;
        }

        [[nodiscard]] std::expected<void, HostError> tabWrite(std::string_view tab,
                                                              std::string_view bytes) override
        {
            auto tabText = borrowed(tab);
            auto output = borrowedBytes(bytes);
            auto error = HostErrorCode {};
            if (!terminal_extensions_daemon_host_tab_write(&tabText, &output, &error))
                return std::unexpected(hostErrorOf(error));
            return {};
        }

        [[nodiscard]] std::expected<std::vector<ActionInfo>, HostError> actionsTake() override
        {
            auto listed = terminal_extensions_daemon_host_list_action_t {};
            auto error = HostErrorCode {};
            if (!terminal_extensions_daemon_host_actions_take(&listed, &error))
                return std::unexpected(hostErrorOf(error));
            auto actions = std::vector<ActionInfo> {};
            actions.reserve(listed.len);
            for (auto const& action: std::span(listed.ptr, listed.len))
                actions.push_back(ActionInfo { .id = copied(action.id), .origin = action.origin });
            terminal_extensions_daemon_host_list_action_free(&listed);
            return actions;
        }

        [[nodiscard]] std::expected<void, HostError> summarySet(std::string_view shortText,
                                                                std::string_view detail) override
        {
            auto shortValue = borrowed(shortText);
            auto detailValue = borrowed(detail);
            auto error = HostErrorCode {};
            if (!terminal_extensions_daemon_host_summary_set(&shortValue, &detailValue, &error))
                return std::unexpected(hostErrorOf(error));
            return {};
        }
    };
} // namespace

IGuestHost& witHost()
{
    static auto host = WitHost {};
    return host;
}

} // namespace orchestrator::guest
