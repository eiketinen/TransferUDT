#pragma once
#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <atomic>
#include <cstddef>
#include <stdexcept>
#include "Logger.h"

/**
 * Thread pool for parallel task execution
 */
class ThreadPool {
public:
    /**
     * Constructor
     * @param numThreads Number of worker threads in the pool
     * @param maxPendingTasks Maximum queued tasks before addTask rejects. If
     * zero, a conservative default based on numThreads is used.
     */
    explicit ThreadPool(size_t numThreads, size_t maxPendingTasks = 0);

    /**
     * Destructor - ensures all threads are stopped
     */
    ~ThreadPool();

    /**
     * Add a task to the pool
     * @param task Function to execute
     * @throws std::runtime_error if pool is stopped or the queue is full
     */
    void addTask(std::function<void()> task);

    /**
     * Stop the thread pool
     * This will wait for all tasks to complete
     */
    void stop();

    /**
     * Check if the pool is running
     * @return true if the pool is running, false if stopped
     */
    bool isRunning() const;

    /**
     * Get number of worker threads
     * @return Number of worker threads
     */
    size_t getThreadCount() const;

    /**
     * Get number of pending tasks
     * @return Number of tasks in the queue
     */
    size_t getPendingTaskCount() const;

    /**
     * Get maximum number of pending queued tasks
     * @return Queue capacity
     */
    size_t getMaxPendingTaskCount() const;

private:
    // Worker threads
    std::vector<std::thread> workers;

    // Task queue
    std::queue<std::function<void()>> tasks;

    // Synchronization
    mutable std::mutex queueMutex;
    std::condition_variable condition;

    // Queue capacity
    size_t maxPendingTasks;

    // Control flags
    std::atomic<bool> stopped;

    // Worker thread function
    void workerThread();
};
