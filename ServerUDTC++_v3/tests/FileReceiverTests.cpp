#include "tests/TestSuites.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include "FileReceiver.h"
#include "ServerConfig.h"

#include <chrono>
#include <fstream>
#include <thread>

namespace {
namespace fs = std::filesystem;

class ConstantVerifier final : public IFileIntegrityVerifier {
public:
  explicit ConstantVerifier(std::string hash) : hash_(std::move(hash)) {}

  std::string calculateFileHash(const std::string &) override { return hash_; }

  std::string calculateChunkHash(const std::vector<char> &) override {
    return hash_;
  }

private:
  std::string hash_;
};

class PoolStopGuard {
public:
  explicit PoolStopGuard(ThreadPool &pool) : pool_(pool) {}
  ~PoolStopGuard() { pool_.stop(); }

private:
  ThreadPool &pool_;
};

class ServerPolicyGuard {
public:
  explicit ServerPolicyGuard(ServerConfig::ChangedFilesServerPolicy policy)
      : previous_(ServerConfig::getInstance().getChangedFilesServerPolicy()) {
    ServerConfig::getInstance().setChangedFilesServerPolicyForTesting(policy);
  }
  ~ServerPolicyGuard() {
    ServerConfig::getInstance().setChangedFilesServerPolicyForTesting(previous_);
  }

private:
  ServerConfig::ChangedFilesServerPolicy previous_;
};

fs::path normalizePathForFs(const fs::path &inputPath) {
#ifdef _WIN32
  if (inputPath.empty()) {
    return inputPath;
  }

  std::error_code ec;
  fs::path absolutePath = inputPath;
  if (!absolutePath.is_absolute()) {
    absolutePath = fs::absolute(absolutePath, ec);
    if (ec) {
      absolutePath = inputPath;
    }
  }

  absolutePath.make_preferred();
  std::wstring winPath = absolutePath.wstring();
  if (winPath.rfind(L"\\\\?\\", 0) == 0) {
    return fs::path(winPath);
  }

  if (winPath.rfind(L"\\\\", 0) == 0) {
    return fs::path(L"\\\\?\\UNC\\" + winPath.substr(2));
  }

  return fs::path(L"\\\\?\\" + winPath);
#else
  return inputPath;
#endif
}

std::string makeLongRelativeDirectory() {
  std::string directory;
  for (int i = 0; i < 18; ++i) {
    if (!directory.empty()) {
      directory += "/";
    }
    directory += "segment_" + std::to_string(i) + "_abcdefghijkl";
  }
  return directory;
}

ChunkMetadata buildChunk(const std::string &client,
                         const std::string &directory,
                         const std::string &filename, int chunkNumber,
                         int totalChunks, const std::string &hash,
                         const std::string &transferId = "") {
  ChunkMetadata chunk;
  chunk.setClientAddress(client);
  chunk.setDirectory(directory);
  chunk.setFilename(filename);
  chunk.setChunkNumber(chunkNumber);
  chunk.setTotalChunk(totalChunks);
  chunk.setHash(hash);
  chunk.setTransferId(transferId);
  return chunk;
}

bool waitForFile(const fs::path &filePath, int timeoutMs) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  while (std::chrono::steady_clock::now() < deadline) {
    if (fs::exists(normalizePathForFs(filePath))) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return fs::exists(normalizePathForFs(filePath));
}

std::string readBinaryFile(const fs::path &filePath) {
  std::ifstream input(normalizePathForFs(filePath), std::ios::binary);
  if (!input.is_open()) {
    return std::string();
  }
  return std::string((std::istreambuf_iterator<char>(input)),
                     std::istreambuf_iterator<char>());
}

bool waitForFileContent(const fs::path &filePath,
                        const std::string &expectedContent, int timeoutMs) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  while (std::chrono::steady_clock::now() < deadline) {
    if (readBinaryFile(filePath) == expectedContent) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return readBinaryFile(filePath) == expectedContent;
}

fs::path waitForAnyFileContent(const fs::path &root,
                               const std::string &expectedContent,
                               int timeoutMs) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  while (std::chrono::steady_clock::now() < deadline) {
    std::error_code ec;
    if (fs::exists(normalizePathForFs(root), ec)) {
      for (const auto &entry :
           fs::recursive_directory_iterator(normalizePathForFs(root), ec)) {
        if (!ec && entry.is_regular_file(ec) &&
            readBinaryFile(entry.path()) == expectedContent) {
          return entry.path();
        }
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return {};
}

bool waitForReconstructedRow(Database &db, const std::string &clientKey,
                             const std::string &filename, int timeoutMs) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  while (std::chrono::steady_clock::now() < deadline) {
    if (db.isFileReconstructed(clientKey, filename)) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return db.isFileReconstructed(clientKey, filename);
}

} // namespace

void runFileReceiverTests(TestStats &stats, Database &db,
                          const TestEnvironment &) {
  runTest(
      "FileReceiver reconstructs file with long Windows path",
      [&]() {
        ThreadPool pool(2);
        auto verifier = std::make_unique<ConstantVerifier>("expected-hash");
        FileReceiver receiver(db, std::move(verifier),
                              ServerConfig::getInstance(), pool);
        PoolStopGuard guard(pool);

        const std::string clientIp = "127.0.0.1";
        const std::string longDirectory = makeLongRelativeDirectory();
        const std::string filename = "long_path_file.bin";
        const std::string payload = "payload-for-long-path-test";
        const std::vector<char> data(payload.begin(), payload.end());

        ChunkMetadata chunk = buildChunk(clientIp, longDirectory, filename, 0,
                                         1, "expected-hash");
        const auto result = receiver.receiveChunk(chunk, data);
        require(result == FileReceiver::ReceiveResult::JustCompleted,
                "Single-chunk file should return JustCompleted");

        const std::string clientKey = clientIp + "/" + longDirectory;
        const fs::path reconstructedPath =
            fs::path(ServerConfig::getInstance().getReconstructedPath()) /
            clientKey / filename;
        require(
            reconstructedPath.string().size() > 260,
            "Test path should exceed MAX_PATH to validate long-path behavior");
        require(waitForFile(reconstructedPath, 5000),
                "Reconstructed file should be created");
        require(readBinaryFile(reconstructedPath) == payload,
                "Reconstructed file content should match payload");
      },
      stats);

  runTest(
      "FileReceiver returns AlreadyCompleted for existing long-path output",
      [&]() {
        ThreadPool pool(1);
        auto verifier = std::make_unique<ConstantVerifier>("expected-hash");
        FileReceiver receiver(db, std::move(verifier),
                              ServerConfig::getInstance(), pool);
        PoolStopGuard guard(pool);

        const std::string clientIp = "127.0.0.1";
        const std::string longDirectory = makeLongRelativeDirectory();
        const std::string filename = "already_completed_long_path.bin";

        const std::string clientKey = clientIp + "/" + longDirectory;
        const fs::path reconstructedPath =
            fs::path(ServerConfig::getInstance().getReconstructedPath()) /
            clientKey / filename;
        require(
            reconstructedPath.string().size() > 260,
            "Test path should exceed MAX_PATH to validate long-path behavior");

        fs::create_directories(
            normalizePathForFs(reconstructedPath.parent_path()));
        {
          std::ofstream output(normalizePathForFs(reconstructedPath),
                               std::ios::binary | std::ios::trunc);
          require(output.is_open(), "Should create reconstructed file fixture");
          output << "already-present";
        }

        const std::vector<char> data{'x', 'y', 'z'};
        ChunkMetadata chunk = buildChunk(clientIp, longDirectory, filename, 0,
                                         1, "expected-hash");

        const auto result = receiver.receiveChunk(chunk, data);
        require(
            result == FileReceiver::ReceiveResult::AlreadyCompleted,
            "Chunk should be skipped when reconstructed file already exists");
      },
      stats);

  runTest(
      "FileReceiver replaces managed output when same path receives new content",
      [&]() {
        ServerPolicyGuard policyGuard(
            ServerConfig::ChangedFilesServerPolicy::Overwrite);
        ThreadPool pool(2);
        auto verifier = std::make_unique<ConstantVerifier>("expected-hash");
        FileReceiver receiver(db, std::move(verifier),
                              ServerConfig::getInstance(), pool);
        PoolStopGuard guard(pool);

        const std::string clientIp = "127.0.0.1";
        const std::string directory = "changed_content";
        const std::string filename = "same_path_changed.bin";
        const std::string clientKey = clientIp + "/" + directory;
        const fs::path reconstructedPath =
            fs::path(ServerConfig::getInstance().getReconstructedPath()) /
            clientKey / filename;

        const std::string oldPayload = "old-version";
        std::vector<char> oldData(oldPayload.begin(), oldPayload.end());
        ChunkMetadata oldChunk =
            buildChunk(clientIp, directory, filename, 0, 1, "expected-hash");
        oldChunk.setChunkOffset(0);
        oldChunk.setTotalFileSize(oldPayload.size());

        require(receiver.receiveChunk(oldChunk, oldData) ==
                    FileReceiver::ReceiveResult::JustCompleted,
                "Initial single-chunk file should complete.");
        require(waitForFileContent(reconstructedPath, oldPayload, 5000),
                "Initial reconstructed file content should match.");
        require(waitForReconstructedRow(db, clientKey, filename, 5000),
                "Initial reconstructed file should be persisted in DB.");

        const std::string newPayload = "new-version-content";
        std::vector<char> newData(newPayload.begin(), newPayload.end());
        ChunkMetadata newChunk =
            buildChunk(clientIp, directory, filename, 0, 1, "expected-hash");
        newChunk.setChunkOffset(0);
        newChunk.setTotalFileSize(newPayload.size());

        require(receiver.receiveChunk(newChunk, newData) ==
                    FileReceiver::ReceiveResult::JustCompleted,
                "New first chunk for the same managed path should replace output.");
        require(waitForFileContent(reconstructedPath, newPayload, 5000),
                "Reconstructed output should contain the changed content.");
        require(waitForReconstructedRow(db, clientKey, filename, 5000),
                "Replacement reconstructed file should be persisted in DB.");
      },
      stats);

  runTest(
      "FileReceiver rejects changed same-path content when configured",
      [&]() {
        ServerPolicyGuard policyGuard(
            ServerConfig::ChangedFilesServerPolicy::Reject);
        ThreadPool pool(2);
        auto verifier = std::make_unique<ConstantVerifier>("expected-hash");
        FileReceiver receiver(db, std::move(verifier),
                              ServerConfig::getInstance(), pool);
        PoolStopGuard guard(pool);

        const std::string clientIp = "127.0.0.1";
        const std::string directory = "changed_content_reject";
        const std::string filename = "same_path_reject.bin";
        const std::string clientKey = clientIp + "/" + directory;
        const fs::path reconstructedPath =
            fs::path(ServerConfig::getInstance().getReconstructedPath()) /
            clientKey / filename;

        const std::string oldPayload = "old-version-reject";
        std::vector<char> oldData(oldPayload.begin(), oldPayload.end());
        ChunkMetadata oldChunk =
            buildChunk(clientIp, directory, filename, 0, 1, "expected-hash");
        oldChunk.setChunkOffset(0);
        oldChunk.setTotalFileSize(oldPayload.size());

        require(receiver.receiveChunk(oldChunk, oldData) ==
                    FileReceiver::ReceiveResult::JustCompleted,
                "Initial file should complete before reject policy is tested.");
        require(waitForFileContent(reconstructedPath, oldPayload, 5000),
                "Initial rejected-policy fixture should be reconstructed.");
        require(waitForReconstructedRow(db, clientKey, filename, 5000),
                "Initial rejected-policy fixture should be in DB.");

        const std::string newPayload = "new-version-rejected";
        std::vector<char> newData(newPayload.begin(), newPayload.end());
        ChunkMetadata newChunk =
            buildChunk(clientIp, directory, filename, 0, 1, "expected-hash");
        newChunk.setChunkOffset(0);
        newChunk.setTotalFileSize(newPayload.size());

        require(receiver.receiveChunk(newChunk, newData) ==
                    FileReceiver::ReceiveResult::AlreadyCompleted,
                "Reject policy should not accept changed same-path content.");
        require(readBinaryFile(reconstructedPath) == oldPayload,
                "Reject policy must preserve the original managed output.");
      },
      stats);

  runTest(
      "FileReceiver versions changed same-path content when configured",
      [&]() {
        ServerPolicyGuard policyGuard(
            ServerConfig::ChangedFilesServerPolicy::Versioned);
        ThreadPool pool(2);
        auto verifier = std::make_unique<ConstantVerifier>("expected-hash");
        FileReceiver receiver(db, std::move(verifier),
                              ServerConfig::getInstance(), pool);
        PoolStopGuard guard(pool);

        const std::string clientIp = "127.0.0.1";
        const std::string directory = "changed_content_versioned";
        const std::string filename = "same_path_versioned.bin";
        const std::string clientKey = clientIp + "/" + directory;
        const fs::path reconstructedPath =
            fs::path(ServerConfig::getInstance().getReconstructedPath()) /
            clientKey / filename;
        const fs::path reconstructedFolder = reconstructedPath.parent_path();

        const std::string oldPayload = "old-version-versioned";
        std::vector<char> oldData(oldPayload.begin(), oldPayload.end());
        ChunkMetadata oldChunk =
            buildChunk(clientIp, directory, filename, 0, 1, "expected-hash");
        oldChunk.setChunkOffset(0);
        oldChunk.setTotalFileSize(oldPayload.size());

        require(receiver.receiveChunk(oldChunk, oldData) ==
                    FileReceiver::ReceiveResult::JustCompleted,
                "Initial file should complete before versioning is tested.");
        require(waitForFileContent(reconstructedPath, oldPayload, 5000),
                "Initial versioning fixture should be reconstructed.");
        require(waitForReconstructedRow(db, clientKey, filename, 5000),
                "Initial versioning fixture should be persisted in DB.");

        const std::string newPayload = "new-version-kept-as-version";
        std::vector<char> newData(newPayload.begin(), newPayload.end());
        ChunkMetadata newChunk =
            buildChunk(clientIp, directory, filename, 0, 1, "expected-hash");
        newChunk.setChunkOffset(0);
        newChunk.setTotalFileSize(newPayload.size());

        require(receiver.receiveChunk(newChunk, newData) ==
                    FileReceiver::ReceiveResult::JustCompleted,
                "Versioned policy should accept changed same-path content.");
        const fs::path versionedPath =
            waitForAnyFileContent(reconstructedFolder, newPayload, 5000);
        require(!versionedPath.empty(),
                "Versioned policy should create a second output file.");
        require(normalizePathForFs(versionedPath) !=
                    normalizePathForFs(reconstructedPath),
                "Versioned output must not overwrite the original path.");
        require(readBinaryFile(reconstructedPath) == oldPayload,
                "Versioned policy must preserve the original managed output.");
      },
      stats);

  runTest(
      "FileReceiver ignores duplicate successful chunk without completing early",
      [&]() {
        ThreadPool pool(2);
        auto verifier = std::make_unique<ConstantVerifier>("expected-hash");
        FileReceiver receiver(db, std::move(verifier),
                              ServerConfig::getInstance(), pool);
        PoolStopGuard guard(pool);

        const std::string clientIp = "127.0.0.1";
        const std::string directory = "duplicate_chunks";
        const std::string filename = "duplicate_chunk_file.bin";
        const std::string clientKey = clientIp + "/" + directory;

        const std::vector<char> firstChunk{'a', 'b', 'c'};
        const std::vector<char> secondChunk{'d', 'e', 'f'};

        ChunkMetadata chunk0 =
            buildChunk(clientIp, directory, filename, 0, 2, "expected-hash");
        chunk0.setChunkOffset(0);
        chunk0.setTotalFileSize(6);

        ChunkMetadata chunk1 =
            buildChunk(clientIp, directory, filename, 1, 2, "expected-hash");
        chunk1.setChunkOffset(3);
        chunk1.setTotalFileSize(6);

        const auto firstResult = receiver.receiveChunk(chunk0, firstChunk);
        require(firstResult == FileReceiver::ReceiveResult::Stored,
                "First chunk of two should be stored without completion");

        const auto duplicateResult = receiver.receiveChunk(chunk0, firstChunk);
        require(duplicateResult == FileReceiver::ReceiveResult::Stored,
                "Duplicate successful chunk should be acknowledged as stored");
        require(!db.isFileReconstructed(clientKey, filename),
                "Duplicate chunk must not mark the file as reconstructed");

        const auto finalResult = receiver.receiveChunk(chunk1, secondChunk);
        require(finalResult == FileReceiver::ReceiveResult::JustCompleted,
                "Second distinct chunk should complete the file");

        const fs::path reconstructedPath =
            fs::path(ServerConfig::getInstance().getReconstructedPath()) /
            clientKey / filename;
        require(waitForFile(reconstructedPath, 5000),
                "Reconstructed file should exist after the final chunk");
        require(readBinaryFile(reconstructedPath) == "abcdef",
                "Reconstructed file content should include each chunk once");
      },
      stats);

  runTest(
      "FileReceiver acknowledges completed transfer retry by identity",
      [&]() {
        ThreadPool pool(1);
        auto verifier = std::make_unique<ConstantVerifier>("expected-hash");
        FileReceiver receiver(db, std::move(verifier),
                              ServerConfig::getInstance(), pool);
        PoolStopGuard guard(pool);

        const std::string client = "resilience-client";
        const std::string directory = "lost_ack";
        const std::string filename = "completed_retry.bin";
        const std::string transferId =
            "1111111111111111111111111111111111111111111111111111111111111111";
        const std::string clientKey = client + "/" + directory;
        const std::vector<char> data{'o', 'k'};

        ChunkMetadata chunk = buildChunk(client, directory, filename, 0, 1,
                                         "expected-hash", transferId);
        chunk.setChunkOffset(0);
        chunk.setTotalFileSize(data.size());

        require(receiver.receiveChunk(chunk, data) ==
                    FileReceiver::ReceiveResult::JustCompleted,
                "Initial transfer should complete");
        require(waitForReconstructedRow(db, clientKey, filename, 5000),
                "Completed transfer should be persisted");
        require(db.getReconstructedTransferId(clientKey, filename) ==
                    transferId,
                "Completed transfer identity should be persisted");
        require(receiver.receiveChunk(chunk, data) ==
                    FileReceiver::ReceiveResult::AlreadyCompleted,
                "Retry after a lost final ACK should be idempotent");

        const fs::path reconstructedPath =
            fs::path(ServerConfig::getInstance().getReconstructedPath()) /
            clientKey / filename;
        require(readBinaryFile(reconstructedPath) == "ok",
                "Idempotent retry must not rewrite completed content");
      },
      stats);

  runTest(
      "FileReceiver replaces interrupted transfer only from chunk zero",
      [&]() {
        ThreadPool pool(2);
        auto verifier = std::make_unique<ConstantVerifier>("expected-hash");
        FileReceiver receiver(db, std::move(verifier),
                              ServerConfig::getInstance(), pool);
        PoolStopGuard guard(pool);

        const std::string client = "resilience-client";
        const std::string directory = "partial_replace";
        const std::string filename = "partial.bin";
        const std::string transferA(64, 'a');
        const std::string transferB(64, 'b');
        const std::string clientKey = client + "/" + directory;

        ChunkMetadata firstA = buildChunk(client, directory, filename, 0, 2,
                                          "expected-hash", transferA);
        firstA.setChunkOffset(0);
        firstA.setTotalFileSize(6);
        require(receiver.receiveChunk(firstA, {'o', 'l', 'd'}) ==
                    FileReceiver::ReceiveResult::Stored,
                "First interrupted transfer chunk should be stored");

        ChunkMetadata nonFirstB = buildChunk(
            client, directory, filename, 1, 2, "expected-hash", transferB);
        nonFirstB.setChunkOffset(3);
        nonFirstB.setTotalFileSize(6);
        require(receiver.receiveChunk(nonFirstB, {'x', 'y', 'z'}) ==
                    FileReceiver::ReceiveResult::Failed,
                "A new transfer must not replace partial state mid-stream");

        ChunkMetadata firstB = buildChunk(client, directory, filename, 0, 2,
                                          "expected-hash", transferB);
        firstB.setChunkOffset(0);
        firstB.setTotalFileSize(6);
        require(receiver.receiveChunk(firstB, {'n', 'e', 'w'}) ==
                    FileReceiver::ReceiveResult::Stored,
                "Chunk zero should replace stale partial transfer state");
        require(db.getActiveTransferId(clientKey, filename) == transferB,
                "Active state should belong to the replacement transfer");
        require(receiver.receiveChunk(nonFirstB, {'1', '2', '3'}) ==
                    FileReceiver::ReceiveResult::JustCompleted,
                "Replacement transfer should complete normally");

        const fs::path reconstructedPath =
            fs::path(ServerConfig::getInstance().getReconstructedPath()) /
            clientKey / filename;
        require(waitForFileContent(reconstructedPath, "new123", 5000),
                "Replacement content should be reconstructed without stale bytes");
      },
      stats);

  runTest(
      "FileReceiver reconstructs adaptive chunks when final declares total",
      [&]() {
        ThreadPool pool(2);
        auto verifier = std::make_unique<ConstantVerifier>("expected-hash");
        FileReceiver receiver(db, std::move(verifier),
                              ServerConfig::getInstance(), pool);
        PoolStopGuard guard(pool);

        const std::string clientIp = "127.0.0.1";
        const std::string directory = "adaptive_chunks";
        const std::string filename = "adaptive_chunk_file.bin";
        const std::string clientKey = clientIp + "/" + directory;
        const std::string payload = "abcdefghi";

        const std::vector<char> firstChunk{'a', 'b', 'c'};
        const std::vector<char> secondChunk{'d', 'e', 'f', 'g'};
        const std::vector<char> finalChunk{'h', 'i'};

        ChunkMetadata chunk0 =
            buildChunk(clientIp, directory, filename, 0, 0, "expected-hash");
        chunk0.setChunkOffset(0);
        chunk0.setTotalFileSize(payload.size());

        ChunkMetadata chunk1 =
            buildChunk(clientIp, directory, filename, 1, 0, "expected-hash");
        chunk1.setChunkOffset(3);
        chunk1.setTotalFileSize(payload.size());

        ChunkMetadata chunk2 =
            buildChunk(clientIp, directory, filename, 2, 3, "expected-hash");
        chunk2.setChunkOffset(7);
        chunk2.setTotalFileSize(payload.size());

        require(receiver.receiveChunk(chunk0, firstChunk) ==
                    FileReceiver::ReceiveResult::Stored,
                "Adaptive non-final chunk should be stored");
        require(receiver.receiveChunk(chunk1, secondChunk) ==
                    FileReceiver::ReceiveResult::Stored,
                "Second adaptive non-final chunk should be stored");
        require(receiver.receiveChunk(chunk2, finalChunk) ==
                    FileReceiver::ReceiveResult::JustCompleted,
                "Final adaptive chunk should complete the file");

        const fs::path reconstructedPath =
            fs::path(ServerConfig::getInstance().getReconstructedPath()) /
            clientKey / filename;
        require(waitForFileContent(reconstructedPath, payload, 5000),
                "Reconstructed adaptive file content should match payload");
      },
      stats);

  runTest(
      "FileReceiver reconstructs nested relative destination tree",
      [&]() {
        ThreadPool pool(2);
        auto verifier = std::make_unique<ConstantVerifier>("expected-hash");
        FileReceiver receiver(db, std::move(verifier),
                              ServerConfig::getInstance(), pool);
        PoolStopGuard guard(pool);

        const std::string clientIp = "127.0.0.1";
        const std::string directory = "XXX/XXXX";
        const std::string filename = "nomedoarquivo.bin";
        const std::string payload = "nested tree payload";
        const std::vector<char> data(payload.begin(), payload.end());

        ChunkMetadata chunk =
            buildChunk(clientIp, directory, filename, 0, 1, "expected-hash");
        chunk.setChunkOffset(0);
        chunk.setTotalFileSize(payload.size());

        require(receiver.receiveChunk(chunk, data) ==
                    FileReceiver::ReceiveResult::JustCompleted,
                "Single nested relative chunk should complete the file");

        const fs::path reconstructedPath =
            fs::path(ServerConfig::getInstance().getReconstructedPath()) /
            clientIp / directory / filename;
        require(waitForFileContent(reconstructedPath, payload, 5000),
                "Reconstructed file should preserve nested directory tree");
      },
      stats);

  runTest(
      "FileReceiver rejects file larger than configured maximum",
      [&]() {
        ThreadPool pool(1);
        auto verifier = std::make_unique<ConstantVerifier>("expected-hash");
        FileReceiver receiver(db, std::move(verifier),
                              ServerConfig::getInstance(), pool);
        PoolStopGuard guard(pool);

        const auto &config = ServerConfig::getInstance();
        const std::string clientIp = "oversized-client";
        const std::string directory = "quota";
        const std::string filename = "too_large.bin";
        const std::vector<char> data{'x'};

        ChunkMetadata chunk = buildChunk(clientIp, directory, filename, 0, 1,
                                         "expected-hash");
        chunk.setChunkOffset(0);
        chunk.setTotalFileSize(config.getMaxFileSizeBytes() + 1);

        const auto result = receiver.receiveChunk(chunk, data);
        require(result == FileReceiver::ReceiveResult::Failed,
                "Oversized logical file should be rejected before preallocation");

        const fs::path reconstructedPath =
            fs::path(config.getReconstructedPath()) / clientIp / directory /
            filename;
        require(!fs::exists(normalizePathForFs(reconstructedPath)),
                "Rejected oversized file should not be created");
      },
      stats);

  runTest(
      "FileReceiver enforces per-client storage budget",
      [&]() {
        ThreadPool pool(1);
        auto verifier = std::make_unique<ConstantVerifier>("expected-hash");
        FileReceiver receiver(db, std::move(verifier),
                              ServerConfig::getInstance(), pool);
        PoolStopGuard guard(pool);

        const auto &config = ServerConfig::getInstance();
        const std::string clientIp = "quota-client";
        const fs::path existingPath =
            fs::path(config.getReconstructedPath()) / clientIp / "existing.bin";
        fs::create_directories(normalizePathForFs(existingPath.parent_path()));
        {
          std::ofstream output(normalizePathForFs(existingPath),
                               std::ios::binary | std::ios::trunc);
          require(output.is_open(), "Should create quota fixture file");
          output.seekp(static_cast<std::streamoff>(
              config.getMaxClientStorageBytes() - 1));
          const char zero = '\0';
          output.write(&zero, 1);
        }

        const std::string directory = "quota";
        const std::string filename = "over_budget.bin";
        const std::vector<char> data{'x'};
        ChunkMetadata chunk = buildChunk(clientIp, directory, filename, 0, 1,
                                         "expected-hash");
        chunk.setChunkOffset(0);
        chunk.setTotalFileSize(data.size());

        const auto result = receiver.receiveChunk(chunk, data);
        require(result == FileReceiver::ReceiveResult::Failed,
                "New file should be rejected when client quota is exhausted");

        const fs::path rejectedPath =
            fs::path(config.getReconstructedPath()) / clientIp / directory /
            filename;
        require(!fs::exists(normalizePathForFs(rejectedPath)),
                "Rejected over-budget file should not be created");
      },
      stats);
}
