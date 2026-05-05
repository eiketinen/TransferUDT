#include "ThreadPool.h"

#include <algorithm>

namespace {
size_t defaultPendingTaskLimit(size_t numThreads) {
    return (std::max<size_t>)(numThreads * 64U, 1U);
}
}

ThreadPool::ThreadPool(size_t numThreads, size_t maxPendingTasksArg)
    : maxPendingTasks(maxPendingTasksArg == 0
                          ? defaultPendingTaskLimit(numThreads)
                          : maxPendingTasksArg),
      stopped(false)
{
    if (numThreads == 0) {
        throw std::invalid_argument("Thread pool must have at least one worker");
    }
    if (maxPendingTasks == 0) {
        throw std::invalid_argument("Thread pool queue capacity must be positive");
    }

    for (size_t i = 0; i < numThreads; ++i) {
        workers.emplace_back([this] { workerThread(); });
    }

    Logger::getInstance().info("ThreadPool", "Thread pool initialized with " +
        std::to_string(numThreads) + " workers and queue capacity " +
        std::to_string(maxPendingTasks));
}

ThreadPool::~ThreadPool()
{
    // Make sure the pool is stopped
    stop();
}

void ThreadPool::addTask(std::function<void()> task)
{
    {
        std::unique_lock<std::mutex> lock(queueMutex);

        // Check if the pool is stopped
        if (stopped) {
            throw std::runtime_error("Cannot add task to stopped thread pool");
        }

        if (tasks.size() >= maxPendingTasks) {
            throw std::runtime_error("Thread pool queue is full");
        }

        // Add the task to the queue
        tasks.push(std::move(task));
    }

    // Notify one waiting thread
    condition.notify_one();
}

void ThreadPool::stop()
{
    {
        std::unique_lock<std::mutex> lock(queueMutex);

        // If already stopped, do nothing
        if (stopped) {
            return;
        }

        // Set the stopped flag
        stopped = true;
    }

    // Notify all threads to wake up and check the flag
    condition.notify_all();

    // Wait for all threads to finish
    for (auto& worker : workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }

    // Clear the worker vector
    workers.clear();

    Logger::getInstance().info("ThreadPool", "Thread pool stopped");
}

bool ThreadPool::isRunning() const
{
    return !stopped;
}

size_t ThreadPool::getThreadCount() const
{
    return workers.size();
}

size_t ThreadPool::getPendingTaskCount() const
{
    std::unique_lock<std::mutex> lock(queueMutex);
    return tasks.size();
}

size_t ThreadPool::getMaxPendingTaskCount() const
{
    return maxPendingTasks;
}

void ThreadPool::workerThread()
{
    while (true) {
        std::function<void()> task;

        {
            // Wait for a task or stop signal
            std::unique_lock<std::mutex> lock(queueMutex);
            condition.wait(lock, [this] {
                return stopped || !tasks.empty();
                });

            // If pool is stopped and no tasks left, exit
            if (stopped && tasks.empty()) {
                return;
            }

            // Get the next task
            task = std::move(tasks.front());
            tasks.pop();
        }

        // Execute the task
        try {
            task();
        }
        catch (const std::exception& e) {
            Logger::getInstance().error("ThreadPool", "Exception in worker thread: " + std::string(e.what()));
        }
        catch (...) {
            Logger::getInstance().error("ThreadPool", "Unknown exception in worker thread");
        }
    }
}
