// Arquivo: Database.h
#pragma once
#include "ChunkMetadata.h"
#include "Logger.h"
#include <filesystem> // Para std::filesystem::path in
#include <memory>
#include <mutex>
#include <sqlite3.h>
#include <stdexcept>
#include <string>
#include <vector>


class SQLiteConnection;
class SQLiteStatement;

namespace fs = std::filesystem;

// RAII para controle transacional
class TransactionGuard {
public:
  explicit TransactionGuard(std::shared_ptr<SQLiteConnection> conn);
  ~TransactionGuard() noexcept;

  void commit();
  void rollback();

private:
  std::shared_ptr<SQLiteConnection> m_conn;
  bool m_committed;
};

class Database {
public:
  static Database &getInstance();
  static void initialize(const std::string &dbPath = "");

  void beginTransaction();
  void commitTransaction();
  void rollbackTransaction();

  void insertChunk(const std::string &client, const std::string &filename,
                   int chunkNumber, int totalChunk, const std::string &hash,
                   const fs::path &filePath);
  void updateChunkStatus(const std::string &client, const std::string &filename,
                         int chunkNumber, const std::string &status);
  int getTotalChunkCount(const std::string &client,
                         const std::string &filename);
  std::vector<ChunkMetadata> getChunksForFile(const std::string &client,
                                              const std::string &filename);
  int getChunkStatus(const std::string &client, const std::string &filename,
                      int chunkNumber);
  bool isChunkSuccessful(const std::string &client, const std::string &filename,
                         int chunkNumber);
  bool isFileReconstructed(const std::string &client,
                           const std::string &filename);

  void addReconstructedFile(const std::string &clientAddress,
                            const std::string &filename, const fs::path &path);
  void deleteChunksByFile(const std::string &clientAddress,
                          const std::string &filename);

  void logError(const std::string &errorMessage, int errorCode = 0,
                const std::string &severity = "ERROR",
                const std::string &stackTrace = "");

  /// Info about a file transfer that was in progress when the server restarted.
  struct InProgressFileInfo {
    std::string clientAddress;
    std::string filename;
    int totalChunks;
    int successfulCount;
  };

  /// Returns counters for all in-progress file transfers (files with some
  /// successful chunks that have not yet been fully reconstructed).
  std::vector<InProgressFileInfo> getInProgressFileCounters();

  virtual ~Database();
  Database(const std::string dbPath);

private:
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
  explicit SQLiteConnection(const std::string &dbPath);
  ~SQLiteConnection();
  sqlite3 *get() const;

  SQLiteConnection(const SQLiteConnection &) = delete;
  SQLiteConnection &operator=(const SQLiteConnection &) = delete;

private:
  sqlite3 *m_db;
};

class SQLiteStatement {
public:
  SQLiteStatement(sqlite3 *db, const std::string &sql);
  ~SQLiteStatement();

  void bindText16(int index, const std::wstring &text);
  void bindText(int index, const std::string &text);
  void bindInt(int index, int value);
  bool step();
  std::string getText(int column) const;
  std::wstring getText16(int column) const;
  int getInt(int column) const;

  SQLiteStatement(const SQLiteStatement &) = delete;
  SQLiteStatement &operator=(const SQLiteStatement &) = delete;

private:
  sqlite3_stmt *m_stmt;
};

class DatabaseException : public std::runtime_error {
public:
  explicit DatabaseException(const std::string &message);
  DatabaseException(const std::string &message, int errorCode);

  int getErrorCode() const;

private:
  int m_errorCode;
};
