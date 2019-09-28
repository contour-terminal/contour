#include "FileChangeWatcher.h"
#include <thread>
#include <chrono>

using namespace std;

FileChangeWatcher::FileChangeWatcher(FileSystem::path _filePath, Notifier _notifier) :
    filePath_{ move(_filePath) },
    notifier_{ move(_notifier) },
    exit_{ false },
    watcher_{ bind(&FileChangeWatcher::watch, this) }
{
}

FileChangeWatcher::~FileChangeWatcher()
{
    stop();
    watcher_.join();
}

void FileChangeWatcher::watch()
{
    auto lastWriteTime = FileSystem::last_write_time(filePath_);
    while (!exit_)
    {
        if (!FileSystem::exists(filePath_))
            notifier_(Event::Erased);

        auto lwt = FileSystem::last_write_time(filePath_);
        if (lwt != lastWriteTime)
        {
            lastWriteTime = lwt;
            notifier_(Event::Modified);
        }
        this_thread::sleep_for(1s);
    }
}

void FileChangeWatcher::stop()
{
    exit_ = true;
}
