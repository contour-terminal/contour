// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/test/ChromeSnapshotPlan.hpp>

#include <core/platform/FileSystem.hpp>

#include <QtCore/QBuffer>
#include <QtCore/QByteArray>
#include <QtCore/QIODevice>
#include <QtGui/QImage>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string_view>
#include <utility>

namespace contour::test::snapshot
{

/// Why a snapshot could not be written.
enum class SnapshotWriteError : uint8_t
{
    DirectoryUnavailable, ///< The output directory could not be created, or a file is in its place.
    EncodeFailed,         ///< The image is null or could not be encoded as a PNG.
    WriteFailed,          ///< The PNG or the manifest line could not be written.
};

/// The manifest's file name, inside the snapshot directory.
inline constexpr std::string_view ManifestFileName = "manifest.tsv";

/// Writes snapshot PNGs and their manifest lines into one directory, through an injected file system.
class ChromeSnapshotWriter
{
  public:
    /// Prepares @p directory for snapshots; the manifest header is written only if no manifest exists yet.
    /// @param files The file system every write goes through; it must outlive the writer.
    /// @param directory The directory to write into; created if missing.
    /// @return The writer, or DirectoryUnavailable when the path is a non-directory or cannot be created.
    [[nodiscard]] static std::expected<ChromeSnapshotWriter, SnapshotWriteError> open(
        core::platform::FileSystem const& files, std::filesystem::path directory)
    {
        if (files.exists(directory) && !files.isDirectory(directory))
            return std::unexpected(SnapshotWriteError::DirectoryUnavailable);
        if (!files.createDirectories(directory) || !files.isDirectory(directory))
            return std::unexpected(SnapshotWriteError::DirectoryUnavailable);
        auto const manifest = directory / ManifestFileName;
        if (!files.exists(manifest) && !files.writeFile(manifest, ManifestHeader))
            return std::unexpected(SnapshotWriteError::WriteFailed);
        return ChromeSnapshotWriter { files, std::move(directory) };
    }

    /// Encodes @p image as a PNG next to the manifest and appends the variant's manifest line.
    /// @param variant The picture being written; it names the file.
    /// @param image The rendered image.
    /// @param facts The machine facts to record in the manifest.
    /// @return The PNG's path, EncodeFailed for a null or unencodable image (nothing is appended), or
    ///         WriteFailed when a file write fails.
    [[nodiscard]] std::expected<std::filesystem::path, SnapshotWriteError> write(
        SnapshotVariant const& variant, QImage const& image, SnapshotFacts const& facts) const
    {
        if (image.isNull())
            return std::unexpected(SnapshotWriteError::EncodeFailed);
        auto bytes = QByteArray {};
        auto buffer = QBuffer { &bytes };
        if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG"))
            return std::unexpected(SnapshotWriteError::EncodeFailed);
        auto const path = _directory / snapshotFileName(variant);
        if (!_files->writeFile(path, std::string_view(bytes.constData(), static_cast<size_t>(bytes.size()))))
            return std::unexpected(SnapshotWriteError::WriteFailed);
        if (!_files->appendFile(_directory / ManifestFileName, manifestLine(variant, facts)))
            return std::unexpected(SnapshotWriteError::WriteFailed);
        return path;
    }

  private:
    ChromeSnapshotWriter(core::platform::FileSystem const& files, std::filesystem::path directory):
        _files { &files }, _directory { std::move(directory) }
    {
    }

    core::platform::FileSystem const* _files;
    std::filesystem::path _directory;
};

} // namespace contour::test::snapshot
