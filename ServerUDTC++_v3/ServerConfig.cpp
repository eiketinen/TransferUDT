#include "ServerConfig.h"
#include <cstring>
#include <limits>
constexpr const char* CONFIG_ENV_VAR = "SERVER_CONFIG_PATH";

static std::string getConfigFilePathFromEnv() {
    char* buffer = nullptr;
    size_t size = 0;
    if (_dupenv_s(&buffer, &size, "SERVER_CONFIG_PATH") == 0 && buffer != nullptr) {
        std::string result(buffer);
        free(buffer); // necessário liberar a memória alocada por _dupenv_s
        return result;
    }
    return "config.properties"; // valor padrão
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

static bool isValidClientPskId(const std::string& value) {
    return !value.empty() && value.size() <= 128 &&
           value.find_first_of(" \t\r\n") == std::string::npos;
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

static bool isValidChangedFilesServerPolicy(const std::string& value) {
    const std::string normalized = normalizeLower(value);
    return normalized == "overwrite" || normalized == "reject" ||
           normalized == "versioned";
}

static ServerConfig::ChangedFilesServerPolicy
parseChangedFilesServerPolicy(const std::string& value) {
    const std::string normalized = normalizeLower(value);
    if (normalized == "reject") {
        return ServerConfig::ChangedFilesServerPolicy::Reject;
    }
    if (normalized == "versioned") {
        return ServerConfig::ChangedFilesServerPolicy::Versioned;
    }
    return ServerConfig::ChangedFilesServerPolicy::Overwrite;
}

static uint64_t megabytesToBytes(int64_t megabytes, const char* key) {
    constexpr uint64_t kBytesPerMegabyte = 1024ULL * 1024ULL;
    if (megabytes <= 0) {
        throw std::runtime_error(std::string(key) + " must be positive.");
    }
    const auto maxMb =
        static_cast<uint64_t>((std::numeric_limits<uint64_t>::max)() /
                              kBytesPerMegabyte);
    if (static_cast<uint64_t>(megabytes) > maxMb) {
        throw std::runtime_error(std::string(key) + " is too large.");
    }
    return static_cast<uint64_t>(megabytes) * kBytesPerMegabyte;
}

static std::vector<int> parseNonNegativeIntList(const std::string& raw,
                                                const char* key) {
    std::vector<int> values;
    std::stringstream stream(raw);
    std::string item;
    while (std::getline(stream, item, ',')) {
        const auto start = item.find_first_not_of(" \t\r\n");
        const auto end = item.find_last_not_of(" \t\r\n");
        item = (start == std::string::npos) ? "" : item.substr(start, end - start + 1);
        if (item.empty()) {
            continue;
        }
        try {
            const long value = std::stol(item);
            if (value < 0 || value > (std::numeric_limits<int>::max)()) {
                throw std::out_of_range(key);
            }
            values.push_back(static_cast<int>(value));
        } catch (const std::exception&) {
            throw std::runtime_error(std::string(key) +
                                     " must contain comma-separated "
                                     "non-negative integer milliseconds.");
        }
    }
    return values;
}

ServerConfig::ServerConfig() {
    auto configMap = ServerConfig::loadConfigFile(getConfigFilePathFromEnv());

    // Carrega os parâmetros de configuração com validação

    serverPort = ServerConfig::loadConfigParam<int>(
        configMap, "server.port", DEFAULT_LISTEN_PORT,
        "Port outside valid range (1-65535)",
        [](int port) { return port > 0 && port <= 65535; }
    );

    bindAddress = ServerConfig::loadConfigParam<std::string>(
        configMap, "server.bind_address", DEFAULT_BIND_ADDRESS,
        "Invalid bind address string",
        [](const std::string& address) { return !address.empty(); }
    );

    dataDir = ServerConfig::loadConfigParam<std::string>(
        configMap, "server.storage_path", DEFAULT_STORAGE_PATH,
        "Invalid directory",
        [](const std::string& dir) {
            if (!fs::exists(dir)) {
                try {
                    return fs::create_directories(dir);
                }
                catch (const std::exception& e) {
                    std::cerr << "Error creating directory '" << dir << "': " << e.what() << std::endl;
                    return false;
                }
            }
            return true;
        }
    );

    reconstructedDir = ServerConfig::loadConfigParam<std::string>(
        configMap, "server.reconstructed_path", DEFAULT_RECONSTRUCTED_PATH,
        "Invalid directory",
        [](const std::string& dir) {
            if (!fs::exists(dir)) {
                try {
                    return fs::create_directories(dir);
                }
                catch (const std::exception& e) {
                    std::cerr << "Error creating directory '" << dir << "': " << e.what() << std::endl;
                    return false;
                }
            }
            return true;
        }
    );

    const int64_t maxFileSizeMb = ServerConfig::loadConfigParam<int64_t>(
        configMap, "server.max_file_size_mb", DEFAULT_MAX_FILE_SIZE_MB,
        "Maximum file size must be positive",
        [](int64_t value) { return value > 0; }
    );
    maxFileSizeBytes =
        megabytesToBytes(maxFileSizeMb, "server.max_file_size_mb");

    const int64_t maxClientStorageMb = ServerConfig::loadConfigParam<int64_t>(
        configMap, "server.max_client_storage_mb",
        DEFAULT_MAX_CLIENT_STORAGE_MB,
        "Maximum client storage must be positive",
        [](int64_t value) { return value > 0; }
    );
    maxClientStorageBytes =
        megabytesToBytes(maxClientStorageMb, "server.max_client_storage_mb");
	
    maxConnection = ServerConfig::loadConfigParam<int>(
		configMap, "max.connection", DEFAULT_MAX_CONNECTION,
		"Max connection must be positive",
		[](int conn) { return conn > 0; }
	);  
    
    numThreads = ServerConfig::loadConfigParam<int>(
        configMap, "work.threads", DEFAULT_WORKER_THREADS,
        "Number of threads must be positive",
        [](int threads) { return threads > 0; }
    );

    const int64_t maxPendingTasksRaw = ServerConfig::loadConfigParam<int64_t>(
        configMap, "work.max_pending_tasks", DEFAULT_MAX_PENDING_TASKS,
        "Maximum pending tasks must be positive",
        [](int64_t value) { return value > 0; }
    );
    if (static_cast<uint64_t>(maxPendingTasksRaw) >
        (std::numeric_limits<size_t>::max)()) {
        throw std::runtime_error("work.max_pending_tasks is too large.");
    }
    maxPendingTasks = static_cast<size_t>(maxPendingTasksRaw);

    cbFailureThreshold = ServerConfig::loadConfigParam<int>(
        configMap, "circuitbreaker.failure.threshold", DEFAULT_CB_THRESHOLD,
        "Threshold must be positive", [](int v) { return v > 0; }
    );

    cbResetTimeoutSeconds = ServerConfig::loadConfigParam<int>(
        configMap, "circuitbreaker.reset.timeout.seconds", DEFAULT_CB_TIMEOUT_S,
        "Timeout must be positive", [](int v) { return v > 0; }
    );

    logFilePath = ServerConfig::loadConfigParam<std::string>(
        configMap, "log.filepath", DEFAULT_LOG_FILEPATH,
        "Invalid log file path string", [](const std::string& s) { return !s.empty(); }
    );

    dbFilePath = ServerConfig::loadConfigParam<std::string>(
        configMap, "db.filepath", DEFAULT_DB_FILEPATH,
        "Invalid DB file path string", [](const std::string& s) { return !s.empty(); }
    );

    logMaxSizeMB = ServerConfig::loadConfigParam<int>(
        configMap, "log.max_size_mb", DEFAULT_LOG_MAX_SIZE_MB,
        "Max log size must be non-negative", [](int v) { return v >= 0; }
    );

    logBackupCount = ServerConfig::loadConfigParam<int>(
        configMap, "log.backup_count", DEFAULT_LOG_BACKUP_COUNT,
        "Backup count must be non-negative", [](int v) { return v >= 0; }
    );

    // Conversão dos buffers de MB para bytes
    int64_t sndBufMB = ServerConfig::loadConfigParam<int64_t>(
        configMap, "snd.buf", DEFAULT_SND_BUF, // 4 MB
        "Buffer size must be positive",
        [](int64_t buf) { return buf > 0; }
    );
    sendBuffer = static_cast<DWORDLONG>(sndBufMB) * 1024 * 1024;

    int64_t rcvBufMB = ServerConfig::loadConfigParam<int64_t>(
        configMap, "rcv.buf", DEFAULT_RCV_BUF, // 4 MB
        "Buffer size must be positive",
        [](int64_t buf) { return buf > 0; }
    );
    receiveBuffer = static_cast<DWORDLONG>(rcvBufMB) * 1024 * 1024;
    
    // Conversão da largura de banda de MB/s para bytes/s
    int64_t bandMB = ServerConfig::loadConfigParam<int64_t>(
        configMap, "max.band", DEFAULT_MAX_BAND, // 10 MB/s
        "Bandwidth must be positive",
        [](int64_t band) { return band > 0; }
    );
    maxBandwidth = static_cast<DWORDLONG>(bandMB) * 1024 * 1024;

    segmentSize = ServerConfig::loadConfigParam<int>(
        configMap, "seg.len", DEFAULT_SEG_LEN,
        "Segment size must be positive",
        [](int len) { return len > 0; }
    );
    // Conversão dos timeouts de segundos para milissegundos
    int sndTimeoutSec = ServerConfig::loadConfigParam<int>(
        configMap, "snd.timeout", DEFAULT_SND_TIMEOUT, // 20 seconds
        "Timeout must be positive",
        [](int timeout) { return timeout > 0; }
    );
    sendTimeout = sndTimeoutSec * 1000;

    int rcvTimeoutSec = ServerConfig::loadConfigParam<int>(
        configMap, "rcv.timeout", DEFAULT_RCV_TIMEOUT, // 20 seconds
        "Timeout must be positive",
        [](int timeout) { return timeout > 0; }
    );
    receiveTimeout = rcvTimeoutSec * 1000;

    lingerOnOff = ServerConfig::loadConfigParam<int>(
		configMap, "ling.on", DEFAULT_LINGER_ONOFF, // 0 - off 1 - on
        "linger needs do 0 or 1",
        [](int linger) { return (linger < 0 || linger >= 2) ? false : true; }
    );

    lingerTime = ServerConfig::loadConfigParam<int>(
        configMap, "ling.time", DEFAULT_LINGER_TIME, // 5 s
        "linger must be positive",
        [](int linger) { return linger > 0; }
    );

    logFlushLevel = ServerConfig::loadConfigParam<std::string>(
        configMap, "log.flush_level", "warn", // Valor padrão é "warn"
        "Invalid flush level string", [](const std::string& s) { return !s.empty(); }
    );

    const std::string ackDelayPatternRaw =
        ServerConfig::loadConfigParam<std::string>(
            configMap, "test.ack_delay_pattern_ms", "",
            "Invalid ACK delay pattern",
            [](const std::string&) { return true; });
    simulatedAckDelayPatternMillis =
        parseNonNegativeIntList(ackDelayPatternRaw,
                                "test.ack_delay_pattern_ms");

    const std::string changedFilesServerPolicyRaw =
        ServerConfig::loadConfigParam<std::string>(
            configMap, "resend.changed_files.server_policy",
            DEFAULT_CHANGED_FILES_SERVER_POLICY,
            "Changed-file server policy must be overwrite, reject, or versioned",
            [](const std::string& value) {
                return isValidChangedFilesServerPolicy(value);
            });
    changedFilesServerPolicy =
        parseChangedFilesServerPolicy(changedFilesServerPolicyRaw);

    securityEnabled = ServerConfig::loadConfigParam<bool>(
        configMap, "security.enabled", DEFAULT_SECURITY_ENABLED,
        "Security enabled must be true or false",
        [](bool) { return true; }
    );

    securityHandshakeEnabled = ServerConfig::loadConfigParam<bool>(
        configMap, "security.handshake.enabled", DEFAULT_SECURITY_HANDSHAKE_ENABLED,
        "Security handshake enabled must be true or false",
        [](bool) { return true; }
    );

    allowInsecureMode = ServerConfig::loadConfigParam<bool>(
        configMap, "security.allow_insecure", DEFAULT_ALLOW_INSECURE_MODE,
        "Security allow_insecure must be true or false",
        [](bool) { return true; }
    );

    securityPreSharedKey = ServerConfig::loadConfigParam<std::string>(
        configMap, "security.psk", DEFAULT_SECURITY_PSK,
        "Security pre-shared key invalid",
        [](const std::string&) { return true; }
    );

    const std::string envPsk = getEnvValue("SERVER_SECURITY_PSK");
    if (!envPsk.empty()) {
        securityPreSharedKey = envPsk;
    } else {
        const std::string sharedEnvPsk = getEnvValue("TRANSFERUDT_SECURITY_PSK");
        if (!sharedEnvPsk.empty()) {
            securityPreSharedKey = sharedEnvPsk;
        }
    }

    securityIdentityMode = ServerConfig::loadConfigParam<std::string>(
        configMap, "security.identity.mode", DEFAULT_SECURITY_IDENTITY_MODE,
        "Security identity mode invalid",
        [](const std::string& value) { return isValidIdentityMode(value); }
    );

    securityServerPrivateKeyPath = ServerConfig::loadConfigParam<std::string>(
        configMap, "security.server_private_key_path", "",
        "Security server private key path invalid",
        [](const std::string&) { return true; }
    );

    securityServerCertificatePath = ServerConfig::loadConfigParam<std::string>(
        configMap, "security.server_certificate_path", "",
        "Security server certificate path invalid",
        [](const std::string&) { return true; }
    );

    securityCaBundlePath = ServerConfig::loadConfigParam<std::string>(
        configMap, "security.ca_bundle_path", "",
        "Security CA bundle path invalid",
        [](const std::string&) { return true; }
    );

    static constexpr const char* CLIENT_PSK_PREFIX = "security.client_psk.";
    static constexpr const char* CLIENT_PUBLIC_KEY_PREFIX = "security.client_public_key.";
    for (const auto& entry : configMap) {
        const std::string& key = entry.first;
        if (key.rfind(CLIENT_PSK_PREFIX, 0) == 0) {
            const std::string clientId = key.substr(std::strlen(CLIENT_PSK_PREFIX));
            if (!isValidClientPskId(clientId)) {
                throw std::runtime_error(
                    "security.client_psk entries must use a non-empty client id without whitespace.");
            }
            clientPreSharedKeys[clientId] = entry.second;
        } else if (key.rfind(CLIENT_PUBLIC_KEY_PREFIX, 0) == 0) {
            const std::string clientId = key.substr(std::strlen(CLIENT_PUBLIC_KEY_PREFIX));
            if (!isValidClientPskId(clientId)) {
                throw std::runtime_error(
                    "security.client_public_key entries must use a non-empty client id without whitespace.");
            }
            clientPublicKeyPaths[clientId] = entry.second;
        }
    }

    const std::string allowedClientsRaw = ServerConfig::loadConfigParam<std::string>(
        configMap, "server.allowed_clients", "",
        "Invalid allowed clients list",
        [](const std::string&) { return true; }
    );
    allowedClients = splitList(allowedClientsRaw);

    const std::string allowedClientIdsRaw = ServerConfig::loadConfigParam<std::string>(
        configMap, "security.allowed_client_ids", "",
        "Invalid allowed client ids list",
        [](const std::string&) { return true; }
    );
    allowedClientIds = splitList(allowedClientIdsRaw);

    if ((!securityEnabled || !securityHandshakeEnabled) && !allowInsecureMode) {
        throw std::runtime_error(
            "security.enabled and security.handshake.enabled must both be true unless security.allow_insecure=true is explicitly configured.");
    }

    for (const auto& entry : clientPreSharedKeys) {
        if (entry.second.size() < 32 || isPlaceholderPsk(entry.second)) {
            throw std::runtime_error(
                "security.client_psk.<client_id> values must contain non-placeholder secrets with at least 32 characters.");
        }
    }

    if (securityIdentityMode == "signed_handshake") {
        if (!securityEnabled || !securityHandshakeEnabled) {
            throw std::runtime_error(
                "signed_handshake requires security.enabled=true and security.handshake.enabled=true.");
        }
        if (securityServerPrivateKeyPath.empty() || clientPublicKeyPaths.empty()) {
            throw std::runtime_error(
                "signed_handshake requires security.server_private_key_path and at least one security.client_public_key.<client_id>.");
        }
    }

    if (securityIdentityMode == "certificate_handshake") {
        if (!securityEnabled || !securityHandshakeEnabled) {
            throw std::runtime_error(
                "certificate_handshake requires security.enabled=true and security.handshake.enabled=true.");
        }
        if (securityServerPrivateKeyPath.empty() ||
            securityServerCertificatePath.empty() || securityCaBundlePath.empty()) {
            throw std::runtime_error(
                "certificate_handshake requires security.server_private_key_path, security.server_certificate_path, and security.ca_bundle_path.");
        }
    }

    const bool hasGlobalPsk =
        securityPreSharedKey.size() >= 32 && !isPlaceholderPsk(securityPreSharedKey);

    if (securityIdentityMode == "psk" &&
        (securityEnabled || securityHandshakeEnabled) &&
        !hasGlobalPsk && clientPreSharedKeys.empty()) {
        throw std::runtime_error(
            "security.psk or at least one security.client_psk.<client_id> must contain a non-placeholder secret with at least 32 characters when security is enabled.");
    }

    if (securityIdentityMode == "psk" &&
        securityEnabled && !securityHandshakeEnabled && !hasGlobalPsk) {
        throw std::runtime_error(
            "security.psk is required when packet encryption is enabled without the authentication handshake.");
    }
}

ServerConfig::~ServerConfig() {
}

ServerConfig& ServerConfig::getInstance() {
    static ServerConfig instance;
    return instance;
}

std::string ServerConfig::getChangedFilesServerPolicyName() const {
    switch (changedFilesServerPolicy) {
    case ChangedFilesServerPolicy::Reject:
        return "reject";
    case ChangedFilesServerPolicy::Versioned:
        return "versioned";
    case ChangedFilesServerPolicy::Overwrite:
    default:
        return "overwrite";
    }
}

