// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What every directory stream shares: it reads the directory in batches of bounded size, one hop
/// to the blocking executor per batch, and hands the entries out one call at a time, so it never
/// holds more of a directory than one batch, however large the directory is. Private to the fs
/// module.

#include <core/async/Task.hpp>
#include <core/fs/Descriptor.hpp>
#include <core/fs/FsError.hpp>

#include <cstddef>
#include <optional>
#include <vector>

namespace core::fs::detail
{

/// The most entries one batch of a stream that counts entries (`readdir`, in memory) reads. One
/// that reads records reads what @c DirectoryBufferBytes holds instead.
inline constexpr std::size_t DirectoryBatchEntries = 64;

/// The buffer a stream that reads records (Linux's `getdents64`, Windows'
/// `FileIdBothDirectoryInfo`) holds and reads each batch into.
inline constexpr std::size_t DirectoryBufferBytes = std::size_t { 64 } * 1024;

/// What one batch read of a directory came to.
struct DirectoryBatch
{
    /// The entries read, in order, each an entry or why it cannot be one (a name that is not UTF-8).
    /// Never `.` or `..`.
    std::vector<FsResult<DirectoryEntry>> entries;
    /// @c StreamEnd::Reached where the directory has nothing after them.
    StreamEnd end = StreamEnd::More;
};

/// A directory stream that reads through @c fetch, a batch at a time, and answers @c next from the
/// batch in hand without a hop until it is spent. One call at a time: a call is made once the one
/// before it has completed. A pending @c next resumes into the stream, which must outlive it.
class BatchedDirectoryStream: public IDirectoryStream
{
  public:
    /// @return The next entry; nullopt once the directory has ended, and from then on. A batch that
    ///         failed is answered with its error once, and ends the stream.
    [[nodiscard]] async::Task<FsResult<std::optional<DirectoryEntry>>> next() final;

  protected:
    /// Reads the next batch, on the blocking executor. Called only once the batch before it is
    /// spent, and never again after one that ended the directory or failed.
    /// @return The batch; why the directory could not be read.
    [[nodiscard]] virtual async::Task<FsResult<DirectoryBatch>> fetch() = 0;

  private:
    std::vector<FsResult<DirectoryEntry>> _entries; ///< The batch in hand.
    std::size_t _position = 0;                      ///< The next of @c _entries to hand out.
    StreamEnd _end = StreamEnd::More;               ///< Whether a batch ended the directory.
};

} // namespace core::fs::detail
