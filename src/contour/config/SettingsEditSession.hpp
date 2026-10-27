// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/GuiConfigStore.hpp>

#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <optional>
#include <span>
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

/// What a session has staged for one file and not yet written.
enum class PendingChange : uint8_t
{
    None = 0, ///< Nothing.
    Write,    ///< New bytes.
    Removal,  ///< Its removal.
};

/// One file of a change staged with stageTogether().
struct StagedFile
{
    SideFile file {};                    ///< The side file.
    std::optional<std::string> bytes {}; ///< Its new content; nullopt removes it.
};

/// One Settings save model over the GUI side files: every change is staged and written after
/// SettingsSaveDebounce, each file is snapshotted when the session first sees it, Revert puts the
/// snapshots back, and a file changed on disk since the session last saw it is never written over.
///
/// The session never writes over bytes it has not seen. A draft seeded from a file says so with observe(),
/// so that what the session last saw is what the draft was seeded from, not what the file holds when the
/// first change is staged. Before writing a file the session reads it; when its bytes differ from what the
/// session last read or wrote, it writes nothing, adopts the disk's bytes as both what it last saw and its
/// snapshot, and reports SettingsErrorKind::ChangedOnDisk. A failed write drops what was pending for that
/// file, so the next reload shows what is on disk; bytes that reached the disk all the same (the replace
/// took, a sync did not) are what the session last saw. Files staged together -- a rename's new file and the
/// old one's removal -- are written together or not at all.
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

    /// Records that a draft was seeded from @p seededFrom, @p file's bytes as the configuration loaded them:
    /// what the session saw of it, whatever the file holds now, so that an edit made outside Settings since
    /// that load is never written over. A file the session has seen already keeps what it saw while a change
    /// to it is pending; with nothing pending, bytes other than what it last saw were read by a reload of a
    /// change made outside Settings, which the draft now shows, and become its snapshot and what it saw, as
    /// a flush adopts a file changed on disk.
    /// @param file The side file.
    /// @param seededFrom Its bytes the draft was seeded from; nullopt: it was absent.
    void observe(SideFile const& file, std::optional<std::string> const& seededFrom);

    /// Stages @p bytes as @p file's next content, and (re)starts the debounce.
    /// The first time the session touches @p file it reads and snapshots it.
    /// @param file  The side file.
    /// @param bytes Its new content; nullopt removes it.
    /// @return Nothing, or why nothing was staged (Locked, or a file that could not be read).
    [[nodiscard]] std::expected<void, SettingsError> stage(SideFile const& file,
                                                           std::optional<std::string> bytes);

    /// Stages @p files as one change, written together or not at all (every write before any removal), and
    /// (re)starts the debounce. A file already staged keeps its place, and every change it was part of
    /// becomes one with this: a later stage() of a rename's new file leaves it in the rename.
    /// @param files The files and their new contents.
    /// @return Nothing, or why nothing was staged.
    [[nodiscard]] std::expected<void, SettingsError> stageTogether(std::span<StagedFile const> files);

    /// Writes every staged file now, then reloads once if anything was written or adopted.
    /// A flush with nothing staged does nothing and does not reload.
    /// @return Nothing, or the first error; the rest go to the error sink.
    [[nodiscard]] std::expected<void, SettingsError> flush();

    /// Writes the change @p file is staged in, and only that one, now, then reloads once if it wrote or
    /// adopted anything. Every other staged change stays pending, with the debounce running, and neither
    /// fails nor is reported by this flush: a caller that asked for one file's save judges that file.
    /// @param file A side file; nothing staged for it does nothing and does not reload.
    /// @return Nothing, or the first error of that change; the rest go to the error sink.
    [[nodiscard]] std::expected<void, SettingsError> flushChangeOf(SideFile const& file);

    /// Drops what is staged and puts every touched file back as it was when the session first saw it
    /// (removing one that was absent), then reloads once. A file changed on disk since the session last saw
    /// it is adopted instead, as a flush adopts it.
    /// @return Nothing, or the first error; the rest go to the error sink.
    [[nodiscard]] std::expected<void, SettingsError> revert();

    /// @return Whether something is staged, or a touched file differs from its snapshot.
    [[nodiscard]] bool hasChanges() const noexcept;

    /// @return Whether something is staged and not yet written.
    [[nodiscard]] bool hasPending() const noexcept;

    /// @param file The side file.
    /// @return What is staged for @p file and not yet written.
    [[nodiscard]] PendingChange pendingFor(SideFile const& file) const;

    /// @param kind The kind of side file.
    /// @return The files of @p kind with new bytes staged and not yet written, in no particular order.
    [[nodiscard]] std::vector<SideFile> pendingWrites(SideFileKind kind) const;

  private:
    /// What the session knows of one file it saw.
    struct TouchedFile
    {
        std::optional<std::string> snapshot; ///< Its bytes when first seen; nullopt: it was absent.
        std::optional<std::string> lastSeen; ///< Its bytes when last read or written by the session.
    };

    /// One staged file.
    struct Pending
    {
        std::optional<std::string> bytes; ///< Its new content; nullopt removes it.
        uint64_t change = 0;              ///< The change it belongs to; files of one change share it.
        uint64_t order = 0;               ///< Its place in the order it was staged in.
    };

    /// Whether settling a change altered what the configuration reads from disk.
    enum class DiskChange : uint8_t
    {
        None = 0, ///< As it was: nothing written, nothing adopted.
        Changed,  ///< The session wrote a file, or adopted a version written outside it.
    };

    /// What settling one change did.
    struct Settled
    {
        DiskChange change = DiskChange::None; ///< Whether the configuration must reload.
        std::vector<SettingsError> errors {}; ///< Why it did not take as asked, if it did not.
    };

    /// Reads @p file as it is on disk now.
    [[nodiscard]] std::expected<std::optional<std::string>, SettingsError> readNow(
        SideFile const& file) const;

    /// Reads and snapshots @p file unless the session saw it already.
    [[nodiscard]] std::expected<void, SettingsError> touch(SideFile const& file);

    /// Makes @p file hold @p bytes (absent for nullopt).
    [[nodiscard]] std::expected<void, std::string> writeNow(SideFile const& file,
                                                            std::optional<std::string> const& bytes);

    /// Makes each file of one change hold its bytes, in order, unless one of them changed on disk since the
    /// session last saw it: then those are adopted and nothing is written. A write that fails puts back
    /// what this change wrote before it.
    [[nodiscard]] Settled settleChange(std::vector<StagedFile> const& files);

    /// Settles every change of @p targets, reloads once if any of them changed on disk, and returns the first
    /// error, sending the rest to the sink.
    [[nodiscard]] std::expected<void, SettingsError> settleAll(std::map<SideFile, Pending> const& targets);

    /// (Re)starts the debounce.
    void scheduleFlush();

    GuiConfigStore& _store;
    SaveScheduler& _scheduler;
    SettingsEditability _editability;
    ReloadCallback _reload;
    ErrorSink _reportError;

    std::map<SideFile, TouchedFile> _touched; ///< Every file seen since the session began.
    std::map<SideFile, Pending> _pending;     ///< Staged, not yet written.
    uint64_t _nextChange = 1;                 ///< The id of the next change staged.
    uint64_t _nextOrder = 0;                  ///< The place of the next file staged.
};

} // namespace contour::config
