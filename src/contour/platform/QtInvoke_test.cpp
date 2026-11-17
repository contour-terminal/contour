// SPDX-License-Identifier: Apache-2.0
//
// postToObject() from another thread: the functor's captures are written on the posting thread and
// read on the receiving one, ordered only by Qt's posted-event queue.

#include <contour/platform/QtInvoke.hpp>

#include <QtCore/QCoreApplication>
#include <QtCore/QObject>
#include <QtCore/QThread>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <string>
#include <thread>

TEST_CASE("postToObject hands the functor's captures to the receiving thread", "[contour][qt][threading]")
{
    // The posting thread says it has posted through a relaxed flag, which orders nothing, and the GUI
    // thread then dispatches the call straight from the queue: Qt's own lock on the queue is the one
    // thing ordering the captures' construction before their use. Under the clang-tsan tree that has
    // to be visible to the sanitizer.
    auto receiver = QObject {};
    auto delivered = std::string {};
    auto posted = std::atomic<bool> { false };
    auto poster = std::thread { [&] {
        auto text = std::string { "made by the poster" };
        contour::platform::postToObject(&receiver, [&delivered, text] { delivered = text; });
        posted.store(true, std::memory_order_relaxed);
    } };
    while (!posted.load(std::memory_order_relaxed))
        std::this_thread::yield();
    QCoreApplication::sendPostedEvents(&receiver);
    poster.join();

    CHECK(delivered == "made by the poster");
}

TEST_CASE("PostTarget posts until it is withdrawn, then drops", "[contour][qt][threading]")
{
    auto receiver = QObject {};
    auto target = contour::platform::PostTarget { receiver };
    auto runs = 0;

    std::thread { [&] { target.post([&runs] { ++runs; }); } }.join();
    QCoreApplication::sendPostedEvents(&receiver);
    CHECK(runs == 1);

    target.withdraw();
    std::thread { [&] { target.post([&runs] { ++runs; }); } }.join();
    QCoreApplication::sendPostedEvents(&receiver);
    CHECK(runs == 1);
}

TEST_CASE("invokeBlocking hands this thread's locals there and the functor's writes back",
          "[contour][qt][threading]")
{
    // The functor runs on a QThread of its own and writes this thread's locals by reference; what orders
    // both directions is a posted-event queue and a QSemaphore inside libQt6Core. Under the clang-tsan tree
    // the sanitizer has to see both, or it reports the locals as raced.
    auto worker = QThread {};
    auto receiver = QObject {};
    receiver.moveToThread(&worker);
    worker.start();

    auto question = std::string { "asked by the caller" };
    auto answer = std::string {};
    contour::platform::invokeBlocking(&receiver, [&] { answer = question + ", answered by the worker"; });
    CHECK(answer == "asked by the caller, answered by the worker");

    // Back to this thread, so it is destroyed where it lives.
    contour::platform::invokeBlocking(&receiver,
                                      [&] { receiver.moveToThread(QCoreApplication::instance()->thread()); });
    worker.quit();
    worker.wait();
}
