#pragma once

#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>

#include "Database.h"

struct TestStats {
    int passed{0};
    int failed{0};
};

struct TestEnvironment {
    std::filesystem::path tempRoot;
    std::filesystem::path logPath;
    std::filesystem::path dbPath;
    std::filesystem::path configPath;
    std::filesystem::path storagePath;
    std::filesystem::path reconstructedPath;
};

class TestFailure : public std::runtime_error {
public:
    explicit TestFailure(const std::string& message)
        : std::runtime_error(message) {
    }
};

void require(bool condition, const std::string& message);
void runTest(const std::string& name, const std::function<void()>& fn, TestStats& stats);
std::filesystem::path createTempRoot();

