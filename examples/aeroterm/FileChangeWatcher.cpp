#include "FileChangeWatcher.h"
#include <thread>
#include <chrono>

using namespace std;

FileChangeWatcher::FileChangeWatcher(filesystem::path _filePath, Notifier _notifier) :
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
    filesystem::file_time_type lastWriteTime = filesystem::last_write_time(filePath_);
    while (!exit_)
    {
        if (!filesystem::exists(filePath_))
            notifier_(Event::Erased);

        auto lwt = filesystem::last_write_time(filePath_);
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
