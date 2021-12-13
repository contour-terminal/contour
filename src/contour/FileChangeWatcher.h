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
#pragma once

#include <crispy/stdfs.h>

#include <functional>
#include <thread>

class FileChangeWatcher
{
  public:
    enum class Event
    {
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
