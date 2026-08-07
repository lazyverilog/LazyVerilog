#include "cpu_budget.hpp"
#include <catch2/catch_test_macros.hpp>
#include <thread>

TEST_CASE("cpu budget: reports a usable worker count on every platform", "[cpu]") {
    const unsigned budget = available_cpu_count();

    // A worker pool sized from this must never be empty, and must never exceed
    // the machine itself: every signal consulted is a restriction on what this
    // process may use, not an expansion of it.
    CHECK(budget >= 1);

    const unsigned hardware = std::thread::hardware_concurrency();
    if (hardware > 0)
        CHECK(budget <= hardware);
}

TEST_CASE("cpu budget: repeated queries agree", "[cpu]") {
    // The worker pool samples this once and never resizes downward, so an
    // unstable answer would make pool sizing depend on call timing.
    CHECK(available_cpu_count() == available_cpu_count());
}
