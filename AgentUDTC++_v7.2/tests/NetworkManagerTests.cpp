#include "tests/TestSuites.h"

#include "NetworkManager.h"

#include <atomic>
#include <fstream>
#include <memory>
#include <vector>

void runNetworkManagerTests(TestStats& stats, Database& db, const TestEnvironment& env) {

    runTest("NetworkManager - constructor requires at least one endpoint", [&]() {
        std::vector<NetworkManager::Endpoint> emptyEndpoints;
        bool threw = false;

        try {
            NetworkManager manager(emptyEndpoints, db);
        }
        catch (const std::invalid_argument&) {
            threw = true;
        }

        require(threw, "NetworkManager constructor must throw when endpoint list is empty.");
    }, stats);

    runTest("NetworkManager - sendPendingFailedChunks returns true when no pending chunks", [&]() {
        std::vector<NetworkManager::Endpoint> endpoints = {
            { "127.0.0.1", 50051 }
        };

        NetworkManager manager(
            endpoints,
            db,
            1,
            5,
            60,
            1,
            0,
            1350,
            64 * 1024,
            64 * 1024,
            100,
            100,
            0,
            false,
            "",
            0,
            false,
            0,
            ""
        );

        std::unique_ptr<FileProcessor> fileProcessor;
        std::atomic<bool> running(true);

        require(manager.sendPendingFailedChunks(running, fileProcessor, 1),
            "sendPendingFailedChunks should return true when there is no work.");
    }, stats);

    runTest("NetworkManager - sendPendingFailedChunks aborts when running flag is false", [&]() {
        const std::filesystem::path filename = "nm_stop_flag.bin";
        const std::filesystem::path filePath = env.tempRoot / "nm_stop" / filename;
        db.insertChunk(filename, 0, 1, 64, 64, "nm_stop_hash", filePath, "pending");

        std::vector<NetworkManager::Endpoint> endpoints = {
            { "127.0.0.1", 50051 }
        };

        NetworkManager manager(
            endpoints,
            db,
            1,
            5,
            60,
            1,
            0,
            1350,
            64 * 1024,
            64 * 1024,
            100,
            100,
            0,
            false,
            "",
            0,
            false,
            0,
            ""
        );

        std::unique_ptr<FileProcessor> fileProcessor;
        std::atomic<bool> running(false);

        require(!manager.sendPendingFailedChunks(running, fileProcessor, 1),
            "sendPendingFailedChunks should abort when running flag is false.");
    }, stats);

    runTest("NetworkManager - pending resend increments retries on acquire failure", [&]() {
        struct UdtScope {
            bool started{ false };
            UdtScope() { started = (UDT::startup() == 0); }
            ~UdtScope() {
                if (started) {
                    UDT::cleanup();
                }
            }
        } udt;

        require(udt.started, "UDT startup failed for pending retry test.");

        const std::filesystem::path filename = "nm_retry_on_acquire_failure.bin";
        const std::filesystem::path filePath = env.tempRoot / "nm_retry" / filename;
        std::filesystem::create_directories(filePath.parent_path());

        const std::string payload = "retry-test-content";
        {
            std::ofstream file(filePath, std::ios::binary);
            file.write(payload.data(), static_cast<std::streamsize>(payload.size()));
        }

        db.insertChunk(filename, 0, 1, static_cast<int>(payload.size()), static_cast<int>(payload.size()), "retry_hash", filePath, "pending");

        std::vector<NetworkManager::Endpoint> endpoints = {
            { "256.256.256.256", 50051 }
        };

        NetworkManager manager(
            endpoints,
            db,
            1,
            5,
            60,
            1,
            0,
            1350,
            64 * 1024,
            64 * 1024,
            100,
            100,
            0,
            false,
            "",
            0,
            false,
            0,
            ""
        );

        std::unique_ptr<FileProcessor> fileProcessor;
        std::atomic<bool> running(true);

        require(manager.sendPendingFailedChunks(running, fileProcessor, 1000),
            "Pending resend routine should complete for acquire failure scenario.");

        auto chunks = getChunksForFile(db, filePath);
        require(chunks.size() == 1, "Expected one chunk after failed resend attempt.");
        require(chunks[0].getRetries() == 1,
            "Chunk retry counter must increment when acquire() fails.");
    }, stats);

    runTest("NetworkManager - invalid pending chunk metadata is marked as failed", [&]() {
        const std::filesystem::path filename = "nm_invalid_offset.bin";
        const std::filesystem::path filePath = env.tempRoot / "nm_invalid" / filename;
        std::filesystem::create_directories(filePath.parent_path());

        {
            std::ofstream file(filePath, std::ios::binary);
            file << "tiny";
        }

        db.insertChunk(filename, 99, 100, 4, 1024, "invalid_hash", filePath, "pending");

        std::vector<NetworkManager::Endpoint> endpoints = {
            { "127.0.0.1", 50051 }
        };

        NetworkManager manager(
            endpoints,
            db,
            1,
            5,
            60,
            1,
            0,
            1350,
            64 * 1024,
            64 * 1024,
            100,
            100,
            0,
            false,
            "",
            0,
            false,
            0,
            ""
        );

        std::unique_ptr<FileProcessor> fileProcessor;
        std::atomic<bool> running(true);

        require(manager.sendPendingFailedChunks(running, fileProcessor, 1),
            "Pending resend should handle invalid metadata without crashing.");

        auto chunks = getChunksForFile(db, filePath);
        require(chunks.size() == 1, "Expected one invalid chunk row after resend attempt.");
        require(chunks[0].getRetries() == 1,
            "Invalid metadata chunk should have retry incremented.");
    }, stats);

    runTest("NetworkManager - missing file increments retries and marks failed", [&]() {
        const std::filesystem::path filename = "nm_missing_file.bin";
        const std::filesystem::path filePath = env.tempRoot / "nm_missing" / filename;

        db.insertChunk(filename, 0, 1, 64, 64, "missing_hash", filePath, "pending");

        std::vector<NetworkManager::Endpoint> endpoints = {
            { "127.0.0.1", 50051 }
        };

        NetworkManager manager(
            endpoints,
            db,
            1,
            5,
            60,
            1,
            0,
            1350,
            64 * 1024,
            64 * 1024,
            100,
            100,
            0,
            false,
            "",
            0,
            false,
            0,
            ""
        );

        std::unique_ptr<FileProcessor> fileProcessor;
        std::atomic<bool> running(true);

        require(manager.sendPendingFailedChunks(running, fileProcessor, 1),
            "Pending resend should complete when source file is missing.");

        auto chunks = getChunksForFile(db, filePath);
        require(chunks.size() == 1, "Expected one chunk row after missing file resend attempt.");
        require(chunks[0].getRetries() == 1,
            "Chunk retry counter must increment when source file cannot be opened.");


    }, stats);
}