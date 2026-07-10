#include "Database.h"
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
  SQLiteStatement stmt(m_conn->get(), "COMMIT;");
  stmt.step();
  m_committed = true;
}
/**
 * @brief Rolls back the active transaction
 */
void TransactionGuard::rollback() {
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
  if (sqlite3_open(dbPath.c_str(), &m_db) != SQLITE_OK) {
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

void SQLiteStatement::bindInt64(int index, int64_t value) {
  if (sqlite3_bind_int64(m_stmt, index, value) != SQLITE_OK) {
    throw DatabaseException("Failed to bind int64 parameter at index " +
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
    throw DatabaseException("Statement execution failed", result);
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

int64_t SQLiteStatement::getInt64(int column) const {
  return sqlite3_column_int64(m_stmt, column);
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
          "Database::Database",
          "Failed to configure SQLite threading mode! Code: " +
              std::to_string(sqlite_config_result));
      throw DatabaseException("Failed to configure SQLite threading mode.");
    }
    sqlite_configured = true;
    Logger::getInstance().info(
        "Database::Database",
        "SQLite threading mode configured to SERIALIZED.");
  }

  std::filesystem::path dbFilePath(m_dbPath);
  if (!dbFilePath.parent_path().empty()) {
    std::filesystem::create_directories(dbFilePath.parent_path());
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
                filename TEXT NOT NULL,
                chunk_number INTEGER NOT NULL,
                total_chunk INTEGER DEFAULT 0,
                hash TEXT NOT NULL,
                file_path TEXT NOT NULL,
                file_size INTEGER DEFAULT 0,    
                chunk_size INTEGER DEFAULT 0,   
                status TEXT CHECK(status IN ('pending', 'success', 'failed' ,'processed', 'abandoned')) DEFAULT 'pending',
                retries INTEGER DEFAULT 0,
                transfer_id TEXT NOT NULL DEFAULT '',
                created_at DATETIME DEFAULT CURRENT_TIMESTAMP
            );
        )";

    SQLiteStatement stmt(conn->get(), sql);
    stmt.step();

    char *alterErrMsg = nullptr;
    if (sqlite3_exec(conn->get(),
                     "ALTER TABLE chunks ADD COLUMN chunk_offset INTEGER "
                     "DEFAULT 0;",
                     nullptr, nullptr, &alterErrMsg) != SQLITE_OK) {
      const std::string alterError =
          alterErrMsg ? alterErrMsg : "Unknown error";
      if (alterError.find("duplicate column name") == std::string::npos) {
        Logger::getInstance().error(
            "Database::Database",
            "Failed to add chunks.chunk_offset column: " + alterError);
      }
      if (alterErrMsg) {
        sqlite3_free(alterErrMsg);
      }
    }

    ignoreDuplicateColumn(
        conn->get(),
        "ALTER TABLE chunks ADD COLUMN transfer_id TEXT NOT NULL DEFAULT '';",
        "chunks.transfer_id");

    SQLiteStatement stmtDropOldIndex(
        conn->get(), "DROP INDEX IF EXISTS idx_chunks_file_chunk;");
    stmtDropOldIndex.step();

    std::string sqlIndexFileChunk = R"(
            CREATE UNIQUE INDEX IF NOT EXISTS idx_chunks_file_chunk ON chunks (file_path, chunk_number);
        )";
    SQLiteStatement stmtIndexFileChunk(conn->get(), sqlIndexFileChunk);
    stmtIndexFileChunk.step();

    std::string sqlIndexStatusRetries = R"(
            CREATE INDEX IF NOT EXISTS idx_chunks_status_retries ON chunks (status, retries);
        )";
    SQLiteStatement stmtIndexStatusRetries(conn->get(), sqlIndexStatusRetries);
    stmtIndexStatusRetries.step();

    std::string sqlProcessedFiles = R"(
            CREATE TABLE IF NOT EXISTS processed_files (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            filename TEXT NOT NULL,
            file_path TEXT NOT NULL UNIQUE, 
            status TEXT CHECK(status IN ('processing', 'processed', 'failed')) DEFAULT 'processing',
            processed_at DATETIME DEFAULT CURRENT_TIMESTAMP);
        )";

    SQLiteStatement stmtProcessedFiles(conn->get(), sqlProcessedFiles);
    stmtProcessedFiles.step();

    ignoreDuplicateColumn(
        conn->get(),
        "ALTER TABLE processed_files ADD COLUMN file_size INTEGER DEFAULT 0;",
        "file_size");
    ignoreDuplicateColumn(
        conn->get(),
        "ALTER TABLE processed_files ADD COLUMN last_write_time_ns INTEGER "
        "DEFAULT 0;",
        "last_write_time_ns");
    ignoreDuplicateColumn(
        conn->get(),
        "ALTER TABLE processed_files ADD COLUMN content_hash TEXT DEFAULT '';",
        "content_hash");

    // Also create an index to speed up lookups.
    std::string sqlProcessedFilesIndex = R"(
            CREATE INDEX IF NOT EXISTS idx_processed_files_filename ON processed_files (filename);
        )";
    SQLiteStatement stmtProcessedFilesIndex(conn->get(),
                                            sqlProcessedFilesIndex);
    stmtProcessedFilesIndex.step();

    std::string sqlFailedFiles = R"(
           CREATE TABLE IF NOT EXISTS permanentlyFailedFiles (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            file_path TEXT NOT NULL UNIQUE,
            reason TEXT,
            timestamp DATETIME DEFAULT CURRENT_TIMESTAMP
);
        )";

    SQLiteStatement stmtFailedFiles(conn->get(), sqlFailedFiles);
    stmtFailedFiles.step();

    // Also create an index to speed up lookups.
    std::string sqlFailedFilesIndex = R"(
            CREATE INDEX IF NOT EXISTS idx_failed_files_filename ON permanentlyFailedFiles (file_path);
        )";

    SQLiteStatement stmtFailedFilesIndex(conn->get(), sqlFailedFilesIndex);
    stmtFailedFilesIndex.step();

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

    SQLiteStatement stmtBackfillTransferIds(
        conn->get(),
        "UPDATE chunks SET transfer_id = COALESCE((SELECT content_hash FROM "
        "processed_files WHERE processed_files.file_path = chunks.file_path), "
        "'') WHERE transfer_id = ''; ");
    stmtBackfillTransferIds.step();

    // No process can still own these rows after startup. Retrying them through
    // the normal failed-file path rebuilds chunks that were not yet persisted
    // when the previous process stopped.
    SQLiteStatement stmtRecoverInterrupted(
        conn->get(),
        "UPDATE processed_files SET status = 'failed' WHERE status = "
        "'processing';");
    stmtRecoverInterrupted.step();

    tx.commit();
    Logger::getInstance().info(
        "Database::Database",
        "Database initialized successfully with WAL mode.");
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
/**
 * Begins a new transaction in the database.
 *
 * @throws DatabaseException if a transaction is already active in the current
 * thread.
 */
void Database::beginTransaction() {
  // If a transaction is already active for this thread, skip nested begin
  // calls.
  if (m_isTransactionActive) {
    throw DatabaseException("Nested transactions are not supported.");
    return;
  }
  auto conn = getConnection();
  SQLiteStatement stmt(conn->get(), "BEGIN TRANSACTION;");
  stmt.step();
  m_isTransactionActive = true;
}
/**
 * Commits the current transaction if it has been initiated by this class.
 *
 * @return void
 *
 * @throws None
 */
void Database::commitTransaction() {
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
/**
 * Rolls back the current transaction if it is active.
 *
 * @return void
 *
 * @throws None
 */
void Database::rollbackTransaction() {
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
    throw DatabaseException(
        "Database not initialized. Call initialize() first.");
  return *instance;
}
/**
 * @brief Initializes the singleton instance of the Database class
 * @param dbPath Path to the SQLite database file
 */
void Database::initialize(const std::string &dbPath) {
  if (instance) {
    if (!dbPath.empty() && dbPath != instance->m_dbPath) {
      Logger::getInstance().warning(
          "Database::initialize",
          "Database is already initialized with path '" + instance->m_dbPath +
              "'. Ignoring reinitialization request for '" + dbPath + "'.");
    }
    return;
  }

  if (dbPath.empty()) {
    throw DatabaseException(
        "Database path cannot be empty on first initialize().");
  }

  std::call_once(initFlag, [&]() { instance.reset(new Database(dbPath)); });
}
/**
 * @brief Sets the database path
 * @param path Path to the SQLite database file
 */
std::shared_ptr<SQLiteConnection> Database::getConnection() {
  thread_local static std::shared_ptr<SQLiteConnection> thread_conn = nullptr;
  thread_local static const Database *thread_conn_owner = nullptr;

  if (!thread_conn || thread_conn_owner != this) {
    try {
      thread_conn = std::make_shared<SQLiteConnection>(m_dbPath);
      thread_conn_owner = this;
    } catch (const DatabaseException &e) {
      Logger::getInstance().critical(
          "Database::getConnection",
          "Failed to create thread-local DB connection: " +
              std::string(e.what()));
      throw DatabaseException("Failed to create thread-local DB connection: " +
                              std::string(e.what()));
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
void Database::insertChunk(const fs::path &filename, int chunkNumber,
                           int totalChunk, int filesize, int chunksize,
                           const std::string &hash, const fs::path &filePath,
                           uint64_t chunkOffset,
                           const std::string &transferId) {
  auto conn = getConnection();
  std::unique_ptr<TransactionGuard> local_tx;
  if (!m_isTransactionActive) {
    local_tx = std::make_unique<TransactionGuard>(conn);
  }

  std::string sql =
      "INSERT INTO chunks (filename, chunk_number, total_chunk, hash, "
      "file_path, file_size, chunk_size, chunk_offset, transfer_id) VALUES (?, "
      "?, ?, ?, ?, ?, ?, ?, ?);";

  try {
    SQLiteStatement stmt(conn->get(), sql);
    stmt.bindText16(1, filename.wstring());
    stmt.bindInt(2, chunkNumber);
    stmt.bindInt(3, totalChunk);
    stmt.bindText(4, hash);
    stmt.bindText16(5, filePath.wstring());
    stmt.bindInt(6, filesize);
    stmt.bindInt(7, chunksize);
    stmt.bindInt64(8, static_cast<int64_t>(chunkOffset));
    stmt.bindText(9, transferId);
    stmt.step();
    if (local_tx) {
      local_tx->commit();
    };
  } catch (const std::exception &e) {
    if (local_tx) {
      local_tx->rollback();
    }
    logError("Failed to insert chunk: " + std::string(e.what()));
    throw DatabaseException("Failed to insert chunk: " + std::string(e.what()));
  }
}
/**
 * Inserts a chunk into the database with the specified status.
 *
 * @param filename Name of the file
 * @param chunkNumber Number of the chunk
 * @param totalChunk Total number of chunks
 * @param filesize Size of the file
 * @param chunksize Size of the chunk
 * @param hash Hash of the chunk
 * @param filePath Path to the file
 * @param status Status of the chunk
 *
 * @throws DatabaseException If an error occurs while inserting the chunk into
 * the database
 */
void Database::insertChunk(const fs::path &filename, int chunkNumber,
                           int totalChunk, int filesize, int chunksize,
                           const std::string &hash, const fs::path &filePath,
                           const std::string &status, uint64_t chunkOffset,
                           const std::string &transferId) {
  auto conn = getConnection();
  std::unique_ptr<TransactionGuard> local_tx;
  if (!m_isTransactionActive) {
    local_tx = std::make_unique<TransactionGuard>(conn);
  }

  std::string sql = "INSERT INTO chunks (filename, chunk_number, total_chunk, "
                    "hash, file_path, file_size, chunk_size, status, "
                    "chunk_offset, transfer_id) VALUES (?, ?, ?, ?, ?, ?, ?, "
                    "?, ?, ?);";

  try {
    SQLiteStatement stmt(conn->get(), sql);
    stmt.bindText16(1, filename.wstring());
    stmt.bindInt(2, chunkNumber);
    stmt.bindInt(3, totalChunk);
    stmt.bindText(4, hash);
    stmt.bindText16(5, filePath.wstring());
    stmt.bindInt(6, filesize);
    stmt.bindInt(7, chunksize);
    stmt.bindText(8, status);
    stmt.bindInt64(9, static_cast<int64_t>(chunkOffset));
    stmt.bindText(10, transferId);
    stmt.step();
    if (local_tx) {
      local_tx->commit();
    };
  } catch (const DatabaseException &db_ex) {
    if (local_tx) {
      local_tx->rollback();
    }
    logError("Failed to insert chunk: " + std::string(db_ex.what()),
             db_ex.getErrorCode());
    throw; // Relanï¿½a a exceï¿½ï¿½o original para que o FileProcessor possa
           // tratï¿½-la (com o rollback)
  } catch (const std::exception &e) {
    if (local_tx) {
      local_tx->rollback();
    }
    logError("Failed to insert chunk: " + std::string(e.what()));
    throw DatabaseException("Failed to insert chunk: " + std::string(e.what()));
  }
}
/**
 * @brief Retrieves pending chunks from the database
 * @return Vector of ChunkMetadata objects representing pending chunks
 */
std::vector<ChunkMetadata> Database::getPendingChunks(int beforeAbandoned) {
  std::vector<ChunkMetadata> chunks;
  auto conn = getConnection();
  std::string sql = "SELECT filename, chunk_number, total_chunk, chunk_size, "
                    "hash, file_path, retries, chunk_offset, transfer_id FROM chunks WHERE status in "
                    "('pending', 'failed') AND retries < ?;";

  try {
    SQLiteStatement stmt(conn->get(), sql);
    stmt.bindInt(1, beforeAbandoned);
    while (stmt.step()) {
      chunks.emplace_back(stmt.getText16(0), stmt.getInt(1), stmt.getInt(2),
                          stmt.getInt(3), stmt.getText(4), stmt.getText16(5),
                          stmt.getInt(6));
      chunks.back().setChunkOffset(static_cast<uint64_t>(stmt.getInt64(7)));
      chunks.back().setTransferId(stmt.getText(8));
    }
  } catch (const std::exception &e) {
    logError("Failed to get pending chunks: " + std::string(e.what()));
    throw DatabaseException("Failed to get pending chunks: " +
                            std::string(e.what()));
  }
  return chunks;
}
/**
 * @brief Updates the status of a chunk in the database
 * @param filename Name of the file
 * @param chunkNumber Number of the chunk
 * @param status New status of the chunk
 */
void Database::updateChunkStatus(const fs::path &filePath, int chunkNumber,
                                 const std::string &status) {
  auto conn = getConnection();
  std::unique_ptr<TransactionGuard> local_tx;
  if (!m_isTransactionActive) {
    local_tx = std::make_unique<TransactionGuard>(conn);
  }
  std::string sql =
      "UPDATE chunks SET status = ? WHERE file_path = ? AND chunk_number = ?;";

  try {
    SQLiteStatement stmt(conn->get(), sql);
    stmt.bindText(1, status);
    stmt.bindText16(2, filePath.wstring());
    stmt.bindInt(3, chunkNumber);
    stmt.step();
    if (local_tx) {
      local_tx->commit();
    }
  } catch (const std::exception &e) {
    if (local_tx) {
      local_tx->rollback();
    }
    logError("Failed to update chunk status: " + std::string(e.what()));
    throw DatabaseException("Failed to update chunk status: " +
                            std::string(e.what()));
  }
}
/**
 * @brief Updates the retry count of a chunk in the database
 * @param filename Name of the file
 * @param chunkNumber Number of the chunk
 * @param retries New retry count
 */
void Database::updateChunkRetries(const fs::path &filePath, int chunkNumber,
                                  int retries) {
  auto conn = getConnection();
  std::unique_ptr<TransactionGuard> local_tx;
  if (!m_isTransactionActive) {
    local_tx = std::make_unique<TransactionGuard>(conn);
  }
  std::string sql =
      "UPDATE chunks SET retries = ? WHERE file_path = ? AND chunk_number = ?;";

  try {
    SQLiteStatement stmt(conn->get(), sql);
    stmt.bindInt(1, retries);
    stmt.bindText16(2, filePath.wstring());
    stmt.bindInt(3, chunkNumber);
    stmt.step();
    if (local_tx) {
      local_tx->commit();
    }
  } catch (const std::exception &e) {
    if (local_tx) {
      local_tx->rollback();
    }
    logError("Failed to update chunk retries: " + std::string(e.what()));
    throw DatabaseException("Failed to update chunk retries: " +
                            std::string(e.what()));
  }
}
/**
 * @brief Retrieves the expected chunk count for a file
 * @param filename Name of the file
 * @return Expected chunk count
 */
int Database::getExpectedChunkCount(const fs::path &filePath) {
  auto conn = getConnection();
  std::string sql = "SELECT COUNT(*) FROM chunks WHERE file_path = ?;";
  try {
    SQLiteStatement stmt(conn->get(), sql);
    stmt.bindText16(1, filePath.wstring());
    if (stmt.step()) {
      return stmt.getInt(0);
    }
  } catch (const std::exception &e) {
    logError("Failed to get expected chunk count: " + std::string(e.what()));
    throw DatabaseException("Failed to get expected chunk count: " +
                            std::string(e.what()));
  }
  return 0;
}
/**
 * Deletes all chunks associated with a given file from the database.
 *
 * @param filename The name of the file for which chunks should be deleted.
 *
 * @return None
 *
 * @throws DatabaseException If an error occurs during the deletion process.
 */
void Database::deleteFileChunks(const fs::path &filePath) {
  auto conn = getConnection();
  std::unique_ptr<TransactionGuard> local_tx;
  if (!m_isTransactionActive) {
    local_tx = std::make_unique<TransactionGuard>(conn);
  }
  std::string sql = "DELETE FROM chunks WHERE file_path = ?;";
  std::string filenameStr = filePath.u8string();

  try {
    SQLiteStatement stmt(conn->get(), sql);
    stmt.bindText16(1, filePath.wstring());
    stmt.step(); // Executa o DELETE
    if (local_tx) {
      local_tx->commit();
    }
    Logger::getInstance().info(
        "Database", "Deleted existing chunks for filepath: " + filenameStr);
  } catch (const std::exception &e) {
    if (local_tx) {
      local_tx->rollback();
    }
    logError("Failed to delete chunks for " + filenameStr + ": " +
             std::string(e.what()));
    throw DatabaseException("Failed to delete chunks for " + filenameStr +
                            ": " + std::string(e.what()));
  }
}
/**
 * Checks if a file has been fully processed by verifying the number of
 * successful chunks in the database.
 *
 * @param filename The path to the file to check.
 *
 * @return true if the file is fully processed, false otherwise.
 *
 * @throws std::exception If an error occurs while accessing the database.
 */
bool Database::isFileFullyProcessed(const fs::path &filename_path) {
  auto conn = getConnection();
  std::string sql_check_processed =
      "SELECT COUNT(*) FROM processed_files WHERE file_path = ? and status = "
      "'processed'";
  try {
    SQLiteStatement stmt(conn->get(), sql_check_processed);
    stmt.bindText16(1, filename_path.wstring());
    if (stmt.step() && stmt.getInt(0) > 0) {
      return true; // Encontrado na tabela de processados, entï¿½o estï¿½
                   // completo.
    }
  } catch (const std::exception &e) {
    logError("Failed to check processed_files table for " +
             filename_path.u8string() + ": " + std::string(e.what()));
  }

  // SQL: reads expected total chunk count and processed chunk count.
  std::string sql = R"(
        SELECT
            COALESCE(MAX(total_chunk), 0),
            COALESCE(SUM(CASE WHEN status = 'success' THEN 1 ELSE 0 END), 0)
        FROM chunks
        WHERE file_path = ?;
    )";

  std::string filenameStr = filename_path.u8string();

  try {
    SQLiteStatement stmt(conn->get(), sql);
    stmt.bindText16(1, filename_path.wstring());

    if (stmt.step()) {
      int total_chunks = stmt.getInt(0);
      int success_chunks = stmt.getInt(1);

      // If total_chunks is 0, the file was never started.
      // Or it has no chunk records, so it is not fully processed.
      // If total_chunks > 0, verify that all chunks are marked success.
      if (total_chunks > 0 && total_chunks == success_chunks) {
        return true; // Sim, estï¿½ totalmente processado!
      }
    }
  } catch (const std::exception &e) {
    logError("Failed to check if file is fully processed (" + filenameStr +
             "): " + std::string(e.what()));
    return false;
  }
  return false;
}

bool Database::markFileAsProcessingIfChanged(const fs::path &filePath,
                                             int64_t fileSize,
                                             int64_t lastWriteTime,
                                             const std::string &contentHash) {
  auto conn = getConnection();
  std::unique_ptr<TransactionGuard> local_tx;
  if (!m_isTransactionActive) {
    local_tx = std::make_unique<TransactionGuard>(conn);
  }

  try {
    std::string sql_select =
        "SELECT status, file_size, last_write_time_ns, content_hash "
        "FROM processed_files WHERE file_path = ? LIMIT 1;";
    SQLiteStatement stmt_select(conn->get(), sql_select);
    stmt_select.bindText16(1, filePath.wstring());

    if (stmt_select.step()) {
      const std::string status = stmt_select.getText(0);
      const int64_t storedSize = stmt_select.getInt64(1);
      const int64_t storedLastWrite = stmt_select.getInt64(2);
      const std::string storedHash = stmt_select.getText(3);
      const bool sameFingerprint = storedSize == fileSize &&
                                   storedLastWrite == lastWriteTime &&
                                   storedHash == contentHash;
      if ((status == "processed" || status == "processing") &&
          sameFingerprint) {
        if (local_tx) {
          local_tx->commit();
        }
        return false;
      }
    }

    std::string sql_insert =
        "INSERT OR IGNORE INTO processed_files "
        "(filename, file_path, status, file_size, last_write_time_ns, "
        "content_hash) VALUES (?, ?, 'processing', ?, ?, ?);";
    SQLiteStatement stmt_insert(conn->get(), sql_insert);
    stmt_insert.bindText16(1, filePath.filename().wstring());
    stmt_insert.bindText16(2, filePath.wstring());
    stmt_insert.bindInt64(3, fileSize);
    stmt_insert.bindInt64(4, lastWriteTime);
    stmt_insert.bindText(5, contentHash);
    stmt_insert.step();

    std::string sql_update =
        "UPDATE processed_files SET status = 'processing', file_size = ?, "
        "last_write_time_ns = ?, content_hash = ? WHERE file_path = ?;";
    SQLiteStatement stmt_update(conn->get(), sql_update);
    stmt_update.bindInt64(1, fileSize);
    stmt_update.bindInt64(2, lastWriteTime);
    stmt_update.bindText(3, contentHash);
    stmt_update.bindText16(4, filePath.wstring());
    stmt_update.step();

    if (local_tx) {
      local_tx->commit();
    }
    return true;
  } catch (const std::exception &e) {
    if (local_tx) {
      local_tx->rollback();
    }
    logError("Failed to mark changed file as processing for " +
             filePath.u8string() + ": " + std::string(e.what()));
    throw;
  }
}

/**
 * Checks if a file is currently being processed in the database.
 *
 * @param filename_path The path of the file to check.
 *
 * @return true if the file is being processed, false otherwise.
 *
 * @throws std::exception if there is an error checking the processed_files
 * table.
 */
bool Database::isFileProcessing(const fs::path &filename_path) {
  auto conn = getConnection();
  std::string sql_check_processed =
      "SELECT COUNT(*) FROM processed_files WHERE file_path = ? AND status = "
      "'processing';";
  try {
    SQLiteStatement stmt(conn->get(), sql_check_processed);
    stmt.bindText16(1, filename_path.wstring());
    if (stmt.step() && stmt.getInt(0) > 0) {
      return true; // Encontrado na tabela de processados com status =
                   // 'processing', entï¿½o estï¿½ sendo processado.
    }
  } catch (const std::exception &e) {
    logError("Failed to check processed_files table for " +
             filename_path.u8string() + ": " + std::string(e.what()));
  }

  return false;
}

/**
 * Marks chunks in the database as abandoned if their status is 'pending' or
 * 'failed' and their retry count is greater than or equal to the specified
 * maximum total retries.
 *
 * @param maxTotalRetries The maximum total retries for a chunk to be considered
 * abandoned.
 *
 * @return None
 *
 * @throws DatabaseException If an error occurs while marking chunks as
 * abandoned.
 */
void Database::markChunksAsAbandoned(int maxTotalRetries) {
  auto conn = getConnection();
  std::unique_ptr<TransactionGuard> local_tx;
  if (!m_isTransactionActive) {
    local_tx = std::make_unique<TransactionGuard>(conn);
  }
  std::string sql = "UPDATE chunks SET status = 'abandoned' WHERE status IN "
                    "('pending', 'failed') AND retries >= ?;";

  try {
    SQLiteStatement stmt(conn->get(), sql);
    stmt.bindInt(1, maxTotalRetries);
    stmt.step();
    if (local_tx) {
      local_tx->commit();
    }
    Logger::getInstance().info("Database", "Marked chunks with retries >= " +
                                               std::to_string(maxTotalRetries) +
                                               " as abandoned.");
  } catch (const std::exception &e) {
    if (local_tx) {
      local_tx->rollback();
    }
    logError("Failed to mark chunks as abandoned: " + std::string(e.what()));
    throw DatabaseException("Failed to mark chunks as abandoned: " +
                            std::string(e.what()));
  }
}
/**
 * @brief Adds a processed file to the database and cleans up associated chunks
 * @param filename Name of the file
 * @param filesize Size of the file
 * @param filePath Path to the file
 */
void Database::addProcessedFileAndCleanupChunks(const fs::path &filePath,
                                                int64_t fileSize,
                                                int64_t lastWriteTime,
                                                const std::string &contentHash) {
  auto conn = getConnection();
  std::unique_ptr<TransactionGuard> local_tx;
  if (!m_isTransactionActive) {
    local_tx = std::make_unique<TransactionGuard>(conn);
  }

  try {
    const bool hasFingerprint =
        fileSize >= 0 && lastWriteTime >= 0 && !contentHash.empty();

    // 1. Ensure a processed_files row exists for this file.
    std::string sql_insert =
        "INSERT OR IGNORE INTO processed_files "
        "(filename, file_path, status, file_size, last_write_time_ns, "
        "content_hash) VALUES (?, ?, 'processed', ?, ?, ?);";
    SQLiteStatement stmt_insert(conn->get(), sql_insert);
    stmt_insert.bindText16(1, filePath.filename().wstring());
    stmt_insert.bindText16(2, filePath.wstring());
    stmt_insert.bindInt64(3, hasFingerprint ? fileSize : 0);
    stmt_insert.bindInt64(4, hasFingerprint ? lastWriteTime : 0);
    stmt_insert.bindText(5, hasFingerprint ? contentHash : "");
    stmt_insert.step();

    // 2. Update status to processed if the row already existed with another
    // status.
    if (hasFingerprint) {
      std::string sql_update =
          "UPDATE processed_files SET status = 'processed', file_size = ?, "
          "last_write_time_ns = ?, content_hash = ? WHERE file_path = ?;";
      SQLiteStatement stmt_update(conn->get(), sql_update);
      stmt_update.bindInt64(1, fileSize);
      stmt_update.bindInt64(2, lastWriteTime);
      stmt_update.bindText(3, contentHash);
      stmt_update.bindText16(4, filePath.wstring());
      stmt_update.step();
    } else {
      std::string sql_update =
          "UPDATE processed_files SET status = 'processed' WHERE file_path = ?;";
      SQLiteStatement stmt_update(conn->get(), sql_update);
      stmt_update.bindText16(1, filePath.wstring());
      stmt_update.step();
    }

    // 3. Remove all chunks associated with this file from chunks table.
    std::string sql_delete = "DELETE FROM chunks WHERE file_path = ?;";
    SQLiteStatement stmt_delete(conn->get(), sql_delete);
    stmt_delete.bindText16(1, filePath.wstring());
    stmt_delete.step();

    // 4. Commit transaction when every step succeeds.
    if (local_tx) {
      local_tx->commit();
    }
    Logger::getInstance().info(
        "Database", "Cleaned up chunks for successfully processed file: " +
                        filePath.u8string());

  } catch (const std::exception &e) {
    if (local_tx) {
      local_tx->rollback();
    }
    logError("Failed during file processing cleanup for " +
             filePath.u8string() + ": " + std::string(e.what()));
    throw; // Re-lanï¿½a a exceï¿½ï¿½o para que o chamador saiba que algo deu
           // errado
  }
}
/**
 * Marks a file as processing in the database.
 *
 * @param filePath The path of the file to be marked as processing.
 *
 * @throws std::exception If an error occurs during the file processing.
 */
void Database::markFileAsProcessing(const fs::path &filePath) {
  auto conn = getConnection();
  std::unique_ptr<TransactionGuard> local_tx;
  if (!m_isTransactionActive) {
    local_tx = std::make_unique<TransactionGuard>(conn);
  }

  try {
    // 1. Insert the record in the processed_files table.
    std::string sql_insert = "INSERT OR IGNORE INTO processed_files (filename, "
                             "file_path, status) VALUES (?, ?, ?);";
    SQLiteStatement stmt_insert(conn->get(), sql_insert);
    stmt_insert.bindText16(1, filePath.filename().wstring());
    stmt_insert.bindText16(2, filePath.wstring());
    stmt_insert.bindText(3, "processing");
    stmt_insert.step();

    std::string sql_update =
        "UPDATE processed_files SET status = 'processing' WHERE file_path = ?;";
    SQLiteStatement stmt_update(conn->get(), sql_update);
    stmt_update.bindText16(1, filePath.wstring());
    stmt_update.step();

    // 3. Commit transaction when every step succeeds.
    if (local_tx) {
      local_tx->commit();
    }
  } catch (const std::exception &e) {
    if (local_tx) {
      local_tx->rollback();
    }
    logError("Failed during file processing for " + filePath.u8string() + ": " +
             std::string(e.what()));
    throw; // Re-lanï¿½a a exceï¿½ï¿½o para que o chamador saiba que algo deu
           // errado
  }
}
/**
 * Marks a file as failed in the database.
 *
 * @param filePath The path of the file to mark as failed.
 *
 * @throws std::exception If an error occurs during the file processing.
 */
void Database::markFileAsFailed(const fs::path &filePath) {
  auto conn = getConnection();
  std::unique_ptr<TransactionGuard> local_tx;
  if (!m_isTransactionActive) {
    local_tx = std::make_unique<TransactionGuard>(conn);
  }

  try {
    std::string sql_insert = "INSERT OR IGNORE INTO processed_files (filename, "
                             "file_path, status) VALUES (?, ?, 'failed');";
    SQLiteStatement stmt_insert(conn->get(), sql_insert);
    stmt_insert.bindText16(1, filePath.filename().wstring());
    stmt_insert.bindText16(2, filePath.wstring());
    stmt_insert.step();

    std::string sql_update =
        "UPDATE processed_files SET status = 'failed' WHERE file_path = ?;";
    SQLiteStatement stmt_update(conn->get(), sql_update);
    stmt_update.bindText16(1, filePath.wstring());
    stmt_update.step();

    // 3. Commit transaction when every step succeeds.
    if (local_tx) {
      local_tx->commit();
    }
  } catch (const std::exception &e) {
    if (local_tx) {
      local_tx->rollback();
    }
    logError("Failed during file processing for " + filePath.u8string() + ": " +
             std::string(e.what()));
    throw; // Re-lanï¿½a a exceï¿½ï¿½o para que o chamador saiba que algo deu
           // errado
  }
}
/**
 * Retrieves a list of files that have failed processing.
 *
 * @return A vector of file paths that have failed processing.
 *
 * @throws DatabaseException if there was an error retrieving the list of failed
 * files.
 */
std::vector<fs::path> Database::getFailedFiles() {
  std::vector<fs::path> files;
  auto conn = getConnection();

  // SQL groups by file name and checks whether total chunk count
  // matches chunk count with success status.
  std::string sql = R"(
        SELECT  file_path
        FROM processed_files
		WHERE status = 'failed' and file_path not in (select file_path from permanentlyFailedFiles);
        
    )";

  try {
    SQLiteStatement stmt(conn->get(), sql);
    while (stmt.step()) {
      files.push_back(stmt.getText16(0));
    }
  } catch (const std::exception &e) {
    logError("Failed to get a list of failed files: " + std::string(e.what()));
    // Return empty list if query fails.
  }
  return files;
}
/**
 * @brief Retrieves a list of files that have been fully sent (all chunks are
 * 'success')
 * @return Vector of file paths that are fully sent
 */
std::vector<fs::path> Database::getFullySentFiles() {
  std::vector<fs::path> files;
  auto conn = getConnection();

  // SQL groups by file name and checks whether total chunk count
  // matches chunk count with success status.
  std::string sql = R"(
        SELECT  file_path, 
                COALESCE(MAX(total_chunk), 0),
                COALESCE(COUNT(*), 0)
        FROM chunks
        GROUP BY file_path
        HAVING SUM(CASE WHEN status != 'success' THEN 1 ELSE 0 END) = 0;
    )";

  try {
    SQLiteStatement stmt(conn->get(), sql);
    while (stmt.step()) {
      if (stmt.getInt(2) == stmt.getInt(1)) {
        files.push_back(stmt.getText16(0));
      }
    }
  } catch (const std::exception &e) {
    logError("Failed to get a list of fully sent files: " +
             std::string(e.what()));
    // Return empty list if query fails.
  }
  return files;
}
/**
 * Logs a permanently failed file in the database and updates the status of the
 * file and its associated chunks.
 *
 * @param filePath The path of the file that failed permanently.
 * @param reason The reason for the file failure.
 *
 * @throws DatabaseException If there was an error logging the permanently
 * failed file.
 */
void Database::logPermanentlyFailedFile(const fs::path &filePath,
                                        const std::string &reason) {

  auto conn = getConnection();
  std::unique_ptr<TransactionGuard> local_tx;
  if (!m_isTransactionActive) {
    local_tx = std::make_unique<TransactionGuard>(conn);
  }

  try {

    std::string sql = "INSERT OR IGNORE INTO permanentlyFailedFiles "
                      "(file_path, reason) VALUES (?, ?);";
    SQLiteStatement stmt(conn->get(), sql);
    stmt.bindText16(1, filePath.wstring());
    stmt.bindText(2, reason);
    stmt.step();

    std::string sql_insert =
        "UPDATE processed_files SET status = 'failed' WHERE file_path = ?;";
    SQLiteStatement stmt_insert(conn->get(), sql_insert);
    stmt_insert.bindText16(1, filePath.wstring());
    stmt_insert.step();

    // 2. Remove all chunks associated with this file from chunks table.
    std::string sql_delete = "DELETE FROM chunks WHERE file_path = ?;";
    SQLiteStatement stmt_delete(conn->get(), sql_delete);
    stmt_delete.bindText16(1, filePath.wstring());
    stmt_delete.step();

    if (local_tx) {
      local_tx->commit();
    }

    Logger::info("Database", "File '{}' registered as permanent failure.",
                 filePath.string());
  } catch (const DatabaseException &dbEx) {
    // If final DB logging also fails, fallback to file logger as last resort.
    if (local_tx) {
      local_tx->rollback();
    }
    logError("Failed to log permanently failed file: " + filePath.string() +
                 " reason: " + reason,
             0, "critical", dbEx.what());
    throw DatabaseException("Failed to log permanently failed file: " +
                                filePath.string() + " reason: " + reason,
                            dbEx.getErrorCode());
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
    Logger::getInstance().error("Database::logError",
                                "Failed to log error to database: " +
                                    std::string(e.what()));
    Logger::getInstance().error(
        "Database::logError", "Original error: " + errorMessage +
                                  " (code: " + std::to_string(errorCode) + ")");
  }
}
