// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the screen-capture client (CaptureScreen.cpp) over an injected transport: a canned
// VT peer answers the screen-size query and the capture request, so the whole protocol round-trip
// (CSI 18 t handshake, CSI > Ps;Ps t request, PM 314 reply chunks, words splitting, timeout
// behavior) runs headlessly.

#include <contour/CaptureScreen.h>

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <sstream>
#include <string>
#include <string_view>

using namespace std::string_view_literals;

namespace
{

/// Canned VT peer: replies to the screen-size query and the capture request from a script.
class CannedTransport final: public contour::CaptureTransport
{
  public:
    explicit CannedTransport(std::string captureReply, bool answerScreenSize = true):
        _captureReply { std::move(captureReply) }, _answerScreenSize { answerScreenSize }
    {
    }

    int wait(timeval*) override { return _readOffset < _pending.size() ? 1 : 0; }

    int read(void* buf, std::size_t size) override
    {
        auto const n = std::min(size, _pending.size() - _readOffset);
        std::memcpy(buf, _pending.data() + _readOffset, n);
        _readOffset += n;
        return static_cast<int>(n);
    }

    int write(std::string_view chunk) override
    {
        requests.append(chunk);
        if (chunk == "\033[18t"sv)
        {
            if (_answerScreenSize)
                _pending += "\033[8;25;80t";
        }
        else if (chunk.starts_with("\033[>"sv))
            _pending += _captureReply;
        return static_cast<int>(chunk.size());
    }

    std::string requests;

  private:
    std::string _captureReply;
    bool _answerScreenSize;
    std::string _pending;
    std::size_t _readOffset = 0;
};

} // namespace

TEST_CASE("captureScreen: a full capture round-trip lands the payload in the output", "[capture]")
{
    // Reply = one PM 314 chunk with the content, one empty PM 314 chunk as the end marker.
    auto transport = CannedTransport { "\033^314;hello capture\n\033\\"
                                       "\033^314;\033\\" };
    auto settings = contour::CaptureSettings {};
    settings.timeout = 1.0;
    settings.lineCount = vtbackend::LineCount(2);

    auto out = std::ostringstream {};
    CHECK(contour::captureScreen(settings, transport, out));
    CHECK(out.str() == "hello capture\n");
    // The request must carry the physical-lines flag and the line count.
    CHECK(transport.requests.find("\033[>0;2t") != std::string::npos);
}

TEST_CASE("captureScreen: words mode splits the payload one word per line", "[capture]")
{
    auto transport = CannedTransport { "\033^314;alpha beta\033\\"
                                       "\033^314;\033\\" };
    auto settings = contour::CaptureSettings {};
    settings.words = true;
    settings.logicalLines = true;

    auto out = std::ostringstream {};
    CHECK(contour::captureScreen(settings, transport, out));
    CHECK(out.str() == "alpha\nbeta\n");
    CHECK(transport.requests.find("\033[>1;0t") != std::string::npos); // logical-lines flag
}

TEST_CASE("captureScreen: an unanswered capture request times out cleanly", "[capture]")
{
    auto transport = CannedTransport { /*captureReply=*/"" };
    auto settings = contour::CaptureSettings {};
    settings.timeout = 0.05;

    auto out = std::ostringstream {};
    CHECK_FALSE(contour::captureScreen(settings, transport, out));
    CHECK(out.str().empty());
}

TEST_CASE("captureScreen: a peer that ignores the screen-size query fails the session", "[capture]")
{
    auto transport = CannedTransport { "", /*answerScreenSize=*/false };
    auto settings = contour::CaptureSettings {};
    settings.timeout = 0.05;

    auto out = std::ostringstream {};
    CHECK_FALSE(contour::captureScreen(settings, transport, out));
}

TEST_CASE("captureScreen: verbose mode logs the screen size and still captures", "[capture]")
{
    // verbosityLevel > 0 takes the diagnostic-logging branch (to stderr) before the capture request;
    // the capture itself must still succeed and land the payload.
    auto transport = CannedTransport { "\033^314;verbose payload\n\033\\"
                                       "\033^314;\033\\" };
    auto settings = contour::CaptureSettings {};
    settings.timeout = 1.0;
    settings.verbosityLevel = 1;
    settings.outputFile = "-";

    auto out = std::ostringstream {};
    CHECK(contour::captureScreen(settings, transport, out));
    CHECK(out.str() == "verbose payload\n");
}
