// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// A filesystem whose reads wait for the test: what keeps a status-line extension's build in flight, on its
/// build thread, for as long as a case needs -- a loader stuck on a hung mount.

#include <condition_variable>
#include <mutex>

#include <vthost/testing/ForwardingFileSystem.hpp>

namespace vthost::testing
{

/// An InMemoryFileSystem whose readFile() parks until open() is called. Every other call passes through.
class GatedFileSystem final: public ForwardingFileSystem
{
  public:
    /// @return Whether a readFile() has started waiting; it waits until open().
    [[nodiscard]] bool entered() const
    {
        auto const lock = std::scoped_lock { _mutex };
        return _entered;
    }

    /// Lets every readFile() through, waiting or still to come.
    void open()
    {
        {
            auto const lock = std::scoped_lock { _mutex };
            _open = true;
        }
        _changed.notify_all();
    }

    [[nodiscard]] std::expected<std::string, std::string> readFile(
        std::filesystem::path const& path) const override
    {
        {
            auto lock = std::unique_lock { _mutex };
            _entered = true;
            _changed.wait(lock, [this] { return _open; });
        }
        return files.readFile(path);
    }

  private:
    mutable std::mutex _mutex;
    mutable std::condition_variable _changed;
    mutable bool _entered = false; ///< Guarded by _mutex.
    bool _open = false;            ///< Guarded by _mutex.
};

} // namespace vthost::testing
