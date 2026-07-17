// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the screen-capture client (CaptureScreen.cpp) over an injected transport: a canned
// VT peer answers the screen-size query and the capture request, so the whole protocol round-trip
// (CSI 18 t handshake, CSI > Ps;Ps t request, PM 314 reply chunks, words splitting, timeout
// behavior) runs headlessly.

#include <contour/CaptureScreen.h>

#include <vtbackend/Functions.h>

#include <catch2/catch_test_macros.hpp>

#include <cctype>
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

/// Resolves the first private (`>`-led) CSI of @p request against the engine's real function table.
///
/// The `>` skips past the `CSI 18 t` screen-size handshake the client sends first; the capture request is
/// the only private-leader sequence it emits.
///
/// Pinning the request as a literal string is not enough on its own: it says the client still sends what
/// it always sent, not that the engine still answers it. The opcode moved out from under the client once
/// already -- XTCAPTURE gained a ',' intermediate, the bare form became xterm's XTSMTITLE, and
/// `contour capture` silently set title modes and captured nothing while every string-literal test kept
/// passing. Looking the request up here is what ties the two together.
vtbackend::Function const* resolveCsi(std::string_view request)
{
    auto const start = request.find("\033[>");
    if (start == std::string_view::npos)
        return nullptr;

    auto i = start + 2;
    auto const isBetween = [&](char lo, char hi) {
        return i < request.size() && lo <= request[i] && request[i] <= hi;
    };

    auto leader = '\0';
    if (isBetween(0x3C, 0x3F))
        leader = request[i++];

    auto argc = 1;
    for (; i < request.size() && (std::isdigit(static_cast<unsigned char>(request[i])) || request[i] == ';');
         ++i)
        if (request[i] == ';')
            ++argc;

    auto intermediate = '\0';
    if (isBetween(0x20, 0x2F))
        intermediate = request[i++];

    if (i >= request.size())
        return nullptr;

    // allFunctions(), not allFunctionsArray(): select() binary-searches, so it needs the sorted table.
    // It is returned BY VALUE, and select() hands back a pointer into whatever span it was given -- so the
    // table has to outlive this call, not be a temporary that dies with the full expression.
    static auto const functions = vtbackend::allFunctions();
    return vtbackend::selectControl(leader, argc, intermediate, request[i], functions);
}

} // namespace

TEST_CASE("captureScreen: the request it sends is the one the engine calls XTCAPTURE", "[capture]")
{
    auto transport = CannedTransport { "\033^314;x\033\\"
                                       "\033^314;\033\\" };
    auto settings = contour::CaptureSettings {};
    settings.timeout = 1.0;
    settings.lineCount = vtbackend::LineCount(2);

    auto out = std::ostringstream {};
    REQUIRE(contour::captureScreen(settings, transport, out));

    auto const* const resolved = resolveCsi(transport.requests);
    REQUIRE(resolved != nullptr);
    CHECK(*resolved == vtbackend::XTCAPTURE);
}

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
    // The request must carry the physical-lines flag and the line count, under the ',' intermediate that
    // distinguishes XTCAPTURE from xterm's XTSMTITLE. @see vtbackend/Functions.h, XTCAPTURE.
    CHECK(transport.requests.find("\033[>0;2,t") != std::string::npos);
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
    CHECK(transport.requests.find("\033[>1;0,t") != std::string::npos); // logical-lines flag
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
