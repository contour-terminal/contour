#pragma once

#include "IncludeFilesystem.h"

#include <functional>
#include <thread>

class FileChangeWatcher {
  public:
    enum class Event {
        Modified,
        Erased,
    };
    using Notifier = std::function<void(Event)>;

    FileChangeWatcher(FileSystem::path _filePath, Notifier _notifier);
    ~FileChangeWatcher();

    // stop watching on that file early
    void stop();

  private:
    void watch();

  private:
    FileSystem::path filePath_;
    Notifier notifier_;
    bool exit_;
    std::thread watcher_;
};
