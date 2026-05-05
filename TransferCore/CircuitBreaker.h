#pragma once
#include <chrono>
#include <mutex>
#include <string>
#include "Logger.h"

/**
 * @class CircuitBreaker
 * @brief Implements the Circuit Breaker pattern to manage calls to unstable services.
 *
 * The CircuitBreaker protects systems from cascading failures by opening the circuit
 * after a configured number of consecutive failures. After a timeout, it allows
 * a test call in HALF_OPEN state.
 */
class CircuitBreaker
{
public:
    /**
     * @enum State
     * @brief Represents the possible states of the Circuit Breaker.
     */
    enum class State
    {
        CLOSED,     /**< Normal operation - calls are allowed. */
        OPEN,       /**< Circuit is open - calls are blocked. */
        HALF_OPEN   /**< Test state - a limited call is allowed. */
    };

private:
    int failureCount;    /**< Counter for consecutive failures. */
    std::chrono::time_point<std::chrono::steady_clock> lastFailure; /**< Timestamp of the last failure. */
    std::chrono::seconds resetTimeout; /**< Time to wait before allowing a new attempt. */
    int failureThreshold; /**< Number of failures to open the circuit. */
    State state;          /**< Current state of the circuit. */
    std::mutex mutex;     /**< Synchronization for concurrent access. */
    std::string serviceName; /**< Name of the monitored service. */

public:
    /**
     * @brief Constructor for the CircuitBreaker.
     * @param service Name of the protected service.
     * @param threshold Number of consecutive failures to open the circuit.
     * @param timeout Wait time before re-evaluating the circuit.
     */
    CircuitBreaker(
        const std::string& service,
        int threshold = 5,
        std::chrono::seconds timeout = std::chrono::seconds(60)
    );

    /**
     * @brief Checks if a service call is currently allowed.
     * @return true if the call is permitted.
     * @return false if the circuit is OPEN and timeout has not elapsed.
     */
    bool shouldAttempt();

    /**
     * @brief Reports a successful service call.
     * Resets the failure counter and closes the circuit.
     */
    void reportSuccess();

    /**
     * @brief Reports a failed service call.
     * Increments the failure counter and opens the circuit if needed.
     */
    void reportFailure();

    /**
     * @brief Checks if the circuit is currently open.
     * @return true if the state is OPEN.
     */
    bool isCircuitOpen();

    /**
     * @brief Returns the current number of consecutive failures.
     * @return The failure count.
     */
    int getFailureCount();

    /**
     * @brief Manually resets the circuit to CLOSED.
     * Also resets the failure counter.
     */
    void reset();

    /**
     * @brief Gets the string representation of the current circuit state.
     * @return A string: "CLOSED", "OPEN" or "HALF_OPEN".
     */
    std::string getStateString();
};