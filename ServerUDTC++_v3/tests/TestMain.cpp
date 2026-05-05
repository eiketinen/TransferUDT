#include "tests/TestSuites.h"

#include "Logger.h"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
void configureServerConfigForTests(const TestEnvironment &env) {
  std::ofstream cfg(env.configPath, std::ios::trunc);
  if (!cfg.is_open()) {
    throw std::runtime_error("Failed to create test config file");
  }

  cfg << "# Test configuration\n";
  cfg << "   \n";
  cfg << "= ignored_line\n";
  cfg << " server.port = 50123\n";
  cfg << " server.storage_path = " << env.storagePath.string() << "\n";
  cfg << " server.reconstructed_path = " << env.reconstructedPath.string()
      << "\n";
  cfg << " max.connection = 16\n";
  cfg << " work.threads = 2\n";
  cfg << " work.max_pending_tasks = 3\n";
  cfg << " server.max_file_size_mb = 1\n";
  cfg << " server.max_client_storage_mb = 1\n";
  cfg << " db.filepath = " << env.dbPath.string() << "\n";
  cfg << " log.filepath = " << env.logPath.string() << "\n";
  cfg << " log.max_size_mb = 1\n";
  cfg << " log.backup_count = 1\n";
  cfg << " log.flush_level = always\n";
  cfg << " security.enabled = true\n";
  cfg << " security.handshake.enabled = true\n";
  cfg << " security.allowed_client_ids = agent-one\n";
  cfg << " security.psk = 0123456789abcdef0123456789abcdef-test-key\n";
  cfg << " security.client_psk.agent-one = 0123456789abcdef0123456789abcdef-secure-test-key-agent-one\n";
  cfg.close();

#ifdef _WIN32
  if (_putenv_s("SERVER_CONFIG_PATH", env.configPath.string().c_str()) != 0) {
    throw std::runtime_error("Failed to set SERVER_CONFIG_PATH");
  }
#else
  if (setenv("SERVER_CONFIG_PATH", env.configPath.string().c_str(), 1) != 0) {
    throw std::runtime_error("Failed to set SERVER_CONFIG_PATH");
  }
#endif
}
} // namespace

int main() {
  TestStats stats;

  TestEnvironment env;
  env.tempRoot = createTempRoot();
  env.logPath = env.tempRoot / "tests.log";
  env.dbPath = env.tempRoot / "tests.db";
  env.configPath = env.tempRoot / "test_server_config.properties";
  env.storagePath = env.tempRoot / "storage";
  env.reconstructedPath = env.tempRoot / "reconstructed";

  configureServerConfigForTests(env);

  Logger::Initialize(env.logPath.string(), 1024 * 1024, 1, "always");

  runServerConfigTests(stats, env);
  runChunkPacketParserTests(stats);
  runCircuitBreakerTests(stats);

  Database::initialize(env.dbPath.string());
  auto &db = Database::getInstance();
  runDatabaseTests(stats, db, env);
  runFileReceiverTests(stats, db, env);
  runSecureTransferIntegrationTests(stats, db, env);

  std::cout << "\nSummary: " << stats.passed << " passed, " << stats.failed
            << " failed.\n";

  // Flush and drop all spdlog loggers before static destructors run.
  // Without this, the ThreadPool destructor (which joins worker threads
  // that may still reference spdlog) races against spdlog's global
  // registry destruction, causing an access violation on exit.
  spdlog::shutdown();

  return stats.failed == 0 ? 0 : 1;
}
