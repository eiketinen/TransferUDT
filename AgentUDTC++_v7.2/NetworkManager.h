#pragma once
#include <string>
#include <vector>
#include <map>
#include <fstream>
#include <atomic>
#include <chrono>
#include <memory>
#include "AdaptiveChunkController.h"
#include "UDTConnection.h"
#include "CircuitBreaker.h"
#include "ChunkMetadata.h"
#include "Database.h"
#include "Logger.h"
#include "ResourceManagers.h"
#include "SecurePacket.h"
#include "UDTConnectionPool.h"
#include "FileWatcher.h"
#include "FileProcessor.h"

/**
 * Class responsible for network operations and chunk transmission
 */
class NetworkManager {
public:
    struct Endpoint {
        std::string host;
        int port;
    };

    enum class ChunkSendResult {
        Success,        /**< Chunk delivered successfully */
        Failure,        /**< Chunk delivery failed */
        AlreadyExists   /**< Server reported the file already exists; skip remaining chunks */
    };
    /**
     * Constructor
     * @param host Server hostname or IP
     * @param port Server port
     * @param db Reference to the database
     * @param maxRetries Maximum number of retries for operations
     * @param circuitBreakerThreshold Failure threshold for circuit breaker
     * @param resetTimeoutSeconds Reset timeout for circuit breaker in seconds
     */
    NetworkManager(
        const std::vector<Endpoint>& endpoints,
        Database& db,
        int maxRetries = 3,
        int circuitBreakerThreshold = 5,
        int resetTimeoutSeconds = 60,
        size_t poolSize = 4,
        int64_t maxBandwidth = 10 * 1024 * 1024,
        int segmentSize = 1350,
        int64_t sendBuffer = 4 * 1024 * 1024,
        int64_t receiveBuffer = 4 * 1024 * 1024,
        int sendTimeout = 20000,
        int receiveTimeout = 20000,
        int retryJitterMaxMillis = 1000,
        bool requireGreeting = true,
        std::string expectedGreeting = "READY",
        int greetingTimeoutMs = 2000,
        bool keepAliveEnabled = false,
        int keepAliveIntervalSeconds = 60,
        std::string keepAlivePayload = "HEARTBEAT",
        bool securityEnabled = false,
        std::string securityPreSharedKey = "",
        bool securityHandshakeEnabled = false,
        std::string securityClientId = "",
        std::string securityIdentityMode = "psk",
        std::string securityClientPrivateKeyPath = "",
        std::string securityServerPublicKeyPath = "",
        std::string securityClientCertificatePath = "",
        std::string securityCaBundlePath = "",
        std::string securityCrlPath = "",
        int securityCertificateExpiryWarningDays = 30,
        std::string securityServerIdentity = "",
        bool adaptiveChunkEnabled = false,
        uint64_t adaptiveChunkMinBytes = 32ULL * 1024ULL,
        uint64_t adaptiveChunkMaxBytes = 4ULL * 1024ULL * 1024ULL,
        uint64_t adaptiveChunkInitialBytes = 256ULL * 1024ULL,
        int adaptiveChunkTargetAckMillis = 700
    );

    virtual ~NetworkManager();


    /**
     * Send a chunk to the server
     * @param chunk Metadata about the chunk
     * @param data Chunk data
     * @return ChunkSendResult describing whether the chunk was sent, failed, or should skip the file
     */
    ChunkSendResult sendChunk(const ChunkMetadata& chunk, const std::vector<char>& data, uint64_t chunkOffset, uint64_t totalFileSize, const std::atomic<bool>* is_running = nullptr);

    /**
     * Process and send all pending and Failed chunks in the database
     * @return true if all chunks were sent successfully, false otherwise
     */
    bool sendPendingFailedChunks(const std::atomic<bool>& is_running, std::unique_ptr<FileProcessor> &fileProcessor, int maxRetriesAbandon);

    uint64_t getRecommendedChunkSize(uint64_t remainingBytes = 0) const;

private:

    // Connection parameters
    int64_t maxBandwidth;
    int segmentSize;
    int64_t sendBuffer;
    int64_t receiveBuffer;
    int sendTimeout;
    int receiveTimeout;
    int circuitBreakerThreshold;
	int resetTimeoutSeconds;
    int retryJitterMaxMillis;
    bool requireGreeting;
    std::string expectedGreeting;
    int greetingTimeoutMs;
    bool keepAliveEnabled;
    int keepAliveIntervalSeconds;
    std::string keepAlivePayload;
    bool securityEnabled;
    std::string securityPreSharedKey;
    bool securityHandshakeEnabled;
    std::string securityClientId;
    std::string securityIdentityMode;
    std::string securityClientPrivateKeyPath;
    std::string securityServerPublicKeyPath;
    std::string securityClientCertificatePath;
    std::string securityCaBundlePath;
    std::string securityCrlPath;
    int securityCertificateExpiryWarningDays;
    std::string securityServerIdentity;
    AdaptiveChunkController::Settings adaptiveChunkSettings;

    // Retry and error handling
    int maxRetries;

    // Database reference
    Database& database;

    struct TargetContext {
        Endpoint endpoint;
        std::unique_ptr<CircuitBreaker> circuitBreaker;
        std::unique_ptr<UDTConnectionPool> connectionPool;
        std::unique_ptr<AdaptiveChunkController> adaptiveChunks;
    };

    std::vector<TargetContext> targets;

    ChunkSendResult sendChunkToTarget(TargetContext& target, const ChunkMetadata& chunk, const std::vector<char>& data, uint64_t chunkOffset, uint64_t totalFileSize, const std::atomic<bool>* is_running);
    ChunkSendResult sendPendingChunkToTarget(TargetContext& target, const ChunkMetadata& chunk, const std::vector<char>& data, int& retryAccumulator, uint64_t chunkOffset, uint64_t totalFileSize, const std::atomic<bool>& is_running);
    ChunkSendResult _sendChunkInternal(PooledUDTConnection& connection_wrapper, TargetContext& target, const ChunkMetadata& chunk, const std::vector<char>& data, uint64_t chunkOffset, uint64_t totalFileSize);
    bool sendControlMessage(PooledUDTConnection& connection, const std::string& message);
    bool receiveControlMessage(PooledUDTConnection& connection, std::string& message);

    std::vector<char> serializeChunkMessage(const std::string& filename, const std::string& directoryPath,
        const std::string& serverAddress,
        const std::string& transferId,
        uint32_t chunkNumber, uint32_t totalChunks,
        uint64_t chunkOffset, uint64_t totalFileSize,
        const std::string& hash, const std::vector<char>& chunkData);
};
