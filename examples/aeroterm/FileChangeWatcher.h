#pragma once

#include <functional>
#include <filesystem>
#include <thread>

class FileChangeWatcher {
  public:
    enum class Event {
        Modified,
        Erased,
    };
    using Notifier = std::function<void(Event)>;

    FileChangeWatcher(std::filesystem::path _filePath, Notifier _notifier);
    ~FileChangeWatcher();

    // stop watching on that file early
    void stop();

  private:
    void watch();

  private:
    std::filesystem::path filePath_;
    Notifier notifier_;
    bool exit_;
    std::thread watcher_;
};
