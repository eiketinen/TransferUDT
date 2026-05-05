#include "tests/TestSupport.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <chrono>
#include <iostream>

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw TestFailure(message);
    }
}

void runTest(const std::string& name, const std::function<void()>& fn, TestStats& stats) {
    try {
        fn();
        ++stats.passed;
        std::cout << "[PASS] " << name << "\n";
    }
    catch (const std::exception& ex) {
        ++stats.failed;
        std::cout << "[FAIL] " << name << " -> " << ex.what() << "\n";
    }
    catch (...) {
        ++stats.failed;
        std::cout << "[FAIL] " << name << " -> unknown error\n";
    }
}

std::filesystem::path createTempRoot() {
    const auto nowTicks = std::chrono::steady_clock::now().time_since_epoch().count();
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        "ServerUDTCppTests" /
        ("run_" + std::to_string(GetCurrentProcessId()) + "_" + std::to_string(nowTicks));
    std::filesystem::create_directories(root);
    return root;
}


