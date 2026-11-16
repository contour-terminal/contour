// SPDX-License-Identifier: Apache-2.0
#ifdef _WIN32

    #include <core/platform/NativeFileSystem.hpp>

    #include <format>
    #include <memory>
    #include <system_error>

    #include <windows.h>

    #include <vthost/DaemonConsentKey.hpp>
    #include <vthost/FilePrivacy.hpp>

namespace vthost
{

namespace
{
    /// What an entry of the consent directory is expected to be.
    enum class EntryKind : uint8_t
    {
        File = 0,
        Directory,
    };

    /// @return Nothing when @p path is a @p kind of its own: not a link, a junction or any other reparse
    ///         point, which every use of it -- restrictToOwner() above all -- would follow to another.
    [[nodiscard]] std::expected<void, std::string> checkEntry(std::filesystem::path const& path,
                                                              EntryKind kind)
    {
        auto error = std::error_code {};
        auto const type = std::filesystem::symlink_status(path, error).type();
        auto const attributes = ::GetFileAttributesW(path.c_str());
        if (type == std::filesystem::file_type::symlink || type == std::filesystem::file_type::junction
            || (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0))
            return std::unexpected(std::format(
                "'{}' is not an entry of its own (a link or junction?); it is not used", path.string()));
        auto const isDirectory =
            attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (attributes == INVALID_FILE_ATTRIBUTES || isDirectory != (kind == EntryKind::Directory))
            return std::unexpected(std::format(
                "'{}' is not a {}", path.string(), kind == EntryKind::Directory ? "directory" : "file"));
        return {};
    }

    /// A key file held open, sharing reading but no deletion: while it is open, no other run removes the file
    /// or replaces it. Not inheritable (no SECURITY_ATTRIBUTES), so no child carries it.
    class HandleHold final: public ConsentKeyHold
    {
      public:
        explicit HandleHold(HANDLE handle) noexcept: _handle { handle } {}
        HandleHold(HandleHold const&) = delete;
        HandleHold(HandleHold&&) = delete;
        HandleHold& operator=(HandleHold const&) = delete;
        HandleHold& operator=(HandleHold&&) = delete;
        ~HandleHold() override { ::CloseHandle(_handle); }

      private:
        HANDLE _handle;
    };

    /// Opens @p file to hold it (HandleHold): as itself, never through a reparse point, which is refused.
    [[nodiscard]] std::expected<std::unique_ptr<ConsentKeyHold>, std::string> holdFile(
        std::filesystem::path const& file)
    {
        auto* const handle = ::CreateFileW(file.c_str(),
                                           GENERIC_READ,
                                           FILE_SHARE_READ,
                                           nullptr,
                                           OPEN_EXISTING,
                                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                                           nullptr);
        if (handle == INVALID_HANDLE_VALUE)
            return std::unexpected(
                std::format("cannot hold '{}': error {}", file.string(), ::GetLastError()));
        auto hold = std::make_unique<HandleHold>(handle);
        auto attributes = FILE_ATTRIBUTE_TAG_INFO {};
        if (::GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &attributes, sizeof(attributes))
                == FALSE
            || (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
            return std::unexpected(
                std::format("'{}' is not a file of its own; it is not used", file.string()));
        return hold;
    }

    /// The OS's consent directory on Windows, where the DACL NativeFilePrivacy sets is what keeps others out.
    class WindowsConsentKeyFiles final: public IConsentKeyFiles
    {
      public:
        [[nodiscard]] std::expected<std::unique_ptr<ConsentKeyHold>, std::string> keep(
            std::filesystem::path const& directory,
            std::string_view name,
            std::string_view content) const override
        {
            auto const& fileSystem = core::platform::NativeFileSystem::instance();
            auto const& privacy = NativeFilePrivacy::instance();
            auto const file = directory / name;
            auto temporary = file;
            temporary += ".tmp";
            std::ignore = fileSystem.remove(temporary);
            // Every entry is checked right before restrictToOwner() sets its DACL by name, so none is set
            // through a link or junction planted there.
            return fileSystem.createDirectories(directory)
                .and_then([&] { return checkEntry(directory, EntryKind::Directory); })
                .and_then([&] { return privacy.restrictToOwner(directory, PrivateEntry::Directory); })
                .and_then([&] { return fileSystem.createNewFile(temporary); })
                .and_then([&] { return checkEntry(temporary, EntryKind::File); })
                .and_then([&] { return privacy.restrictToOwner(temporary, PrivateEntry::File); })
                .and_then([&] { return fileSystem.writeFile(temporary, content); })
                .and_then([&] { return fileSystem.syncFile(temporary); })
                .and_then([&] { return fileSystem.replaceFile(temporary, file); })
                .and_then([&] { return holdFile(file); })
                .transform_error([&](std::string detail) {
                    std::ignore = fileSystem.remove(temporary);
                    return detail;
                });
        }

        [[nodiscard]] std::expected<std::string, std::string> read(std::filesystem::path const& directory,
                                                                   std::string_view name) const override
        {
            auto const file = directory / name;
            return checkEntry(directory, EntryKind::Directory)
                .and_then([&] { return checkEntry(file, EntryKind::File); })
                .and_then([&] { return NativeFilePrivacy::instance().readPrivateFile(file); });
        }

        [[nodiscard]] std::expected<std::vector<std::string>, std::string> list(
            std::filesystem::path const& directory) const override
        {
            if (auto checked = checkEntry(directory, EntryKind::Directory); !checked)
                return std::unexpected(std::move(checked.error()));
            auto names = std::vector<std::string> {};
            auto error = std::error_code {};
            for (auto const& entry: std::filesystem::directory_iterator(directory, error))
                names.push_back(entry.path().filename().string());
            return names;
        }

        [[nodiscard]] std::expected<KeyFileRemoval, std::string> removeUnlessHeld(
            std::filesystem::path const& directory, std::string_view name) const override
        {
            if (auto checked = checkEntry(directory, EntryKind::Directory); !checked)
                return std::unexpected(std::move(checked.error()));
            // DeleteFileW removes a link itself, never its target. A live run's hold shares no deletion.
            auto const file = directory / name;
            if (::DeleteFileW(file.c_str()) != FALSE)
                return KeyFileRemoval::Removed;
            switch (auto const error = ::GetLastError(); error)
            {
                case ERROR_FILE_NOT_FOUND:
                case ERROR_PATH_NOT_FOUND: return KeyFileRemoval::Removed;
                case ERROR_SHARING_VIOLATION:
                case ERROR_ACCESS_DENIED: return KeyFileRemoval::Held;
                default:
                    return std::unexpected(std::format("cannot remove '{}': error {}", file.string(), error));
            }
        }
    };
} // namespace

IConsentKeyFiles const& nativeConsentKeyFiles()
{
    static auto const files = WindowsConsentKeyFiles {};
    return files;
}

} // namespace vthost

#endif
