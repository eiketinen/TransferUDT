#include "RadarConfig.h"

constexpr const char* CONFIG_ENV_VAR = "AGENT_CONFIG_PATH";

// Resolves configuration file path from environment with default fallback.
static std::string getConfigFilePathFromEnv() {
    char* buffer = nullptr;
    size_t size = 0;
    if (_dupenv_s(&buffer, &size, "AGENT_CONFIG_PATH") == 0 && buffer != nullptr) {
        std::string result(buffer);
        free(buffer); // _dupenv_s allocates memory that must be released.
        return result;
    }
    return "config.properties"; // default fallback file
}

static std::string getEnvValue(const char *name) {
    char* buffer = nullptr;
    size_t size = 0;
    if (_dupenv_s(&buffer, &size, name) == 0 && buffer != nullptr) {
        std::string result(buffer);
        free(buffer);
        return result;
    }
    return "";
}

static bool isPlaceholderPsk(const std::string& value) {
    return value == "replace-with-a-strong-shared-secret-of-32-plus-chars";
}

static bool isValidIdentityMode(const std::string& value) {
    return value == "psk" || value == "signed_handshake" ||
           value == "certificate_handshake";
}

static std::string normalizeLower(std::string value) {
    std::transform(
        value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

static bool isValidChangedFilesIdentity(const std::string& value) {
    return normalizeLower(value) == "sha256";
}

// Parses a semicolon-separated host:port list and keeps only valid endpoints.
std::vector<RadarConfig::ServerEndpoint> RadarConfig::parseServerTargets(const std::string& targets) {
    std::vector<ServerEndpoint> endpoints;

    if (targets.empty()) {
        return endpoints;
    }

    auto entries = split(targets, ';');
    for (const auto& entry : entries) {
        if (entry.empty()) {
            continue;
        }

        auto separatorPos = entry.find(':');
        if (separatorPos == std::string::npos) {
            std::cerr << "RadarConfig: invalid server target '" << entry << "'. Expected format host:port.\n";
            continue;
        }

        std::string host = trim(entry.substr(0, separatorPos));
        std::string portStr = trim(entry.substr(separatorPos + 1));

        if (host.empty()) {
            std::cerr << "RadarConfig: invalid server target '" << entry << "'. Host cannot be empty.\n";
            continue;
        }

        int port = 0;
        try {
            port = std::stoi(portStr);
        }
        catch (const std::exception&) {
            std::cerr << "RadarConfig: invalid port in server target '" << entry << "'.\n";
            continue;
        }

        if (port <= 0 || port > 65535) {
            std::cerr << "RadarConfig: port out of range in server target '" << entry << "'.\n";
            continue;
        }

        endpoints.push_back({ std::move(host), port });
    }

    return endpoints;
}

// Loads config.properties values and applies defaults/validation rules.
RadarConfig::RadarConfig() {
    auto configMap = RadarConfig::loadConfigFile(getConfigFilePathFromEnv());

    // Load configuration parameters with validation.
    std::string serverTargetsRaw = RadarConfig::loadConfigParam<std::string>(
        configMap, "server.targets", "",
        "Invalid server targets string", [](const std::string&) { return true; }
    );

    if (!serverTargetsRaw.empty()) {
        auto parsedTargets = parseServerTargets(serverTargetsRaw);
        if (!parsedTargets.empty()) {
            serverEndpoints = std::move(parsedTargets);
        }
        else {
            std::cerr << "RadarConfig: ignoring server.targets because no valid endpoints were found.\n";
        }
    }
    if (serverEndpoints.empty()) {
        serverEndpoints.push_back({ DEFAULT_SERVER_HOST, DEFAULT_SERVER_PORT });
        std::cerr << "RadarConfig: using default server target "
                  << DEFAULT_SERVER_HOST << ":" << DEFAULT_SERVER_PORT << ".\n";
    }
    // chunk.size is configured in KB and converted to bytes.
    int64_t chunkSizeIn = RadarConfig::loadConfigParam<int64_t>(
        configMap, "chunk.size", DEFAULT_CHUNK_SIZE,
        "Size must be positive",
        [](int64_t size) { return size > 0; }
    );

    constexpr int64_t MAX_CHUNK_SIZE_KB = 4 * 1024;
    if (chunkSizeIn > MAX_CHUNK_SIZE_KB) {
        std::cerr << "RadarConfig: chunk.size=" << chunkSizeIn
                  << " KB exceeds the max supported value (" << MAX_CHUNK_SIZE_KB
                  << " KB). Capping to 4 MB." << std::endl;
        chunkSizeIn = MAX_CHUNK_SIZE_KB;
    }

    chunkSize = static_cast<DWORDLONG>(chunkSizeIn) * 1024;

    adaptiveChunkEnabled = RadarConfig::loadConfigParam<bool>(
        configMap, "chunk.adaptive.enabled", DEFAULT_ADAPTIVE_CHUNK_ENABLED,
        "Adaptive chunk enabled must be true or false",
        [](bool) { return true; }
    );

    auto adaptiveMinKb = RadarConfig::loadConfigParam<int64_t>(
        configMap, "chunk.adaptive.min_kb", DEFAULT_ADAPTIVE_CHUNK_MIN_KB,
        "Adaptive minimum chunk size must be positive",
        [](int64_t value) { return value > 0; }
    );
    auto adaptiveMaxKb = RadarConfig::loadConfigParam<int64_t>(
        configMap, "chunk.adaptive.max_kb", DEFAULT_ADAPTIVE_CHUNK_MAX_KB,
        "Adaptive maximum chunk size must be positive",
        [](int64_t value) { return value > 0; }
    );
    auto adaptiveInitialKb = RadarConfig::loadConfigParam<int64_t>(
        configMap, "chunk.adaptive.initial_kb", DEFAULT_ADAPTIVE_CHUNK_INITIAL_KB,
        "Adaptive initial chunk size must be positive",
        [](int64_t value) { return value > 0; }
    );

    if (adaptiveMaxKb > MAX_CHUNK_SIZE_KB) {
        std::cerr << "RadarConfig: chunk.adaptive.max_kb=" << adaptiveMaxKb
                  << " KB exceeds the max supported value (" << MAX_CHUNK_SIZE_KB
                  << " KB). Capping to 4 MB." << std::endl;
        adaptiveMaxKb = MAX_CHUNK_SIZE_KB;
    }

    if (adaptiveMinKb > adaptiveMaxKb) {
        std::cerr << "RadarConfig: chunk.adaptive.min_kb exceeds max_kb. Using max_kb as minimum." << std::endl;
        adaptiveMinKb = adaptiveMaxKb;
    }

    if (adaptiveInitialKb < adaptiveMinKb) {
        adaptiveInitialKb = adaptiveMinKb;
    }
    if (adaptiveInitialKb > adaptiveMaxKb) {
        adaptiveInitialKb = adaptiveMaxKb;
    }

    adaptiveChunkMinBytes = adaptiveMinKb * 1024;
    adaptiveChunkMaxBytes = adaptiveMaxKb * 1024;
    adaptiveChunkInitialBytes = adaptiveInitialKb * 1024;
    adaptiveChunkTargetAckMillis = RadarConfig::loadConfigParam<int>(
        configMap, "chunk.adaptive.target_ack_ms", DEFAULT_ADAPTIVE_CHUNK_TARGET_ACK_MS,
        "Adaptive target ACK time must be positive",
        [](int value) { return value > 0; }
    );

    std::string dirs_str = RadarConfig::loadConfigParam<std::string>(
        configMap, "data.dirs", DEFAULT_DATA_DIRS,
        "Invalid directories string", [](const std::string& s) {
            return !s.empty();
        }
    );

    for (const auto& dir : split(dirs_str, ';')) {
        if (!dir.empty()) {
            dataDirs.push_back(dir);
        }
    }

    if (dataDirs.empty()) {
        for (const auto& fallbackDir : split(DEFAULT_DATA_DIRS, ';')) {
            if (!fallbackDir.empty()) {
                dataDirs.push_back(fallbackDir);
            }
        }
    }

    for (const auto& dir : dataDirs) {
        if (fs::exists(dir)) {
            continue;
        }

        try {
            if (!fs::create_directories(dir)) {
                std::cerr << "Failed to create directory, but create_directories returned false: " << dir << std::endl;
            }
        }
        catch (const std::exception& e) {
            std::cerr << "Error creating directory '" << dir << "': " << e.what() << std::endl;
        }
    }

    // Convert bandwidth from MB/s to bytes/s.
    int64_t bandMB = RadarConfig::loadConfigParam<int64_t>(
        configMap, "max.band", DEFAULT_MAX_BAND, // 10 MB/s
        "Bandwidth must be positive",
        [](int64_t band) { return band > 0; }
    );
    maxBandwidth = bandMB * 1024 * 1024;

    segmentSize = RadarConfig::loadConfigParam<int>(
        configMap, "seg.len", DEFAULT_SEG_LEN,
        "Segment size must be positive",
        [](int len) { return len > 0; }
    );

    // Convert buffer sizes from MB to bytes.
    int64_t sndBufMB = RadarConfig::loadConfigParam<int64_t>(
        configMap, "snd.buf", DEFAULT_SND_BUF, // 4 MB
        "Buffer size must be positive",
        [](int64_t buf) { return buf > 0; }
    );
    sendBuffer = sndBufMB * 1024 * 1024;

    int64_t rcvBufMB = RadarConfig::loadConfigParam<int64_t>(
        configMap, "rcv.buf", DEFAULT_RCV_BUF, // 4 MB
        "Buffer size must be positive",
        [](int64_t buf) { return buf > 0; }
    );
    receiveBuffer = rcvBufMB * 1024 * 1024;

    // Convert timeout values from seconds to milliseconds.
    int sndTimeoutSec = RadarConfig::loadConfigParam<int>(
        configMap, "snd.timeout", DEFAULT_SND_TIMEOUT, // 20 seconds
        "Timeout must be positive",
        [](int timeout) { return timeout > 0; }
    );
    sendTimeout = sndTimeoutSec * 1000;

    int rcvTimeoutSec = RadarConfig::loadConfigParam<int>(
        configMap, "rcv.timeout", DEFAULT_RCV_TIMEOUT, // 20 seconds
        "Timeout must be positive",
        [](int timeout) { return timeout > 0; }
    );
    receiveTimeout = rcvTimeoutSec * 1000;

    maxRetries = RadarConfig::loadConfigParam<int>(
        configMap, "max.retries", DEFAULT_MAX_RETRIES,
        "Number of retries must be positive",
        [](int retries) { return retries > 0; }
    );

    numThreads = RadarConfig::loadConfigParam<int>(
        configMap, "work.threads", DEFAULT_WORKER_THREADS, // Default 4 threads
        "Number of threads must be positive",
        [](int threads) { return threads > 0; }
    );

    stabilityCheckInterval = RadarConfig::loadConfigParam<int>(
        configMap, "stability.check.interval.seconds", DEFAULT_STABILITY_INTERVAL_S,
        "Interval must be positive", [](int v) { return v > 0; }
    );

    stabilityCheckCount = RadarConfig::loadConfigParam<int>(
        configMap, "stability.check.count", DEFAULT_STABILITY_CHECKS,
        "Count must be positive", [](int v) { return v > 0; }
    );

    watcherCheckInterval = RadarConfig::loadConfigParam<int>(
        configMap, "watcher.check.interval.seconds", DEFAULT_WATCHER_INTERVAL_S,
        "Interval must be positive", [](int v) { return v > 0; }
    );

    pendingCheckInterval = RadarConfig::loadConfigParam<int>(
        configMap, "pending.check.interval.seconds", DEFAULT_PENDING_INTERVAL_S,
        "Interval must be positive", [](int v) { return v > 0; }
    );

    cbFailureThreshold = RadarConfig::loadConfigParam<int>(
        configMap, "circuitbreaker.failure.threshold", DEFAULT_CB_THRESHOLD,
        "Threshold must be positive", [](int v) { return v > 0; }
    );

    cbResetTimeoutSeconds = RadarConfig::loadConfigParam<int>(
        configMap, "circuitbreaker.reset.timeout.seconds", DEFAULT_CB_TIMEOUT_S,
        "Timeout must be positive", [](int v) { return v > 0; }
    );

    memoryUsagePercentLimit = RadarConfig::loadConfigParam<int>(
        configMap, "memory.usage.percent.limit", DEFAULT_MEM_USAGE_PERCENT,
        "Percentage must be between 0 and 100", [](int v) { return v >= 0 && v <= 100; }
    );

    logFilePath = RadarConfig::loadConfigParam<std::string>(
        configMap, "log.filepath", DEFAULT_LOG_FILEPATH,
        "Invalid log file path string", [](const std::string& s) { return !s.empty(); }
    );

    dbFilePath = RadarConfig::loadConfigParam<std::string>(
        configMap, "db.filepath", DEFAULT_DB_FILEPATH,
        "Invalid DB file path string", [](const std::string& s) { return !s.empty(); }
    );

    logMaxSizeMB = RadarConfig::loadConfigParam<int>(
        configMap, "log.max_size_mb", DEFAULT_LOG_MAX_SIZE_MB,
        "Max log size must be non-negative", [](int v) { return v >= 0; }
    );

    logBackupCount = RadarConfig::loadConfigParam<int>(
        configMap, "log.backup_count", DEFAULT_LOG_BACKUP_COUNT,
        "Backup count must be non-negative", [](int v) { return v >= 0; }
    );

    poolSize = RadarConfig::loadConfigParam<int>(
        configMap, "pool.size", DEFAULT_POOL_SIZE,
        "Pool size must be positive", [](int v) { return v > 0; }
    );

    maxRetryAbandon = RadarConfig::loadConfigParam<int>(
        configMap, "max.retries.abandon", DEFAULT_MAX_RETRY_ABANDON,
        "Total number of retries must be positive",
        [](int retries) { return retries > 0; }
    );

    retryJitterMaxMillis = RadarConfig::loadConfigParam<int>(
        configMap, "retry.jitter.max.ms", DEFAULT_RETRY_JITTER_MAX_MS,
        "Retry jitter max must be non-negative",
        [](int jitter) { return jitter >= 0; }
    );

    udtRequireGreeting = RadarConfig::loadConfigParam<bool>(
        configMap, "udt.require.greeting", DEFAULT_UDT_REQUIRE_GREETING,
        "UDT require greeting must be true or false",
        [](bool) { return true; }
    );

    udtExpectedGreeting = RadarConfig::loadConfigParam<std::string>(
        configMap, "udt.expected.greeting", DEFAULT_UDT_EXPECTED_GREETING,
        "UDT expected greeting string invalid",
        [](const std::string&) { return true; }
    );

    udtGreetingTimeoutMs = RadarConfig::loadConfigParam<int>(
        configMap, "udt.greeting.timeout.ms", DEFAULT_UDT_GREETING_TIMEOUT_MS,
        "UDT greeting timeout must be non-negative",
        [](int timeout) { return timeout >= 0; }
    );

    udtKeepAliveEnabled = RadarConfig::loadConfigParam<bool>(
        configMap, "udt.keepalive.enabled", DEFAULT_UDT_KEEPALIVE_ENABLED,
        "UDT keepalive enabled must be true or false",
        [](bool) { return true; }
    );

    udtKeepAliveIntervalSeconds = RadarConfig::loadConfigParam<int>(
        configMap, "udt.keepalive.interval.seconds", DEFAULT_UDT_KEEPALIVE_INTERVAL_SECONDS,
        "UDT keepalive interval must be non-negative",
        [](int interval) { return interval >= 0; }
    );

    udtKeepAlivePayload = RadarConfig::loadConfigParam<std::string>(
        configMap, "udt.keepalive.payload", DEFAULT_UDT_KEEPALIVE_PAYLOAD,
        "UDT keepalive payload invalid",
        [](const std::string&) { return true; }
    );

    securityEnabled = RadarConfig::loadConfigParam<bool>(
        configMap, "security.enabled", DEFAULT_SECURITY_ENABLED,
        "Security enabled must be true or false",
        [](bool) { return true; }
    );

    securityHandshakeEnabled = RadarConfig::loadConfigParam<bool>(
        configMap, "security.handshake.enabled", DEFAULT_SECURITY_HANDSHAKE_ENABLED,
        "Security handshake enabled must be true or false",
        [](bool) { return true; }
    );

    allowInsecureMode = RadarConfig::loadConfigParam<bool>(
        configMap, "security.allow_insecure", DEFAULT_ALLOW_INSECURE_MODE,
        "Security allow_insecure must be true or false",
        [](bool) { return true; }
    );

    securityPreSharedKey = RadarConfig::loadConfigParam<std::string>(
        configMap, "security.psk", DEFAULT_SECURITY_PSK,
        "Security pre-shared key invalid",
        [](const std::string&) { return true; }
    );

    securityClientId = RadarConfig::loadConfigParam<std::string>(
        configMap, "security.client_id", "agent-default",
        "Security client id invalid",
        [](const std::string& value) {
            return !value.empty() && value.size() <= 128 &&
                   value.find_first_of(" \t\r\n") == std::string::npos;
        }
    );

    securityIdentityMode = RadarConfig::loadConfigParam<std::string>(
        configMap, "security.identity.mode", DEFAULT_SECURITY_IDENTITY_MODE,
        "Security identity mode invalid",
        [](const std::string& value) { return isValidIdentityMode(value); }
    );

    securityClientPrivateKeyPath = RadarConfig::loadConfigParam<std::string>(
        configMap, "security.client_private_key_path", "",
        "Security client private key path invalid",
        [](const std::string&) { return true; }
    );

    securityServerPublicKeyPath = RadarConfig::loadConfigParam<std::string>(
        configMap, "security.server_public_key_path", "",
        "Security server public key path invalid",
        [](const std::string&) { return true; }
    );

    securityClientCertificatePath = RadarConfig::loadConfigParam<std::string>(
        configMap, "security.client_certificate_path", "",
        "Security client certificate path invalid",
        [](const std::string&) { return true; }
    );

    securityCaBundlePath = RadarConfig::loadConfigParam<std::string>(
        configMap, "security.ca_bundle_path", "",
        "Security CA bundle path invalid",
        [](const std::string&) { return true; }
    );

    securityServerIdentity = RadarConfig::loadConfigParam<std::string>(
        configMap, "security.server_identity", "",
        "Security server identity invalid",
        [](const std::string& value) {
            return value.size() <= 253 &&
                   value.find_first_of(" \t\r\n") == std::string::npos;
        }
    );

    dashboardEnabled = RadarConfig::loadConfigParam<bool>(
        configMap, "dashboard.enabled", DEFAULT_DASHBOARD_ENABLED,
        "Dashboard enabled must be true or false",
        [](bool) { return true; }
    );

    dashboardUrl = RadarConfig::loadConfigParam<std::string>(
        configMap, "dashboard.url", "",
        "Dashboard URL invalid",
        [](const std::string& value) {
            return value.empty() || value.rfind("https://", 0) == 0;
        }
    );

    dashboardHeartbeatIntervalSeconds = RadarConfig::loadConfigParam<int>(
        configMap, "dashboard.heartbeat.interval.seconds",
        DEFAULT_DASHBOARD_HEARTBEAT_INTERVAL_SECONDS,
        "Dashboard heartbeat interval must be positive",
        [](int value) { return value > 0; }
    );

    dashboardLogTailLines = RadarConfig::loadConfigParam<int>(
        configMap, "dashboard.log_tail.lines", DEFAULT_DASHBOARD_LOG_TAIL_LINES,
        "Dashboard log tail lines must be non-negative",
        [](int value) { return value >= 0 && value <= 5000; }
    );

    changedFilesResendEnabled = RadarConfig::loadConfigParam<bool>(
        configMap, "resend.changed_files.enabled",
        DEFAULT_CHANGED_FILES_RESEND_ENABLED,
        "Changed-file resend enabled must be true or false",
        [](bool) { return true; }
    );

    changedFilesIdentity = RadarConfig::loadConfigParam<std::string>(
        configMap, "resend.changed_files.identity",
        DEFAULT_CHANGED_FILES_IDENTITY,
        "Changed-file identity must be sha256",
        [](const std::string& value) { return isValidChangedFilesIdentity(value); }
    );
    changedFilesIdentity = normalizeLower(changedFilesIdentity);

    const std::string envPsk = getEnvValue("AGENT_SECURITY_PSK");
    if (!envPsk.empty()) {
        securityPreSharedKey = envPsk;
    } else {
        const std::string sharedEnvPsk = getEnvValue("TRANSFERUDT_SECURITY_PSK");
        if (!sharedEnvPsk.empty()) {
            securityPreSharedKey = sharedEnvPsk;
        }
    }

    if ((!securityEnabled || !securityHandshakeEnabled) && !allowInsecureMode) {
        throw std::runtime_error(
            "security.enabled and security.handshake.enabled must both be true unless security.allow_insecure=true is explicitly configured.");
    }

    if (securityIdentityMode == "signed_handshake") {
        if (!securityEnabled || !securityHandshakeEnabled) {
            throw std::runtime_error(
                "signed_handshake requires security.enabled=true and security.handshake.enabled=true.");
        }
        if (securityClientPrivateKeyPath.empty() || securityServerPublicKeyPath.empty()) {
            throw std::runtime_error(
                "signed_handshake requires security.client_private_key_path and security.server_public_key_path.");
        }
    }

    if (securityIdentityMode == "certificate_handshake") {
        if (!securityEnabled || !securityHandshakeEnabled) {
            throw std::runtime_error(
                "certificate_handshake requires security.enabled=true and security.handshake.enabled=true.");
        }
        if (securityClientPrivateKeyPath.empty() ||
            securityClientCertificatePath.empty() || securityCaBundlePath.empty()) {
            throw std::runtime_error(
                "certificate_handshake requires security.client_private_key_path, security.client_certificate_path, and security.ca_bundle_path.");
        }
    }

    if (dashboardEnabled) {
        if (dashboardUrl.empty() || dashboardUrl.rfind("https://", 0) != 0) {
            throw std::runtime_error(
                "dashboard.enabled=true requires dashboard.url to start with https://.");
        }
        if (securityClientPrivateKeyPath.empty()) {
            throw std::runtime_error(
                "dashboard.enabled=true requires security.client_private_key_path for signed heartbeats.");
        }
    }

    if (securityIdentityMode == "psk" &&
        (securityEnabled || securityHandshakeEnabled) &&
        (securityPreSharedKey.size() < 32 || isPlaceholderPsk(securityPreSharedKey))) {
        throw std::runtime_error(
            "security.psk must contain a non-placeholder secret with at least 32 characters when security is enabled.");
    }

    logFlushLevel = RadarConfig::loadConfigParam<std::string>(
        configMap, "log.flush_level", "warn", // Default value is "warn"
        "Invalid flush level string", [](const std::string& s) { return !s.empty(); }
    );

    std::string exclude_files_str = RadarConfig::loadConfigParam<std::string>(
        configMap, "watcher.exclude.files", "agent.log", // Default exclusion list
        "Invalid exclusion string", [](const std::string&) { return true; }
    );
    watcherExcludeFiles = split(exclude_files_str, ',');

    maxFileProcessingAttempts = RadarConfig::loadConfigParam<int>(
        configMap, "max.file.processing.attempts", MAX_FILE_PROCESSING_ATTEMPTS,
        "Total number of retries must be positive",
        [](int retries) { return retries > 0; }
    );
}

// Trivial destructor kept for explicit singleton lifecycle control.
RadarConfig::~RadarConfig() {
}

// Returns the singleton RadarConfig instance.
RadarConfig& RadarConfig::getInstance() {
    static RadarConfig instance;
    return instance;
}

