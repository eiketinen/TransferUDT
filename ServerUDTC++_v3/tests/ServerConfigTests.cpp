#include "tests/TestSuites.h"

#include "ServerConfig.h"

#include <filesystem>

namespace {
std::filesystem::path normalizePath(const std::filesystem::path& path) {
    std::error_code ec;
    auto resolved = std::filesystem::weakly_canonical(path, ec);
    if (ec) {
        return path.lexically_normal();
    }
    return resolved;
}
}

void runServerConfigTests(TestStats& stats, const TestEnvironment& env) {
    runTest("ServerConfig loads trimmed values from file", [&]() {
        auto& config = ServerConfig::getInstance();

        require(config.getServerPort() == 50123, "server.port should be loaded from test config");
        require(config.getNumThreads() == 2, "work.threads should be loaded from test config");
        require(config.getMaxPendingTasks() == 3, "work.max_pending_tasks should be loaded from test config");
        require(config.getMaxConnection() == 16, "max.connection should be loaded from test config");
        require(config.getMaxFileSizeBytes() == 1024 * 1024, "server.max_file_size_mb should be loaded from test config");
        require(config.getMaxClientStorageBytes() == 1024 * 1024, "server.max_client_storage_mb should be loaded from test config");

        const auto expectedStorage = normalizePath(env.storagePath);
        const auto expectedReconstructed = normalizePath(env.reconstructedPath);
        const auto actualStorage = normalizePath(std::filesystem::path(config.getStoragePath()));
        const auto actualReconstructed = normalizePath(std::filesystem::path(config.getReconstructedPath()));

        require(actualStorage == expectedStorage, "storage path should match configured value");
        require(actualReconstructed == expectedReconstructed, "reconstructed path should match configured value");

        require(std::filesystem::exists(actualStorage), "configured storage path should exist");
        require(std::filesystem::exists(actualReconstructed), "configured reconstructed path should exist");
        require(config.isSecurityEnabled(), "security.enabled should be loaded from test config");
        require(config.isSecurityHandshakeEnabled(), "security.handshake.enabled should be loaded from test config");
        require(config.getSecurityPreSharedKey().size() >= 32, "security.psk should be loaded from test config");
        require(config.hasClientPreSharedKeys(), "security.client_psk entries should be loaded from test config");
        require(config.isClientIdentityAllowed("agent-one"), "configured per-client PSK identity should be allowed");
        require(!config.isClientIdentityAllowed("agent-two"), "unknown per-client PSK identity should be rejected");
        require(config.getSecurityPreSharedKeyForClient("agent-one") != config.getSecurityPreSharedKey(), "client-specific PSK should override global PSK");
    }, stats);
}
