// CircuitBreaker.cpp
#include "CircuitBreaker.h"
/**
 * Constructor for the CircuitBreaker.
 *
 * Initializes a new instance of the CircuitBreaker class with the specified service name, failure threshold, and reset timeout.
 *
 * @param service Name of the protected service.
 * @param threshold Number of consecutive failures to open the circuit.
 * @param timeout Wait time before re-evaluating the circuit.
 */
CircuitBreaker::CircuitBreaker(const std::string& service, int threshold, std::chrono::seconds timeout)
    : serviceName(service),
    failureCount(0),
    resetTimeout(timeout),
    failureThreshold(threshold),
    state(State::CLOSED) {
}
/**
 * Checks if a service call is currently allowed based on the Circuit Breaker state.
 *
 * @return true if the call is permitted, false if the circuit is OPEN and timeout has not elapsed
 */
bool CircuitBreaker::shouldAttempt() {
    std::lock_guard<std::mutex> lock(mutex);

    if (state == State::OPEN) {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = now - lastFailure;

        if (elapsed >= resetTimeout) {
            Logger::getInstance().info("CircuitBreaker::shouldAttempt", "Timeout reached for " + serviceName + ". Transitioning to HALF_OPEN state.");
            state = State::HALF_OPEN;
            return true;
        }

        Logger::getInstance().debug("CircuitBreaker::shouldAttempt", "Circuit is OPEN for " + serviceName + ". Attempt denied.");
        return false;
    }

    return true;
}
/**
 * Reports a successful operation to the CircuitBreaker, resetting its state and failure count.
 *
 * @return None
 */
void CircuitBreaker::reportSuccess() {
    std::lock_guard<std::mutex> lock(mutex);

    if (state == State::HALF_OPEN || state == State::OPEN) {
        Logger::getInstance().info("CircuitBreaker::reportSuccess", "Operation successful. Closing circuit for " + serviceName);
    }

    failureCount = 0;
    state = State::CLOSED;
}
/**
 * Reports a failure to the CircuitBreaker, updating its state and logging a warning if necessary.
 *
 * @throws None
 *
 * @return None
 */
void CircuitBreaker::reportFailure() {
    std::lock_guard<std::mutex> lock(mutex);

    failureCount++;
    lastFailure = std::chrono::steady_clock::now();

    if (state == State::HALF_OPEN || (state == State::CLOSED && failureCount >= failureThreshold)) {
        state = State::OPEN;
        Logger::getInstance().warning("CircuitBreaker::reportFailure",
            "Failure threshold reached or failure in HALF_OPEN. Opening circuit for " + serviceName +
            " (Failures: " + std::to_string(failureCount) + ")");
    }
}
/**
 * Checks if the circuit breaker is currently in the open state.
 *
 * @return true if the circuit is open, false otherwise
 */
bool CircuitBreaker::isCircuitOpen() {
    std::lock_guard<std::mutex> lock(mutex);
    return state == State::OPEN;
}
/**
 * Retrieves the current number of consecutive failures.
 *
 * @return The current failure count.
 */
int CircuitBreaker::getFailureCount() {
    std::lock_guard<std::mutex> lock(mutex);
    return failureCount;
}
/**
 * Resets the CircuitBreaker to its initial state, closing the circuit and resetting the failure count.
 *
 * @return None
 */
void CircuitBreaker::reset() {
    std::lock_guard<std::mutex> lock(mutex);
    failureCount = 0;
    state = State::CLOSED;
    Logger::getInstance().info("CircuitBreaker::reset", "Manual circuit reset for " + serviceName);
}
/**
 * Returns a string representation of the current circuit breaker state.
 *
 * @return A string indicating the current state of the circuit breaker (CLOSED, OPEN, HALF_OPEN, or UNKNOWN).
 */
std::string CircuitBreaker::getStateString() {
    std::lock_guard<std::mutex> lock(mutex);
    switch (state) {
    case State::CLOSED: return "CLOSED";
    case State::OPEN: return "OPEN";
    case State::HALF_OPEN: return "HALF_OPEN";
    default: return "UNKNOWN";
    }
}

