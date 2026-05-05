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
                         int totalChunks, const std::string &hash) {
  ChunkMetadata chunk;
  chunk.setClientAddress(client);
  chunk.setDirectory(directory);
  chunk.setFilename(filename);
  chunk.setChunkNumber(chunkNumber);
  chunk.setTotalChunk(totalChunks);
  chunk.setHash(hash);
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
