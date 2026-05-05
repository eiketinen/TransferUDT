#include "MemoryManager.h"
/**
 * Retrieves the amount of available physical memory (RAM) in the system.
 *
 * @return The amount of available physical memory in bytes.
 *
 * @throws None.
 */
DWORDLONG MemoryManager::getAvailablePhysicalMemory()
{
    MEMORYSTATUSEX memInfo;
    memInfo.dwLength = sizeof(memInfo);
    DWORDLONG availPhys = 0;

    if (GlobalMemoryStatusEx(&memInfo)) {
        availPhys = memInfo.ullAvailPhys;
        Logger::getInstance().info("MemoryManager::getAvailablePhysicalMemory", "Available memory (RAM): " +
            std::to_string(static_cast<long double>(availPhys) / (1024.0 * 1024.0)) + " MB");
    }
    else {
        Logger::getInstance().error("MemoryManager::getAvailablePhysicalMemory", "Failed to get memory information");
    }

    return availPhys;
}
/**
 * Calculates the optimal chunk size for processing a file based on its size and available memory.
 *
 * @param fileSize The size of the file in bytes.
 * @param defaultChunkSize The default chunk size to use if the file size is smaller than available memory.
 * @param memoryUsagePercent The percentage of available memory to use for chunk size calculation.
 *
 * @return The optimal chunk size for processing the file.
 *
 * @throws None.
 */
DWORDLONG MemoryManager::calculateOptimalChunkSize(
    std::streamsize fileSize,
    DWORDLONG defaultChunkSize,
    int memoryUsagePercent)
{
    // Validate memory usage percent (0-100)
    if (memoryUsagePercent < 0 || memoryUsagePercent > 100) {
        memoryUsagePercent = 70; // Default to 70%
    }

    // Get available memory
    DWORDLONG availPhys = getAvailablePhysicalMemory();
    DWORDLONG optimalChunkSize = defaultChunkSize;

    // If file is smaller than available memory, use file size as chunk size
    if (availPhys && ((DWORDLONG)fileSize < availPhys)) {
        if (optimalChunkSize <= 0 || optimalChunkSize > (DWORDLONG)fileSize) {
            optimalChunkSize = fileSize;
        }
    }
    // Otherwise use a percentage of available memory
    else if (availPhys) {
        optimalChunkSize = (int64_t)roundl(availPhys * (memoryUsagePercent / 100.0));
    }

    constexpr DWORDLONG MIN_CHUNK_SIZE = 32 * 1024; // 32 KB
    constexpr DWORDLONG MAX_CHUNK_SIZE = 4 * 1024 * 1024; // 4 MB

    if (optimalChunkSize > MAX_CHUNK_SIZE) {
        Logger::getInstance().warning("FileProcessor::calculateOptimalChunkSize",
            "Calculated chunk size too large (" + std::to_string(optimalChunkSize) +
            " bytes). Capping to maximum chunk size: " + std::to_string(MAX_CHUNK_SIZE) + " bytes.");
        optimalChunkSize = MAX_CHUNK_SIZE;
    }

    // Ensure chunk size is reasonable
    if (optimalChunkSize < MIN_CHUNK_SIZE) {
        Logger::getInstance().warning("FileProcessor::calculateOptimalChunkSize",
            "Calculated chunk size too small (" + std::to_string(optimalChunkSize) +
            " bytes). Using minimum safe chunk size: " + std::to_string(MIN_CHUNK_SIZE) + " bytes.");
        optimalChunkSize = MIN_CHUNK_SIZE; // 32 KB fallback
    }

    return optimalChunkSize;
}