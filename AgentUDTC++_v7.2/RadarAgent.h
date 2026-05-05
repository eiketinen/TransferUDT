#pragma once
#include <string>
#include <memory>
#include <atomic>
#include <thread>
#include "FileWatcher.h"
#include "FileProcessor.h"
#include "NetworkManager.h"
#include "ThreadPool.h"
#include "Database.h"
#include "Interfaces.h"
#include "Logger.h"
#include "RadarConfig.h"

/**
 * Main agent class that coordinates all components
 */
class RadarAgent {
public:
    /**
     * Constructor - initializes all components
     * @param database Database instance
     * @param verifier File integrity verifier instance
     */
    RadarAgent(Database& database, IFileIntegrityVerifier& verifier);

    /**
     * Destructor - ensures agent is stopped
     */
    ~RadarAgent();

    /**
     * Start the agent
     */
    void start();

    /**
     * Stop the agent
     */
    void stop();

private:
    // Database reference
    Database& database;

    // File integrity verifier
    IFileIntegrityVerifier& fileVerifier;

    // Components
    std::vector<std::unique_ptr<FileWatcher>> fileWatchers;
    std::unique_ptr<FileProcessor> fileProcessor;
    std::unique_ptr<NetworkManager> networkManager;
    std::unique_ptr<ThreadPool> threadPool;
    std::mutex shutdownMutex;
    std::condition_variable shutdownCv;
    // Control flags
    std::atomic<bool> running;

    // Main thread for sending pending chunks
    std::thread pendingChunksThread;

    // Handle new file detected by the watcher
    void handleNewFile(const fs::path& filePath);

    // Thread function for sending pending chunks
    void pendingChunksThreadFunc();

    void cleanupSentFiles();
};