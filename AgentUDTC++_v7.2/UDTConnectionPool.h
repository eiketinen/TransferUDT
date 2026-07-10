#pragma once

#include <vector>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <atomic>
#include <chrono>
#include <string>
#include "UDTConnection.h"
#include "CircuitBreaker.h"
#include "Logger.h"

class UDTConnectionPool;

/**
 * @class PooledUDTConnection
 * @brief RAII wrapper for a UDTConnection obtained from the pool.
 *
 * Ensures that the connection is automatically released back to the pool
 * when this object goes out of scope.
 */
class PooledUDTConnection {
    UDTConnectionPool* pool_;
    std::shared_ptr<UDTConnection> connection_;
    bool moved_out_; // Flag to track if the connection was moved

    void release(); // Private release helper

public:
    /**
     * @brief Constructor.
     * @param pool Pointer to the pool that owns this connection.
     * @param conn Shared pointer to the UDT connection.
     */
    PooledUDTConnection(UDTConnectionPool* pool, std::shared_ptr<UDTConnection> conn);

    /**
     * @brief Destructor. Releases the connection back to the pool.
     */
    ~PooledUDTConnection();

    // --- Movable ---
    PooledUDTConnection(PooledUDTConnection&& other) noexcept;
    PooledUDTConnection& operator=(PooledUDTConnection&& other) noexcept;

    // --- Non-copyable ---
    PooledUDTConnection(const PooledUDTConnection&) = delete;
    PooledUDTConnection& operator=(const PooledUDTConnection&) = delete;

    // --- Accessors ---
    UDTConnection* operator->() { return connection_.get(); }
    const UDTConnection* operator->() const { return connection_.get(); }
    std::shared_ptr<UDTConnection> get() { return connection_; }

    /**
     * @brief Checks if the wrapped connection is valid and connected.
     * @return true if the connection is valid, false otherwise.
     */
    bool isValid() const { return connection_ != nullptr && connection_->isConnected(); }

    /**
     * @brief Explicitly invalidates the connection (e.g., after an error),
     * preventing it from being returned to the pool as 'good'.
     */
    void invalidate();
};


/**
 * @class UDTConnectionPool
 * @brief Manages a pool of UDT connections for reuse.
 */
class UDTConnectionPool {
    friend class PooledUDTConnection; // Allow wrapper to call release

    // --- Connection Parameters ---
    std::string serverHost_;
    int serverPort_;
    int64_t maxBandwidth_;
    int segmentSize_;
    int64_t sendBuffer_;
    int64_t receiveBuffer_;
    int sendTimeout_;
    int receiveTimeout_;
    CircuitBreaker& circuitBreaker_; // Reference to the CB

    // --- Pool State ---
    size_t max_size_;
    std::atomic<size_t> current_size_;
    std::queue<std::shared_ptr<UDTConnection>> available_connections_;
    mutable std::mutex pool_mutex_; // Mutable to allow locking in const methods if needed (though not used here)
    std::condition_variable pool_cv_;
    std::atomic<bool> shutting_down_;
    bool requireGreeting_;
    std::string expectedGreeting_;
    int greetingTimeoutMs_;
    bool keepAliveEnabled_;
    std::chrono::seconds keepAliveInterval_;
    std::string keepAlivePayload_;
    bool securityEnabled_;
    bool securityHandshakeEnabled_;
    std::string securityPreSharedKey_;
    std::string securityClientId_;
    std::string securityIdentityMode_;
    std::string securityClientPrivateKeyPath_;
    std::string securityServerPublicKeyPath_;
    std::string securityClientCertificatePath_;
    std::string securityCaBundlePath_;
    std::string securityCrlPath_;
    int securityCertificateExpiryWarningDays_;
    std::string securityServerIdentity_;
    std::thread keepAliveThread_;
    std::atomic<size_t> keepAliveInFlight_{0};

    /**
     * @brief Creates and connects a new UDTConnection.
     * @return A shared_ptr to the new connection, or nullptr on failure.
     */
    std::shared_ptr<UDTConnection> _createConnection();

    /**
     * @brief Releases a connection back to the pool.
     * @param conn The connection to release.
     */
    void release(std::shared_ptr<UDTConnection> conn);

    /**
     * @brief Handles a connection that died or was invalidated.
     * @param conn The dead connection (can be nullptr).
     */
    void handleDeadConnection(std::shared_ptr<UDTConnection> conn);

    void keepAliveLoop();

    void performKeepAlivePass();


public:
    /**
     * @brief Constructor.
     * @param max_size Maximum number of connections in the pool.
     * @param host Server host.
     * @param port Server port.
     * @param maxBandwidth Max bandwidth.
     * @param segmentSize Segment size.
     * @param sendBuffer Send buffer size.
     * @param receiveBuffer Receive buffer size.
     * @param sendTimeout Send timeout.
     * @param receiveTimeout Receive timeout.
     * @param cb Reference to the CircuitBreaker.
     */
    UDTConnectionPool(
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
        bool securityHandshakeEnabled = false,
        const std::string& securityPreSharedKey = "",
        const std::string& securityClientId = "",
        const std::string& securityIdentityMode = "psk",
        const std::string& securityClientPrivateKeyPath = "",
        const std::string& securityServerPublicKeyPath = "",
        const std::string& securityClientCertificatePath = "",
        const std::string& securityCaBundlePath = "",
        const std::string& securityCrlPath = "",
        int securityCertificateExpiryWarningDays = 30,
        const std::string& securityServerIdentity = "",
        bool securityEnabled = false
    );

    /**
     * @brief Destructor. Shuts down the pool.
     */
    ~UDTConnectionPool();

    /**
     * @brief Acquires a connection from the pool.
     *
     * Waits for a connection to become available or creates a new one
     * if the pool size allows. Returns a unique_ptr to a RAII wrapper.
     *
     * @param timeout Maximum time to wait for a connection.
     * @return A unique_ptr to PooledUDTConnection, or nullptr if timed out or failed.
     */
    std::unique_ptr<PooledUDTConnection> acquire(std::chrono::milliseconds timeout = std::chrono::seconds(10));

    /**
     * @brief Shuts down the pool, closing all connections.
     */
    void shutdown();

    /**
     * @brief Gets the number of currently available connections.
     * @return Number of available connections.
     */
    size_t getAvailableCount();

    /**
     * @brief Gets the total number of connections (available + in use).
     * @return Total number of connections.
     */
    size_t getCurrentSize() const;
};
