#include "tests/TestSuites.h"

#include "MemoryManager.h"

void runMemoryManagerTests(TestStats& stats) {
    runTest("MemoryManager - chunk size is capped at 4 MB", [&]() {
        constexpr DWORDLONG oversizedDefaultChunk = 8ULL * 1024ULL * 1024ULL;
        const DWORDLONG computed = MemoryManager::calculateOptimalChunkSize(
            64LL * 1024LL * 1024LL,
            oversizedDefaultChunk,
            70);

        require(computed == 4ULL * 1024ULL * 1024ULL,
            "Chunk size must be capped at 4 MB.");
    }, stats);

    runTest("MemoryManager - minimum chunk size is preserved", [&]() {
        const DWORDLONG computed = MemoryManager::calculateOptimalChunkSize(
            1024,
            1024,
            70);

        require(computed == 32ULL * 1024ULL,
            "Chunk size must not go below 32 KB.");
    }, stats);
}