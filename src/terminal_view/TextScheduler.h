/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019 Christian Parpart <christian@parpart.family>
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

#include <terminal/Screen.h>

#include <unicode/run_segmenter.h>

#include <functional>
#include <vector>

namespace terminal::view {

class TextScheduler {
  public:
    using Flusher = std::function<void(TextScheduler const&)>;

    explicit TextScheduler(Flusher _flusher);

    constexpr cursor_pos_t row() const noexcept { return row_; }
    constexpr cursor_pos_t startColumn() const noexcept { return startColumn_; }
    constexpr ScreenBuffer::GraphicsAttributes attributes() const noexcept { return attributes_; }

    std::vector<char32_t> const& codepoints() const noexcept { return codepoints_; }
    std::vector<unsigned> const& clusters() const noexcept { return clusters_; }

    unicode::run_segmenter::range const& run() const noexcept { return run_; }

    void reset();
    void reset(cursor_pos_t _row, cursor_pos_t _col, ScreenBuffer::GraphicsAttributes const& _attr);
    void schedule(cursor_pos_t _row, cursor_pos_t _col, Screen::Cell const& _cell);
    void flush();

  private:
    void extend(ScreenBuffer::Cell const& _cell, cursor_pos_t _column);

  private:
    enum class State { Empty, Filling };

    State state_ = State::Empty;
    cursor_pos_t row_ = 1;
    cursor_pos_t startColumn_ = 1;
    ScreenBuffer::GraphicsAttributes attributes_ = {};
    std::vector<char32_t> codepoints_{};
    std::vector<unsigned> clusters_{};
    unsigned clusterOffset_ = 0;

    unicode::run_segmenter::range run_{};

    Flusher flusher_;
};

} // end namespace
