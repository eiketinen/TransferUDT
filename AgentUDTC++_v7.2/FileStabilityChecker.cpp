#include "FileStabilityChecker.h"
#include <thread>

FileStabilityChecker::FileStabilityChecker(int checkInterval, int consecutiveChecks)
    : checkIntervalSeconds(checkInterval),
    requiredConsecutiveChecks(consecutiveChecks)
{
}

void FileStabilityChecker::waitForFileStability(const fs::path& filePath)
{
    uintmax_t lastSize = 0;
    std::string filename = filePath.filename().u8string();  

    try {
        lastSize = fs::file_size(filePath);
    }
    catch (const std::exception& e) {
        throw std::runtime_error("Error checking file size " + filename + ": " + e.what());
    }

    int sameSizeCount = 0;

    while (sameSizeCount < requiredConsecutiveChecks)
    {
        // Wait for the specified interval
        std::this_thread::sleep_for(std::chrono::seconds(checkIntervalSeconds));

        try {
            uintmax_t currentSize = fs::file_size(filePath);

            if (currentSize == lastSize) {
                // Size hasn't changed, increment counter
                sameSizeCount++;
            }
            else {
                // Size has changed, reset counter
                sameSizeCount = 0;
                lastSize = currentSize;
            }
        }
        catch (const std::exception& e) {
            throw std::runtime_error("Error checking file stability " + filename + ": " + e.what());
        }
    }

    // File has been stable for the required number of checks
}