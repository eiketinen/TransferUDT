#include "Database.h"
#include "ServerConfig.h"
#include <filesystem>
#include <mutex>

std::unique_ptr<Database> Database::instance = nullptr;
std::once_flag Database::initFlag;
thread_local bool Database::m_isTransactionActive = false;

namespace {
void ignoreDuplicateColumn(sqlite3 *db, const std::string &sql,
                           const std::string &columnName) {
  char *errMsg = nullptr;
  if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &errMsg) != SQLITE_OK) {
    const std::string error = errMsg ? errMsg : "Unknown error";
    if (error.find("duplicate column name") == std::string::npos) {
      Logger::getInstance().error(
          "Database::Database",
          "Failed to add " + columnName + " column: " + error);
    }
    if (errMsg) {
      sqlite3_free(errMsg);
    }
  }
}

void rollbackNoThrow(std::unique_ptr<TransactionGuard> &tx,
                     const char *context) {
  if (!tx) {
    return;
  }

  try {
    tx->rollback();
  } catch (const std::exception &ex) {
    Logger::getInstance().error(
        "Database", std::string(context) + ": rollback failed: " + ex.what());
  } catch (...) {
    Logger::getInstance().error("Database",
                                std::string(context) +
                                    ": rollback failed with unknown error.");
  }
}

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
} // namespace
/**
 * @brief TransactionGuard constructor: begins a transaction
 * @param conn Shared pointer to the SQLite connection
 */
TransactionGuard::TransactionGuard(std::shared_ptr<SQLiteConnection> conn)
    : m_conn(conn), m_committed(false) {
  SQLiteStatement stmt(m_conn->get(), "BEGIN TRANSACTION;");
  stmt.step();
}
/**
 * @brief TransactionGuard destructor: rolls back the transaction if not
 * committed
 */
TransactionGuard::~TransactionGuard() noexcept {
  if (!m_committed) {
    try {
      rollback();
    } catch (const std::exception &ex) {
      Logger::getInstance().error(
          "Database", "Rollback in TransactionGuard destructor failed: " +
                          std::string(ex.what()));
    } catch (...) {
      Logger::getInstance().error(
          "Database",
          "Rollback in TransactionGuard destructor failed with unknown error.");
    }
  }
}
/**
 * @brief Commits the active transaction
 */
void TransactionGuard::commit() {
  if (m_committed) {
    return;
  }
  SQLiteStatement stmt(m_conn->get(), "COMMIT;");
  stmt.step();
  m_committed = true;
}
/**
 * @brief Rolls back the active transaction
 */
void TransactionGuard::rollback() {
  if (m_committed) {
    return;
  }
  SQLiteStatement stmt(m_conn->get(), "ROLLBACK;");
  stmt.step();
  m_committed = true;
}
/**
 * @brief Constructs a DatabaseException and logs it
 * @param message Error message
 */
DatabaseException::DatabaseException(const std::string &message)
    : std::runtime_error(message), m_errorCode(0) {
  Logger::getInstance().error("DatabaseException", message);
}
/**
 * @brief Constructs a DatabaseException with error code and logs it
 * @param message Error message
 * @param errorCode Error code associated with the exception
 */
DatabaseException::DatabaseException(const std::string &message, int errorCode)
    : std::runtime_error(message), m_errorCode(errorCode) {
  Logger::getInstance().error("DatabaseException",
                              message +
                                  " Error code: " + std::to_string(errorCode));
}

/**
 * @brief Retrieves the error code
 * @return Integer representing the error code
 */
int DatabaseException::getErrorCode() const { return m_errorCode; }
/**
 * @brief Opens a connection to the SQLite database
 * @param dbPath Path to the SQLite database file
 * @throws DatabaseException if connection fails
 */
SQLiteConnection::SQLiteConnection(const std::string &dbPath) : m_db(nullptr) {
#ifdef _WIN32
  std::error_code ec;
  fs::path sqlitePath = fs::path(dbPath);
  if (!sqlitePath.is_absolute()) {
    sqlitePath = fs::absolute(sqlitePath, ec);
    if (ec) {
      sqlitePath = fs::path(dbPath);
    }
  }

  std::wstring widePath = sqlitePath.wstring();
  if (widePath.rfind(L"\\\\?\\", 0) != 0) {
    if (widePath.rfind(L"\\\\", 0) == 0) {
      widePath = L"\\\\?\\UNC\\" + widePath.substr(2);
    } else {
      widePath = L"\\\\?\\" + widePath;
    }
  }

  if (sqlite3_open16(widePath.c_str(), &m_db) != SQLITE_OK)
#else
  if (sqlite3_open(dbPath.c_str(), &m_db) != SQLITE_OK)
#endif
  {
    std::string errorMsg = sqlite3_errmsg(m_db);
    sqlite3_close(m_db);
    m_db = nullptr;
    throw DatabaseException("Failed to open database: " + errorMsg);
  }

  sqlite3_busy_timeout(m_db, 5000);
}
/**
 * @brief Closes the SQLite database connection
 */
SQLiteConnection::~SQLiteConnection() {
  if (m_db) {
    sqlite3_close(m_db);
    m_db = nullptr;
  }
}
/**
 * @brief Returns the native SQLite database handle
 * @return sqlite3 pointer
 */
sqlite3 *SQLiteConnection::get() const { return m_db; }

/**
 * @brief Prepares an SQL statement for execution
 * @param db SQLite database handle
 * @param sql SQL statement to prepare
 * @throws DatabaseException if preparation fails
 */
SQLiteStatement::SQLiteStatement(sqlite3 *db, const std::string &sql)
    : m_stmt(nullptr) {
  if (sqlite3_prepare_v2(db, sql.c_str(), -1, &m_stmt, nullptr) != SQLITE_OK) {
    std::string errorMsg = sqlite3_errmsg(db);
    throw DatabaseException("Failed to prepare statement: " + errorMsg,
                            sqlite3_errcode(db));
  }
}
/**
 * @brief Finalizes the prepared statement
 */
SQLiteStatement::~SQLiteStatement() {
  if (m_stmt) {
    sqlite3_finalize(m_stmt);
    m_stmt = nullptr;
  }
}
/**
 * @brief Binds a text parameter to the prepared statement
 * @param index Index of the parameter (1-based)
 * @param text Text value to bind
 * @throws DatabaseException if binding fails
 */
void SQLiteStatement::bindText(int index, const std::string &text) {
  if (sqlite3_bind_text(m_stmt, index, text.c_str(), -1, SQLITE_TRANSIENT) !=
      SQLITE_OK) {
    throw DatabaseException("Failed to bind text parameter at index " +
                            std::to_string(index));
  }
}
/**
 * Binds a Unicode text parameter to the prepared statement.
 *
 * @param index Index of the parameter (1-based)
 * @param text  Unicode text value to bind
 *
 * @throws DatabaseException if binding fails
 */
void SQLiteStatement::bindText16(int index, const std::wstring &text) {
  if (sqlite3_bind_text16(m_stmt, index, text.c_str(), -1, SQLITE_TRANSIENT) !=
      SQLITE_OK) {
    throw DatabaseException("Failed to bind text parameter at index " +
                            std::to_string(index));
  }
}
/**
 * @brief Binds an integer parameter to the prepared statement
 * @param index Index of the parameter (1-based)
 * @param value Integer value to bind
 * @throws DatabaseException if binding fails
 */
void SQLiteStatement::bindInt(int index, int value) {
  if (sqlite3_bind_int(m_stmt, index, value) != SQLITE_OK) {
    throw DatabaseException("Failed to bind int parameter at index " +
                            std::to_string(index));
  }
}
/**
 * @brief Executes the prepared statement
 * @return true if a row is returned, false if no more rows are available
 * @throws DatabaseException if execution fails
 */
bool SQLiteStatement::step() {
  int result = sqlite3_step(m_stmt);
  if (result == SQLITE_ROW || result == SQLITE_DONE) {
    return result == SQLITE_ROW;
  } else {
    sqlite3 *db = sqlite3_db_handle(m_stmt);
    const char *dbMessage = db ? sqlite3_errmsg(db) : "Unknown SQLite error";
    throw DatabaseException(
        std::string("Statement execution failed: ") + dbMessage, result);
  }
}
/**
 * @brief Retrieves a text value from the specified column
 * @param column Index of the column (0-based)
 * @return Text value from the column
 */
std::string SQLiteStatement::getText(int column) const {
  const char *text =
      reinterpret_cast<const char *>(sqlite3_column_text(m_stmt, column));
  return text ? std::string(text) : std::string();
}
/**
 * Retrieves a text value from the specified column as a wide string.
 *
 * @param column Index of the column (0-based)
 *
 * @return Text value from the column as a wide string
 */
std::wstring SQLiteStatement::getText16(int column) const {
  const wchar_t *text =
      reinterpret_cast<const wchar_t *>(sqlite3_column_text16(m_stmt, column));
  return text ? std::wstring(text) : std::wstring();
}
/**
 * @brief Retrieves an integer value from the specified column
 * @param column Index of the column (0-based)
 * @return Integer value from the column
 */
int SQLiteStatement::getInt(int column) const {
  return sqlite3_column_int(m_stmt, column);
}

/**
 * @brief Database constructor: initializes the database and creates necessary
 * tables
 * @param dbPath Path to the SQLite database file
 * @throws DatabaseException if initialization fails
 */
Database::Database(const std::string dbPath) : m_dbPath(dbPath) {
  static bool sqlite_configured = false;
  if (!sqlite_configured) {
    int sqlite_config_result = sqlite3_config(SQLITE_CONFIG_SERIALIZED);
    if (sqlite_config_result != SQLITE_OK) {
      Logger::getInstance().error(
          "Database", "Failed to configure SQLite threading mode! Code: " +
                          std::to_string(sqlite_config_result));
      throw DatabaseException("Failed to configure SQLite threading mode.");
    }
    sqlite_configured = true;
    Logger::getInstance().info(
        "Database", "SQLite threading mode configured to SERIALIZED.");
  }
  std::filesystem::path dbFilePath(m_dbPath);
  const std::filesystem::path dbParentPath = dbFilePath.parent_path();
  if (!dbParentPath.empty()) {
    std::error_code ec;
    const fs::path normalizedParentPath = normalizePathForFs(dbParentPath);
    if (std::filesystem::create_directories(normalizedParentPath, ec)) {
      Logger::getInstance().debug("Database", "Created directory: {}",
                                  dbParentPath.string());
    } else if (ec) {
      Logger::getInstance().error("Database",
                                  "Failed to create directory '{}': {}",
                                  dbParentPath.string(), ec.message());
      throw DatabaseException("Failed to create database directory: " +
                              ec.message());
    } else {
      Logger::getInstance().info("Database", "Directory: {} exists",
                                 dbParentPath.string());
    }
  }

  try {

    auto conn = getConnection();

    char *errMsg = nullptr;
    if (sqlite3_exec(conn->get(), "PRAGMA journal_mode=WAL;", nullptr, nullptr,
                     &errMsg) != SQLITE_OK) {
      std::string err = errMsg ? errMsg : "Unknown error";
      if (errMsg)
        sqlite3_free(errMsg);
      Logger::getInstance().error("Database::Database",
                                  "Failed to set WAL mode: " + err);
    }

    if (sqlite3_exec(conn->get(), "PRAGMA synchronous=FULL;", nullptr, nullptr,
                     &errMsg) != SQLITE_OK) {
      std::string err = errMsg ? errMsg : "Unknown error";
      if (errMsg)
        sqlite3_free(errMsg);
      Logger::getInstance().error("Database::Database",
                                  "Failed to set synchronous=FULL: " + err);
    }

    TransactionGuard tx(conn);

    std::string sql = R"(
           CREATE TABLE IF NOT EXISTS chunks (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            client_address TEXT NOT NULL,
            filename TEXT NOT NULL,
            chunk_number INTEGER NOT NULL,
            total_chunk INTEGER DEFAULT 0,
            hash TEXT NOT NULL,
            file_path TEXT NOT NULL,
            status TEXT CHECK(status IN ('pending', 'success', 'failed')) DEFAULT 'pending',
            retries INTEGER DEFAULT 0,
            transfer_id TEXT NOT NULL DEFAULT '',
            created_at DATETIME DEFAULT CURRENT_TIMESTAMP,
            UNIQUE (client_address, filename, chunk_number)
            );
        )";

    SQLiteStatement stmt(conn->get(), sql);
    stmt.step();

    ignoreDuplicateColumn(
        conn->get(),
        "ALTER TABLE chunks ADD COLUMN transfer_id TEXT NOT NULL DEFAULT '';",
        "chunks.transfer_id");

    std::string sqlIndexFileChunk = R"(
            CREATE UNIQUE INDEX IF NOT EXISTS idx_chunks_file_chunk ON chunks (client_address, filename, chunk_number);
        )";
    SQLiteStatement stmtIndexFileChunk(conn->get(), sqlIndexFileChunk);
    stmtIndexFileChunk.step();

    std::string sqlIndexStatusRetries = R"(
            CREATE INDEX IF NOT EXISTS idx_chunks_status_retries ON chunks (status, retries);
        )";
    SQLiteStatement stmtIndexStatusRetries(conn->get(), sqlIndexStatusRetries);
    stmtIndexStatusRetries.step();

    std::string sqlReconstructed = R"(
        CREATE TABLE IF NOT EXISTS reconstructed_files (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            client_address TEXT NOT NULL,
            filename TEXT NOT NULL,
            reconstructed_path TEXT NOT NULL,
            transfer_id TEXT NOT NULL DEFAULT '',
            created_at DATETIME DEFAULT CURRENT_TIMESTAMP,
            UNIQUE (client_address, filename)
        );
    )";
    SQLiteStatement stmtReconstructed(conn->get(), sqlReconstructed);
    stmtReconstructed.step();

    ignoreDuplicateColumn(
        conn->get(),
        "ALTER TABLE reconstructed_files ADD COLUMN transfer_id TEXT NOT NULL "
        "DEFAULT '';",
        "reconstructed_files.transfer_id");

    std::string sqlErrors = R"(
            CREATE TABLE IF NOT EXISTS error_logs (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                error_message TEXT NOT NULL,
                error_code INTEGER DEFAULT 0,
                severity TEXT DEFAULT 'ERROR',
                stack_trace TEXT,
                created_at DATETIME DEFAULT CURRENT_TIMESTAMP
            );
        )";

    SQLiteStatement stmtErrors(conn->get(), sqlErrors);
    stmtErrors.step();

    tx.commit();
    Logger::getInstance().info("Database",
                               "Database initialized successfully.");
  } catch (const std::exception &e) {
    throw DatabaseException("Failed to initialize database: " +
                            std::string(e.what()));
  } catch (...) {
    throw DatabaseException("Failed to initialize database: Unknown error");
  }
}
/**
 * @brief Database destructor: cleans up the database instance
 */
Database::~Database() {}

void Database::beginTransaction() {
  // Se uma transação já está ativa nesta thread, não faz nada para evitar
  // aninhamento problemático.
  if (m_isTransactionActive) {
    throw DatabaseException("Nested transactions are not supported.");
    return;
  }
  auto conn = getConnection();
  SQLiteStatement stmt(conn->get(), "BEGIN TRANSACTION;");
  stmt.step();
  m_isTransactionActive = true;
}

void Database::commitTransaction() {
  // Só commita se uma transação foi iniciada por esta classe
  if (!m_isTransactionActive) {
    return;
  }

  auto conn = getConnection();
  try {
    SQLiteStatement stmt(conn->get(), "COMMIT;");
    stmt.step();
    m_isTransactionActive = false;
  } catch (...) {
    // Avoid leaving this thread in a permanently "transaction active" state.
    m_isTransactionActive = false;
    throw;
  }
}

void Database::rollbackTransaction() {
  // Só faz rollback se uma transação foi iniciada por esta classe
  if (!m_isTransactionActive) {
    return;
  }

  auto conn = getConnection();
  try {
    SQLiteStatement stmt(conn->get(), "ROLLBACK;");
    stmt.step();
    m_isTransactionActive = false;
  } catch (...) {
    // Rollback itself can fail, but caller should still be able to continue
    // safely.
    m_isTransactionActive = false;
    throw;
  }
}
/**
 * @brief Retrieves the singleton instance of the Database class
 * @return Reference to the Database instance
 * @throws DatabaseException if the instance is not initialized
 */
Database &Database::getInstance() {
  if (!instance)
    throw DatabaseException("The database has not been initialized. Please "
                            "call initialize() before proceeding.");
  return *instance;
}
/**
 * @brief Initializes the singleton instance of the Database class
 * @param dbPath Path to the SQLite database file
 */
void Database::initialize(const std::string &dbPath) {
  std::string resolvedDbPath = dbPath;
  if (resolvedDbPath.empty()) {
    resolvedDbPath = ServerConfig::getInstance().getDBFilePath();
  }
  if (resolvedDbPath.empty()) {
    throw DatabaseException("Database path is empty.");
  }

  std::call_once(initFlag, [resolvedDbPath]() {
    instance = std::make_unique<Database>(resolvedDbPath);
  });

  if (instance && instance->m_dbPath != resolvedDbPath) {
    Logger::getInstance().warning("Database",
                                  "Database already initialized with '{}'. "
                                  "Ignoring reinitialize request for '{}'.",
                                  instance->m_dbPath, resolvedDbPath);
  }
}
/**
 * @brief Sets the database path
 * @param path Path to the SQLite database file
 */
std::shared_ptr<SQLiteConnection> Database::getConnection() {
  thread_local static std::shared_ptr<SQLiteConnection> thread_conn = nullptr;

  if (!thread_conn) {
    try {
      thread_conn = std::make_shared<SQLiteConnection>(m_dbPath);
    } catch (const DatabaseException &e) {
      Logger::getInstance().critical(
          "Database", "Failed to create thread-local DB connection: " +
                          std::string(e.what()));
      throw;
    }
  }
  return thread_conn;
}
/**
 * @brief Inserts a chunk into the database
 * @param filename Name of the file
 * @param chunkNumber Number of the chunk
 * @param totalChunk Total number of chunks
 * @param hash Hash of the chunk
 * @param filePath Path to the file
 */
void Database::insertChunk(const std::string &client,
                           const std::string &filename, int chunkNumber,
                           int totalChunk, const std::string &hash,
                           const fs::path &filePath,
                           const std::string &transferId) {
  auto conn = getConnection();
  std::unique_ptr<TransactionGuard> local_tx;
  if (!m_isTransactionActive) {
    local_tx = std::make_unique<TransactionGuard>(conn);
  }

  std::string sql =
      "INSERT INTO chunks (client_address, filename, chunk_number, "
      "total_chunk, hash, file_path, transfer_id) VALUES (?, ?, ?, ?, ?, ?, "
      "?);";

  try {
    SQLiteStatement stmt(conn->get(), sql);
    stmt.bindText(1, client);
    stmt.bindText(2, filename);
    stmt.bindInt(3, chunkNumber);
    stmt.bindInt(4, totalChunk);
    stmt.bindText(5, hash);
    stmt.bindText16(6, filePath.wstring());
    stmt.bindText(7, transferId);
    stmt.step();
    if (local_tx) {
      local_tx->commit();
    };
  } catch (const DatabaseException &db_ex) {
    rollbackNoThrow(local_tx, "insertChunk");
    logError("Failed to insert chunk: " + std::string(db_ex.what()),
             db_ex.getErrorCode());
    throw;
  } catch (const std::exception &e) {
    rollbackNoThrow(local_tx, "insertChunk");
    logError("Failed to insert chunk: " + std::string(e.what()));
    throw DatabaseException("Failed to insert chunk: " + std::string(e.what()));
  }
}
/**
 * @brief Updates the status of a chunk in the database
 * @param filename Name of the file
 * @param chunkNumber Number of the chunk
 * @param status New status of the chunk
 */
void Database::updateChunkStatus(const std::string &client,
                                 const std::string &filename, int chunkNumber,
                                 const std::string &status) {
  auto conn = getConnection();
  std::unique_ptr<TransactionGuard> local_tx;
  if (!m_isTransactionActive) {
    local_tx = std::make_unique<TransactionGuard>(conn);
  }

  std::string sql = "UPDATE chunks SET status = ? WHERE filename = ? AND "
                    "chunk_number = ? AND client_address = ?;";

  try {
    SQLiteStatement stmt(conn->get(), sql);
    stmt.bindText(1, status);
    stmt.bindText(2, filename);
    stmt.bindInt(3, chunkNumber);
    stmt.bindText(4, client);
    stmt.step();
    if (local_tx) {
      local_tx->commit();
    };
  } catch (const std::exception &e) {
    rollbackNoThrow(local_tx, "updateChunkStatus");
    logError("Failed to update chunk status: " + std::string(e.what()));
    throw DatabaseException("Failed to update chunk status: " +
                            std::string(e.what()));
  }
}
/**
 * @brief Retrieves the total chunk count for a file
 * @param filename Name of the file
 * @return Total chunk count
 */
int Database::getTotalChunkCount(const std::string &client,
                                 const std::string &filename) {
  auto conn = getConnection();
  std::string sql = "SELECT COALESCE(MAX(total_chunk), 0) FROM chunks WHERE "
                    "filename = ? AND client_address = ?;";
  try {
    SQLiteStatement stmt(conn->get(), sql);
    stmt.bindText(1, filename);
    stmt.bindText(2, client);
    if (stmt.step()) {
      return stmt.getInt(0);
    }
  } catch (const std::exception &e) {
    logError("Failed to get total chunk count: " + std::string(e.what()));
    throw DatabaseException("Failed to get total chunk count: " +
                            std::string(e.what()));
  }
  return 0;
}
/**
 * @brief Retrieves the expected pending chunk count for a file
 * @param filename Name of the file
 * @return Expected pending chunk count
 */
int Database::getChunkStatus(const std::string &client,
                              const std::string &filename, int chunkNumber) {
  auto conn = getConnection();
  std::string sql = "SELECT COUNT(*) FROM chunks WHERE filename = ? AND "
                    "chunk_number = ? AND client_address = ?;";
  try {
    SQLiteStatement stmt(conn->get(), sql);
    stmt.bindText(1, filename);
    stmt.bindInt(2, chunkNumber);
    stmt.bindText(3, client);
    if (stmt.step()) {
      return stmt.getInt(0);
    }
  } catch (const std::exception &e) {
    logError("Failed to get expected pending chunk count: " +
             std::string(e.what()));
    throw DatabaseException("Failed to get expected pending chunk count: " +
                            std::string(e.what()));
  }
  return 0;
}

int Database::getChunkRowCount(const std::string &client,
                               const std::string &filename) {
  auto conn = getConnection();
  std::string sql = "SELECT COUNT(*) FROM chunks WHERE filename = ? AND "
                    "client_address = ?;";
  try {
    SQLiteStatement stmt(conn->get(), sql);
    stmt.bindText(1, filename);
    stmt.bindText(2, client);
    if (stmt.step()) {
      return stmt.getInt(0);
    }
  } catch (const std::exception &e) {
    logError("Failed to get chunk row count: " + std::string(e.what()));
    throw DatabaseException("Failed to get chunk row count: " +
                            std::string(e.what()));
  }
  return 0;
}

bool Database::isChunkSuccessful(const std::string &client,
                                 const std::string &filename,
                                 int chunkNumber) {
  auto conn = getConnection();
  const std::string sql =
      "SELECT 1 FROM chunks WHERE filename = ? AND chunk_number = ? AND "
      "client_address = ? AND status = 'success' LIMIT 1;";
  try {
    SQLiteStatement stmt(conn->get(), sql);
    stmt.bindText(1, filename);
    stmt.bindInt(2, chunkNumber);
    stmt.bindText(3, client);
    return stmt.step();
  } catch (const std::exception &e) {
    logError("Failed to check successful chunk status: " +
             std::string(e.what()));
    throw DatabaseException("Failed to check successful chunk status: " +
                            std::string(e.what()));
  }
}

bool Database::isFileReconstructed(const std::string &client,
                                    const std::string &filename) {
  auto conn = getConnection();
  const std::string sql =
      "SELECT 1 FROM reconstructed_files WHERE client_address = ? AND "
      "filename = ? LIMIT 1;";
  try {
    SQLiteStatement stmt(conn->get(), sql);
    stmt.bindText(1, client);
    stmt.bindText(2, filename);
    return stmt.step();
  } catch (const std::exception &e) {
    logError("Failed to check reconstructed file status: " +
             std::string(e.what()));
    throw DatabaseException("Failed to check reconstructed file status: " +
                            std::string(e.what()));
  }
}

std::string Database::getActiveTransferId(const std::string &client,
                                          const std::string &filename) {
  auto conn = getConnection();
  const std::string sql =
      "SELECT transfer_id FROM chunks WHERE client_address = ? AND filename = "
      "? ORDER BY id LIMIT 1;";
  try {
    SQLiteStatement stmt(conn->get(), sql);
    stmt.bindText(1, client);
    stmt.bindText(2, filename);
    return stmt.step() ? stmt.getText(0) : std::string{};
  } catch (const std::exception &e) {
    throw DatabaseException("Failed to read active transfer id: " +
                            std::string(e.what()));
  }
}

std::string
Database::getReconstructedTransferId(const std::string &client,
                                     const std::string &filename) {
  auto conn = getConnection();
  const std::string sql =
      "SELECT transfer_id FROM reconstructed_files WHERE client_address = ? "
      "AND filename = ? LIMIT 1;";
  try {
    SQLiteStatement stmt(conn->get(), sql);
    stmt.bindText(1, client);
    stmt.bindText(2, filename);
    return stmt.step() ? stmt.getText(0) : std::string{};
  } catch (const std::exception &e) {
    throw DatabaseException("Failed to read reconstructed transfer id: " +
                            std::string(e.what()));
  }
}

/**
 * @brief Retrieves chunks for a specific file
 * @param filename Name of the file
 * @return Vector of ChunkMetadata objects representing the chunks
 */
std::vector<ChunkMetadata>
Database::getChunksForFile(const std::string &client,
                           const std::string &filename) {
  std::vector<ChunkMetadata> chunks;
  auto conn = getConnection();
  std::string sql =
      "SELECT client_address, filename, chunk_number, total_chunk, hash, "
      "file_path, transfer_id FROM chunks WHERE filename = ? AND client_address = ? AND "
      "status = 'success' ORDER BY chunk_number ASC;";

  try {
    SQLiteStatement stmt(conn->get(), sql);
    stmt.bindText(1, filename);
    stmt.bindText(2, client);
    while (stmt.step()) {
      chunks.emplace_back(stmt.getText(0), stmt.getText(1), stmt.getInt(2),
                          stmt.getInt(3), stmt.getText(4), stmt.getText16(5));
      chunks.back().setTransferId(stmt.getText(6));
    }
  } catch (const std::exception &e) {
    logError("Failed to get chunks for file: " + std::string(e.what()));
    throw DatabaseException("Failed to get chunks for file: " +
                            std::string(e.what()));
  }
  return chunks;
}
/**
 * @brief Adds a reconstructed file entry to the database
 * @param clientAddress Address of the client
 * @param filename Name of the reconstructed file
 * @param path Path where the file was reconstructed
 */
void Database::addReconstructedFile(const std::string &clientAddress,
                                    const std::string &filename,
                                    const fs::path &path,
                                    const std::string &transferId) {
  auto conn = getConnection();
  std::unique_ptr<TransactionGuard> local_tx;
  if (!m_isTransactionActive) {
    local_tx = std::make_unique<TransactionGuard>(conn);
  }
  // Usa a diretiva 'OR IGNORE' para evitar erros se o arquivo já foi
  // adicionado, o que é seguro neste fluxo.
  std::string sql =
      "INSERT OR IGNORE INTO reconstructed_files (client_address, filename, "
      "reconstructed_path, transfer_id) VALUES (?, ?, ?, ?);";
  try {
    SQLiteStatement stmt(conn->get(), sql);
    stmt.bindText(1, clientAddress);
    stmt.bindText(2, filename);
    stmt.bindText16(3, path.wstring());
    stmt.bindText(4, transferId);
    stmt.step();
    if (local_tx) {
      local_tx->commit();
    };
  } catch (const std::exception &e) {
    rollbackNoThrow(local_tx, "addReconstructedFile");
    logError("Falha ao adicionar arquivo reconstruído: " +
             std::string(e.what()));
    throw DatabaseException("Falha ao adicionar arquivo reconstruído: " +
                            std::string(e.what()));
  }
}

void Database::deleteReconstructedFile(const std::string &clientAddress,
                                       const std::string &filename) {
  auto conn = getConnection();
  std::unique_ptr<TransactionGuard> local_tx;
  if (!m_isTransactionActive) {
    local_tx = std::make_unique<TransactionGuard>(conn);
  }
  const std::string sql =
      "DELETE FROM reconstructed_files WHERE client_address = ? AND "
      "filename = ?;";

  try {
    SQLiteStatement stmt(conn->get(), sql);
    stmt.bindText(1, clientAddress);
    stmt.bindText(2, filename);
    stmt.step();
    if (local_tx) {
      local_tx->commit();
    };
  } catch (const std::exception &e) {
    rollbackNoThrow(local_tx, "deleteReconstructedFile");
    logError("Falha ao deletar arquivo reconstruído: " +
             std::string(e.what()));
    throw DatabaseException("Falha ao deletar arquivo reconstruído: " +
                            std::string(e.what()));
  }
}

void Database::deleteChunksByFile(const std::string &clientAddress,
                                  const std::string &filename) {
  auto conn = getConnection();
  std::unique_ptr<TransactionGuard> local_tx;
  if (!m_isTransactionActive) {
    local_tx = std::make_unique<TransactionGuard>(conn);
  }
  std::string sql =
      "DELETE FROM chunks WHERE client_address = ? AND filename = ?;";

  try {
    SQLiteStatement stmt(conn->get(), sql);
    stmt.bindText(1, clientAddress);
    stmt.bindText(2, filename);
    stmt.step();
    if (local_tx) {
      local_tx->commit();
    };
  } catch (const std::exception &e) {
    rollbackNoThrow(local_tx, "deleteChunksByFile");
    logError("Falha ao deletar chunks pelo arquivo: " + std::string(e.what()));
    throw DatabaseException("Falha ao deletar chunks pelo arquivo: " +
                            std::string(e.what()));
  }
}

/**
 * @brief Logs an error message to the database
 * @param errorMessage Error message to log
 * @param errorCode Error code associated with the error
 * @param severity Severity level of the error
 * @param stackTrace Stack trace of the error
 */
void Database::logError(const std::string &errorMessage, int errorCode,
                        const std::string &severity,
                        const std::string &stackTrace) {
  auto conn = getConnection();

  try {
    std::string sql = "INSERT INTO error_logs (error_message, error_code, "
                      "severity, stack_trace) VALUES (?, ?, ?, ?);";
    SQLiteStatement stmt(conn->get(), sql);
    stmt.bindText(1, errorMessage);
    stmt.bindInt(2, errorCode);
    stmt.bindText(3, severity);
    stmt.bindText(4, stackTrace);
    stmt.step();
  } catch (const std::exception &e) {
    Logger::getInstance().error("Database",
                                "Failed to log error to database: " +
                                    std::string(e.what()));
    Logger::getInstance().error(
        "Database", "Original error: " + errorMessage +
                        " (code: " + std::to_string(errorCode) + ")");
  }
}

/**
 * @brief Returns counters for all in-progress file transfers.
 *
 * Queries files that have rows in the chunks table but are NOT yet present
 * in the reconstructed_files table. Groups by (client_address, filename) and
 * returns total_chunk alongside the count of 'success' chunks.
 *
 * This is intended to be called once at startup so that the FileReceiver
 * can pre-populate its in-memory remaining-chunk counters, enabling
 * resilience to server restarts mid-transfer.
 */
std::vector<Database::InProgressFileInfo>
Database::getInProgressFileCounters() {
  std::vector<InProgressFileInfo> result;
  auto conn = getConnection();

  std::string sql = R"(
    SELECT c.client_address,
           c.filename,
           COALESCE(MAX(c.total_chunk), 0) AS total_chunk,
           COUNT(CASE WHEN c.status = 'success' THEN 1 END) AS successful_count
    FROM chunks c
    LEFT JOIN reconstructed_files rf
           ON c.client_address = rf.client_address
          AND c.filename = rf.filename
    WHERE rf.id IS NULL
    GROUP BY c.client_address, c.filename;
  )";

  try {
    SQLiteStatement stmt(conn->get(), sql);
    while (stmt.step()) {
      InProgressFileInfo info;
      info.clientAddress = stmt.getText(0);
      info.filename = stmt.getText(1);
      info.totalChunks = stmt.getInt(2);
      info.successfulCount = stmt.getInt(3);
      result.push_back(std::move(info));
    }
  } catch (const std::exception &e) {
    logError("Failed to get in-progress file counters: " +
             std::string(e.what()));
    throw DatabaseException("Failed to get in-progress file counters: " +
                            std::string(e.what()));
  }
  return result;
}
