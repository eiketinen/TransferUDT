#include "tests/TestSuites.h"

#include "CircuitBreaker.h"
#include "ThreadPool.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

void runCircuitBreakerTests(TestStats& stats) {
    runTest("CircuitBreaker opens after threshold", [&]() {
        CircuitBreaker cb("svc", 2, std::chrono::seconds(5));
        require(cb.shouldAttempt(), "First attempt should be allowed");

        cb.reportFailure();
        require(!cb.isCircuitOpen(), "Circuit should still be closed after first failure");

        cb.reportFailure();
        require(cb.isCircuitOpen(), "Circuit should open when threshold is reached");
        require(!cb.shouldAttempt(), "Attempt should be blocked while open and timeout not reached");
        }, stats);

    runTest("CircuitBreaker transitions HALF_OPEN and closes on success", [&]() {
        CircuitBreaker cb("svc", 1, std::chrono::seconds(1));
        cb.reportFailure();
        require(cb.isCircuitOpen(), "Circuit should be open after first failure");

        std::this_thread::sleep_for(std::chrono::milliseconds(1100));
        require(cb.shouldAttempt(), "Circuit should allow attempt after timeout");
        require(cb.getStateString() == "HALF_OPEN", "Circuit should be in HALF_OPEN state");

        cb.reportSuccess();
        require(!cb.isCircuitOpen(), "Circuit should close after success in HALF_OPEN");
        require(cb.getFailureCount() == 0, "Failure count should reset after success");
        }, stats);

    runTest("ThreadPool rejects tasks when pending queue is full", [&]() {
        ThreadPool pool(1, 1);
        std::atomic<bool> blockerStarted{false};
        bool releaseBlocker = false;
        std::mutex releaseMutex;
        std::condition_variable releaseCv;

        try {
            pool.addTask([&]() {
                blockerStarted.store(true);
                std::unique_lock<std::mutex> lock(releaseMutex);
                releaseCv.wait(lock, [&]() { return releaseBlocker; });
            });

            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (!blockerStarted.load() &&
                   std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            require(blockerStarted.load(), "Blocking task should start");

            pool.addTask([]() {});

            bool rejected = false;
            try {
                pool.addTask([]() {});
            } catch (const std::runtime_error&) {
                rejected = true;
            }
            require(rejected, "ThreadPool should reject when queue is full");
        } catch (...) {
            {
                std::lock_guard<std::mutex> lock(releaseMutex);
                releaseBlocker = true;
            }
            releaseCv.notify_all();
            pool.stop();
            throw;
        }

        {
            std::lock_guard<std::mutex> lock(releaseMutex);
            releaseBlocker = true;
        }
        releaseCv.notify_all();
        pool.stop();
        }, stats);
}
