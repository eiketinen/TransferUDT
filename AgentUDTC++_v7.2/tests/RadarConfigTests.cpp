#include "tests/TestSuites.h"

#include "RadarConfig.h"

#include <cstdlib>
#include <fstream>

void runRadarConfigTests(TestStats &stats, const TestEnvironment &env) {
  runTest(
      "RadarConfig - defaults, bool fallback and chunk cap",
      [&]() {
        const std::filesystem::path malformedConfigPath =
            env.tempRoot / "malformed_config.properties";
        {
          std::ofstream cfg(malformedConfigPath);
          cfg << "# malformed/empty lines to stress parser trim\n";
          cfg << "=\n";
          cfg << "chunk.size = 8192\n";
          cfg << "max.band =   \n";
          cfg << "snd.buf =   \n";
          cfg << "rcv.buf =   \n";
          cfg << "snd.timeout =   \n";
          cfg << "rcv.timeout =   \n";
          cfg << "data.dirs =   \n";
          cfg << "udt.require.greeting = maybe\n";
          cfg << "udt.keepalive.enabled = definitely\n";
          cfg << "resend.changed_files.enabled = true\n";
          cfg << "resend.changed_files.identity = SHA256\n";
          cfg << "security.psk = 0123456789abcdef0123456789abcdef-test-key\n";
          cfg << "server.targets = "
                 "invalid_target_without_port;localhost:70000\n";
        }

        _putenv_s("AGENT_CONFIG_PATH", malformedConfigPath.string().c_str());

        auto &cfg = RadarConfig::getInstance();
        require(cfg.getChunkSize() == 4ULL * 1024ULL * 1024ULL,
                "chunk.size must be capped to 4 MB.");
        require(cfg.getMaxBandwidth() == 10LL * 1024LL * 1024LL,
                "Default max bandwidth must be 10 MB/s.");
        require(cfg.getSendBuffer() == 64ULL * 1024ULL * 1024ULL,
                "Default send buffer must be 64 MB.");
        require(cfg.getReceiveBuffer() == 64ULL * 1024ULL * 1024ULL,
                "Default receive buffer must be 64 MB.");
        require(cfg.getSendTimeout() == 20 * 1000,
                "Default send timeout must be 20 seconds.");
        require(cfg.getReceiveTimeout() == 20 * 1000,
                "Default receive timeout must be 20 seconds.");
        require(!cfg.isAdaptiveChunkEnabled(),
                "Adaptive chunk telemetry should be disabled by default.");
        require(cfg.getAdaptiveChunkMinBytes() == 32ULL * 1024ULL,
                "Adaptive minimum chunk size must default to 32 KB.");
        require(cfg.getAdaptiveChunkMaxBytes() == 4ULL * 1024ULL * 1024ULL,
                "Adaptive maximum chunk size must default to 4 MB.");
        require(cfg.getAdaptiveChunkInitialBytes() == 256ULL * 1024ULL,
                "Adaptive initial chunk size must default to 256 KB.");
        require(cfg.getAdaptiveChunkTargetAckMillis() == 700,
                "Adaptive target ACK must default to 700 ms.");

        require(cfg.isUDTGreetingRequired(),
                "Invalid bool must fallback to default true for "
                "udt.require.greeting.");
        require(!cfg.isUDTKeepAliveEnabled(),
                "Invalid bool must fallback to default false for "
                "udt.keepalive.enabled.");
        require(cfg.isChangedFilesResendEnabled(),
                "Changed-file resend should be enabled from config.");
        require(cfg.getChangedFilesIdentity() == "sha256",
                "Changed-file identity should normalize to sha256.");

        const auto &endpoints = cfg.getServerEndpoints();
        require(endpoints.size() == 1,
                "A default endpoint must be provided when server.targets is "
                "missing/invalid.");
        require(endpoints[0].host == "127.0.0.1",
                "Default endpoint host must be 127.0.0.1.");
        require(endpoints[0].port == 50051,
                "Default endpoint port must be 50051.");
      },
      stats);
}
