// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for ScreenshotAnswer, which carries OSC 533's central promise on the pixel path: every
// request is answered, so an application that writes one and blocks on a read is never left hanging.
//
// The interesting case is the one no happy-path test reaches — the capture that never arrives because
// the render target was destroyed mid-flight, or because a second capture overwrote the pending one.
// Testing that needs the answer to be droppable on demand, which is why the thread hop is injected
// rather than reached for: no window, no scene graph and no GPU appear here.

#include <contour/display/ScreenshotAnswer.hpp>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <utility>
#include <vector>

using contour::display::ScreenshotAnswer;
using vtbackend::screenshot::Capture;
using vtbackend::screenshot::CaptureResult;
using vtbackend::screenshot::Status;

namespace
{

/// Collects what was answered, and how the delivering thread was reached.
struct Recorder
{
    std::vector<CaptureResult> answers;
    int posts = 0;

    /// A hop that runs the work immediately, so a test asserts on the ANSWER rather than on the hop.
    [[nodiscard]] ScreenshotAnswer::Post post()
    {
        return [this](std::function<void()> const& work) {
            ++posts;
            work();
        };
    }

    [[nodiscard]] contour::session::DisplaySurface::ScreenshotCaptureCallback sink()
    {
        return [this](CaptureResult result) {
            answers.push_back(std::move(result));
        };
    }
};

[[nodiscard]] Capture testCapture()
{
    return Capture { .content = "pixels",
                     .pixelSize = vtbackend::ImageSize { vtbackend::Width(4), vtbackend::Height(4) } };
}

} // namespace

TEST_CASE("ScreenshotAnswer delivers a capture once", "[screenshot]")
{
    auto recorder = Recorder {};
    {
        auto answer = ScreenshotAnswer { recorder.post(), recorder.sink() };
        answer.deliver(testCapture());

        // Delivered through the hop, not on the spot: the readback completes on the render thread and
        // the terminal's reply queue is reached from the GUI thread.
        CHECK(recorder.posts == 1);
        REQUIRE(recorder.answers.size() == 1);
        REQUIRE(recorder.answers[0].has_value());
        CHECK(recorder.answers[0]->content == "pixels");
    }

    // ... and going out of scope afterwards adds nothing. An application that already read its
    // screenshot must not then find a refusal of the same request behind it.
    CHECK(recorder.answers.size() == 1);
}

TEST_CASE("ScreenshotAnswer answers Unavailable when the capture never arrives", "[screenshot]")
{
    // The render target was destroyed mid-capture, or a second capture overwrote this one: the
    // callback is dropped without ever being called. Silence here is an application hung on a read,
    // so the drop has to become a status instead.
    auto recorder = Recorder {};
    {
        auto const answer = ScreenshotAnswer { recorder.post(), recorder.sink() };
        (void) answer;
    }

    REQUIRE(recorder.answers.size() == 1);
    REQUIRE(!recorder.answers[0].has_value());
    // Unavailable and not Denied: nothing refused the read, and the application may usefully retry.
    CHECK(recorder.answers[0].error() == Status::Unavailable);
}

TEST_CASE("ScreenshotAnswer refuses to answer twice", "[screenshot]")
{
    auto recorder = Recorder {};
    {
        auto answer = ScreenshotAnswer { recorder.post(), recorder.sink() };
        answer.deliver(testCapture());
        answer.deliver(std::unexpected { Status::Denied });
    }

    REQUIRE(recorder.answers.size() == 1);
    REQUIRE(recorder.answers[0].has_value());
}

TEST_CASE("ScreenshotAnswer survives the copies std::function makes of it", "[screenshot]")
{
    // The renderer stores its callback in a std::function, which copies whatever the lambda captured.
    // Sharing one answer is what keeps a copy's destruction from refusing a request that is still in
    // flight -- and what still answers when the LAST copy goes.
    auto recorder = Recorder {};
    {
        auto shared = std::make_shared<ScreenshotAnswer>(recorder.post(), recorder.sink());
        auto held = std::function<void()> { [shared]() { shared->deliver(testCapture()); } };
        auto const duplicate = held;

        // Two copies exist and one has just gone out of use; nothing has been answered yet.
        CHECK(recorder.answers.empty());
        held();
        REQUIRE(recorder.answers.size() == 1);
        CHECK(recorder.answers[0].has_value());
    }

    CHECK(recorder.answers.size() == 1);
}
