// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/GuiConfigStore.hpp>

#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace contour::config
{

/// Why a Settings change did not take.
enum class SettingsErrorKind : uint8_t
{
    WriteFailed = 0, ///< The file system refused a side file's write or removal.
    Locked,          ///< `gui_config_locked` is set in contour.yml: Settings writes nothing.
    ReadOnly,        ///< The entity cannot be edited here (defined in contour.yml, built in, or unparsable).
    ChangedOnDisk,   ///< The side file changed outside Settings; its version was adopted, nothing written.
    InvalidValue,    ///< A value the loader would reject, refused before it was staged.
    UnknownSetting,  ///< A key no Settings row has.
    NotApplied,      ///< A value settings.yml would hold but the loader does not apply.
    NameTaken,       ///< A name another profile or colour scheme already has.
    ReloadFailed,    ///< The side files were written, but the configuration did not reload.
};

/// A Settings change that did not take, and the sentence the page shows for it.
struct SettingsError
{
    SettingsErrorKind kind = SettingsErrorKind::WriteFailed; ///< Why.
    std::string message {};                                  ///< The sentence the page shows.
};

/// Runs a deferred flush: the time source of the save model, injected so tests fire it by hand.
class SaveScheduler
{
  public:
    virtual ~SaveScheduler() = default;

    /// Fires @p fire once after @p delay, replacing any fire still pending: a restart, which is what a
    /// debounce is.
    /// @param delay How long from now.
    /// @param fire  What to run then.
    virtual void schedule(std::chrono::milliseconds delay, std::function<void()> fire) = 0;

    /// Drops the pending fire, if any.
    virtual void cancel() = 0;
};

/// How long the save model waits after the last change before it writes: a slider dragged across its
/// range writes once, not once per step.
inline constexpr auto SettingsSaveDebounce = std::chrono::milliseconds { 300 };

/// What the page says while `gui_config_locked` refuses every change.
inline constexpr std::string_view SettingsLockedMessage =
    "Settings are locked by gui_config_locked in contour.yml.";

/// Whether Settings may write the GUI side files at all.
enum class SettingsEditability : uint8_t
{
    Editable = 0, ///< Changes are staged and written.
    Locked,       ///< `gui_config_locked` is set in contour.yml: nothing is staged.
};

/// One Settings save model over the GUI side files: every change is staged and written after
/// SettingsSaveDebounce, each file is snapshotted when the session first touches it, Revert puts the
/// snapshots back, and a file changed on disk since the session last saw it is never written over.
///
/// The session never writes over bytes it has not seen. Before writing a file it reads it; when its bytes
/// differ from what the session last read or wrote, it writes nothing, adopts the disk's bytes as both what
/// it last saw and its snapshot, and reports SettingsErrorKind::ChangedOnDisk. A failed write drops what was
/// pending for that file, so the next reload shows what is on disk.
///
/// Configured at construction: the store, the scheduler and the editability are fixed for its lifetime.
/// GUI thread only, as the scheduler fires there.
class SettingsEditSession
{
  public:
    /// Reloads the configuration after a flush or a Revert wrote something.
    using ReloadCallback = std::function<std::expected<void, std::string>()>;

    /// Receives the errors a flush finds that no caller is there to return them to: every error of a
    /// timer-driven flush, and every one but the first of a direct one.
    using ErrorSink = std::function<void(SettingsError const&)>;

    /// @param store       Where the side files are read and written. Borrowed: it outlives the session.
    /// @param scheduler   Runs the debounced flush. Borrowed: it outlives the session.
    /// @param editability Whether anything may be staged.
    /// @param reload      Reloads the configuration once a flush or a Revert has written.
    /// @param reportError Receives the errors no caller is returned.
    SettingsEditSession(GuiConfigStore& store,
                        SaveScheduler& scheduler,
                        SettingsEditability editability,
                        ReloadCallback reload,
                        ErrorSink reportError);

    SettingsEditSession(SettingsEditSession const&) = delete;
    SettingsEditSession(SettingsEditSession&&) = delete;
    SettingsEditSession& operator=(SettingsEditSession const&) = delete;
    SettingsEditSession& operator=(SettingsEditSession&&) = delete;

    /// Cancels a pending flush: the scheduler must not call into a destroyed session.
    ~SettingsEditSession();

    /// Stages @p bytes as @p file's next content, and (re)starts the debounce.
    /// The first time the session touches @p file it reads and snapshots it.
    /// @param file  The side file.
    /// @param bytes Its new content; nullopt removes it.
    /// @return Nothing, or why nothing was staged (Locked, or a file that could not be read).
    [[nodiscard]] std::expected<void, SettingsError> stage(SideFile const& file,
                                                           std::optional<std::string> bytes);

    /// Writes every staged file now, then reloads once if anything was written or adopted.
    /// A flush with nothing staged does nothing and does not reload.
    /// @return Nothing, or the first error; the rest go to the error sink.
    [[nodiscard]] std::expected<void, SettingsError> flush();

    /// Drops what is staged and puts every touched file back as it was when the session first touched it
    /// (removing one that was absent), then reloads once. A file changed on disk since the session last saw
    /// it is adopted instead, as a flush adopts it.
    /// @return Nothing, or the first error; the rest go to the error sink.
    [[nodiscard]] std::expected<void, SettingsError> revert();

    /// @return Whether something is staged, or a touched file differs from its snapshot.
    [[nodiscard]] bool hasChanges() const noexcept;

    /// @return Whether something is staged and not yet written.
    [[nodiscard]] bool hasPending() const noexcept;

    /// @param file The side file.
    /// @return Whether something is staged for @p file and not yet written.
    [[nodiscard]] bool hasPendingFor(SideFile const& file) const;

  private:
    /// What the session knows of one file it touched.
    struct TouchedFile
    {
        std::optional<std::string> snapshot; ///< Its bytes when first touched; nullopt: it was absent.
        std::optional<std::string> lastSeen; ///< Its bytes when last read or written by the session.
    };

    /// Whether settling a file changed what the configuration reads from disk.
    enum class DiskChange : uint8_t
    {
        None = 0, ///< The file is as it was: nothing written, nothing adopted.
        Changed,  ///< The session wrote the file, or adopted a version written outside it.
    };

    /// What settling one file did.
    struct Settled
    {
        DiskChange change = DiskChange::None;  ///< Whether the configuration must reload.
        std::optional<SettingsError> error {}; ///< Why it did not take as asked, if it did not.
    };

    /// Reads @p file as it is on disk now.
    [[nodiscard]] std::expected<std::optional<std::string>, SettingsError> readNow(
        SideFile const& file) const;

    /// Makes @p file hold @p bytes (absent for nullopt), unless it changed on disk since the session last
    /// saw it: then the disk's version is adopted and nothing is written.
    [[nodiscard]] Settled settle(SideFile const& file, std::optional<std::string> const& bytes);

    /// Settles each of @p targets, reloads once if any of them changed on disk, and returns the first
    /// error, sending the rest to the sink.
    [[nodiscard]] std::expected<void, SettingsError> settleAll(
        std::map<SideFile, std::optional<std::string>> const& targets);

    GuiConfigStore& _store;
    SaveScheduler& _scheduler;
    SettingsEditability _editability;
    ReloadCallback _reload;
    ErrorSink _reportError;

    std::map<SideFile, TouchedFile> _touched;                ///< Every file staged since the session began.
    std::map<SideFile, std::optional<std::string>> _pending; ///< Staged, not yet written.
};

} // namespace contour::config
