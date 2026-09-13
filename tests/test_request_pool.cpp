#include "request_pool.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>

// The pool answers LSP requests off the thread that reads messages, so every
// task in it is a reply a client is waiting for.  Dropping one, or tearing the
// server down while one is still running, is not a lost optimisation -- it is a
// request that never gets an answer and, on the way out, a handler reading
// state the destructor is already taking apart.
//
// That is exactly what shipped: `exit` stopped the transport and returned from
// run() with fold work still in flight, and two macOS runners caught it as a
// missing reply and a dead process.  These pin the shutdown contract itself,
// which the CLI smoke tests can only catch when they happen to lose the race.

using namespace std::chrono_literals;

TEST_CASE("request pool: shutdown waits for work already accepted", "[request_pool]") {
    std::atomic<int> finished{0};
    {
        RequestPool pool(2);
        for (int i = 0; i < 8; ++i)
            REQUIRE(pool.submit([&finished] {
                std::this_thread::sleep_for(20ms);
                ++finished;
            }));
        pool.shutdown();
        // Not "eventually": shutdown has returned, so every accepted task has
        // already run.  Nothing below this line may still be executing when the
        // server's destructor starts pulling the analyzer apart.
        REQUIRE(finished.load() == 8);
    }
    REQUIRE(finished.load() == 8);
}

TEST_CASE("request pool: a task running at shutdown finishes before it returns",
          "[request_pool]") {
    std::atomic<bool> started{false};
    std::atomic<bool> done{false};

    RequestPool pool(1);
    REQUIRE(pool.submit([&] {
        started = true;
        std::this_thread::sleep_for(50ms);
        done = true;
    }));
    while (!started.load())
        std::this_thread::sleep_for(1ms);

    pool.shutdown();
    REQUIRE(done.load());
}

TEST_CASE("request pool: a shut-down pool refuses work instead of swallowing it",
          "[request_pool]") {
    RequestPool pool(1);
    pool.shutdown();

    bool ran = false;
    // The caller still owes the client a reply, so it has to be told the task
    // was not taken -- see the fallback in answer_off_the_dispatch_thread().
    REQUIRE_FALSE(pool.submit([&ran] { ran = true; }));
    REQUIRE_FALSE(ran);
}

TEST_CASE("request pool: a throwing task does not take the pool with it", "[request_pool]") {
    std::atomic<int> ran{0};
    RequestPool      pool(1);

    REQUIRE(pool.submit([] { throw std::runtime_error("handler blew up"); }));
    REQUIRE(pool.submit([&ran] { ++ran; }));
    pool.shutdown();
    REQUIRE(ran.load() == 1);
}

TEST_CASE("request pool: shutdown is idempotent", "[request_pool]") {
    RequestPool pool(2);
    std::atomic<int> ran{0};
    REQUIRE(pool.submit([&ran] { ++ran; }));
    pool.shutdown();
    pool.shutdown(); // and the destructor makes a third
    REQUIRE(ran.load() == 1);
}
