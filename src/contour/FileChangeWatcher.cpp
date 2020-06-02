/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019-2020 Christian Parpart <christian@parpart.family>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
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
