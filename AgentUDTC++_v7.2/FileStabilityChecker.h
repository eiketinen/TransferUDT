#pragma once
#include <string>
#include <chrono>
#include <filesystem>
#include <stdexcept>
#include "Logger.h"

namespace fs = std::filesystem;

/**
 * Class responsible for checking if a file has stabilized (not being modified)
 */
class FileStabilityChecker {
public:
    /**
     * Constructor
     * @param checkInterval Time interval between stability checks (in seconds)
     * @param consecutiveChecks Number of consecutive checks with same size to consider stable
     */
    FileStabilityChecker(int checkInterval = 5, int consecutiveChecks = 3);

    /**
     * Wait until the file stabilizes (size does not change for several consecutive checks)
     * @param filePath Path to the file to check
     * @throws std::runtime_error if the file cannot be accessed
     */
    void waitForFileStability(const fs::path& filePath);

private:
    // Time interval between checks (in seconds)
    int checkIntervalSeconds;

    // Number of consecutive same-size checks required
    int requiredConsecutiveChecks;
};