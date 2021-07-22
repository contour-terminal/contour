/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019-2021 Christian Parpart <christian@parpart.family>
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

#include <terminal/Terminal.h>
#include <terminal/logging.h>
#include <terminal/pty/MockViewPty.h>

#include <crispy/debuglog.h>

#include <libtermbench/termbench.h>

#include <iostream>
#include <optional>
#include <random>

#include <fmt/format.h>

using namespace std;

class HeadlessBench: public terminal::Terminal::Events
{
public:
    HeadlessBench(terminal::PageSize _pageSize,
                  int _ptyReadBufferSize,
                  std::optional<terminal::LineCount> _maxHistoryLineCount);

    terminal::MockViewPty& pty() noexcept { return *pty_; }
    terminal::Terminal& terminal() noexcept { return vt_; }

private:
    std::unique_ptr<terminal::MockViewPty> pty_;
    terminal::Terminal vt_;
};

HeadlessBench::HeadlessBench(terminal::PageSize _pageSize,
                             int _ptyReadBufferSize,
                             std::optional<terminal::LineCount> _maxHistoryLineCount):
    pty_{std::make_unique<terminal::MockViewPty>(_pageSize)},
    vt_{
        *pty_,
        _ptyReadBufferSize,
        *this,
        _maxHistoryLineCount
    }
{
    vt_.screen().setMode(terminal::DECMode::AutoWrap, true);
}

int main(int argc, char const* argv[])
{
    crispy::debugtag::disable(terminal::VTParserTag);

    auto pageSize = terminal::PageSize{terminal::LineCount(25), terminal::ColumnCount(80)};
    auto const ptyReadBufferSize = 8192;
    auto maxHistoryLineCount = optional{terminal::LineCount(10000)};
    auto hb = HeadlessBench{pageSize, ptyReadBufferSize, maxHistoryLineCount};

    auto tbp = contour::termbench::Benchmark{
        [&](char const* a, size_t b)
        {
            hb.pty().setReadData({a, b});
            do hb.terminal().processInputOnce();
            while (!hb.pty().stdoutBuffer().empty());
        },
        16, // MB
        80,
        24,
        [&](contour::termbench::Test const& _test)
        {
            cout << fmt::format("Running test {} ...\n", _test.name);
        }
    };

    tbp.add(contour::termbench::tests::many_lines());
    tbp.add(contour::termbench::tests::long_lines());
    tbp.add(contour::termbench::tests::sgr_fg_lines());
    tbp.add(contour::termbench::tests::sgr_fgbg_lines());
    //tbp.add(contour::termbench::tests::binary());

    tbp.runAll();

    cout << '\n';
    tbp.summarize(cout);
    cout << fmt::format("{:>12}: {}\n",
                        "history size",
                        hb.terminal().screen().maxHistoryLineCount().value_or(terminal::LineCount(0)));

    return EXIT_SUCCESS;
}
