#pragma once
#include <filesystem>
#include <memory>
#include <mutex>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include <sqlite3.h>

#include "ChunkMetadata.h"
#include "Logger.h"

class SQLiteConnection;
class SQLiteStatement;

namespace fs = std::filesystem;

/**
 * RAII helper that manages one SQLite transaction scope.
 * It starts a transaction in the constructor and rolls it back in the
 * destructor unless commit() is called.
 */
class TransactionGuard {
public:
  /// Starts a transaction on the provided connection.
  explicit TransactionGuard(std::shared_ptr<SQLiteConnection> conn);
  /// Rolls back automatically when commit() was not called.
  ~TransactionGuard() noexcept;

  /// Commits the guarded transaction.
  void commit();
  /// Rolls back the guarded transaction.
  void rollback();

private:
  std::shared_ptr<SQLiteConnection> m_conn;
  bool m_committed;
};

class Database {
public:
  /// Returns the singleton instance.
  static Database &getInstance();
  /// Initializes the singleton on first call with the provided DB path.
  static void initialize(const std::string &dbPath = "");

  /// Begins a thread-local transaction when one is not active yet.
  void beginTransaction();
  /// Commits the active thread-local transaction.
  void commitTransaction();
  /// Rolls back the active thread-local transaction.
  void rollbackTransaction();

  /// Inserts chunk metadata with default status behavior.
  void insertChunk(const fs::path &filename, int chunkNumber, int totalChunk,
                   int filesize, int chunksize, const std::string &hash,
                   const fs::path &filePath, uint64_t chunkOffset = 0,
                   const std::string &transferId = "");

  /// Inserts chunk metadata with an explicit status value.
  void insertChunk(const fs::path &filename, int chunkNumber, int totalChunk,
                   int filesize, int chunksize, const std::string &hash,
                   const fs::path &filePath, const std::string &status,
                   uint64_t chunkOffset = 0,
                   const std::string &transferId = "");

  /// Returns pending chunks that match the abandonment age threshold.
  std::vector<ChunkMetadata> getPendingChunks(int beforeAbandoned);

  /// Updates the status of a specific chunk.
  void updateChunkStatus(const fs::path &filePath, int chunkNumber,
                         const std::string &status);

  /// Updates retry counter for a specific chunk.
  void updateChunkRetries(const fs::path &filePath, int chunkNumber,
                          int retries);

  /// Returns expected total chunk count for a file.
  int getExpectedChunkCount(const fs::path &filePath);

  /// Returns true when all chunks for the file are marked as success.
  bool isFileFullyProcessed(const fs::path &filename);

  /// Marks the file as processing only when its persisted fingerprint changed.
  bool markFileAsProcessingIfChanged(const fs::path &filePath,
                                     int64_t fileSize,
                                     int64_t lastWriteTime,
                                     const std::string &contentHash);

  /// Returns true when the file is currently marked as processing.
  bool isFileProcessing(const fs::path &filename);

  /// Marks the file as processing in the tracking table.
  void markFileAsProcessing(const fs::path &filePath);

  /// Removes all chunk rows for the file.
  void deleteFileChunks(const fs::path &filePath);

  /// Marks chunks as abandoned when retry totals exceed limits.
  void markChunksAsAbandoned(int maxTotalRetries);

  /// Marks file as processed and removes related chunk rows.
  void addProcessedFileAndCleanupChunks(const fs::path &filePath,
                                        int64_t fileSize = -1,
                                        int64_t lastWriteTime = -1,
                                        const std::string &contentHash = "");

  /// Persists a permanent file failure and the associated reason.
  void logPermanentlyFailedFile(const fs::path &filePath,
                                const std::string &reason);

  /// Marks a file as failed in the processed files table.
  void markFileAsFailed(const fs::path &filePath);

  /// Returns files that were fully sent and completed.
  std::vector<fs::path> getFullySentFiles();

  /// Returns files currently tracked as failed.
  std::vector<fs::path> getFailedFiles();

  /// Persists an application error row.
  void logError(const std::string &errorMessage, int errorCode = 0,
                const std::string &severity = "ERROR",
                const std::string &stackTrace = "");

  /// Releases database resources.
  virtual ~Database();

private:
  explicit Database(std::string dbPath);
  Database() = delete;
  Database(const Database &) = delete;
  Database &operator=(const Database &) = delete;

  std::string m_dbPath;
  std::shared_ptr<SQLiteConnection> getConnection();

  static std::unique_ptr<Database> instance;
  static std::once_flag initFlag;
  static thread_local bool m_isTransactionActive;
};

class SQLiteConnection {
public:
  /// Opens a sqlite3 connection for the provided file path.
  explicit SQLiteConnection(const std::string &dbPath);
  /// Closes the sqlite3 connection.
  ~SQLiteConnection();
  /// Returns the raw sqlite3 handle.
  sqlite3 *get() const;

  SQLiteConnection(const SQLiteConnection &) = delete;
  SQLiteConnection &operator=(const SQLiteConnection &) = delete;

private:
  sqlite3 *m_db;
};

class SQLiteStatement {
public:
  /// Prepares SQL statement text against the given sqlite3 handle.
  SQLiteStatement(sqlite3 *db, const std::string &sql);
  /// Finalizes the prepared statement.
  ~SQLiteStatement();

  /// Binds UTF-8 text using a 1-based parameter index.
  void bindText(int index, const std::string &text);

  /// Binds UTF-16 text using a 1-based parameter index.
  void bindText16(int index, const std::wstring &text);

  /// Binds integer value using a 1-based parameter index.
  void bindInt(int index, int value);
  void bindInt64(int index, int64_t value);
  /// Executes one step and returns true when a row is available.
  bool step();
  /// Reads UTF-8 text from a result column.
  std::string getText(int column) const;

  /// Reads UTF-16 text from a result column.
  std::wstring getText16(int column) const;
  /// Reads integer value from a result column.
  int getInt(int column) const;
  int64_t getInt64(int column) const;

  SQLiteStatement(const SQLiteStatement &) = delete;
  SQLiteStatement &operator=(const SQLiteStatement &) = delete;

private:
  sqlite3_stmt *m_stmt;
};

class DatabaseException : public std::runtime_error {
public:
  /// Constructs exception with message only.
  explicit DatabaseException(const std::string &message);
  /// Constructs exception with message and sqlite error code.
  DatabaseException(const std::string &message, int errorCode);

  /// Returns associated sqlite error code.
  int getErrorCode() const;

private:
  int m_errorCode;
};
