#include "tests/TestSuites.h"

#include "CircuitBreaker.h"

#include <chrono>
#include <thread>

void runCircuitBreakerTests(TestStats& stats) {
    runTest("CircuitBreaker - opens after threshold", [&]() {
        CircuitBreaker cb("cb_open_threshold", 2, std::chrono::seconds(60));
        require(cb.shouldAttempt(), "Circuit should allow attempts when closed.");

        cb.reportFailure();
        require(!cb.isCircuitOpen(), "Circuit should still be closed after first failure.");

        cb.reportFailure();
        require(cb.isCircuitOpen(), "Circuit should open after reaching threshold.");
        require(!cb.shouldAttempt(), "Circuit should deny attempts while open and timeout not elapsed.");
    }, stats);

    runTest("CircuitBreaker - transitions to half-open and recovers on success", [&]() {
        CircuitBreaker cb("cb_half_open", 1, std::chrono::seconds(1));
        cb.reportFailure();
        require(cb.isCircuitOpen(), "Circuit should open after a single failure at threshold 1.");

        std::this_thread::sleep_for(std::chrono::milliseconds(2200));
        require(cb.shouldAttempt(), "Circuit should allow a trial attempt after timeout.");

        cb.reportSuccess();
        require(!cb.isCircuitOpen(), "Circuit should close after successful trial.");
        require(cb.getStateString() == "CLOSED", "Circuit state should be CLOSED after recovery.");
    }, stats);
}