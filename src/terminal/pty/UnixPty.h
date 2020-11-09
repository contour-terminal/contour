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

#include <terminal/pty/Pty.h>

#include <optional>

#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif

namespace terminal {

class UnixPty : public Pty
{
  public:
    explicit UnixPty(Size const& windowSize);
    ~UnixPty() override;

    int read(char* buf, size_t size) override;
    int write(char const* buf, size_t size) override;
    Size screenSize() const noexcept override;
    void resizeScreen(Size _cells, std::optional<Size> _pixels = std::nullopt) override;

    void prepareParentProcess() override;
    void prepareChildProcess() override;
    void close() override;

  private:
    Size size_;
    int master_;
    int slave_;
};

}  // namespace terminal
