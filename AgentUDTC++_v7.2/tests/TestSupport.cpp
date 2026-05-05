#include "tests/TestSupport.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <sqlite3.h>

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
        ("AgentUDTCppTests_" + std::to_string(GetCurrentProcessId()) + "_" + std::to_string(nowTicks));
    std::filesystem::create_directories(root);
    return root;
}

std::vector<ChunkMetadata> getChunksForFile(Database& db, const std::filesystem::path& filePath) {
    std::vector<ChunkMetadata> filtered;
    auto all = db.getPendingChunks(10'000);
    for (const auto& chunk : all) {
        if (chunk.getFilePath() == filePath) {
            filtered.push_back(chunk);
        }
    }
    return filtered;
}

std::string getChunkStatusRaw(const std::filesystem::path& dbPath, const std::filesystem::path& filePath, int chunkNumber) {
    sqlite3* rawDb = nullptr;
    if (sqlite3_open(dbPath.string().c_str(), &rawDb) != SQLITE_OK) {
        if (rawDb != nullptr) {
            sqlite3_close(rawDb);
        }
        throw TestFailure("Failed to open DB directly for status verification.");
    }

    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT status FROM chunks WHERE file_path = ? AND chunk_number = ? LIMIT 1;";
    if (sqlite3_prepare_v2(rawDb, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        sqlite3_close(rawDb);
        throw TestFailure("Failed to prepare raw status query.");
    }

    const std::wstring filePathWide = filePath.wstring();
    sqlite3_bind_text16(stmt, 1, filePathWide.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, chunkNumber);

    std::string status;
    const int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        const unsigned char* text = sqlite3_column_text(stmt, 0);
        if (text != nullptr) {
            status = reinterpret_cast<const char*>(text);
        }
    }

    sqlite3_finalize(stmt);
    sqlite3_close(rawDb);
    return status;
}