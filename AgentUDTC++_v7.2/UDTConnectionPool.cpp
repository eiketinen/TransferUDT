#include "UDTConnectionPool.h"
#include "SecurityHandshake.h"
#include <stdexcept>
#include <string>

namespace {

const char* udtStateToString(UDTSTATUS status)
{
    switch (status) {
    case INIT: return "INIT";
    case OPENED: return "OPENED";
    case LISTENING: return "LISTENING";
    case CONNECTING: return "CONNECTING";
    case CONNECTED: return "CONNECTED";
    case BROKEN: return "BROKEN";
    case CLOSING: return "CLOSING";
    case CLOSED: return "CLOSED";
    case NONEXIST: return "NONEXIST";
    default: return "UNKNOWN";
    }
}

}
/**
 * Constructs a PooledUDTConnection object, representing a connection obtained from the pool.
 *
 * @param pool Pointer to the UDTConnectionPool that owns this connection.
 * @param conn Shared pointer to the UDTConnection object.
 */
PooledUDTConnection::PooledUDTConnection(UDTConnectionPool* pool, std::shared_ptr<UDTConnection> conn)
    : pool_(pool), connection_(conn), moved_out_(false) {
}
/**
 * Constructs a PooledUDTConnection object, representing a connection obtained from the pool.
 *
 * @param pool Pointer to the UDTConnectionPool that owns this connection.
 * @param conn Shared pointer to the UDTConnection object.
 */
PooledUDTConnection::~PooledUDTConnection() {
    release(); 
}
/**
 * Constructs a PooledUDTConnection object, representing a connection obtained from the pool.
 *
 * @param pool Pointer to the UDTConnectionPool that owns this connection.
 * @param conn Shared pointer to the UDTConnection object.
 */
PooledUDTConnection::PooledUDTConnection(PooledUDTConnection&& other) noexcept
    : pool_(other.pool_), connection_(std::move(other.connection_)), moved_out_(other.moved_out_) {
    other.moved_out_ = true; 
}
/**
 * Constructs a PooledUDTConnection object, representing a connection obtained from the pool.
 *
 * @param pool Pointer to the UDTConnectionPool that owns this connection.
 * @param conn Shared pointer to the UDTConnection object.
 */
PooledUDTConnection& PooledUDTConnection::operator=(PooledUDTConnection&& other) noexcept {
    if (this != &other) {
        release(); 
        pool_ = other.pool_;
        connection_ = std::move(other.connection_);
        moved_out_ = other.moved_out_;
        other.moved_out_ = true;
    }
    return *this;
}
/**
 * Destructor for the PooledUDTConnection object, releasing the connection back to the pool.
 *
 * @param None
 *
 * @return None
 *
 * @throws None
 */
void PooledUDTConnection::release() {
    if (!moved_out_ && pool_ && connection_) {
        if (connection_->isConnected()) {
            pool_->release(connection_);
        }
        else {
            pool_->handleDeadConnection(connection_);
        }
        moved_out_ = true; // Ensure release happens only once
        connection_.reset(); // Release ownership
    }
}
/**
 * Explicitly invalidates the connection, preventing it from being returned to the pool as 'good'.
 * This is typically called after an error occurs to ensure the connection is not reused.
 *
 * @note This function does not release the connection back to the pool. The destructor will handle this.
 */
void PooledUDTConnection::invalidate() {
    if (connection_) {
        connection_->close(); // Mark it as not connected
    }
    // The destructor will now call handleDeadConnection when it runs.
}
/**
 * Constructs a UDTConnectionPool object, initializing the connection pool with the specified parameters.
 *
 * @param max_size The maximum number of connections in the pool.
 * @param host The server host.
 * @param port The server port.
 * @param maxBandwidth The maximum bandwidth in bytes/sec (0 for unlimited).
 * @param segmentSize The UDT segment size.
 * @param sendBuffer The send buffer size in bytes.
 * @param receiveBuffer The receive buffer size in bytes.
 * @param sendTimeout The send timeout in milliseconds.
 * @param receiveTimeout The receive timeout in milliseconds.
 * @param cb A reference to the CircuitBreaker.
 */
UDTConnectionPool::UDTConnectionPool(
    size_t max_size,
    const std::string& host, int port,
    int64_t maxBandwidth, int segmentSize,
    int64_t sendBuffer, int64_t receiveBuffer,
    int sendTimeout, int receiveTimeout,
    CircuitBreaker& cb,
    bool requireGreeting,
    const std::string& expectedGreeting,
    int greetingTimeoutMs,
    bool keepAliveEnabled,
    int keepAliveIntervalSeconds,
    const std::string& keepAlivePayload,
    bool securityHandshakeEnabled,
    const std::string& securityPreSharedKey,
    const std::string& securityClientId,
    const std::string& securityIdentityMode,
    const std::string& securityClientPrivateKeyPath,
    const std::string& securityServerPublicKeyPath)
    : serverHost_(host),
    serverPort_(port),
    maxBandwidth_(maxBandwidth),
    segmentSize_(segmentSize),
    sendBuffer_(sendBuffer),
    receiveBuffer_(receiveBuffer),
    sendTimeout_(sendTimeout),
    receiveTimeout_(receiveTimeout),
    circuitBreaker_(cb),
    max_size_(max_size),
    current_size_(0),
    shutting_down_(false),
    requireGreeting_(requireGreeting),
    expectedGreeting_(expectedGreeting),
    greetingTimeoutMs_(greetingTimeoutMs),
    keepAliveEnabled_(keepAliveEnabled),
    keepAliveInterval_(std::chrono::seconds(keepAliveIntervalSeconds)),
    keepAlivePayload_(keepAlivePayload),
    securityHandshakeEnabled_(securityHandshakeEnabled),
    securityPreSharedKey_(securityPreSharedKey),
    securityClientId_(securityClientId),
    securityIdentityMode_(securityIdentityMode),
    securityClientPrivateKeyPath_(securityClientPrivateKeyPath),
    securityServerPublicKeyPath_(securityServerPublicKeyPath)
{
    if (max_size_ == 0) max_size_ = 1; // Ensure at least 1 connection
    Logger::getInstance().info("UDTConnectionPool::UDTConnectionPool", "Pool initialized with max size: " + std::to_string(max_size_));
    if (keepAliveEnabled_ && keepAliveInterval_ > std::chrono::seconds::zero()) {
        keepAliveThread_ = std::thread(&UDTConnectionPool::keepAliveLoop, this);
        Logger::getInstance().info("UDTConnectionPool::UDTConnectionPool",
            "Keep-alive thread started (interval=" + std::to_string(keepAliveInterval_.count()) + "s).");
    }
}
/**
 * Releases the connection back to the pool, handling both connected and disconnected cases.
 *
 * @param None
 *
 * @return None
 *
 * @throws None
 */
UDTConnectionPool::~UDTConnectionPool() {
    shutdown();
}
/**
 * Creates and connects a new UDTConnection.
 *
 * @return A shared_ptr to the new connection, or nullptr on failure.
 */
std::shared_ptr<UDTConnection> UDTConnectionPool::_createConnection() {
    if (shutting_down_ || !circuitBreaker_.shouldAttempt()) {
        Logger::getInstance().warning("UDTConnectionPool::_createConnection", "Creation denied: Shutting down or Circuit Breaker OPEN.");
        return nullptr;
    }

    auto connection = std::make_shared<UDTConnection>();
    bool connected = connection->connect(
        serverHost_, serverPort_,
        maxBandwidth_, segmentSize_,
        sendBuffer_, receiveBuffer_,
        sendTimeout_, receiveTimeout_
    );

    if (connected) {
        if (requireGreeting_) {
            bool timeoutAdjusted = false;
            if (greetingTimeoutMs_ > 0 && greetingTimeoutMs_ != receiveTimeout_) {
                if (connection->setReceiveTimeout(greetingTimeoutMs_)) {
                    timeoutAdjusted = true;
                }
                else {
                    Logger::getInstance().warning("UDTConnectionPool::_createConnection",
                        "Failed to apply custom greeting timeout. Using default receive timeout.");
                }
            }

            std::string response;
            bool greetingReceived = connection->recvString(response);

            if (timeoutAdjusted) {
                connection->setReceiveTimeout(receiveTimeout_);
            }

            if (!greetingReceived) {
                Logger::getInstance().warning("UDTConnectionPool::_createConnection",
                    "Handshake greeting not received from server (timeout " +
                    std::to_string(timeoutAdjusted ? greetingTimeoutMs_ : receiveTimeout_) + " ms). Closing connection.");
                connection->close();
                circuitBreaker_.reportFailure();
                return nullptr;
            }
            if (securityHandshakeEnabled_) {
                if (securityIdentityMode_ == "signed_handshake") {
                    SecurityHandshake::SignedChallenge challenge;
                    if (!SecurityHandshake::TryParseSignedChallengeMessage(
                            response, challenge)) {
                        Logger::getInstance().warning("UDTConnectionPool::_createConnection",
                            "Expected signed authentication challenge but received '" + response + "'.");
                        connection->close();
                        circuitBreaker_.reportFailure();
                        return nullptr;
                    }

                    try {
                        const auto clientEphemeral =
                            SecurityHandshake::CreateEphemeralKeyPair();
                        const std::string clientNonce =
                            SecurityHandshake::CreateChallenge();
                        const std::string signedResponse =
                            SecurityHandshake::BuildSignedResponseMessage(
                                challenge, securityClientId_, clientNonce,
                                clientEphemeral.publicKeyHex,
                                securityClientPrivateKeyPath_);
                        SecurityHandshake::SignedResponse parsedResponse;
                        if (!SecurityHandshake::TryParseSignedResponseMessage(
                                signedResponse, parsedResponse)) {
                            throw std::runtime_error(
                                "Built signed response could not be parsed.");
                        }
                        if (!connection->sendString(signedResponse)) {
                            Logger::getInstance().warning("UDTConnectionPool::_createConnection",
                                "Failed to send signed authentication response.");
                            connection->close();
                            circuitBreaker_.reportFailure();
                            return nullptr;
                        }

                        if (!connection->recvString(response)) {
                            Logger::getInstance().warning("UDTConnectionPool::_createConnection",
                                "Signed server proof not received after authentication.");
                            connection->close();
                            circuitBreaker_.reportFailure();
                            return nullptr;
                        }

                        if (response == SecurityHandshake::kAuthFailed) {
                            Logger::getInstance().warning("UDTConnectionPool::_createConnection",
                                "Server rejected signed authentication response.");
                            connection->close();
                            circuitBreaker_.reportFailure();
                            return nullptr;
                        }

                        std::string serverSignature;
                        if (!SecurityHandshake::VerifySignedOkMessage(
                                challenge, parsedResponse, response,
                                securityServerPublicKeyPath_,
                                &serverSignature)) {
                            Logger::getInstance().warning("UDTConnectionPool::_createConnection",
                                "Signed server proof verification failed.");
                            connection->close();
                            circuitBreaker_.reportFailure();
                            return nullptr;
                        }

                        connection->setSecureSessionKey(
                            SecurityHandshake::DeriveSignedSessionSecret(
                                clientEphemeral.privateKeyHex,
                                challenge.serverEphemeralPublicKeyHex,
                                challenge, parsedResponse, serverSignature));
                        Logger::getInstance().info("UDTConnectionPool::_createConnection",
                            "Signed authentication succeeded for client identity '" +
                            securityClientId_ + "'.");
                    }
                    catch (const std::exception& ex) {
                        Logger::getInstance().warning("UDTConnectionPool::_createConnection",
                            "Signed authentication failed: " +
                            std::string(ex.what()));
                        connection->close();
                        circuitBreaker_.reportFailure();
                        return nullptr;
                    }

                    if (!connection->recvString(response)) {
                        Logger::getInstance().warning("UDTConnectionPool::_createConnection",
                            "READY greeting not received after signed authentication.");
                        connection->close();
                        circuitBreaker_.reportFailure();
                        return nullptr;
                    }
                } else {
                std::string challenge;
                if (!SecurityHandshake::TryParseChallengeMessage(response, challenge)) {
                    Logger::getInstance().warning("UDTConnectionPool::_createConnection",
                        "Expected authentication challenge but received '" + response + "'.");
                    connection->close();
                    circuitBreaker_.reportFailure();
                    return nullptr;
                }

                try {
                    if (!connection->sendString(SecurityHandshake::BuildResponseMessage(
                        challenge, securityPreSharedKey_, securityClientId_))) {
                        Logger::getInstance().warning("UDTConnectionPool::_createConnection",
                            "Failed to send authentication challenge response.");
                        connection->close();
                        circuitBreaker_.reportFailure();
                        return nullptr;
                    }
                }
                catch (const std::exception& ex) {
                    Logger::getInstance().warning("UDTConnectionPool::_createConnection",
                        "Failed to build authentication challenge response: " +
                        std::string(ex.what()));
                    connection->close();
                    circuitBreaker_.reportFailure();
                    return nullptr;
                }

                if (!connection->recvString(response)) {
                    Logger::getInstance().warning("UDTConnectionPool::_createConnection",
                        "READY greeting not received after authentication.");
                    connection->close();
                    circuitBreaker_.reportFailure();
                    return nullptr;
                }

                if (response == SecurityHandshake::kAuthFailed) {
                    Logger::getInstance().warning("UDTConnectionPool::_createConnection",
                        "Server rejected authentication challenge response.");
                    connection->close();
                    circuitBreaker_.reportFailure();
                    return nullptr;
                }
                }
            }
            if (!expectedGreeting_.empty() && response != expectedGreeting_) {
                Logger::getInstance().warning("UDTConnectionPool::_createConnection",
                    "Unexpected handshake greeting '" + response + "'. Expected '" + expectedGreeting_ + "'.");
                connection->close();
                circuitBreaker_.reportFailure();
                return nullptr;
            }
            Logger::getInstance().info("UDTConnectionPool::_createConnection",
                "Handshake greeting received: '" + response + "'. Connection ready.");
        }
        else {
            Logger::getInstance().debug("UDTConnectionPool::_createConnection",
                "Greeting check disabled for new connection.");
        }

        circuitBreaker_.reportSuccess();
        return connection;
    }
    else {
        Logger::getInstance().error("UDTConnectionPool::_createConnection", "Failed to create new UDT connection.");
        circuitBreaker_.reportFailure();
        return nullptr;
    }
}
/**
 * Acquires a connection from the pool, waiting for a specified timeout if necessary.
 * If a connection is available, it is returned immediately. Otherwise, the function
 * waits until a connection becomes available or the timeout expires.
 *
 * @param timeout The maximum time to wait for a connection to become available.
 *
 * @return A unique pointer to a PooledUDTConnection if a connection was acquired, nullptr otherwise.
 *
 * @throws None
 */
std::unique_ptr<PooledUDTConnection> UDTConnectionPool::acquire(std::chrono::milliseconds timeout) {
    if (shutting_down_) return nullptr;

    auto boundedTimeout = timeout;
    if (boundedTimeout < std::chrono::milliseconds::zero()) {
        boundedTimeout = std::chrono::milliseconds::zero();
    }
    const auto deadline = std::chrono::steady_clock::now() + boundedTimeout;

    std::unique_lock<std::mutex> lock(pool_mutex_);

    while (true) {
        const auto now = std::chrono::steady_clock::now();
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        if (remaining < std::chrono::milliseconds::zero()) {
            remaining = std::chrono::milliseconds::zero();
        }

        if (!pool_cv_.wait_for(lock, remaining, [this] {
            return shutting_down_ || !available_connections_.empty() || current_size_ < max_size_;
            })) {
            Logger::getInstance().warning("UDTConnectionPool::acquire", "Acquire timed out.");
            return nullptr;
        }

        if (shutting_down_) {
            return nullptr;
        }

        // Prioritize existing connections
        while (!available_connections_.empty()) {
            std::shared_ptr<UDTConnection> conn = available_connections_.front();
            available_connections_.pop();

            if (conn) {
                UDTSTATUS state = conn->getState();
                if (state == CONNECTED) {
                    Logger::info("UDTConnectionPool::acquire", "Acquired existing connection (state={}).", udtStateToString(state));
                    return std::make_unique<PooledUDTConnection>(this, conn);
                }

                Logger::warning("UDTConnectionPool::acquire", "Discarding pooled connection with state={}.", udtStateToString(state));
                conn->close();
            }
            else {
                Logger::getInstance().warning("UDTConnectionPool::acquire", "Encountered null connection in pool. Discarding.");
            }

            if (current_size_ > 0) {
                current_size_--;
            }
            else {
                Logger::getInstance().warning("UDTConnectionPool::acquire", "current_size_ already zero when discarding connection.");
            }
        }

        if (shutting_down_) {
            return nullptr;
        }

        if (keepAliveInFlight_.load(std::memory_order_relaxed) > 0) {
            const auto nowDuringKeepAlive = std::chrono::steady_clock::now();
            if (nowDuringKeepAlive >= deadline) {
                Logger::getInstance().warning("UDTConnectionPool::acquire", "Acquire timed out.");
                return nullptr;
            }

            const auto remainingDuringKeepAlive =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - nowDuringKeepAlive);
            const auto keepAliveWaitSlice =
                (remainingDuringKeepAlive < std::chrono::milliseconds(10))
                ? remainingDuringKeepAlive
                : std::chrono::milliseconds(10);

            pool_cv_.wait_for(lock, keepAliveWaitSlice, [this] {
                return shutting_down_ || keepAliveInFlight_.load(std::memory_order_relaxed) == 0;
            });
            continue; // Connections temporarily unavailable due to keep-alive
        }

        // If no existing, try creating a new one
        if (current_size_ < max_size_) {
            current_size_++; // Optimistically increment
            lock.unlock();   // Unlock before creating (blocking call)

            std::shared_ptr<UDTConnection> new_conn = _createConnection();

            if (new_conn) {
                Logger::getInstance().info("UDTConnectionPool::acquire", "Acquired new connection.");
                return std::make_unique<PooledUDTConnection>(this, new_conn);
            }
            else {
                lock.lock(); // Relock to decrement on failure
                if (current_size_ > 0) {
                    current_size_--;
                }
                Logger::getInstance().error("UDTConnectionPool::acquire", "Failed to acquire new connection.");
                return nullptr;
            }
        }

        // Should not happen if wait logic is correct, but as a fallback
        Logger::getInstance().error("UDTConnectionPool::acquire", "Could not acquire connection (Pool full & empty?).");
        return nullptr;
    }
}

/**
 * Releases a connection back to the pool, or closes it if the pool is full.
 *
 * @param conn The connection to release.
 *
 * @return None
 *
 * @throws None
 */
void UDTConnectionPool::release(std::shared_ptr<UDTConnection> conn) {
    if (shutting_down_ || !conn) {
        handleDeadConnection(conn); // Ensure size is decremented if needed
        return;
    }

    {
        std::lock_guard<std::mutex> lock(pool_mutex_);
        if (current_size_ <= max_size_) { // Only return if pool isn't 'overbooked' (shouldn't happen)
            Logger::getInstance().info("UDTConnectionPool::release", "Releasing connection back to pool.");
            available_connections_.push(conn);
        }
        else {
            // Should not happen, but if it does, just close it.
            Logger::getInstance().warning("UDTConnectionPool::release", "Pool size exceeded? Closing released connection.");
            conn->close();
            if (current_size_ > 0) {
                current_size_--;
            }
        }
    }
    pool_cv_.notify_one(); // Notify one waiting thread
}
/**
 * Releases a connection back to the pool, or closes it if the pool is full.
 *
 * @param conn The connection to release.
 *
 * @return None
 *
 * @throws None
 */
void UDTConnectionPool::handleDeadConnection(std::shared_ptr<UDTConnection> conn) {
    Logger::getInstance().warning("UDTConnectionPool::handleDeadConnection", "Handling dead connection.");
    if (conn) {
        conn->close();
    }
	// Decrement current size, but ensure we don't go negative
    std::lock_guard<std::mutex> lock(pool_mutex_);
    if (current_size_ > 0) {
        current_size_--; // Potentially risky - needs robust tracking.
    }
    pool_cv_.notify_one(); // Notify someone, maybe they can create now.
}

// Background loop that periodically probes idle pooled connections.
void UDTConnectionPool::keepAliveLoop() {
    auto sleepInterval = keepAliveInterval_;
    if (sleepInterval <= std::chrono::seconds::zero()) {
        sleepInterval = std::chrono::seconds(1);
    }

    std::unique_lock<std::mutex> lock(pool_mutex_);
    while (!shutting_down_) {
        pool_cv_.wait_for(lock, sleepInterval, [this] {
            return shutting_down_.load();
        });

        if (shutting_down_) {
            break;
        }

        lock.unlock();
        try {
            performKeepAlivePass();
        }
        catch (const std::exception& ex) {
            Logger::getInstance().warning("UDTConnectionPool::keepAliveLoop",
                "Exception during keep-alive: " + std::string(ex.what()));
        }
        catch (...) {
            Logger::getInstance().warning("UDTConnectionPool::keepAliveLoop",
                "Unknown exception during keep-alive.");
        }
        lock.lock();
    }
}

// Performs one keep-alive sweep and drops unhealthy idle connections.
void UDTConnectionPool::performKeepAlivePass() {
    if (shutting_down_) {
        return;
    }

    std::vector<std::shared_ptr<UDTConnection>> snapshot;
    {
        std::lock_guard<std::mutex> lock(pool_mutex_);
        if (available_connections_.empty()) {
            return;
        }

        const size_t count = available_connections_.size();
        snapshot.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            snapshot.push_back(available_connections_.front());
            available_connections_.pop();
        }
    }

    std::vector<std::shared_ptr<UDTConnection>> survivors;
    survivors.reserve(snapshot.size());

    if (!snapshot.empty()) {
        keepAliveInFlight_.fetch_add(snapshot.size(), std::memory_order_relaxed);
    }

    size_t processedEntries = 0;
    try {
        for (auto& conn : snapshot) {
            ++processedEntries;
            struct KeepAliveInFlightDecrementGuard {
                std::atomic<size_t>& counter;
                ~KeepAliveInFlightDecrementGuard() {
                    counter.fetch_sub(1, std::memory_order_relaxed);
                }
            } decrementGuard{ keepAliveInFlight_ };

            if (!conn) {
                Logger::getInstance().warning("UDTConnectionPool::performKeepAlivePass",
                    "Encountered null connection during keep-alive. Removing.");
                handleDeadConnection(conn);
                continue;
            }

            UDTSTATUS state = conn->getState();
            if (state != CONNECTED) {
                Logger::getInstance().warning("UDTConnectionPool::performKeepAlivePass",
                    "Removing idle connection with state={}.", udtStateToString(state));
                handleDeadConnection(conn);
                continue;
            }

            if (!keepAlivePayload_.empty()) {
                if (!conn->sendKeepAlive(keepAlivePayload_)) {
                    Logger::getInstance().warning("UDTConnectionPool::performKeepAlivePass",
                        "Keep-alive payload failed. Closing connection.");
                    handleDeadConnection(conn);
                    continue;
                }
                Logger::getInstance().info("UDTConnectionPool::performKeepAlivePass",
                    "Keep-alive payload sent.");
            }

            survivors.push_back(conn);
        }
    }
    catch (...) {
        const size_t remainingEntries = snapshot.size() - processedEntries;
        if (remainingEntries > 0) {
            keepAliveInFlight_.fetch_sub(remainingEntries, std::memory_order_relaxed);
        }
        throw;
    }

    bool returnToPool = false;
    {
        std::lock_guard<std::mutex> lock(pool_mutex_);
        returnToPool = !shutting_down_;
        if (returnToPool) {
            for (auto& conn : survivors) {
                available_connections_.push(conn);
            }
        }
    }

    if (returnToPool) {
        pool_cv_.notify_all();
    }
    else {
        for (auto& conn : survivors) {
            handleDeadConnection(conn);
        }
    }
}
// Stops worker activity and closes all pooled connections.
void UDTConnectionPool::shutdown() {
    if (shutting_down_.exchange(true)) {
        return; // Already shutting down
    }

    Logger::getInstance().info("UDTConnectionPool::shutdown", "Shutting down connection pool...");
    pool_cv_.notify_all(); // Wake up all waiting threads

    // Join keep-alive first so no thread can repopulate the pool during shutdown.
    if (keepAliveThread_.joinable()) {
        keepAliveThread_.join();
    }

    {
        std::lock_guard<std::mutex> lock(pool_mutex_);
        while (!available_connections_.empty()) {
            auto conn = available_connections_.front();
            available_connections_.pop();
            if (conn) {
                conn->close();
            }
        }
        current_size_ = 0;
    }

    Logger::getInstance().info("UDTConnectionPool::shutdown", "Shutdown complete.");
}
/**
 * Returns the number of currently idle connections available for acquire().
 */
size_t UDTConnectionPool::getAvailableCount() {
    std::lock_guard<std::mutex> lock(pool_mutex_);
    return available_connections_.size();
}
/**
 * Returns the total number of connection objects tracked by the pool.
 */
size_t UDTConnectionPool::getCurrentSize() const {
    return current_size_.load();
}
