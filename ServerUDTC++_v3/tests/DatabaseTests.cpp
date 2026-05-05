#include "tests/TestSuites.h"

#include <fstream>

void runDatabaseTests(TestStats& stats, Database& db, const TestEnvironment& env) {
    const std::string client = "127.0.0.1/incoming";
    const std::string file = "archive.bin";

    runTest("Database stores and reads chunk metadata", [&]() {
        const std::filesystem::path chunkPath = env.tempRoot / "chunk0.part";
        std::ofstream chunkFile(chunkPath, std::ios::binary | std::ios::trunc);
        chunkFile << "chunk-data";
        chunkFile.close();

        db.insertChunk(client, file, 0, 2, "hash0", chunkPath);
        require(db.getChunkStatus(client, file, 0) == 1, "Chunk status should indicate existing row");

        db.updateChunkStatus(client, file, 0, "success");

        const auto chunks = db.getChunksForFile(client, file);
        require(chunks.size() == 1, "Expected one successful chunk");
        require(chunks[0].getChunkNumber() == 0, "Chunk number should be 0");
        require(chunks[0].getTotalChunk() == 2, "Total chunk should be 2");
        require(chunks[0].getHash() == "hash0", "Chunk hash should match");
        require(chunks[0].getFilePath() == chunkPath, "Chunk file path should match");

        require(db.getTotalChunkCount(client, file) == 2, "Total chunk count should be 2");

        db.deleteChunksByFile(client, file);
        require(db.getChunkStatus(client, file, 0) == 0, "Chunk should be removed after delete");
        }, stats);

    runTest("Database singleton remains a single instance", [&]() {
        Database* instanceBefore = &db;
        Database::initialize((env.tempRoot / "other.db").string());
        Database* instanceAfter = &Database::getInstance();
        require(instanceBefore == instanceAfter, "Database initialize should keep single instance");
        }, stats);
}
