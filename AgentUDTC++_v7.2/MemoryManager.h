#pragma once
#include <windows.h>
#include <string>
#include <cmath>
#include "Logger.h"

/**
 * Class responsible for managing and monitoring system memory
 */
class MemoryManager {
public:
    /**
     * Get available physical memory in bytes
     * @return Amount of available physical memory in bytes, or 0 if failed
     */
    static DWORDLONG getAvailablePhysicalMemory();

    /**
     * Calculate optimal chunk size based on file size and available memory
     * @param fileSize Size of the file to process
     * @param defaultChunkSize Default chunk size to use if memory check fails
     * @param memoryUsagePercent Percentage of available memory to use (0-100)
     * @return Optimal chunk size in bytes
     */
    static DWORDLONG calculateOptimalChunkSize(
        std::streamsize fileSize,
        DWORDLONG defaultChunkSize,
        int memoryUsagePercent = 70
    );
};