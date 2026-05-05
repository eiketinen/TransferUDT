#include "tests/TestSuites.h"

#include "CircuitBreaker.h"
#include "UDTConnectionPool.h"

#include <chrono>
#include <thread>

void runConnectionPoolTests(TestStats& stats) {
    runTest("UDTConnectionPool - shutdown interrupts keep-alive wait", [&]() {
        CircuitBreaker cb("pool_shutdown_interrupt", 5, std::chrono::seconds(60));
        UDTConnectionPool pool(
            1,
            "127.0.0.1",
            50051,
            0,
            1350,
            64 * 1024,
            64 * 1024,
            100,
            100,
            cb,
            false,
            "",
            0,
            true,
            30,
            ""
        );

        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        const auto start = std::chrono::steady_clock::now();
        pool.shutdown();
        const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);

        require(elapsedMs < std::chrono::milliseconds(1500),
            "Shutdown should interrupt keep-alive sleep and finish quickly.");
    }, stats);

    runTest("UDTConnectionPool - acquire(0) attempts creation when capacity exists", [&]() {
        CircuitBreaker cb("pool_acquire_zero", 5, std::chrono::seconds(60));
        UDTConnectionPool pool(
            1,
            "256.256.256.256",
            50051,
            0,
            1350,
            64 * 1024,
            64 * 1024,
            100,
            100,
            cb,
            false,
            "",
            0,
            false,
            0,
            ""
        );

        auto conn = pool.acquire(std::chrono::milliseconds::zero());
        require(conn == nullptr, "acquire(0) should return nullptr when connection cannot be established.");
        require(cb.getFailureCount() == 1,
            "acquire(0) should attempt connection creation when the pool has capacity.");

        pool.shutdown();
    }, stats);

    runTest("UDTConnectionPool - acquire returns nullptr after shutdown", [&]() {
        CircuitBreaker cb("pool_after_shutdown", 5, std::chrono::seconds(60));
        UDTConnectionPool pool(
            1,
            "127.0.0.1",
            50051,
            0,
            1350,
            64 * 1024,
            64 * 1024,
            100,
            100,
            cb,
            false,
            "",
            0,
            false,
            0,
            ""
        );

        pool.shutdown();
        auto conn = pool.acquire(std::chrono::milliseconds(50));
        require(conn == nullptr, "acquire should return nullptr when pool is shutdown.");
    }, stats);
}