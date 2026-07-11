#include "tests/TestSuites.h"

#include <chrono>
#include <thread>

void runDatabaseTests(TestStats& stats, Database& db, const TestEnvironment& env) {
    runTest("Database - same filename/chunk allowed for different file paths", [&]() {
        const std::filesystem::path filename = "same_name_scope_database.bin";
        const std::filesystem::path filePathA = env.tempRoot / "db_scope" / "A" / filename;
        const std::filesystem::path filePathB = env.tempRoot / "db_scope" / "B" / filename;

        db.insertChunk(filename, 0, 1, 100, 100, "hash_a", filePathA, "pending");
        db.insertChunk(filename, 0, 1, 100, 100, "hash_b", filePathB, "pending");

        auto chunksA = getChunksForFile(db, filePathA);
        auto chunksB = getChunksForFile(db, filePathB);
        require(chunksA.size() == 1, "Expected exactly one pending chunk for filePathA.");
        require(chunksB.size() == 1, "Expected exactly one pending chunk for filePathB.");
    }, stats);

    runTest("Database - status and retries updates are scoped by file_path", [&]() {
        const std::filesystem::path filename = "same_name_scope_updates.bin";
        const std::filesystem::path filePathA = env.tempRoot / "db_scope_updates" / "A" / filename;
        const std::filesystem::path filePathB = env.tempRoot / "db_scope_updates" / "B" / filename;

        db.insertChunk(filename, 0, 1, 100, 100, "hash_a2", filePathA, "failed");
        db.insertChunk(filename, 0, 1, 100, 100, "hash_b2", filePathB, "failed");

        db.updateChunkRetries(filePathA, 0, 7);

        int retriesA = -1;
        int retriesB = -1;
        auto pendingAfterRetryUpdate = db.getPendingChunks(10'000);
        for (const auto& chunk : pendingAfterRetryUpdate) {
            if (chunk.getFilePath() == filePathA) {
                retriesA = chunk.getRetries();
            }
            if (chunk.getFilePath() == filePathB) {
                retriesB = chunk.getRetries();
            }
        }

        require(retriesA == 7, "Retry count for filePathA should be updated to 7.");
        require(retriesB == 0, "Retry count for filePathB should remain unchanged.");

        db.updateChunkStatus(filePathA, 0, "success");

        bool foundAInPending = false;
        bool foundBInPending = false;
        auto pendingAfterStatusUpdate = db.getPendingChunks(10'000);
        for (const auto& chunk : pendingAfterStatusUpdate) {
            if (chunk.getFilePath() == filePathA) {
                foundAInPending = true;
            }
            if (chunk.getFilePath() == filePathB) {
                foundBInPending = true;
            }
        }

        require(!foundAInPending, "filePathA chunk should not remain in pending/failed after success status.");
        require(foundBInPending, "filePathB chunk should remain in pending/failed.");
    }, stats);

    runTest("Database - isFileFullyProcessed counts only success chunks", [&]() {
        const std::filesystem::path filename = "fully_processed_semantics.bin";
        const std::filesystem::path filePath = env.tempRoot / "db_processed" / filename;

        db.insertChunk(filename, 0, 3, 100, 100, "hash0", filePath, "success");
        db.insertChunk(filename, 1, 3, 100, 100, "hash1", filePath, "success");
        db.insertChunk(filename, 2, 3, 100, 100, "hash2", filePath, "failed");

        require(!db.isFileFullyProcessed(filePath), "File must not be fully processed while one chunk is failed.");

        db.updateChunkStatus(filePath, 2, "success");
        require(db.isFileFullyProcessed(filePath), "File should be fully processed after all chunks become success.");
    }, stats);

    runTest("Database - addProcessedFileAndCleanupChunks keeps processed state", [&]() {
        const std::filesystem::path filename = "cleanup_keeps_processed.bin";
        const std::filesystem::path filePath = env.tempRoot / "db_cleanup" / filename;

        db.insertChunk(filename, 0, 2, 100, 100, "hash_0", filePath, "success");
        db.insertChunk(filename, 1, 2, 100, 100, "hash_1", filePath, "failed");

        db.addProcessedFileAndCleanupChunks(filePath);

        auto remaining = getChunksForFile(db, filePath);
        require(remaining.empty(), "Pending/failed chunks for the file should be removed after cleanup.");

        db.insertChunk(filename, 0, 2, 100, 100, "rehash_0", filePath, "pending");
        db.insertChunk(filename, 1, 2, 100, 100, "rehash_1", filePath, "pending");
        auto reinserted = getChunksForFile(db, filePath);
        require(reinserted.size() == 2, "Chunk rows should be fully cleared, allowing reinsert of the same chunk numbers.");
        require(db.isFileFullyProcessed(filePath), "File must remain marked as processed after chunk cleanup.");
    }, stats);

    runTest("Database - changed fingerprint reprocesses same file path", [&]() {
        const std::filesystem::path filename = "same_path_fingerprint.bin";
        const std::filesystem::path filePath = env.tempRoot / "db_fingerprint" / filename;

        require(db.markFileAsProcessingIfChanged(filePath, 3, 100, "hash_a"),
                "First fingerprint should be accepted for processing.");
        db.addProcessedFileAndCleanupChunks(filePath);

        require(!db.markFileAsProcessingIfChanged(filePath, 3, 100, "hash_a"),
                "Same fingerprint should not be processed again.");
        require(db.isFileFullyProcessed(filePath),
                "Same fingerprint should keep the file marked as processed.");

        require(db.markFileAsProcessingIfChanged(filePath, 4, 101, "hash_b"),
                "Changed fingerprint at the same path should be accepted.");
        require(!db.isFileFullyProcessed(filePath),
                "Changed fingerprint should move the file out of processed state.");
    }, stats);

    runTest("Database - markFileAsFailed creates status row when missing", [&]() {
        const std::filesystem::path filePath = env.tempRoot / "db_failed" / "failed_without_processing.bin";
        db.markFileAsFailed(filePath);

        auto failedFiles = db.getFailedFiles();
        bool found = false;
        for (const auto& failedPath : failedFiles) {
            if (failedPath == filePath) {
                found = true;
                break;
            }
        }

        require(found, "markFileAsFailed should create/update a failed row in processed_files.");
    }, stats);

    runTest("Database - pending chunks preserve dynamic byte offset", [&]() {
        const std::filesystem::path filename = "dynamic_offset.bin";
        const std::filesystem::path filePath = env.tempRoot / "db_offsets" / filename;
        constexpr uint64_t chunkOffset = 12345;
        const std::string transferId =
            "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789";

        db.insertChunk(filename, 2, 0, 200, 75, "hash_offset", filePath,
                       "failed", chunkOffset, transferId);

        bool found = false;
        auto pendingChunks = db.getPendingChunks(10'000);
        for (const auto& chunk : pendingChunks) {
            if (chunk.getFilePath() == filePath && chunk.getChunkNumber() == 2) {
                found = true;
                require(chunk.getTotalChunk() == 0,
                        "Adaptive non-final pending chunk should keep unknown total.");
                require(chunk.getChunkOffset() == chunkOffset,
                        "Pending chunk should preserve its byte offset for retry.");
                require(chunk.getTransferId() == transferId,
                        "Pending chunk should preserve its transfer identity.");
            }
        }

        require(found, "Inserted failed adaptive chunk should be returned as pending.");
    }, stats);

    runTest("Database - transfer metrics follow the file lifecycle", [&]() {
        const auto initial = db.getTransferMetrics();
        const std::filesystem::path pendingPath =
            env.tempRoot / "db_metrics" / "multi_chunk_pending.bin";
        const std::filesystem::path failedPath =
            env.tempRoot / "db_metrics" / "failed.bin";

        db.insertChunk(pendingPath.filename(), 0, 3, 300, 100,
                       "metrics_hash_0", pendingPath, "pending");
        db.insertChunk(pendingPath.filename(), 1, 3, 300, 100,
                       "metrics_hash_1", pendingPath, "failed");
        db.insertChunk(pendingPath.filename(), 2, 3, 300, 100,
                       "metrics_hash_2", pendingPath, "pending");

        const auto queued = db.getTransferMetrics();
        require(queued.pendingFiles == initial.pendingFiles + 1,
                "Three chunks for one path must count as one pending file.");

        db.markFileAsFailed(failedPath);
        const auto failed = db.getTransferMetrics();
        require(failed.failedFiles == initial.failedFiles + 1,
                "A failed file must increment the file-level failure counter.");

        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        db.addProcessedFileAndCleanupChunks(pendingPath);
        const auto completed = db.getTransferMetrics();
        require(completed.pendingFiles == initial.pendingFiles,
                "Completing a file must remove it from the pending counter.");
        require(completed.processedFiles == initial.processedFiles + 1,
                "Completing a file must increment the processed counter.");
        require(!completed.lastTransferAt.empty(),
                "Completing a file must publish the last transfer timestamp.");
        require(initial.lastTransferAt.empty() ||
                    completed.lastTransferAt > initial.lastTransferAt,
                "Completing a file must advance the last transfer timestamp.");
        require(completed.lastTransferAt.back() == 'Z',
                "Last transfer timestamp must be expressed in UTC.");
    }, stats);
}
