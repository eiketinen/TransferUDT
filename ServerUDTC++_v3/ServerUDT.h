#pragma once

#include <winsock2.h> // Must be included before windows.h for UDT
#include <ws2tcpip.h>
#include <windows.h> // For service integration potentially needing handles etc.
#pragma warning(push)
#pragma warning(disable : 4251)
#include <udt/udt.h>
#pragma warning(pop)
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <memory>
#include <filesystem>
#include <stdexcept> // For runtime_error


// Include project headers
#include "ThreadPool.h"
#include "FileReceiver.h"
#include "ServerConfig.h"
#include "Database.h"
#include "Logger.h"
#include "ClientHandler.h" // Included as acceptClients creates handlers
#include "Interfaces.h" // For IFileIntegrityVerifier implementation

namespace fs = std::filesystem;

class ServerUDT {
public:
    /**
     * @brief Constructor for ServerUDT.
     * Initializes configuration and thread pool. Database and FileReceiver are initialized in run().
     */
    explicit ServerUDT();

    /**
     * @brief Destructor for ServerUDT.
     * Ensures the server is stopped cleanly.
     */
    ~ServerUDT();

    // Delete copy constructor and assignment operator
    ServerUDT(const ServerUDT&) = delete;
    ServerUDT& operator=(const ServerUDT&) = delete;

    /**
     * @brief Initialize and start the UDT server.
     * Initializes database, file receiver, networking, and starts listening.
     * Can run in blocking (console) or non-blocking (service) mode.
     *
     * @param runAsService If true, starts server threads and returns immediately.
     * If false, blocks the calling thread until shutdown is signaled.
     * @return true if server initialization and startup were successful, false otherwise.
     */
    bool run(bool runAsService = false);

	bool isRunning() const { return running.load(); } // Check if server is running

    /**
     * @brief Signals the server to begin a graceful shutdown process.
     * Sets the shutdown flag, notifies waiting threads, and closes the listening socket.
     */
    void signalShutdown();

private:
    /**
     * @brief The main loop for accepting incoming client connections.
     * Runs in a separate thread (`acceptThread_`).
     */
    void acceptClients();

    /**
     * @brief Configures UDT socket options for the main server listening socket.
     * Reads parameters from ServerConfig.
     */
    void configureServerSocketOptions();

    /**
    * @brief Stops the server, cleans up resources, and joins threads.
    * Called internally during shutdown.
    */
    void stop();

    /**
     * @brief Ensures a directory exists, creating it if necessary.
     * @param dir The path to the directory.
     * @return true if the directory exists or was created, false otherwise.
     */
    bool ensureDirectoryExists(const fs::path& dir);


    // --- Member Variables ---
    Database* database = nullptr;             // Pointer to the Database singleton instance (set in run)
    std::unique_ptr<ThreadPool> threadPool;     // Manages worker threads for client handling
    std::shared_ptr<FileReceiver> fileReceiver; // Handles received file chunks (set in run)

    std::atomic<bool> running{ false };           // Flag indicating if the server is actively running
    UDTSOCKET serverSocket = UDT::INVALID_SOCK; // The main UDT listening socket

    std::thread acceptThread;                 // Thread running the acceptClients loop
    std::mutex shutdownMutex;                 // Mutex protecting shutdown flag and related resources
    std::condition_variable shutdownCV;       // Condition variable to signal/wait for shutdown
    std::atomic<bool> shutdownRequested{ false }; // Flag indicating a shutdown has been requested
};